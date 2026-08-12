//! M3.2 ACCEPTANCE — scene assembly, with no GPU anywhere in sight.
//!
//! Every assertion here runs against `NullBackend`, which records draw calls instead of making
//! them. That is the whole reason the seam is three narrow interfaces rather than one
//! `render(scene)` (ADR 0007): the bugs that live in this layer — wrong instance counts, uploads
//! that should have deduped, an element slot mapping to the wrong name — are all catchable
//! without a graphics stack, and CI has no GPU.

use cad_tests::*;

/// Helper: a translation-only 4x3 affine, column-major.
fn at(x: f32, y: f32, z: f32) -> [f32; 12] {
    [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, x, y, z]
}

/// THE scale assertion, at the scene layer this time: many placements of identical geometry
/// collapse to ONE draw call and ONE set of GPU buffers.
///
/// This is what makes 100k parts submit in ~1000 calls. If it regresses, no amount of GPU work
/// recovers it — the CPU simply cannot issue 100k draw calls per frame.
#[test]
fn identical_parts_collapse_to_one_draw_call() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);

    // 200 placements of the same part, spread out in space.
    for i in 0..200 {
        s.add_placement(b, Some(&at(i as f32 * 30.0, 0.0, 0.0)))
            .unwrap();
    }
    s.scene_update(0.05, 0.35).unwrap();
    s.scene_submit().unwrap();

    let st = s.scene_stats().unwrap();
    assert_eq!(st.instances, 200, "every placement must become an instance");
    assert_eq!(st.unique_meshes, 1, "200 identical boxes are one mesh");
    assert_eq!(
        st.draw_calls, 2,
        "one shaded batch plus one edge batch — NOT one call per part, got {}",
        st.draw_calls
    );
    assert_eq!(st.frame_instances, 200);
    // 12 triangles per box, drawn 200 times.
    assert_eq!(st.frame_triangles, 12 * 200);
}

/// Distinct geometry must NOT be merged, or picking and colouring would be wrong.
#[test]
fn distinct_geometry_gets_its_own_batch() {
    let mut s = session();
    let a = s.add_box(20.0, 20.0, 20.0).unwrap();
    let b = s.add_box(30.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);

    s.add_placement(a, None).unwrap();
    s.add_placement(b, Some(&at(50.0, 0.0, 0.0))).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    s.scene_submit().unwrap();

    let st = s.scene_stats().unwrap();
    assert_eq!(st.unique_meshes, 2);
    assert_eq!(st.draw_calls, 4, "two shaded plus two edge batches");
}

/// Uploads are deduplicated by content hash. 200 identical parts must transfer one mesh's
/// worth of buffers, not 200.
#[test]
fn uploads_are_deduplicated_by_content() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    for i in 0..200 {
        s.add_placement(b, Some(&at(0.0, i as f32 * 30.0, 0.0)))
            .unwrap();
    }

    s.scene_reset_stats().unwrap();
    s.scene_update(0.05, 0.35).unwrap();

    let st = s.scene_stats().unwrap();
    // Three buffers for one mesh: vertices, indices, edge vertices.
    assert_eq!(
        st.gpu_uploads, 3,
        "one mesh means three buffers regardless of placement count, got {}",
        st.gpu_uploads
    );
}

/// An idle redraw must cost nothing. If this regresses the app is inexplicably busy while the
/// user is doing nothing, which is the kind of thing people notice as "it feels heavy".
#[test]
fn an_unchanged_document_rebuilds_nothing() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();

    s.scene_update(0.05, 0.35).unwrap();
    let first = s.scene_stats().unwrap();
    assert_eq!(first.rebuilds, 1);

    for _ in 0..10 {
        s.scene_update(0.05, 0.35).unwrap();
    }
    let after = s.scene_stats().unwrap();
    assert_eq!(after.rebuilds, 1, "ten idle updates must do no work");
    assert_eq!(after.gpu_uploads, first.gpu_uploads, "and upload nothing");
}

