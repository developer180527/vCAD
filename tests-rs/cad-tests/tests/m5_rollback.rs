//! M5 — the rollback marker.
//!
//! The tree-position control every history-based CAD application has: features after the marker are
//! SUSPENDED, so you can move it up, insert a feature in the middle, and move it back down.
//!
//! The assertions worth reading are the ones about what rollback must NOT do: it must not report
//! suspended features as failures, must not leave their geometry behind, and must not invalidate a
//! single cache entry.

use cad_tests::*;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("cad-rb-{}-{}", name, std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// Box, then a fillet on it. Rolling back to the box must suspend the fillet: no output, and the
/// document's geometry is the unfilleted box again.
#[test]
fn rolling_back_suspends_later_features() {
    let mut s = session();
    let b = s.add_box(40.0, 40.0, 20.0).unwrap();
    recompute_ok(&mut s);
    let plain = s.volume(b).unwrap();

    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    let f = s.add_fillet(b, &edge, 5.0).unwrap();
    recompute_ok(&mut s);
    let filleted = s.volume(f).unwrap();
    assert!(filleted < plain, "the fillet should have removed material");

    s.set_rollback(Some(b)).unwrap();
    let report = s.recompute().unwrap();

    assert_eq!(s.rollback().unwrap(), Some(b));
    // The suspended fillet has no geometry at all.
    assert!(s.volume(f).is_err(), "a suspended feature still had geometry");
    // And it is NOT reported as a failure: nothing is wrong.
    assert_eq!(report.failed, 0, "a suspended feature was reported as failed");
    assert_eq!(report.blocked, 0, "a suspended feature was reported as blocked");
    // The feature that is still in scope is untouched.
    assert!((s.volume(b).unwrap() - plain).abs() < 1e-9);
}

/// Rolling forward must restore the model, and must do it FROM THE CACHE. The marker is not part of
/// the document digest precisely so that moving it invalidates nothing — that is what makes dragging
/// it feel instant instead of recomputing the tail of a heavy model.
#[test]
fn rolling_forward_restores_from_cache_without_recomputing() {
    let mut s = session();
    let b = s.add_box(30.0, 30.0, 30.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    let f = s.add_fillet(b, &edge, 4.0).unwrap();
    recompute_ok(&mut s);
    let filleted = s.volume(f).unwrap();

    s.set_rollback(Some(b)).unwrap();
    let _ = s.recompute().unwrap();

    s.reset_cache_stats().unwrap();
    s.set_rollback(None).unwrap();
    let report = s.recompute().unwrap();

    assert!((s.volume(f).unwrap() - filleted).abs() < 1e-9, "rolling forward changed the geometry");
    assert_eq!(
        report.computed, 0,
        "rolling forward recomputed {} features; the marker must not invalidate cache keys",
        report.computed
    );
    assert!(report.cached > 0, "nothing was served from the cache");
}

/// A feature added while rolled back lands in the middle of the tree, which is the entire point of
/// the marker. The suspended tail must still be suspended afterwards.
#[test]
fn a_feature_can_be_inserted_while_rolled_back() {
    let mut s = session();
    let b = s.add_box(50.0, 50.0, 50.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    let f = s.add_fillet(b, &edge, 6.0).unwrap();
    recompute_ok(&mut s);

    s.set_rollback(Some(b)).unwrap();
    let _ = s.recompute().unwrap();

    // Inserted "before" the fillet in tree terms: it computes, the fillet does not.
    let inserted = s.add_box(10.0, 10.0, 10.0).unwrap();
    let _ = s.recompute().unwrap();
    // The new feature has a HIGHER id than the marker, so it is suspended too — which is correct and
    // worth pinning: inserting while rolled back puts the feature after the marker, and the user
    // moves the marker down to bring it in. Anything else would silently reorder the tree.
    assert!(
        s.volume(inserted).is_err(),
        "a feature added while rolled back computed anyway, which reorders the tree behind the user"
    );

    s.set_rollback(None).unwrap();
    let _ = s.recompute().unwrap();
    assert!(s.volume(inserted).unwrap() > 0.0);
    assert!(s.volume(f).unwrap() > 0.0);
}

/// The marker is document state, so it has to survive save and reopen. A model that reopens fully
/// computed when it was left rolled back is a model whose geometry silently changed on load.
#[test]
fn the_marker_survives_save_and_reopen() {
    let path = scratch("persist").join("rolled.vpart");

    let mut s = session();
    let b = s.add_box(20.0, 20.0, 20.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    let f = s.add_fillet(b, &edge, 3.0).unwrap();
    recompute_ok(&mut s);

    s.set_rollback(Some(b)).unwrap();
    let _ = s.recompute().unwrap();
    s.save(path.to_str().unwrap()).unwrap();

    let mut t = session();
    t.open(path.to_str().unwrap()).unwrap();
    assert_eq!(t.rollback().unwrap(), Some(b), "the marker was lost on save/open");
    assert!(t.volume(f).is_err(), "the reopened document computed a suspended feature");
}

/// Rolling back to something that does not exist must be refused, not silently ignored.
#[test]
fn rolling_back_to_a_missing_object_is_refused() {
    let mut s = session();
    let _ = s.add_box(10.0, 10.0, 10.0).unwrap();
    let err = s.set_rollback(Some(Object(999))).expect_err("a bogus marker was accepted");
    assert_eq!(err.code, cad::ErrorCode::BadHandle);
    assert_eq!(s.rollback().unwrap(), None, "a refused rollback changed the marker anyway");
}
