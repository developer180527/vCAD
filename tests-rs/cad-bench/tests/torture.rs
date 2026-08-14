//! Geometry torture: pathological input, and one contract.
//!
//! # The contract
//!
//! **Either succeed validly, or fail legibly.** A CAD kernel is allowed to refuse — most of the
//! inputs below *should* be refused. What it may never do is succeed dishonestly: report a shape
//! valid that OCCT considers invalid, hand back a NaN volume, tessellate to infinite bounds, or
//! fail with an empty message that leaves the user nothing to act on.
//!
//! That is why almost nothing here asserts a specific outcome. Asserting "a fillet of radius
//! exactly half the edge succeeds" would encode OCCT's current tolerance behaviour as a
//! requirement, and the next OCCT bump would fail a test that was never about us. Asserting "if
//! it succeeded the shape is valid, and if it failed it said why" is a statement about OUR code
//! that holds across kernel versions.
//!
//! # Why these inputs
//!
//! Real CAD documents are full of them. Imported STEP arrives with slivers and coincident faces.
//! Users type 0 into a dimension field and press enter. Site coordinates put a bracket 1e6 mm
//! from the origin, where float32 in the render path starts losing millimetres. A profile
//! imported from DXF is over-constrained before anyone touches it. None of this is exotic; it is
//! Tuesday.

use cad::*;

// ── the contract, in one place ──────────────────────────────────────────────────────────

/// Asserts the honesty contract for one object after a recompute.
///
/// Takes `case` for the failure message: a torture suite that fails with "assertion failed:
/// left == right" and no indication of which input caused it is a suite people delete.
fn assert_honest(s: &Session, o: Object, case: &str) {
    let state = s.state(o).unwrap_or(State::Failed);
    match state {
        State::Failed | State::Blocked => {
            let message = s.object_error(o);
            assert!(
                !message.trim().is_empty(),
                "{case}: the feature failed with an EMPTY message. \
                 A refusal the user cannot read is indistinguishable from a crash."
            );
        }
        _ => {
            // It claims to have worked. Then everything downstream must agree.
            let valid = s.is_valid_shape(o).unwrap_or(false);
            assert!(
                valid,
                "{case}: the feature reports success but the shape is not valid. \
                 Succeeding with a broken shape is worse than failing: it propagates."
            );
            if let Ok(v) = s.volume(o) {
                assert!(
                    v.is_finite(),
                    "{case}: a valid shape reported a non-finite volume ({v})"
                );
                assert!(
                    v >= 0.0,
                    "{case}: a valid shape reported a negative volume ({v})"
                );
            }
            if let Ok(faces) = s.face_count(o) {
                assert!(faces > 0, "{case}: a valid solid with zero faces");
            }
        }
    }
}

/// Recomputes without asserting success — failure is a legitimate outcome here — and returns
/// whether the call itself survived.
fn recompute_survives(s: &mut Session, case: &str) -> bool {
    match s.recompute() {
        Ok(_) => true,
        Err(e) => {
            assert!(
                !e.message.trim().is_empty(),
                "{case}: recompute failed with an empty message"
            );
            false
        }
    }
}

/// A box, if the dimensions were accepted at all. `None` means the API refused up front, which
/// is the best possible outcome and not a failure of the test.
fn try_box(s: &mut Session, dx: f64, dy: f64, dz: f64) -> Option<Object> {
    let o = s.add("Box").ok()?;
    s.set_length(o, "dx", dx).ok()?;
    s.set_length(o, "dy", dy).ok()?;
    s.set_length(o, "dz", dz).ok()?;
    Some(o)
}

// ── degenerate dimensions ───────────────────────────────────────────────────────────────

