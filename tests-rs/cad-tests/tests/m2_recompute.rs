//! M2 ACCEPTANCE — the document DAG and the recompute engine.
//!
//! The gate for M2, as the naming stability suite was for M1. Written in Rust and driven
//! through the C ABI on purpose: this is the same surface plugins and the iPad shell will
//! use, so an ABI regression fails here rather than being discovered by a third party.

use cad_tests::*;

/// The M2 headline: build a multi-feature part headlessly, change one upstream parameter,
/// and recompute exactly the affected subgraph — not the whole document.
#[test]
fn changing_one_parameter_recomputes_only_what_depends_on_it() {
    let mut s = session();

    // Two independent branches that meet at a cut:
    //
    //     base ──> fillet ──┐
    //                        ├──> cut
    //     tool ──> moved ───┘
    let base = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);

    let edge = s.box_edge_between(base, BoxFace::ZMax, BoxFace::YMin);
    assert!(
        !edge.is_empty(),
        "expected a top-front edge on the base box"
    );

    let fillet = s.add_fillet(base, &edge, 5.0).unwrap();
    let tool = s.add_box(20.0, 20.0, 200.0).unwrap();
    let moved = s.add_translate(tool, 40.0, 20.0, -50.0).unwrap();
    let cut = s.add_cut(fillet, moved).unwrap();

    let first = recompute_ok(&mut s);
    // Four, not five: the base box was already computed by the recompute above, and the
    // persistent document kept its output, so this pass skips it.
    assert_eq!(first.computed, 4, "the four new objects are computed");
    assert_eq!(first.skipped, 1, "the already-computed base is left alone");
    assert!(s.is_valid_shape(cut).unwrap());

    // A second pass with no edits must do nothing at all. If this recomputes anything, the
    // dirty tracking is wrong and every interaction will feel sluggish for no reason.
    let idle = recompute_ok(&mut s);
    assert_eq!(
        idle.computed, 0,
        "an idle recompute must not compute anything"
    );
    assert_eq!(idle.skipped, 5);

    // Now change the TOOL's size. The base and its fillet do not depend on it, so they must
    // not be recomputed; the moved tool and the cut must be.
    s.reset_cache_stats().unwrap();
    s.set_length(tool, "dx", 30.0).unwrap();

    let after = recompute_ok(&mut s);
    assert_eq!(
        after.skipped, 2,
        "base and fillet are untouched by a change to the tool"
    );
    assert_eq!(
        after.computed + after.cached,
        3,
        "tool, moved and cut must be re-evaluated"
    );
    assert_eq!(s.state(base).unwrap(), State::Clean);
    assert_eq!(s.state(fillet).unwrap(), State::Clean);
}

/// Undoing a parameter change must restore the previous geometry exactly, and the cache
/// must serve it rather than recomputing — the value was computed once already.
#[test]
fn undo_restores_geometry_and_hits_the_cache() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);

    let original_hash = s.content_hash(b);
    let original_volume = s.volume(b).unwrap();
    assert!((original_volume - box_volume(100.0, 60.0, 40.0)).abs() < 1e-6);

    s.set_length(b, "dx", 250.0).unwrap();
    recompute_ok(&mut s);
    assert!((s.volume(b).unwrap() - box_volume(250.0, 60.0, 40.0)).abs() < 1e-6);
    assert_ne!(s.content_hash(b), original_hash);

    assert!(s.undo().unwrap());
    s.reset_cache_stats().unwrap();
    let after_undo = recompute_ok(&mut s);

    assert_eq!(
        s.content_hash(b),
        original_hash,
        "undo must restore the exact geometry, not merely a similar one"
    );

    // Undo costs NOTHING — not even a cache lookup.
    //
    // This is the payoff of the persistent document (ADR 0003). Undo does not replay an
    // inverse edit and then recompute; it restores the previous document version, which
    // still holds the computed output and its Clean state. A mutable document would have to
    // recompute here, or maintain a parallel undo cache that can disagree with the model.
    assert_eq!(after_undo.computed, 0, "undo must not recompute anything");
    assert_eq!(
        after_undo.skipped, 1,
        "the restored version is already clean"
    );
    assert_eq!(s.cache_stats().unwrap().hits, 0);

    assert!(s.redo().unwrap());
    recompute_ok(&mut s);
    assert!((s.volume(b).unwrap() - box_volume(250.0, 60.0, 40.0)).abs() < 1e-6);
}

