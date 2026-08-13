//! M5 — the sketcher, through the C ABI.
//!
//! These replace a spike. `spikes/sketch_solve` and `spikes/dxf_import` proved the C++ works; this
//! proves the BOUNDARY works — the same surface a plugin and the iPad shell use. A solver that
//! solves in-process but returns garbage through the ABI is broken for every real consumer.

use cad_tests::*;

/// The tripwire the cad-sys module comment always claimed existed but did not: constants in the
/// binding versus the version the linked library reports. It sat at 4 while the header reached 6.
#[test]
fn abi_version_is_current() {
    if let Err((major, minor)) = cad::sys::abi_version_matches() {
        panic!(
            "cad-sys declares ABI {}.{} but the library reports {major}.{minor}. \
             The C header and cad-sys must change in the same commit.",
            cad::sys::CAD_ABI_VERSION_MAJOR,
            cad::sys::CAD_ABI_VERSION_MINOR
        );
    }
}

/// A rectangle constrained into shape from deliberately wrong coordinates. Nothing tells the solver
/// where the corners belong — it derives that. This is the core behaviour of a sketcher.
#[test]
fn constrained_rectangle_solves_to_exact_dimensions() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();

    let bottom = s.add_line(sk, (0.0, 0.0), (63.0, 7.0)).unwrap();
    let right = s.add_line(sk, (63.0, 7.0), (71.0, 44.0)).unwrap();
    let top = s.add_line(sk, (71.0, 44.0), (-5.0, 39.0)).unwrap();
    let left = s.add_line(sk, (-5.0, 39.0), (0.0, 0.0)).unwrap();

    for (a, b) in [(bottom, right), (right, top), (top, left), (left, bottom)] {
        s.constrain(sk, Con::Coincident(a, Pt::End, b, Pt::Start)).unwrap();
    }
    s.constrain(sk, Con::Horizontal(bottom)).unwrap();
    s.constrain(sk, Con::Horizontal(top)).unwrap();
    s.constrain(sk, Con::Vertical(left)).unwrap();
    s.constrain(sk, Con::Vertical(right)).unwrap();
    // Without these the shape is right but floats: position is still free.
    s.constrain(sk, Con::LockX(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::LockY(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::Distance(bottom, Pt::Start, bottom, Pt::End, 80.0)).unwrap();
    s.constrain(sk, Con::Distance(left, Pt::End, top, Pt::End, 50.0)).unwrap();

    let report = s.solve(sk).unwrap();
    assert_eq!(report.solved, 1, "the sketch did not solve");
    assert_eq!(report.dofs, 0, "expected fully constrained, got {} dofs", report.dofs);
    assert_eq!(report.conflicting, 0);

    let geo = s.sketch_geometry(sk).unwrap();
    assert_eq!(geo.len(), 4);
    // Read the solved coordinates back through the ABI rather than trusting the report.
    let b = geo[0];
    assert_eq!(b.kind, cad::GEO_LINE);
    let width = ((b.p[2] - b.p[0]).powi(2) + (b.p[3] - b.p[1]).powi(2)).sqrt();
    assert!((width - 80.0).abs() < 1e-6, "width {width} should be 80");
    // Bottom edge is horizontal and pinned at the origin.
    assert!(b.p[0].abs() < 1e-9 && b.p[1].abs() < 1e-9);
    assert!((b.p[3] - b.p[1]).abs() < 1e-9);
}

/// An over-constrained sketch must be REPORTED, not silently resolved to one interpretation.
/// planegcs only fills its conflict report from diagnose(), so this is the assertion that catches
/// solve() being called without it.
#[test]
fn contradictory_constraints_are_reported() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let line = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).unwrap();

    s.constrain(sk, Con::LockX(line, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::LockY(line, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::Horizontal(line)).unwrap();
    s.constrain(sk, Con::Distance(line, Pt::Start, line, Pt::End, 100.0)).unwrap();
    // Cannot also be 80 long.
    s.constrain(sk, Con::Distance(line, Pt::Start, line, Pt::End, 80.0)).unwrap();

    let report = s.solve(sk).unwrap();
    assert!(
        report.conflicting > 0 || report.solved == 0,
        "a sketch that cannot be satisfied reported success with no conflicts"
    );
}

/// An under-constrained sketch must report the degrees of freedom left, which is the number a user
/// needs to know whether the sketch is finished.
#[test]
fn degrees_of_freedom_are_reported() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let _ = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).unwrap();
    let report = s.solve(sk).unwrap();
    // A free line is four unknowns.
    assert_eq!(report.dofs, 4, "a single unconstrained line should have 4 dofs");
}