/// Camera changes must not touch instance data. Orbiting a 1M-instance assembly re-uploading
/// 56 MB per frame would make navigation unusable, and it is the reason `setCamera` and
/// `update` are separate calls.
#[test]
fn camera_movement_does_not_rebuild_or_upload() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    for i in 0..50 {
        s.add_placement(b, Some(&at(i as f32 * 25.0, 0.0, 0.0)))
            .unwrap();
    }
    s.scene_update(0.05, 0.35).unwrap();
    let before = s.scene_stats().unwrap();

    for _ in 0..30 {
        s.orbit(4.0, 2.0).unwrap();
        s.pan(3.0, 1.0).unwrap();
        s.zoom(1.0).unwrap();
        s.scene_update(0.05, 0.35).unwrap();
        s.scene_submit().unwrap();
    }

    let after = s.scene_stats().unwrap();
    assert_eq!(
        after.rebuilds, before.rebuilds,
        "navigation must not rebuild"
    );
    assert_eq!(
        after.gpu_uploads, before.gpu_uploads,
        "navigation must not upload"
    );
    assert_eq!(after.frames, 30, "but it must still be drawing");
}

/// Editing the document DOES rebuild — the other half of the diff contract. A diff that never
/// invalidates is just a stale cache.
#[test]
fn editing_the_document_rebuilds() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    let before = s.scene_stats().unwrap();

    s.set_length(b, "dx", 40.0).unwrap();
    recompute_ok(&mut s);
    s.scene_update(0.05, 0.35).unwrap();

    let after = s.scene_stats().unwrap();
    assert_eq!(after.rebuilds, before.rebuilds + 1);
    assert!(
        after.gpu_uploads > before.gpu_uploads,
        "new geometry must upload new buffers"
    );
}

/// A pick resolves to a stable ElementName, and to the document object that owns it.
///
/// This is the payoff of carrying an element index in every vertex, and it is the assertion
/// that makes GPU ID-buffer picking worth building at all: a pick has to become a reference
/// that survives a rebuild, not a triangle number.
#[test]
fn a_pick_resolves_to_an_element_name_and_its_owner() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();

    let st = s.scene_stats().unwrap();
    assert!(st.element_slots >= 18, "6 faces + 12 edges at least");

    for slot in 0..st.element_slots as u32 {
        s.set_next_hit(0, slot, true).unwrap();
        let name = s.pick(100, 100);
        assert!(!name.is_empty(), "slot {slot} did not resolve to a name");
        assert_eq!(s.pick_owner(100, 100).unwrap(), b, "owner must be the box");
    }

    // An invalid hit resolves to nothing rather than to element zero.
    s.set_next_hit(0, 0, false).unwrap();
    assert!(s.pick(100, 100).is_empty(), "a miss must not resolve");
}

/// Each placement gets its OWN element slots even though they share a mesh. Without this, 50,000
/// identical bolts would all resolve to the same faces and selection would be meaningless.
#[test]
fn each_placement_has_its_own_element_slots() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.add_placement(b, Some(&at(50.0, 0.0, 0.0))).unwrap();
    s.scene_update(0.05, 0.35).unwrap();

    let one = s.scene_stats().unwrap().element_slots;

    s.clear_placements().unwrap();
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    let half = s.scene_stats().unwrap().element_slots;

    assert_eq!(
        one,
        half * 2,
        "two placements must have twice the element slots"
    );
}