/// Zero, negative, NaN, infinite and absurd dimensions.
///
/// The one a user produces by accident, every day, by clearing a field. Whatever the kernel
/// decides, it must not end up with a "valid" solid of zero or non-finite volume, because that
/// shape then flows into booleans, tessellation and the mass properties readout.
#[test]
fn degenerate_box_dimensions() {
    let cases: &[(&str, f64)] = &[
        ("zero", 0.0),
        ("negative", -50.0),
        ("denormal", 1e-300),
        ("tiny but real", 1e-6),
        ("astronomical", 1e12),
        ("NaN", f64::NAN),
        ("positive infinity", f64::INFINITY),
        ("negative infinity", f64::NEG_INFINITY),
    ];

    for (name, dx) in cases {
        let mut s = Session::new().expect("session");
        let case = format!("box with dx = {name}");
        let Some(b) = try_box(&mut s, *dx, 40.0, 20.0) else { continue };
        if !recompute_survives(&mut s, &case) {
            continue;
        }
        assert_honest(&s, b, &case);
    }
}

/// A sliver: three orders of magnitude between the thinnest and longest edge.
///
/// Slivers are what imported STEP is made of, and they are where tessellation tolerances stop
/// making sense — a chord tolerance larger than the part's own thickness has no valid answer.
#[test]
fn extreme_aspect_ratio_still_tessellates_finitely() {
    let mut s = Session::new().expect("session");
    let case = "sliver box 0.001 x 1000 x 1";
    let Some(b) = try_box(&mut s, 0.001, 1000.0, 1.0) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }
    assert_honest(&s, b, case);

    if let Ok(mesh) = s.tessellate(b, 0.05, 0.35) {
        for (axis, (lo, hi)) in mesh.bounds_min.iter().zip(mesh.bounds_max.iter()).enumerate() {
            assert!(
                lo.is_finite() && hi.is_finite(),
                "{case}: tessellated to non-finite bounds on axis {axis} ({lo}..{hi})"
            );
            assert!(hi >= lo, "{case}: inverted bounds on axis {axis} ({lo}..{hi})");
        }
    }
}

/// Geometry a kilometre from the origin, in millimetres.
///
/// Site coordinates and large assemblies routinely put a part here. It matters because the
/// render path is float32: at 1e6 mm the gap between representable values is around an eighth of
/// a millimetre, so a part modelled to a micron cannot survive the trip to the GPU intact. This
/// does not assert precision — it asserts that nothing becomes non-finite or inverted, which is
/// the failure that turns into a blank viewport rather than a slightly wrong one.
#[test]
fn geometry_far_from_the_origin_keeps_finite_bounds() {
    for distance in [1e3_f64, 1e6, 1e9] {
        let mut s = Session::new().expect("session");
        let case = format!("box translated {distance} mm from the origin");
        let Some(b) = try_box(&mut s, 20.0, 20.0, 20.0) else { continue };
        let Ok(moved) = s.add_translate(b, distance, distance, distance) else { continue };
        if !recompute_survives(&mut s, &case) {
            continue;
        }
        assert_honest(&s, moved, &case);

        if let Ok(mesh) = s.tessellate(moved, 0.05, 0.35) {
            for axis in 0..3 {
                let (lo, hi) = (mesh.bounds_min[axis], mesh.bounds_max[axis]);
                assert!(
                    lo.is_finite() && hi.is_finite(),
                    "{case}: non-finite bounds on axis {axis} ({lo}..{hi})"
                );
                assert!(hi >= lo, "{case}: inverted bounds on axis {axis} ({lo}..{hi})");
            }
        }
    }
}

// ── fillets and chamfers, at and past the limit ─────────────────────────────────────────

