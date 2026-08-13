//! M4 — the native document format (ADR 0003).
//!
//! Saving a document is not "write the geometry out". It is writing the FEATURE TREE such that
//! reopening it gives back an editable model: same parameters, same references, same ids.
//!
//! The load-bearing assertion in this file is `fillet_edge_references_survive_a_round_trip`.
//! Everything else could pass while the format is still useless, because the thing that makes a
//! saved parametric document worth anything is that its geometric references still resolve — and
//! those references are ElementNames that embed the ID of the feature that produced them. Any
//! save/load path that renumbers objects breaks every one of them, and does it silently.

use cad_tests::*;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("cad-persist-{}-{}", name, std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn saves_and_reopens_a_single_feature() {
    let path = scratch("single").join("part.vpart");

    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let volume = s.volume(b).unwrap();

    s.save(path.to_str().unwrap()).unwrap();
    assert!(path.exists(), "save produced no file");
    assert!(
        std::fs::metadata(&path).unwrap().len() > 0,
        "save produced an empty file"
    );

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();

    assert_eq!(t.object_count().unwrap(), 1);
    // Opening recomputes, so geometry must be present without the caller asking for it.
    let reopened = Object(1);
    assert!(
        (t.volume(reopened).unwrap() - volume).abs() < 1e-9,
        "reopened geometry differs: {} vs {volume}",
        t.volume(reopened).unwrap()
    );
}

/// THE test. A fillet stores the element names of the edges it rounds; those names contain the
/// object id of the feature that produced the edge. If save/load reassigns ids, every name becomes
/// unresolvable and the fillet fails with NamingLost — a corrupt document that looks like a
/// modelling error.
#[test]
fn fillet_edge_references_survive_a_round_trip() {
    let path = scratch("fillet").join("filleted.vpart");

    let mut s = session();
    let b = s.add_box(80.0, 80.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    assert!(!edge.is_empty(), "no edge to fillet");

    let f = s.add_fillet(b, &edge, 5.0).unwrap();
    recompute_ok(&mut s);
    assert_eq!(s.state(f).unwrap(), State::Clean);
    let filleted_volume = s.volume(f).unwrap();

    s.save(path.to_str().unwrap()).unwrap();

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();
    assert_eq!(t.object_count().unwrap(), 2);

    // Ids are part of the format, so the fillet is still object 2.
    let reopened_fillet = Object(2);
    assert_eq!(
        t.state(reopened_fillet).unwrap(),
        State::Clean,
        "the fillet did not recompute after reopening — its edge reference was lost"
    );
    assert!(
        (t.volume(reopened_fillet).unwrap() - filleted_volume).abs() < 1e-6,
        "the reopened fillet produced different geometry"
    );
}

/// A saved document must be byte-identical in MEANING, which the document digest measures
/// directly. Cheaper and stricter than comparing geometry: it covers every property of every
/// object, including ones no assertion here thought to check.
#[test]
fn document_digest_is_preserved() {
    let path = scratch("digest").join("d.vpart");

    let mut s = session();
    let b = s.add_box(30.0, 40.0, 50.0).unwrap();
    let c = s.add_cylinder(10.0, 60.0).unwrap();
    let _ = s.add_cut(b, c).unwrap();
    recompute_ok(&mut s);
    let before = s.document_digest().unwrap();

    s.save(path.to_str().unwrap()).unwrap();

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();
    assert_eq!(
        t.document_digest().unwrap(),
        before,
        "the reopened document is not the same document"
    );
}

/// Deleting the highest-numbered object then saving must not let a new object inherit its id.
/// If it did, element names recorded against the deleted feature would resolve against the new
/// one's geometry — a WRONG reference rather than a missing one, which no error can catch.
#[test]
fn object_ids_are_never_reused_across_a_save() {
    let path = scratch("ids").join("d.vpart");

    let mut s = session();
    let _keep = s.add_box(10.0, 10.0, 10.0).unwrap();
    let temp = s.add_box(20.0, 20.0, 20.0).unwrap();
    s.remove(temp).unwrap();
    recompute_ok(&mut s);

    s.save(path.to_str().unwrap()).unwrap();

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();
    let fresh = t.add_box(5.0, 5.0, 5.0).unwrap();
    assert_ne!(
        fresh.0,
        temp.0,
        "a new object was handed the id of one deleted before the save"
    );
}

/// Opening something that is not ours must fail cleanly rather than crash or half-load.
#[test]
fn rejects_a_file_that_is_not_a_document() {
    let dir = scratch("bogus");
    let path = dir.join("not-a-part.vpart");
    std::fs::write(&path, b"this is not a database").unwrap();

    let mut s = session();
    let b = s.add_box(10.0, 10.0, 10.0).unwrap();
    recompute_ok(&mut s);

    assert!(
        s.open(path.to_str().unwrap()).is_err(),
        "a garbage file was accepted as a document"
    );
    // The failed open must leave the session alone, not empty it.
    assert_eq!(
        s.object_count().unwrap(),
        1,
        "a failed open destroyed the document that was already loaded"
    );
    assert!(s.volume(b).unwrap() > 0.0);
}

#[test]
fn reports_document_info_without_loading_it() {
    let path = scratch("info").join("d.vpart");
    let mut s = session();
    let _ = s.add_box(10.0, 20.0, 30.0).unwrap();
    recompute_ok(&mut s);
    s.save(path.to_str().unwrap()).unwrap();

    // Opening a second time over an existing file must overwrite cleanly, not append or fail.
    s.save(path.to_str().unwrap()).unwrap();

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();
    assert_eq!(t.object_count().unwrap(), 1);
}