/// Highlighting is per element and does not rebuild. Hover fires on every mouse move, so a
/// rebuild here would make the viewport stutter whenever the pointer moves.
#[test]
fn highlighting_is_cheap_and_per_element() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    let before = s.scene_stats().unwrap();

    let face = s.box_face_name(b, BoxFace::ZMax);
    assert!(!face.is_empty());
    s.set_highlight(&face, Highlight::Selected).unwrap();
    s.scene_submit().unwrap();

    let after = s.scene_stats().unwrap();
    assert_eq!(after.highlighted, 1, "exactly one element highlighted");
    assert_eq!(
        after.rebuilds, before.rebuilds,
        "highlighting must not rebuild"
    );

    s.clear_highlights().unwrap();
    s.scene_submit().unwrap();
    assert_eq!(s.scene_stats().unwrap().highlighted, 0);
}

/// Zoom-to-fit frames the geometry, and does something sane for an empty scene rather than
/// producing a zero distance and NaNs downstream.
#[test]
fn zoom_to_fit_frames_geometry_and_survives_emptiness() {
    let mut s = session();

    // Empty scene: fit must not produce a degenerate camera.
    s.scene_update(0.05, 0.35).unwrap();
    s.fit().unwrap();
    let empty = s.camera_distance().unwrap();
    assert!(
        empty.is_finite() && empty > 0.0,
        "empty fit gave distance {empty}"
    );

    let b = s.add_box(1000.0, 1000.0, 1000.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    s.fit().unwrap();

    let big = s.camera_distance().unwrap();
    assert!(
        big > 1000.0,
        "a 1m box needs to be framed from further than 1m, got {big}"
    );
    assert!(big.is_finite());
}

/// Orthographic is the CAD default — engineers check alignment in it, and perspective makes
/// coincident faces ambiguous. The opposite default to a game engine, so worth pinning.
#[test]
fn the_default_projection_is_orthographic() {
    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(b, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    s.scene_submit().unwrap();
    assert!(
        s.scene_stats().unwrap().orthographic,
        "CAD defaults to ortho"
    );

    s.set_orthographic(false).unwrap();
    s.scene_submit().unwrap();
    assert!(!s.scene_stats().unwrap().orthographic);
}

/// Navigation presets. Cheap to offer and people are religious about them — a user with
/// middle-drag-orbits in their fingers finds any other mapping unusable.
#[test]
fn navigation_presets_map_gestures_differently() {
    const LEFT: i32 = 0;
    const MIDDLE: i32 = 1;
    let mut s = session();

    s.set_nav_preset(NavPreset::Cad).unwrap();
    assert_eq!(s.drag_for(MIDDLE, false, false).unwrap(), Drag::Orbit);
    assert_eq!(s.drag_for(MIDDLE, true, false).unwrap(), Drag::Pan);

    // Fusion inverts it, deliberately.
    s.set_nav_preset(NavPreset::Fusion).unwrap();
    assert_eq!(s.drag_for(MIDDLE, false, false).unwrap(), Drag::Pan);
    assert_eq!(s.drag_for(MIDDLE, true, false).unwrap(), Drag::Orbit);

    // Left is selection under every preset; navigation must never steal it.
    for p in [NavPreset::Cad, NavPreset::Fusion, NavPreset::Blender] {
        s.set_nav_preset(p).unwrap();
        assert_eq!(s.drag_for(LEFT, false, false).unwrap(), Drag::None);
    }
}

/// An invisible placement draws nothing but must not disturb the rest of the scene.
#[test]
fn a_hidden_placement_is_skipped() {
    let mut s = session();
    let a = s.add_box(20.0, 20.0, 20.0).unwrap();
    let b = s.add_box(30.0, 30.0, 30.0).unwrap();
    recompute_ok(&mut s);
    s.add_placement(a, None).unwrap();
    s.add_placement(b, Some(&at(60.0, 0.0, 0.0))).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    s.scene_submit().unwrap();
    let both = s.scene_stats().unwrap();
    assert_eq!(both.instances, 2);

    s.clear_placements().unwrap();
    s.add_placement(a, None).unwrap();
    s.scene_update(0.05, 0.35).unwrap();
    s.scene_submit().unwrap();
    assert_eq!(s.scene_stats().unwrap().instances, 1);
}
