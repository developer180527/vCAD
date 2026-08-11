//! Property tests: invariants that must hold for ALL inputs, not the handful an
//! example-based test happens to pick.
//!
//! This tier earns its place in a CAD system specifically because geometry kernels fail on
//! the inputs nobody thought to type. A fillet radius of exactly half the edge length, a box
//! one micron thick, a parameter sweep that crosses a topological transition — proptest
//! finds these and, more valuably, shrinks them to the smallest reproducer.
//!
//! Rules for this file:
//!   * Assert INVARIANTS, never specific values. "The volume is positive and less than the
//!     bounding box" is a property; "the volume is 240000" is an example.
//!   * A geometric operation is allowed to FAIL. It is not allowed to succeed and produce
//!     an invalid shape, or to succeed and lose a name. That distinction is the point.
//!   * Every failure the kernel reports must be legible, because these same failures reach
//!     users.

use cad_tests::*;
use proptest::prelude::*;

/// Dimensions spanning four orders of magnitude, including the sub-millimetre range where
/// OCCT tolerances start to matter.
fn dimension() -> impl Strategy<Value = f64> {
    prop_oneof![
        3 => 1.0f64..500.0,      // ordinary
        1 => 0.01f64..1.0,       // thin — tolerance territory
        1 => 500.0f64..5000.0,   // large
    ]
}

