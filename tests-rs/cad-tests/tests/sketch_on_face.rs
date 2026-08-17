//! Sketching on a model face, through the ABI a shell would use.
//!
//! Step 1c. The core could already place a sketch on a face (1b); nothing above the core could ASK
//! for one. This is that request, and the extrude at the end is the half that matters: it proves
//! the resolved frame is USABLE downstream rather than merely stored.
//!
//! The face is identified by NAME, never by index. That is what makes the placement survive an
//! edit that reorders faces, and it is the failure FreeCAD is known for.

use cad::{BoxFace, Plane, Session};

#[test]
fn a_sketch_can_be_created_on_a_named_face() {
    let mut s = Session::new().expect("session");
    let body = s.add_box(40.0, 30.0, 20.0).expect("box");
    s.recompute().expect("recompute");

    let face = s.box_face_name(body, BoxFace::ZMax);
    assert!(!face.is_empty(), "a box's face must have a stable name");

    let sketch = s.new_sketch_on_face(&face).expect("sketch on face");
    s.add_line(sketch, (0.0, 0.0), (10.0, 0.0)).expect("line");
    assert_eq!(s.sketch_geometry(sketch).expect("geometry").len(), 1);
}

#[test]
fn a_face_sketch_needs_its_body_before_it_can_build() {
    // The refusal, reached through the real API. A face-placed sketch that does not reference the
    // body has no way to be located, and building it on a global plane instead would put the
    // profile somewhere the user never drew it.
    let mut s = Session::new().expect("session");
    let body = s.add_box(40.0, 30.0, 20.0).expect("box");
    s.recompute().expect("recompute");
    let face = s.box_face_name(body, BoxFace::ZMax);

    let sketch = s.new_sketch_on_face(&face).expect("sketch on face");
    s.add_line(sketch, (0.0, 0.0), (10.0, 0.0)).expect("line");
    s.add_line(sketch, (10.0, 0.0), (10.0, 10.0)).expect("line");
    s.add_line(sketch, (10.0, 10.0), (0.0, 0.0)).expect("line");

    let text = s.sketch_text(sketch);
    let feature = s.add_sketch_feature(&text).expect("sketch feature");
    // Deliberately NOT linked to the body.
    s.recompute()
        .expect("recompute returns a report, not an error");

    let state = s.state(feature).expect("state");
    assert_ne!(
        state,
        cad::State::Clean,
        "a face sketch with no body must not compute; it has no way to know where it is"
    );
    assert!(!s.object_error(feature).is_empty(), "and it must say why");
}

#[test]
fn a_face_sketch_extrudes_from_the_face_it_was_drawn_on() {
    // The whole point of step 1. A profile drawn on the top of a box, extruded, must produce a
    // solid sitting ON that face — not one starting at the origin.
    let mut s = Session::new().expect("session");
    let body = s.add_box(40.0, 30.0, 20.0).expect("box");
    s.recompute().expect("recompute");
    let face = s.box_face_name(body, BoxFace::ZMax);

    let sketch = s.new_sketch_on_face(&face).expect("sketch on face");
    s.add_line(sketch, (0.0, 0.0), (10.0, 0.0)).expect("line");
    s.add_line(sketch, (10.0, 0.0), (10.0, 10.0)).expect("line");
    s.add_line(sketch, (10.0, 10.0), (0.0, 10.0)).expect("line");
    s.add_line(sketch, (0.0, 10.0), (0.0, 0.0)).expect("line");

    let text = s.sketch_text(sketch);
    let feature = s.add_sketch_feature(&text).expect("sketch feature");
    s.set_input(feature, "body", body)
        .expect("the body the face belongs to");

    s.recompute().expect("recompute");
    let state = s.state(feature).expect("state");
    assert_eq!(
        state,
        cad::State::Clean,
        "a face sketch with its body should compute: {}",
        s.object_error(feature)
    );

    let pad = s.add_extrude(feature, 5.0, Plane::Xy).expect("extrude");
    s.recompute().expect("recompute");
    assert_eq!(
        s.state(pad).expect("state"),
        cad::State::Clean,
        "extruding a face-placed profile should work: {}",
        s.object_error(pad)
    );

    // 10 x 10 x 5. The VOLUME catches an extrude that silently did nothing, far more reliably than
    // a face count.
    //
    // WHAT THIS DOES NOT PROVE: that the pad sits ON the face rather than at the origin. Volume is
    // translation-invariant, so a profile built on the wrong plane and extruded the right distance
    // passes this. The POSITION is asserted in tests/acceptance/sketch_plane.cpp, which can reach
    // the centroid; the Rust wrapper has no position accessor and adding one is ABI surface that
    // should be added for a reason, not for a test. Stated rather than left for someone to assume.
    let volume = s.volume(pad).expect("volume");
    assert!(
        (volume - 500.0).abs() < 1.0,
        "expected a 10x10x5 pad, got volume {volume}"
    );
}