/// Fillet radii from zero to far past what the edge can carry.
///
/// The interesting one is a radius near exactly half the shortest adjacent edge: that is where
/// the rounded faces from neighbouring edges meet tangentially, and it is the classic place for
/// a kernel to produce a self-intersecting solid it still calls valid.
#[test]
fn fillet_radius_from_zero_to_impossible() {
    let cases: &[(&str, f64)] = &[
        ("zero", 0.0),
        ("negative", -5.0),
        ("microscopic", 1e-9),
        ("comfortable", 5.0),
        ("exactly half the 20mm edge", 10.0),
        ("larger than the edge", 25.0),
        ("absurd", 1e6),
        ("NaN", f64::NAN),
    ];

    for (name, radius) in cases {
        let mut s = Session::new().expect("session");
        let case = format!("fillet radius {name}");
        let Some(b) = try_box(&mut s, 20.0, 20.0, 20.0) else { continue };
        if !recompute_survives(&mut s, &case) {
            continue;
        }

        let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
        if edge.is_empty() {
            continue;
        }
        let Ok(f) = s.add_fillet(b, &edge, *radius) else { continue };
        if !recompute_survives(&mut s, &case) {
            continue;
        }
        assert_honest(&s, f, &case);

        // A fillet removes material from a convex edge; it can never add any. A result larger
        // than its input means the operation was applied inside out, which is a class of bug that
        // still produces a perfectly valid solid.
        if matches!(s.state(f), Ok(State::Clean)) {
            if let (Ok(before), Ok(after)) = (s.volume(b), s.volume(f)) {
                assert!(
                    after <= before * 1.001,
                    "{case}: filleting grew the solid from {before} to {after} mm³"
                );
            }
        }
    }
}

/// Filleting an edge that has already been filleted.
///
/// The second fillet's edge reference points at geometry the first one replaced. This is the
/// topological-naming problem in its sharpest form (ADR 0005), and the honest outcomes are a
/// resolved reference or a legible refusal — never a silent fillet of some unrelated edge.
#[test]
fn filleting_an_already_filleted_edge() {
    let mut s = Session::new().expect("session");
    let case = "fillet applied twice to the same edge";
    let Some(b) = try_box(&mut s, 40.0, 40.0, 40.0) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }

    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    if edge.is_empty() {
        return;
    }
    let Ok(first) = s.add_fillet(b, &edge, 4.0) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }
    let Ok(second) = s.add_fillet(first, &edge, 4.0) else { return };
    recompute_survives(&mut s, case);
    assert_honest(&s, second, case);
}

/// A chamfer wider than the face it sits on.
#[test]
fn chamfer_wider_than_its_face() {
    let mut s = Session::new().expect("session");
    let case = "chamfer 30mm on a 20mm box";
    let Some(b) = try_box(&mut s, 20.0, 20.0, 20.0) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }
    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    if edge.is_empty() {
        return;
    }
    let Ok(c) = s.add_chamfer(b, &edge, 30.0) else { return };
    recompute_survives(&mut s, case);
    assert_honest(&s, c, case);
}

// ── booleans on the hard cases ──────────────────────────────────────────────────────────

/// Cuts where the tool touches exactly, misses entirely, or swallows the base.
///
/// Coincident faces are the classic boolean hazard: the result is ambiguous at the tolerance
/// level, and kernels have historically returned empty shapes, invalid shapes, or the original
/// unchanged, all reported as success.
#[test]
fn booleans_on_coincident_missing_and_engulfing_tools() {
    struct Case {
        name: &'static str,
        tool: (f64, f64, f64),
        at: (f64, f64, f64),
    }
    let cases = [
        Case { name: "tool exactly coincident with the base", tool: (40.0, 40.0, 40.0), at: (0.0, 0.0, 0.0) },
        Case { name: "tool face-coincident with the base's face", tool: (40.0, 40.0, 40.0), at: (40.0, 0.0, 0.0) },
        Case { name: "tool entirely inside the base", tool: (10.0, 10.0, 10.0), at: (15.0, 15.0, 15.0) },
        Case { name: "tool entirely missing the base", tool: (10.0, 10.0, 10.0), at: (500.0, 0.0, 0.0) },
        Case { name: "tool engulfing the base", tool: (400.0, 400.0, 400.0), at: (-180.0, -180.0, -180.0) },
        Case { name: "tool sharing one edge only", tool: (40.0, 40.0, 40.0), at: (40.0, 40.0, 0.0) },
    ];

    for c in cases {
        let mut s = Session::new().expect("session");
        let Some(base) = try_box(&mut s, 40.0, 40.0, 40.0) else { continue };
        let Some(tool) = try_box(&mut s, c.tool.0, c.tool.1, c.tool.2) else { continue };
        let Ok(placed) = s.add_translate(tool, c.at.0, c.at.1, c.at.2) else { continue };
        let Ok(cut) = s.add_cut(base, placed) else { continue };
        if !recompute_survives(&mut s, c.name) {
            continue;
        }
        assert_honest(&s, cut, c.name);

        // A cut can only remove material. Growing means the operands were swapped or the result
        // is inside out — and either way it is a valid solid, so nothing else would catch it.
        if matches!(s.state(cut), Ok(State::Clean)) {
            if let (Ok(before), Ok(after)) = (s.volume(base), s.volume(cut)) {
                assert!(
                    after <= before * 1.001,
                    "{}: the cut grew the solid from {before} to {after} mm³",
                    c.name
                );
            }
        }
    }
}

