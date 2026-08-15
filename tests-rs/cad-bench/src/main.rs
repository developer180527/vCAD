//! Prints the performance report. `cargo run -p cad-bench --release --bin cad-perf`
//!
//! Separate from the tests on purpose. The tests assert what must not regress and stay quiet;
//! this prints every number, including the ones nothing asserts, because the first question when
//! something feels slow is "where does the time actually go" and a pass/fail suite cannot answer
//! it. Run it, read it, then decide what to optimise — rather than guessing and optimising the
//! wrong thing, which is how this project spent a day inside bgfx's instance binding while the
//! bug was a matrix constructor.

use cad_bench::workloads::*;
use cad_bench::{heading, measure, Scaling};

fn main() {
    println!("cad-perf — measured, not assumed");
    println!(
        "build: {}",
        if cfg!(debug_assertions) {
            "debug (numbers are not release numbers)"
        } else {
            "release"
        }
    );

    recompute();
    kernel();
    tessellation();
    scene();
    sketch();
    document();

    println!("\nDone. Growth per doubling is the number to watch; absolute times are for context.");
}

fn recompute() {
    heading("Recompute — the DAG walk, with geometry cost held near zero");

    Scaling::measure_prepared(
        "cold recompute of a translate chain",
        &[64, 128, 256, 512],
        5,
        translate_chain,
        |(s, _)| {
            s.recompute().expect("recompute");
        },
    )
    .report();

    Scaling::measure_prepared(
        "recompute after editing the LAST feature",
        &[64, 128, 256, 512],
        5,
        |n| {
            let (mut s, tip) = translate_chain(n);
            s.recompute().expect("recompute");
            (s, tip)
        },
        |(s, tip)| {
            s.set_length(*tip, "dx", 3.0).expect("edit");
            s.recompute().expect("recompute");
        },
    )
    .report();
    println!("    (the DDC's whole claim: one leaf changed, so cost should not track n)");

    Scaling::measure(
        "BUILD ONLY — n add() calls, no recompute",
        &[64, 128, 256, 512],
        5,
        |n| {
            let _ = translate_chain(n);
        },
    )
    .report();
}

fn kernel() {
    heading("Kernel — OCCT booleans, where the geometry actually is");

    Scaling::measure("chain of boolean cuts", &[8, 16, 32], 3, |n| {
        let (mut s, _) = cut_chain(n);
        s.recompute().expect("recompute");
    })
    .report();
}

fn tessellation() {
    heading("Tessellation — shape to mesh, and the cache in front of it");

    Scaling::measure("tessellate n distinct boxes", &[32, 64, 128], 3, |n| {
        let (mut s, objects) = distinct_boxes(n);
        for o in objects {
            s.tessellate(o, 0.05, 0.35).expect("tessellate");
        }
    })
    .report();

    let (mut s, objects) = distinct_boxes(64);
    for &o in &objects {
        s.tessellate(o, 0.05, 0.35).expect("tessellate");
    }
    s.reset_mesh_cache_stats().expect("reset");
    let warm = measure(
        "re-tessellate the same 64 boxes (all cache hits)",
        64,
        5,
        |_| {
            for &o in &objects {
                s.tessellate(o, 0.05, 0.35).expect("tessellate");
            }
        },
    );
    let stats = s.mesh_cache_stats().expect("stats");
    println!(
        "  re-tessellate 64 cached boxes: {:.3} ms   hits {} misses {}",
        warm.ms(),
        stats.hits,
        stats.misses
    );
}

fn scene() {
    heading("Scene assembly — placements to batches, the CPU half of the renderer");

    Scaling::measure(
        "scene build from n placements",
        &[1_000, 4_000, 16_000],
        3,
        |n| {
            let mut s = placements(n);
            s.scene_update(0.05, 0.35).expect("scene update");
        },
    )
    .report();

    let mut s = placements(16_000);
    s.scene_update(0.05, 0.35).expect("scene update");
    let idle = measure("scene_update on an unchanged document", 16_000, 20, |_| {
        s.scene_update(0.05, 0.35).expect("scene update");
    });
    let stats = s.scene_stats().expect("stats");
    println!(
        "  idle scene_update at 16k placements: {:.3} ms   rebuilds {}  instances {}  unique meshes {}",
        idle.ms(),
        stats.rebuilds,
        stats.instances,
        stats.unique_meshes
    );
    println!(
        "    (the digest walk is O(placements) and unavoidable; rebuilds is what must stay at 1)"
    );
}

fn sketch() {
    heading("Sketch solver — planegcs, over a connected constraint graph");

    Scaling::measure_prepared(
        "solve a chain of n constrained lines",
        &[16, 32, 64, 128],
        3,
        sketch_chain,
        |(s, sk)| {
            s.solve(*sk).expect("solve");
        },
    )
    .report();
    println!(
        "    (measured, not asserted: the solver's complexity class is not something we chose)"
    );
}

fn document() {
    heading("Document — save and reopen");

    let dir = std::env::temp_dir();
    Scaling::measure_prepared(
        "save then open a chain of n features",
        &[64, 128, 256],
        3,
        |n| {
            let (mut s, _) = translate_chain(n);
            s.recompute().expect("recompute");
            (s, n)
        },
        |(s, n)| {
            let path = dir.join(format!("cad-perf-{}.vcad", *n));
            let path = path.to_str().expect("utf-8 temp path");
            s.save(path).expect("save");
            let mut reopened = cad::Session::new().expect("session");
            reopened.open(path).expect("open");
        },
    )
    .report();
}
