//! What must not regress, asserted.
//!
//! Two kinds of assertion here, and the difference matters.
//!
//! **Counter assertions** come first wherever the core exposes a counter. "The second
//! tessellation was a cache hit" is a statement about behaviour that a fast machine cannot
//! satisfy by accident and a slow one cannot fail. Prefer these always.
//!
//! **Growth assertions** are the fallback for work with no counter. They check the complexity
//! class between doubling sizes, never a wall-clock threshold, because a threshold encodes the
//! machine it was written on and gets raised rather than investigated.
//!
//! Two workloads are measured and NOT asserted green, because they are currently wrong — see the
//! `#[ignore]`d tests at the bottom. They are written as the assertion we want, so fixing the
//! code turns them on rather than requiring someone to first work out what to assert.

use cad_bench::workloads::*;
use cad_bench::{Growth, Scaling};

// ── counter assertions ──────────────────────────────────────────────────────────────────

/// The DDC's central claim: editing one leaf recomputes one feature, whatever the chain length.
///
/// Counted, not timed. The timing agrees — 1.7 ms at n=64 and 3.0 ms at n=512 — but the timing
/// is a consequence, and a machine fast enough to recompute all 512 in 3 ms would pass a timing
/// assertion while the cache did nothing.
#[test]
fn editing_a_leaf_recomputes_exactly_one_feature() {
    for n in [64usize, 256] {
        let (mut s, tip) = translate_chain(n);
        s.recompute().expect("first recompute");

        s.set_length(tip, "dx", 7.5).expect("edit the tip");
        let report = s.recompute().expect("second recompute");

        assert_eq!(
            report.computed, 1,
            "editing the last feature of a chain of {n} recomputed {} features; \
             only the edited one depends on the change",
            report.computed
        );
        assert!(
            report.cached + report.skipped >= n as u64,
            "chain of {n}: only {} of the untouched features were served from cache or skipped",
            report.cached + report.skipped
        );
    }
}

/// Editing the ROOT must recompute the whole chain. The mirror of the test above: without it,
/// "computed == 1" could be satisfied by a cache that never invalidates anything.
#[test]
fn editing_the_root_recomputes_the_whole_chain() {
    let n = 64usize;
    let mut s = cad::Session::new().expect("session");
    let root = s.add_box(20.0, 20.0, 20.0).expect("box");
    let mut tip = root;
    for _ in 0..n {
        tip = s.add_translate(tip, 1.0, 0.0, 0.0).expect("translate");
    }
    s.recompute().expect("first recompute");
    let _ = tip;

    s.set_length(root, "dx", 25.0).expect("edit the root");
    let report = s.recompute().expect("second recompute");
    assert!(
        report.computed >= n as u64,
        "editing the root recomputed only {} of {n} dependent features",
        report.computed
    );
}

/// Tessellating the same shape twice hits the cache. The mesh cache is what makes an orbit and a
/// reopen cheap, and a miss here is invisible in every other test.
#[test]
fn re_tessellating_the_same_shapes_is_all_cache_hits() {
    let (mut s, objects) = distinct_boxes(32);
    for &o in &objects {
        s.tessellate(o, 0.05, 0.35).expect("first tessellation");
    }

    s.reset_mesh_cache_stats().expect("reset");
    for &o in &objects {
        s.tessellate(o, 0.05, 0.35).expect("second tessellation");
    }

    let stats = s.mesh_cache_stats().expect("stats");
    assert_eq!(stats.misses, 0, "re-tessellation missed the cache {} times", stats.misses);
    assert!(stats.hits >= objects.len() as u64, "expected a hit per shape, got {}", stats.hits);
}

/// Identical placements collapse to one mesh, and an unchanged document does not rebuild.
///
/// Both are counters the scale story depends on, and both were reported correct for months by a
/// renderer that was drawing one transform repeatedly — so they are asserted here, at the scene
/// layer, where no GPU is involved and the count cannot be confused with a picture.
#[test]
fn identical_placements_dedupe_and_idle_updates_do_not_rebuild() {
    let n = 2_000usize;
    let mut s = placements(n);

    s.scene_update(0.05, 0.35).expect("first scene update");
    let first = s.scene_stats().expect("stats");
    assert_eq!(first.unique_meshes, 1, "one box placed {n} times is one unique mesh");
    assert_eq!(first.instances, n as u64, "every placement must become an instance");

    for _ in 0..5 {
        s.scene_update(0.05, 0.35).expect("idle scene update");
    }
    let after = s.scene_stats().expect("stats");
    assert_eq!(
        after.rebuilds, first.rebuilds,
        "five updates of an unchanged document rebuilt the scene {} extra times",
        after.rebuilds - first.rebuilds
    );
}