#[test]
fn arcs_and_circles_survive_the_boundary() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let c = s.add_sketch_circle(sk, (5.0, 6.0), 7.0).unwrap();
    let _ = s.add_sketch_arc(sk, (0.0, 0.0), 3.0, 0.0, 1.5).unwrap();
    s.constrain(sk, Con::Radius(c, 12.0)).unwrap();
    s.solve(sk).unwrap();

    let geo = s.sketch_geometry(sk).unwrap();
    assert_eq!(geo.len(), 2);
    assert_eq!(geo[0].kind, cad::GEO_CIRCLE);
    assert!((geo[0].p[2] - 12.0).abs() < 1e-9, "the radius constraint was not applied");
    assert_eq!(geo[1].kind, cad::GEO_ARC);
    // Arc angles are radians on both sides of the ABI.
    assert!((geo[1].p[4] - 1.5).abs() < 1e-9);
}

/// The text form a document stores must round-trip through the ABI byte-identically, and the result
/// must still solve — ids and constraint targets have to survive.
#[test]
fn serialized_sketch_round_trips() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xz).unwrap();
    let a = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).unwrap();
    let b = s.add_line(sk, (10.0, 0.0), (10.0, 5.0)).unwrap();
    s.constrain(sk, Con::Coincident(a, Pt::End, b, Pt::Start)).unwrap();
    s.constrain(sk, Con::Horizontal(a)).unwrap();

    let text = s.sketch_text(sk);
    assert!(text.starts_with("sketch 1"), "unexpected format header: {text:?}");
    assert!(text.contains("plane XZ"), "the plane was lost");

    let back = s.sketch_from_text(&text).unwrap();
    assert_eq!(s.sketch_text(back), text, "round-trip changed the sketch");
    assert_eq!(s.sketch_constraint_count(back).unwrap(), 2);
    assert_eq!(s.solve(back).unwrap().solved, 1);
}

/// Import a DXF, then infer constraints. The DOF collapse is the assertion that matters: it is the
/// difference between a pile of lines and an editable shape.
#[test]
fn dxf_import_then_inference_reduces_freedom() {
    // Relative to the crate; tests run from the workspace root.
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/data/sketch_profile.dxf");

    let mut s = session();
    let sk = s.import_dxf(path, Plane::Xy, 1.0).unwrap();

    let geo = s.sketch_geometry(sk).unwrap();
    // 4 square edges + construction centreline + circle + arc. The degenerate line is dropped and
    // the SPLINE is not imported.
    assert_eq!(geo.len(), 7, "unexpected import: {geo:?}");
    assert_eq!(geo.iter().filter(|g| g.kind == cad::GEO_CIRCLE).count(), 1);
    assert_eq!(geo.iter().filter(|g| g.kind == cad::GEO_ARC).count(), 1);
    assert_eq!(geo.iter().filter(|g| g.construction == 1).count(), 1);

    let before = s.solve(sk).unwrap().dofs;
    let inferred = s.infer(sk, 0.0, 0.0, false).unwrap();

    assert_eq!(inferred.coincident, 4, "the square's four corners should fuse");
    assert_eq!(inferred.horizontal, 3);
    assert_eq!(inferred.vertical, 2);
    assert!(
        inferred.dofs_after < inferred.dofs_before,
        "inference did not reduce freedom: {} -> {}",
        inferred.dofs_before,
        inferred.dofs_after
    );
    assert_eq!(inferred.dofs_before, before);
    assert_eq!(
        inferred.conflicting, 0,
        "inference contradicted itself — the rules or the tolerance are wrong"
    );
}