proptest! {
    #![proptest_config(ProptestConfig {
        cases: 64,          // each case drives OCCT; keep the suite usable as a pre-commit gate
        max_shrink_iters: 256,
        ..ProptestConfig::default()
    })]

    /// A box always produces a valid solid with exactly six faces and twelve edges, and its
    /// volume is the product of its dimensions. The last part is a real check on the unit
    /// plumbing: if millimetres leaked as metres anywhere, this fails by 10^9.
    #[test]
    fn a_box_is_always_a_valid_six_faced_solid(
        dx in dimension(), dy in dimension(), dz in dimension()
    ) {
        let mut s = session();
        let b = s.add_box(dx, dy, dz).unwrap();
        let report = s.recompute().unwrap();

        prop_assert!(report.all_succeeded(), "a box should never fail to build");
        prop_assert!(s.is_valid_shape(b).unwrap());
        prop_assert_eq!(s.face_count(b).unwrap(), 6);
        prop_assert_eq!(s.edge_count(b).unwrap(), 12);

        let volume = s.volume(b).unwrap();
        let expected = dx * dy * dz;
        prop_assert!(
            (volume - expected).abs() <= expected * 1e-9,
            "volume {volume} != {expected}"
        );
    }

    /// The M1 guarantee, over the whole parameter space rather than one sweep: an edge
    /// reference taken at one size resolves at every other size.
    #[test]
    fn an_edge_reference_survives_any_resize(
        dx in dimension(), dy in dimension(), dz in dimension(),
        new_dx in dimension()
    ) {
        let mut s = session();
        let b = s.add_box(dx, dy, dz).unwrap();
        recompute_ok(&mut s);

        let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
        prop_assert!(!edge.is_empty());

        s.set_length(b, "dx", new_dx).unwrap();
        recompute_ok(&mut s);

        let after = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
        prop_assert_eq!(
            edge, after,
            "the top-front edge must keep its name across a resize"
        );
    }

    /// A fillet either succeeds with a valid solid, or fails with a legible message. It must
    /// never succeed and hand back geometry that BRepCheck rejects — a silently invalid
    /// solid poisons everything downstream and is very hard to trace back.
    #[test]
    fn a_fillet_either_succeeds_validly_or_fails_legibly(
        dx in 5.0f64..200.0, dy in 5.0f64..200.0, dz in 5.0f64..200.0,
        radius in 0.1f64..100.0
    ) {
        let mut s = session();
        let b = s.add_box(dx, dy, dz).unwrap();
        recompute_ok(&mut s);
        let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);

        let f = s.add_fillet(b, &edge, radius).unwrap();
        let report = s.recompute().unwrap();

        if report.failed == 0 {
            prop_assert_eq!(s.state(f).unwrap(), State::Clean);
            prop_assert!(
                s.is_valid_shape(f).unwrap(),
                "a fillet that reports success must produce a valid solid"
            );
            // A fillet removes material from a convex edge, so it cannot grow the part.
            let filleted = s.volume(f).unwrap();
            let plain = s.volume(b).unwrap();
            prop_assert!(
                filleted <= plain * (1.0 + 1e-9),
                "filleting an outside edge cannot increase volume: {filleted} > {plain}"
            );
        } else {
            prop_assert_eq!(s.state(f).unwrap(), State::Failed);
            prop_assert!(
                !s.object_error(f).is_empty(),
                "a failed fillet must explain itself; this message reaches users"
            );
        }
    }

    /// Recompute is idempotent: running it twice with no edits in between must be a no-op
    /// the second time. A violation means dirty tracking is wrong, which shows up to users
    /// as an application that is inexplicably busy.
    #[test]
    fn recompute_is_idempotent(
        dx in dimension(), dy in dimension(), dz in dimension()
    ) {
        let mut s = session();
        let b = s.add_box(dx, dy, dz).unwrap();
        recompute_ok(&mut s);
        let hash = s.content_hash(b);

        let second = recompute_ok(&mut s);
        prop_assert_eq!(second.computed, 0);
        prop_assert_eq!(second.cached, 0);
        prop_assert_eq!(s.content_hash(b), hash);
    }

    /// The cache key is a pure function of content. Setting a property to the value it
    /// already has must not invalidate anything.
    #[test]
    fn setting_a_property_to_its_current_value_changes_nothing(
        dx in dimension(), dy in dimension(), dz in dimension()
    ) {
        let mut s = session();
        let b = s.add_box(dx, dy, dz).unwrap();
        recompute_ok(&mut s);
        let key = s.cache_key(b).unwrap();
        let hash = s.content_hash(b);

        s.set_length(b, "dx", dx).unwrap();   // same value
        recompute_ok(&mut s);

        prop_assert_eq!(s.cache_key(b).unwrap(), key);
        prop_assert_eq!(s.content_hash(b), hash);
    }

    /// Undo/redo is a true round trip for any number of edits.
    #[test]
    fn undo_redo_round_trips_for_any_edit_sequence(
        widths in prop::collection::vec(dimension(), 1..8)
    ) {
        let mut s = session();
        let b = s.add_box(100.0, 60.0, 40.0).unwrap();

        let mut digests = vec![s.document_digest().unwrap()];
        for w in &widths {
            s.set_length(b, "dx", *w).unwrap();
            digests.push(s.document_digest().unwrap());
        }

        // Walk all the way back…
        for expected in digests.iter().rev().skip(1) {
            prop_assert!(s.undo().unwrap());
            prop_assert_eq!(s.document_digest().unwrap(), *expected);
        }
        // …and all the way forward.
        for expected in digests.iter().skip(1) {
            prop_assert!(s.redo().unwrap());
            prop_assert_eq!(s.document_digest().unwrap(), *expected);
        }
    }

    /// Cutting a solid out of itself-sized tool leaves nothing, and cutting a disjoint tool
    /// leaves everything. Between those, volume must be monotonically non-increasing.
    #[test]
    fn a_cut_never_adds_material(
        dx in 10.0f64..200.0, dy in 10.0f64..200.0, dz in 10.0f64..200.0,
        tool_x in 1.0f64..100.0, offset in -50.0f64..150.0
    ) {
        let mut s = session();
        let base = s.add_box(dx, dy, dz).unwrap();
        let tool = s.add_box(tool_x, dy * 2.0, dz * 2.0).unwrap();
        let moved = s.add_translate(tool, offset, -dy * 0.25, -dz * 0.25).unwrap();
        let cut = s.add_cut(base, moved).unwrap();

        let report = s.recompute().unwrap();
        prop_assume!(report.failed == 0);   // a failed boolean is reported elsewhere

        let before = s.volume(base).unwrap();
        let after = s.volume(cut).unwrap();
        prop_assert!(after >= -1e-9, "volume cannot be negative: {after}");
        prop_assert!(
            after <= before * (1.0 + 1e-9),
            "a cut cannot add material: {after} > {before}"
        );
        prop_assert!(s.is_valid_shape(cut).unwrap());
    }
}

// Length parsing round-trips for any value in any system. Separate from the proptest! block
// above because it needs no session, so it can afford many more cases.
proptest! {
    #![proptest_config(ProptestConfig { cases: 512, ..ProptestConfig::default() })]

    #[test]
    fn parsing_a_formatted_length_is_the_identity(mm in -1e6f64..1e6) {
        for (suffix, factor) in [("mm", 1.0), ("cm", 10.0), ("m", 1000.0),
                                 ("in", 25.4), ("ft", 304.8)] {
            let text = format!("{}{}", mm / factor, suffix);
            let parsed = parse_length(&text, UnitSystem::Millimetre).unwrap();
            prop_assert!(
                (parsed - mm).abs() <= mm.abs() * 1e-9 + 1e-9,
                "{text} parsed as {parsed}, expected {mm}"
            );
        }
    }
}