// ── growth assertions ───────────────────────────────────────────────────────────────────

#[test]
fn cold_recompute_is_linear_in_chain_length() {
    let s = Scaling::measure_prepared(
        "cold recompute",
        &[64, 128, 256],
        3,
        translate_chain,
        |(s, _)| {
            s.recompute().expect("recompute");
        },
    );
    s.report();
    s.assert_growth(Growth::Linear, 1.0);
}

#[test]
fn tessellation_is_linear_in_shape_count() {
    let s = Scaling::measure_prepared(
        "tessellate n distinct boxes",
        &[32, 64, 128],
        3,
        distinct_boxes,
        |(s, objects)| {
            for &o in objects.iter() {
                s.tessellate(o, 0.05, 0.35).expect("tessellate");
            }
        },
    );
    s.report();
    s.assert_growth(Growth::Linear, 1.0);
}

/// Scene assembly sorts placements into a spatial grid, so n log n is the honest expectation.
#[test]
fn scene_build_is_loglinear_in_placement_count() {
    let s = Scaling::measure_prepared(
        "scene build",
        &[1_000, 4_000, 16_000],
        3,
        placements,
        |s| {
            s.scene_update(0.05, 0.35).expect("scene update");
        },
    );
    s.report();
    s.assert_growth(Growth::Loglinear, 1.0);
}

// ── known bad, written as the assertion we want ─────────────────────────────────────────

/// Building a document is QUADRATIC in feature count. Measured, release build:
///
/// ```text
/// n=64     9.3 ms
/// n=128   33.1 ms   3.57x per doubling
/// n=256  121.9 ms   3.68x
/// n=512  446.1 ms   3.66x
/// ```
///
/// A steady ~3.6x per doubling is n^1.85 — quadratic, not noise. This is the cost of EDITING, so
/// it is what a user feels while modelling: every added feature re-pays for all the ones before
/// it. At 512 features, adding one costs most of a second.
///
/// Nothing about geometry is involved — `Translate` barely touches OCCT and this row excludes
/// recompute entirely. The suspect is the persistent document: `Document::add` returns a new
/// document, and if that copies the object map rather than sharing it, n adds copy n²/2 entries.
/// Suspect, not diagnosis — the next step is a profile, not another guess.
#[test]
#[ignore = "known quadratic: document build. See the doc comment for the measurement."]
fn building_a_document_is_linear_in_feature_count() {
    let s = Scaling::measure("build only", &[64, 128, 256, 512], 3, |n| {
        let _ = translate_chain(n);
    });
    s.report();
    s.assert_growth(Growth::Linear, 1.0);
}

/// The sketch solver is roughly CUBIC in constraint count. Measured, release build:
///
/// ```text
/// n=16     2.6 ms
/// n=32    15.6 ms   6.00x per doubling
/// n=64   114.4 ms   7.35x
/// n=128  894.7 ms   7.82x
/// ```
///
/// ~7.8x per doubling is n^2.9. A real sketch — an imported DXF profile, a bracket outline — is
/// hundreds of segments, so this is the difference between a solver that feels instant and one
/// that hangs the UI on every drag. It is the single worst scaling number in the application.
///
/// planegcs is FreeCAD's solver and is not cubic by nature, so the likely cause is on our side:
/// re-solving the whole system per call, or rebuilding the constraint graph on each solve rather
/// than keeping it. Again: worth profiling before touching.
#[test]
#[ignore = "known cubic: sketch solve. See the doc comment for the measurement."]
fn sketch_solving_is_loglinear_in_constraint_count() {
    let s = Scaling::measure_prepared(
        "sketch solve",
        &[16, 32, 64, 128],
        3,
        sketch_chain,
        |(s, sk)| {
            s.solve(*sk).expect("solve");
        },
    );
    s.report();
    s.assert_growth(Growth::Loglinear, 1.0);
}