/// Inference must not invent parallel/perpendicular unless asked: two arbitrary lines are
/// coincidentally perpendicular often enough that guessing does real damage.
#[test]
fn parallel_inference_is_opt_in() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    // Two lines at 30 and 120 degrees — perpendicular to each other, neither axis-aligned.
    let _ = s.add_line(sk, (0.0, 0.0), (10.0, 5.7735)).unwrap();
    let _ = s.add_line(sk, (20.0, 0.0), (14.2265, 10.0)).unwrap();

    let off = s.infer(sk, 0.0, 0.0, false).unwrap();
    assert_eq!(off.perpendicular, 0, "perpendicular was inferred without being asked");
    assert_eq!(off.parallel, 0);
}

/// A stale sketch handle must be an error the caller sees, never a crash.
#[test]
fn released_sketch_handle_is_rejected() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let _ = s.add_line(sk, (0.0, 0.0), (1.0, 0.0)).unwrap();
    s.release_sketch(sk);

    let err = s.solve(sk).expect_err("a released sketch handle was accepted");
    assert_eq!(err.code, cad::ErrorCode::BadHandle);
}

#[test]
fn importing_a_non_dxf_fails_cleanly() {
    let mut s = session();
    let err = s
        .import_dxf(concat!(env!("CARGO_MANIFEST_DIR"), "/Cargo.toml"), Plane::Xy, 1.0)
        .expect_err("a non-DXF file was imported");
    assert!(!err.message.is_empty());
}