/// Undo is only cheap because the document is persistent. This checks the property that
/// makes that true: an edit leaves the previous version genuinely intact.
#[test]
fn undo_redo_round_trips_the_document_digest() {
    let mut s = session();
    let b = s.add_box(10.0, 10.0, 10.0).unwrap();
    let before = s.document_digest().unwrap();

    s.set_length(b, "dx", 20.0).unwrap();
    let after = s.document_digest().unwrap();
    assert_ne!(before, after);

    assert!(s.undo().unwrap());
    assert_eq!(s.document_digest().unwrap(), before);
    assert!(s.redo().unwrap());
    assert_eq!(s.document_digest().unwrap(), after);

    // Undoing past the beginning is a no-op, not an error and not a corruption.
    while s.undo().unwrap() {}
    assert!(!s.undo().unwrap());
    assert_eq!(s.document_digest().unwrap(), s.document_digest().unwrap());
}

/// One broken feature must not stop the other branches from computing. A user with a
/// fifty-feature part and one bad fillet must still see the other forty-nine.
#[test]
fn a_failed_feature_blocks_only_its_own_branch() {
    let mut s = session();

    let good = s.add_box(50.0, 50.0, 50.0).unwrap();
    let base = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(base, BoxFace::ZMax, BoxFace::YMin);

    // A fillet radius far larger than the part cannot be built.
    let broken = s.add_fillet(base, &edge, 500.0).unwrap();
    let downstream = s.add_translate(broken, 10.0, 0.0, 0.0).unwrap();

    let report = s.recompute().expect("recompute call itself must not fail");
    assert_eq!(report.failed, 1, "exactly the fillet failed");
    assert_eq!(report.blocked, 1, "exactly its dependent was blocked");

    assert_eq!(s.state(broken).unwrap(), State::Failed);
    assert_eq!(s.state(downstream).unwrap(), State::Blocked);

    // The independent branch is untouched and usable.
    assert_eq!(s.state(good).unwrap(), State::Clean);
    assert!(s.is_valid_shape(good).unwrap());

    // And the failure is legible rather than an opaque code.
    let message = s.object_error(broken);
    assert!(!message.is_empty(), "a failed feature must say why");
}

/// A reference to an edge that a later feature destroys must fail loudly — never rebind to
/// a neighbouring edge. This is the M1 guarantee surviving all the way out to the ABI.
#[test]
fn a_lost_edge_reference_fails_rather_than_rebinding() {
    let mut s = session();
    let base = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(base, BoxFace::ZMax, BoxFace::YMin);
    assert!(!edge.is_empty());

    // Chop the whole top-front corner away, so no edge bounds that face pair any more.
    let tool = s.add_box(140.0, 30.0, 30.0).unwrap();
    let moved = s.add_translate(tool, -20.0, -10.0, 30.0).unwrap();
    let chopped = s.add_cut(base, moved).unwrap();
    recompute_ok(&mut s);

    let fillet = s.add_fillet(chopped, &edge, 3.0).unwrap();
    let report = s.recompute().unwrap();

    assert_eq!(report.failed, 1);
    assert_eq!(s.state(fillet).unwrap(), State::Failed);
    let message = s.object_error(fillet);
    assert!(
        message.contains("no longer exists"),
        "expected a message about the lost reference, got {message:?}"
    );
}

