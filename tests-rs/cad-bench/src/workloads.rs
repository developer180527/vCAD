//! The workloads themselves, kept separate from the harness so a measurement and its assertion
//! read the same construction.
//!
//! Each builder returns a session in a known state. They are deliberately dull: a workload that
//! is clever about what it builds measures the cleverness.

use cad::*;

/// A box with `n` translates chained off it.
///
/// `Translate` because its geometry cost is near zero — it moves a shape, it does not rebuild
/// one. That is the point: this workload measures the RECOMPUTE ENGINE and the DAG walk, and a
/// chain of booleans would bury both under OCCT. Use [`cut_chain`] when the kernel is what is
/// being measured.
///
/// Returns the session and the last object in the chain.
pub fn translate_chain(n: usize) -> (Session, Object) {
    let mut s = Session::new().expect("session");
    let mut tip = s.add_box(20.0, 20.0, 20.0).expect("box");
    for i in 0..n {
        tip = s
            .add_translate(tip, 1.0 + i as f64 * 0.01, 0.0, 0.0)
            .expect("translate");
    }
    (s, tip)
}

/// A box with `n` boolean cuts taken out of it. Every node is real kernel work.
///
/// The tools are offset so each cut actually removes material: a cut that misses is a fast path
/// through OCCT and would make this measure nothing.
pub fn cut_chain(n: usize) -> (Session, Object) {
    let mut s = Session::new().expect("session");
    let mut tip = s.add_box(200.0, 200.0, 40.0).expect("box");
    for i in 0..n {
        let tool = s.add_box(10.0, 10.0, 100.0).expect("tool");
        let placed = s
            .add_translate(tool, 12.0 * (i % 15) as f64, 12.0 * (i / 15) as f64, -30.0)
            .expect("translate");
        tip = s.add_cut(tip, placed).expect("cut");
    }
    (s, tip)
}

/// `n` boxes with distinct dimensions, so no two share a content hash and nothing dedupes.
///
/// Distinct on purpose. Identical shapes collapse in the content-addressed cache, which is the
/// behaviour worth having and exactly the wrong thing to measure tessellation with — it would
/// report the cost of one box no matter how many were asked for. That is the same mistake the
/// scale spike made with 20 unique meshes standing in for 100k parts.
pub fn distinct_boxes(n: usize) -> (Session, Vec<Object>) {
    let mut s = Session::new().expect("session");
    let objects = (0..n)
        .map(|i| {
            let d = 8.0 + i as f64 * 0.5;
            s.add_box(d, d * 1.5, d * 0.75).expect("box")
        })
        .collect();
    s.recompute().expect("recompute");
    (s, objects)
}

/// One box placed `n` times. The assembly-scale path: many placements, one mesh.
pub fn placements(n: usize) -> Session {
    let mut s = Session::new().expect("session");
    let b = s.add_box(20.0, 20.0, 20.0).expect("box");
    s.recompute().expect("recompute");
    let side = (n as f64).cbrt().ceil().max(1.0) as usize;
    for i in 0..n {
        let (x, y, z) = (i % side, (i / side) % side, i / (side * side));
        let t = [
            1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
            x as f32 * 40.0, y as f32 * 40.0, z as f32 * 40.0,
        ];
        s.add_placement(b, Some(&t)).expect("placement");
    }
    s
}

/// A chain of `n` end-to-end lines, coincident and alternately horizontal and vertical.
///
/// A staircase rather than a closed profile: it keeps the constraint graph connected — which is
/// what makes the solver do real work — without the degeneracy a self-touching loop introduces.
pub fn sketch_chain(n: usize) -> (Session, Sketch) {
    let mut s = Session::new().expect("session");
    let sk = s.new_sketch(Plane::Xy).expect("sketch");
    let mut previous: Option<Geo> = None;
    for i in 0..n {
        let (x, y) = (i as f64 * 10.0, (i % 2) as f64 * 10.0);
        let (nx, ny) = ((i + 1) as f64 * 10.0, ((i + 1) % 2) as f64 * 10.0);
        let line = s.add_line(sk, (x, y), (nx, ny)).expect("line");
        if let Some(prev) = previous {
            s.constrain(sk, Con::Coincident(prev, Pt::End, line, Pt::Start))
                .expect("coincident");
        }
        previous = Some(line);
    }
    (s, sk)
}