/// Export then re-import, and compare. The strongest test available for either direction: the
/// writer is ours and hand-rolled, the reader is dime, so a round trip is checked by an INDEPENDENT
/// implementation rather than by the same code twice.
#[test]
fn dxf_export_round_trips_through_the_importer() {
    let dir = std::env::temp_dir().join(format!("cad-dxf-out-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("profile.dxf");
    let out = path.to_str().unwrap();

    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    // A solved shape, so the exported coordinates are the solver's own output.
    let bottom = s.add_line(sk, (0.0, 0.0), (40.0, 0.0)).unwrap();
    let _ = s.add_line(sk, (40.0, 0.0), (40.0, 25.0)).unwrap();
    let circle = s.add_sketch_circle(sk, (20.0, 12.5), 5.0).unwrap();
    let arc = s.add_sketch_arc(sk, (60.0, 0.0), 8.0, 0.0, std::f64::consts::FRAC_PI_2).unwrap();
    s.constrain(sk, Con::Horizontal(bottom)).unwrap();
    s.constrain(sk, Con::Radius(circle, 5.0)).unwrap();
    s.solve(sk).unwrap();

    s.export_dxf(sk, out, 1.0).unwrap();
    assert!(std::fs::metadata(&path).unwrap().len() > 0);

    let mut t = session();
    let back = t.import_dxf(out, Plane::Xy, 1.0).unwrap();
    let before = s.sketch_geometry(sk).unwrap();
    let after = t.sketch_geometry(back).unwrap();

    assert_eq!(after.len(), before.len(), "geometry count changed: {after:?}");
    for (a, b) in before.iter().zip(after.iter()) {
        assert_eq!(a.kind, b.kind, "an entity changed kind through the round trip");
        // RELATIVE 1e-6, because dime declares `typedef float dxfdouble` and reads coordinates in
        // single precision. Our writer emits 17 digits, so the floor is the reader's, not the
        // file's. A tighter assertion would pass here anyway -- every coordinate in this sketch is
        // exactly representable in float32 -- and that is the trap: it would look like a guarantee
        // of exactness that does not exist, then fail the moment a solved value is not dyadic.
        for i in 0..5 {
            let tol = 1e-6 * a.p[i].abs().max(1.0);
            assert!(
                (a.p[i] - b.p[i]).abs() < tol,
                "parameter {i} drifted: {} -> {} (kind {})",
                a.p[i], b.p[i], a.kind
            );
        }
    }
    // Arc angles specifically: radians out, degrees in the file, radians back.
    let arcs: Vec<_> = after.iter().filter(|g| g.kind == cad::GEO_ARC).collect();
    assert_eq!(arcs.len(), 1);
    assert!((arcs[0].p[4] - std::f64::consts::FRAC_PI_2).abs() < 1e-6);
    let _ = arc;

    // Scale must invert: export at 25.4 writes inches, re-importing at 25.4 restores millimetres.
    let inch_path = dir.join("inches.dxf");
    s.export_dxf(sk, inch_path.to_str().unwrap(), 25.4).unwrap();
    let restored = t.import_dxf(inch_path.to_str().unwrap(), Plane::Xy, 25.4).unwrap();
    let r = t.sketch_geometry(restored).unwrap();
    // 40/25.4 is not representable in float32, so this is where dime's precision floor shows.
    assert!((r[0].p[2] - 40.0).abs() < 1e-4, "scale did not invert: {}", r[0].p[2]);
}

/// Construction geometry must survive a round trip on its own layer, not be silently promoted to
/// profile geometry — which would turn a centreline into an edge of the solid.
#[test]
fn construction_geometry_survives_a_dxf_round_trip() {
    let dir = std::env::temp_dir().join(format!("cad-dxf-con-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("c.dxf");

    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let _ = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).unwrap();
    s.add_construction_line(sk, (0.0, 5.0), (10.0, 5.0)).unwrap();

    s.export_dxf(sk, path.to_str().unwrap(), 1.0).unwrap();

    let mut t = session();
    let back = t.import_dxf(path.to_str().unwrap(), Plane::Xy, 1.0).unwrap();
    let geo = t.sketch_geometry(back).unwrap();
    assert_eq!(geo.len(), 2);
    assert_eq!(
        geo.iter().filter(|g| g.construction == 1).count(),
        1,
        "construction geometry was not preserved through the layer"
    );
}

#[test]
fn exporting_an_empty_sketch_is_refused() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let path = std::env::temp_dir().join(format!("cad-empty-{}.dxf", std::process::id()));
    let err = s
        .export_dxf(sk, path.to_str().unwrap(), 1.0)
        .expect_err("an empty sketch was exported as a valid file");
    assert!(!err.message.is_empty());
    assert!(!path.exists(), "a file was left behind by a refused export");
}


/// The whole point of a sketcher: a constrained profile becomes a solid, and editing the
/// CONSTRAINT changes the solid. This is the first test in the suite where a dimension drives
/// geometry through the full stack -- solver, document, kernel.
#[test]
fn sketch_feature_extrudes_to_a_solid() {
    let mut s = session();

    // A 40 x 25 rectangle, closed and fully constrained.
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let bottom = s.add_line(sk, (0.0, 0.0), (40.0, 0.0)).unwrap();
    let right = s.add_line(sk, (40.0, 0.0), (40.0, 25.0)).unwrap();
    let top = s.add_line(sk, (40.0, 25.0), (0.0, 25.0)).unwrap();
    let left = s.add_line(sk, (0.0, 25.0), (0.0, 0.0)).unwrap();
    for (a, b) in [(bottom, right), (right, top), (top, left), (left, bottom)] {
        s.constrain(sk, Con::Coincident(a, Pt::End, b, Pt::Start)).unwrap();
    }
    s.constrain(sk, Con::Horizontal(bottom)).unwrap();
    s.constrain(sk, Con::Horizontal(top)).unwrap();
    s.constrain(sk, Con::Vertical(right)).unwrap();
    s.constrain(sk, Con::Vertical(left)).unwrap();
    s.constrain(sk, Con::LockX(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::LockY(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::Distance(bottom, Pt::Start, bottom, Pt::End, 40.0)).unwrap();
    s.constrain(sk, Con::Distance(right, Pt::Start, right, Pt::End, 25.0)).unwrap();
    assert_eq!(s.solve(sk).unwrap().dofs, 0);

    let text = s.sketch_text(sk);
    let profile = s.add_sketch_feature(&text).unwrap();
    let solid = s.add_extrude(profile, 10.0, Plane::Xy).unwrap();
    recompute_ok(&mut s);

    assert_eq!(s.state(solid).unwrap(), State::Clean, "{}", s.object_error(solid));
    assert!(s.is_valid_shape(solid).unwrap());
    // 40 x 25 x 10.
    let volume = s.volume(solid).unwrap();
    assert!((volume - 10_000.0).abs() < 1e-6, "volume {volume} should be 10000");
    assert_eq!(s.face_count(solid).unwrap(), 6, "a rectangular prism has six faces");
}

/// Editing a DIMENSION in the sketch must change the solid. Without this, the pipeline is a
/// one-way import rather than a parametric model.
#[test]
fn editing_a_sketch_dimension_changes_the_solid() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let bottom = s.add_line(sk, (0.0, 0.0), (40.0, 0.0)).unwrap();
    let right = s.add_line(sk, (40.0, 0.0), (40.0, 25.0)).unwrap();
    let top = s.add_line(sk, (40.0, 25.0), (0.0, 25.0)).unwrap();
    let left = s.add_line(sk, (0.0, 25.0), (0.0, 0.0)).unwrap();
    for (a, b) in [(bottom, right), (right, top), (top, left), (left, bottom)] {
        s.constrain(sk, Con::Coincident(a, Pt::End, b, Pt::Start)).unwrap();
    }
    s.constrain(sk, Con::Horizontal(bottom)).unwrap();
    s.constrain(sk, Con::Horizontal(top)).unwrap();
    s.constrain(sk, Con::Vertical(right)).unwrap();
    s.constrain(sk, Con::Vertical(left)).unwrap();
    s.constrain(sk, Con::LockX(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::LockY(bottom, Pt::Start, 0.0)).unwrap();
    s.constrain(sk, Con::Distance(bottom, Pt::Start, bottom, Pt::End, 40.0)).unwrap();
    s.constrain(sk, Con::Distance(right, Pt::Start, right, Pt::End, 25.0)).unwrap();
    s.solve(sk).unwrap();

    let profile = s.add_sketch_feature(&s.sketch_text(sk)).unwrap();
    let solid = s.add_extrude(profile, 10.0, Plane::Xy).unwrap();
    recompute_ok(&mut s);
    let before = s.volume(solid).unwrap();

    // Widen the rectangle to 80 by editing the sketch's own constraint, then re-store the text.
    // Deliberately NOT by moving coordinates: the constraint is the definition, and the solver has
    // to produce the new geometry from it.
    let wider = s.new_sketch(Plane::Xy).unwrap();
    let _ = wider; // the edit path below goes through the stored text, as the UI will
    let text = s.sketch_text(sk).replace("distance 0 0 0 1 40", "distance 0 0 0 1 80");
    assert!(text.contains("80"), "the dimension edit did not apply to the text");
    s.set_text(profile, "sketch", &text).unwrap();
    recompute_ok(&mut s);

    let after = s.volume(solid).unwrap();
    assert!(
        (after - before * 2.0).abs() < 1e-6,
        "doubling the width should double the volume: {before} -> {after}"
    );
}

/// An unclosed profile must fail at the SKETCH, naming the profile — not deep inside the extrude.
#[test]
fn an_open_profile_fails_at_the_sketch_feature() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();
    let _ = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).unwrap();
    let _ = s.add_line(sk, (10.0, 0.0), (10.0, 10.0)).unwrap();
    s.solve(sk).unwrap();

    let profile = s.add_sketch_feature(&s.sketch_text(sk)).unwrap();
    let solid = s.add_extrude(profile, 5.0, Plane::Xy).unwrap();
    // recompute reports failure rather than erroring: partial failure is the engine's contract.
    let _ = s.recompute();

    assert_eq!(s.state(profile).unwrap(), State::Failed, "an open profile was accepted");
    let message = s.object_error(profile);
    assert!(message.contains("profile") || message.contains("open"), "unhelpful: {message}");
    // The extrude is blocked by its failed input rather than failing on its own terms.
    assert_eq!(s.state(solid).unwrap(), State::Blocked);
}

/// An arc's angles must agree with its endpoints after solving.
///
/// Written as a regression test for a reported stale-angle bug, and it is NOT one: it passes
/// against both implementations. That is worth recording rather than quietly deleting.
///
/// The report assumed startAngle/endAngle are not solver variables. In our setup they are —
/// Sketch::solve declares both with addParam, so they are unknowns, and addConstraintArcRules ties
/// them to the endpoints. planegcs therefore updates them itself, and recomputing them with atan2
/// afterwards lands on the same numbers. The atan2 is kept as a cheap invariant, not a fix.
///
/// What this test does guard is the invariant everything downstream depends on: toWire() derives an
/// arc's midpoint from these angles, so if they ever stop agreeing with the endpoints — a different
/// solver, a dropped ArcRules, a loose convergence tolerance — the wire build silently produces an
/// inverted arc. Centre and radius stay correct in that failure, so only an angle check catches it.
#[test]
fn arc_angles_follow_the_solved_endpoints() {
    let mut s = session();
    let sk = s.new_sketch(Plane::Xy).unwrap();

    // A quarter arc of radius 10 about the origin: starts at (10,0), ends at (0,10).
    let arc = s
        .add_sketch_arc(sk, (0.0, 0.0), 10.0, 0.0, std::f64::consts::FRAC_PI_2)
        .unwrap();
    s.constrain(sk, Con::LockX(arc, Pt::Center, 0.0)).unwrap();
    s.constrain(sk, Con::LockY(arc, Pt::Center, 0.0)).unwrap();

    // Drag the END round to (-10, 0) — half a turn instead of a quarter. Nothing here mentions an
    // angle; the solver has to derive it from the endpoint position.
    //
    // Note there is deliberately NO radius constraint. Pinning centre, radius AND both end
    // coordinates is one equation more than the arc has freedom — the end lies on the circle, so
    // its two coordinates carry only one independent unknown — and planegcs correctly reports that
    // as a conflict. Letting the radius follow from the endpoint is both consistent and closer to
    // how the constraint would arrive from a real sketch.
    s.constrain(sk, Con::LockX(arc, Pt::End, -10.0)).unwrap();
    s.constrain(sk, Con::LockY(arc, Pt::End, 0.0)).unwrap();

    let report = s.solve(sk).unwrap();
    assert_eq!(report.solved, 1, "the arc sketch did not solve");
    assert_eq!(report.conflicting, 0, "over-constrained: {report:?}");

    let geo = s.sketch_geometry(sk).unwrap();
    let a = geo.iter().find(|g| g.kind == cad::GEO_ARC).expect("no arc");

    // Centre and radius are unchanged, which is exactly why the stale-angle bug hid here.
    assert!(a.p[0].abs() < 1e-6 && a.p[1].abs() < 1e-6, "centre moved");
    // Radius follows from the constrained endpoint rather than being asserted directly.
    assert!((a.p[2] - 10.0).abs() < 1e-6, "radius should have followed to 10, got {}", a.p[2]);

    // THE assertion: the end angle must now be pi, not the pi/2 it was placed with.
    let end = a.p[4];
    assert!(
        (end - std::f64::consts::PI).abs() < 1e-6,
        "end angle is {end}, expected pi — it was left at its placement value of pi/2, so the \
         arc's sweep no longer matches its endpoints"
    );
    // The start angle is still free (nothing constrains it), so it is not asserted — only that the
    // END angle tracked its endpoint.

    // And the angles must agree with the endpoints they describe, which is the invariant the whole
    // downstream wire build depends on.
    let ex = a.p[0] + a.p[2] * end.cos();
    let ey = a.p[1] + a.p[2] * end.sin();
    assert!(
        (ex + 10.0).abs() < 1e-6 && ey.abs() < 1e-6,
        "the end angle points at ({ex}, {ey}), not the constrained (-10, 0)"
    );
}