/// Cosmetic properties must not invalidate geometry. Getting this wrong in one direction
/// wastes work; getting it wrong in the other serves stale geometry.
#[test]
fn cosmetic_properties_do_not_invalidate_the_cache() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    s.set_text(b, "colour", "red").unwrap();
    s.set_cosmetic(b, "colour", true).unwrap();
    recompute_ok(&mut s);

    let key_before = s.cache_key(b).unwrap();
    let hash_before = s.content_hash(b);

    s.set_text(b, "colour", "blue").unwrap();
    s.set_cosmetic(b, "colour", true).unwrap();
    s.reset_cache_stats().unwrap();
    recompute_ok(&mut s);

    assert_eq!(
        s.cache_key(b).unwrap(),
        key_before,
        "a cosmetic change must not alter the recompute key"
    );
    assert_eq!(s.content_hash(b), hash_before);

    // A non-cosmetic change to the same property must invalidate.
    s.set_text(b, "colour", "green").unwrap();
    s.set_cosmetic(b, "colour", false).unwrap();
    recompute_ok(&mut s);
    assert_ne!(s.cache_key(b).unwrap(), key_before);
}

/// Two documents that describe the same geometry by different routes must share cache
/// entries. This is the assetlib rule — dependencies are recorded by content, not identity.
#[test]
fn identical_geometry_shares_cache_entries_across_object_ids() {
    let mut s = session();

    let first = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let first_key = s.cache_key(first).unwrap();

    // A different object, different id, same parameters.
    let second = s.add_box(100.0, 60.0, 40.0).unwrap();
    s.reset_cache_stats().unwrap();
    recompute_ok(&mut s);

    assert_eq!(
        s.cache_key(second).unwrap(),
        first_key,
        "the cache key must be a function of content, not of object identity"
    );
    assert_eq!(
        s.cache_stats().unwrap().hits,
        1,
        "the second box must be served from cache, not recomputed"
    );
}

/// A cycle is a modelling mistake the user made, so it must produce a legible message
/// rather than a hang or a stack overflow.
#[test]
fn a_dependency_cycle_is_reported_not_hung() {
    let mut s = session();
    let a = s.add_box(10.0, 10.0, 10.0).unwrap();
    let b = s.add_translate(a, 1.0, 0.0, 0.0).unwrap();
    // Close the loop: a now consumes b.
    s.set_input(a, "loop", b).unwrap();

    let err = s.recompute().expect_err("a cycle must be an error");
    assert_eq!(err.code, ErrorCode::InvalidInput);
    assert!(
        err.message.contains("loop"),
        "expected a message naming the loop, got {:?}",
        err.message
    );
}

/// Deleting an object that others depend on must surface as a failure on the dependents,
/// never as a silently dropped reference.
#[test]
fn deleting_an_input_fails_its_dependents_visibly() {
    let mut s = session();
    let base = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let edge = s.box_edge_between(base, BoxFace::ZMax, BoxFace::YMin);
    let fillet = s.add_fillet(base, &edge, 5.0).unwrap();
    recompute_ok(&mut s);
    assert_eq!(s.state(fillet).unwrap(), State::Clean);

    s.remove(base).unwrap();
    let report = s.recompute().unwrap();

    assert!(!report.all_succeeded(), "the dangling fillet must not pass");
    assert_ne!(s.state(fillet).unwrap(), State::Clean);
}

/// Unit parsing is exposed through the ABI so bindings never reimplement it — a second
/// implementation is a second place for a 25.4x scaling bug to live.
#[test]
fn length_parsing_is_shared_with_the_core() {
    assert!((parse_length("10", UnitSystem::Millimetre).unwrap() - 10.0).abs() < 1e-9);
    assert!((parse_length("10", UnitSystem::Inch).unwrap() - 254.0).abs() < 1e-9);
    assert!((parse_length("1.5in", UnitSystem::Millimetre).unwrap() - 38.1).abs() < 1e-9);
    assert!((parse_length("2ft 6in", UnitSystem::Millimetre).unwrap() - 762.0).abs() < 1e-9);
    assert!(parse_length("10 furlongs", UnitSystem::Millimetre).is_err());
}