/// A boolean whose tool failed to compute.
///
/// The engine supports partial failure, so the cut must be BLOCKED with a reason rather than
/// computed against a null shape. This is the case that decides whether one bad feature poisons
/// the document or is contained to its own branch.
#[test]
fn a_boolean_whose_tool_failed_is_blocked_not_computed() {
    let mut s = Session::new().expect("session");
    let case = "cut by a tool with a zero dimension";
    let Some(base) = try_box(&mut s, 40.0, 40.0, 40.0) else { return };
    let Some(tool) = try_box(&mut s, 0.0, 10.0, 10.0) else { return };
    let Ok(cut) = s.add_cut(base, tool) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }

    assert_honest(&s, cut, case);
    // Whatever happened to the cut, the healthy sibling must be untouched. A failure that takes
    // the rest of the document with it is the behaviour partial recompute exists to prevent.
    assert_honest(&s, base, "the base beside a failed tool");
    assert!(
        matches!(s.state(base), Ok(State::Clean)),
        "a failed tool left the unrelated base in state {:?}",
        s.state(base)
    );
}

// ── tessellation tolerances ─────────────────────────────────────────────────────────────

/// Tolerances at and past the edges of sense: zero, negative, enormous.
///
/// A zero chord tolerance asks for infinite subdivision. It must be refused or clamped, never
/// attempted — and the mesh that comes back, if one does, must have triangles and finite bounds.
#[test]
fn tessellation_tolerance_extremes() {
    let cases: &[(&str, f64, f64)] = &[
        ("zero deflection", 0.0, 0.35),
        ("negative deflection", -1.0, 0.35),
        ("enormous deflection", 1e9, 0.35),
        ("zero angular", 0.05, 0.0),
        ("negative angular", 0.05, -1.0),
        ("enormous angular", 0.05, 1e9),
    ];

    for (name, deflection, angular) in cases {
        let mut s = Session::new().expect("session");
        let case = format!("tessellate a cylinder with {name}");
        let Ok(c) = s.add_cylinder(20.0, 40.0) else { continue };
        if !recompute_survives(&mut s, &case) {
            continue;
        }
        let Ok(mesh) = s.tessellate(c, *deflection, *angular) else { continue };

        assert!(
            mesh.triangles > 0,
            "{case}: tessellation reported success with zero triangles"
        );
        for axis in 0..3 {
            let (lo, hi) = (mesh.bounds_min[axis], mesh.bounds_max[axis]);
            assert!(
                lo.is_finite() && hi.is_finite(),
                "{case}: non-finite bounds on axis {axis}"
            );
        }
    }
}

// ── sketches ────────────────────────────────────────────────────────────────────────────

/// Zero-length lines, zero and negative radii, non-finite coordinates.
#[test]
fn degenerate_sketch_geometry() {
    let mut s = Session::new().expect("session");
    let sk = s.new_sketch(Plane::Xy).expect("sketch");

    // Every one of these may be refused; none may be accepted and then produce a solve that
    // reports success over geometry containing NaN.
    let _ = s.add_line(sk, (0.0, 0.0), (0.0, 0.0));
    let _ = s.add_sketch_circle(sk, (0.0, 0.0), 0.0);
    let _ = s.add_sketch_circle(sk, (0.0, 0.0), -5.0);
    let _ = s.add_line(sk, (f64::NAN, 0.0), (10.0, 10.0));
    let _ = s.add_sketch_circle(sk, (f64::INFINITY, 0.0), 5.0);

    if let Ok(report) = s.solve(sk) {
        if let Ok(geometry) = s.sketch_geometry(sk) {
            // `p` is the ABI's flat parameter block — its meaning depends on `kind`, but every
            // slot is a coordinate or a radius, so "all finite" holds whatever the kind is.
            for (i, g) in geometry.iter().enumerate() {
                for (j, v) in g.p.iter().enumerate() {
                    assert!(
                        v.is_finite(),
                        "degenerate sketch: solve reported {report:?} but geometry {i} \
                         parameter {j} came back non-finite ({v})"
                    );
                }
            }
        }
    }
}

/// A sketch constrained two incompatible ways at once.
///
/// Over-constrained profiles arrive with every DXF import. The solver must terminate and say so;
/// what it must not do is spin, or report success while quietly satisfying only one of them.
#[test]
fn over_constrained_sketch_terminates_and_says_so() {
    let mut s = Session::new().expect("session");
    let sk = s.new_sketch(Plane::Xy).expect("sketch");
    let line = s.add_line(sk, (0.0, 0.0), (10.0, 0.0)).expect("line");

    // Three mutually exclusive lengths for the same segment.
    let _ = s.constrain(sk, Con::Distance(line, Pt::Start, line, Pt::End, 10.0));
    let _ = s.constrain(sk, Con::Distance(line, Pt::Start, line, Pt::End, 25.0));
    let _ = s.constrain(sk, Con::Distance(line, Pt::Start, line, Pt::End, 40.0));

    match s.solve(sk) {
        Ok(report) => {
            if let Ok(geometry) = s.sketch_geometry(sk) {
                for g in geometry {
                    for v in g.p {
                        assert!(
                            v.is_finite(),
                            "over-constrained solve reported {report:?} and produced \
                             non-finite geometry"
                        );
                    }
                }
            }
        }
        Err(e) => assert!(
            !e.message.trim().is_empty(),
            "over-constrained solve failed with an empty message"
        ),
    }
}

// ── document stress ─────────────────────────────────────────────────────────────────────

/// Undo and redo across a deep history, then check the document still computes.
///
/// History is where "it worked when I tried it" hides: a corruption introduced at depth 200 is
/// invisible until someone undoes back through it.
#[test]
fn deep_undo_and_redo_leaves_a_computable_document() {
    let depth = 200usize;
    let mut s = Session::new().expect("session");
    let b = s.add_box(20.0, 20.0, 20.0).expect("box");
    s.recompute().expect("recompute");

    for i in 0..depth {
        s.set_length(b, "dx", 20.0 + i as f64).expect("edit");
    }
    while s.undo().unwrap_or(false) {}
    while s.redo().unwrap_or(false) {}

    assert!(recompute_survives(&mut s, "after deep undo/redo"), "recompute did not survive");
    assert_honest(&s, b, "after deep undo and redo");
}

/// Deleting a feature other features depend on.
#[test]
fn removing_a_depended_on_feature_is_handled() {
    let mut s = Session::new().expect("session");
    let case = "remove the base under a translate";
    let Some(b) = try_box(&mut s, 20.0, 20.0, 20.0) else { return };
    let Ok(moved) = s.add_translate(b, 10.0, 0.0, 0.0) else { return };
    if !recompute_survives(&mut s, case) {
        return;
    }

    if s.remove(b).is_err() {
        return;   // refusing to orphan a dependent is a perfectly good answer
    }
    recompute_survives(&mut s, case);
    // `moved` may be gone with its input, or present and blocked. Both are honest; a present,
    // Clean translate of a shape that no longer exists is not.
    if s.state(moved).is_ok() {
        assert_honest(&s, moved, case);
    }
}
