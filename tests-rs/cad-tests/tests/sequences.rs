//! Multi-step permutation testing: random sequences of real editing actions, with invariants
//! checked after EVERY step.
//!
//! # Why sequences, when there are already unit tests
//!
//! Every bug that survived to be found late in this project was a sequence bug, not a single-call
//! bug. A blocked feature's error message was empty — but only after a `remove` followed by a
//! `recompute`. Host-built shapes were named non-deterministically — but only from the SECOND
//! call onward. A test harness raced on shared state — but only when tests interleaved. None of
//! those is reachable by calling one function and checking what it returned.
//!
//! Professionals will keep a day's work in one of these documents. What matters is not that any
//! single operation is correct, but that no ORDER of correct operations leaves the document in a
//! state that lies about itself.
//!
//! # What is asserted, and what deliberately is not
//!
//! Not outcomes. A random sequence has no expected result, and asserting one would mean
//! reimplementing the modeller inside the test. What is asserted are INVARIANTS — properties that
//! must hold after every step whatever the sequence did:
//!
//!   1. A feature that failed says why. An empty message is indistinguishable from a crash.
//!   2. A feature that claims success has a valid shape, and a finite non-negative volume.
//!   3. Undo to the bottom and redo to the top returns the document it started from.
//!   4. Save and reopen preserves what was there.
//!
//! Failure is a legitimate outcome for any individual action — a fillet too large, a cut that
//! removes everything. What is never legitimate is a document that reports Clean over geometry it
//! cannot produce, or reports Failed without saying what failed.
//!
//! # Running a real campaign
//!
//! The default case count is deliberately modest so this stays usable as a pre-commit gate. To
//! actually hunt, raise it:
//!
//!     PROPTEST_CASES=1200 cargo test -p cad-tests --release --test sequences
//!
//! That is 4,800 sequences in about two minutes. `the_generator_reaches_interesting_states` at the
//! bottom is the check on the check: it reports what the campaign actually reached, and fails if
//! the generator stops producing failures or geometry — because a green suite over empty documents
//! proves nothing.
//!
//! # Shrinking is the point
//!
//! proptest reduces a 40-step failure to the two or three steps that actually matter. That is the
//! difference between a report someone can act on and a log nobody reads.

use cad::*;
use proptest::prelude::*;
use std::sync::atomic::{AtomicU64, Ordering};

// ── the actions a user can take ─────────────────────────────────────────────────────────

/// One editing action.
///
/// Object references are INDICES, reduced modulo the live list when applied, rather than ids.
/// Generating raw ids would produce sequences that are almost entirely rejected, and a suite that
/// mostly tests the rejection path looks busy while covering nothing.
#[derive(Debug, Clone)]
enum Action {
    AddBox(u16, u16, u16),
    AddCylinder(u16, u16),
    Translate(usize, i16, i16, i16),
    Cut(usize, usize),
    Fillet(usize, u16),
    /// Retypes a dimension on an existing feature — the single most common real edit.
    SetLength(usize, u8, u16),
    Remove(usize),
    Undo,
    Redo,
    Recompute,
    /// Suspends everything after a feature. A mode, and modes are where state machines break.
    Rollback(Option<usize>),
    SaveReopen,
}

fn action_strategy() -> impl Strategy<Value = Action> {
    // Weighted towards building and editing, because a sequence that is mostly undo never gets
    // deep enough to find anything. Recompute is frequent: it is what makes state observable.
    prop_oneof![
        4 => (1u16..80, 1u16..80, 1u16..80).prop_map(|(x, y, z)| Action::AddBox(x, y, z)),
        2 => (1u16..40, 1u16..80).prop_map(|(r, h)| Action::AddCylinder(r, h)),
        2 => (any::<usize>(), -50i16..50, -50i16..50, -50i16..50)
            .prop_map(|(o, x, y, z)| Action::Translate(o, x, y, z)),
        2 => (any::<usize>(), any::<usize>()).prop_map(|(a, b)| Action::Cut(a, b)),
        2 => (any::<usize>(), 1u16..30).prop_map(|(o, r)| Action::Fillet(o, r)),
        4 => (any::<usize>(), any::<u8>(), 1u16..90)
            .prop_map(|(o, p, v)| Action::SetLength(o, p, v)),
        1 => any::<usize>().prop_map(Action::Remove),
        2 => Just(Action::Undo),
        1 => Just(Action::Redo),
        4 => Just(Action::Recompute),
        1 => proptest::option::of(any::<usize>()).prop_map(Action::Rollback),
        1 => Just(Action::SaveReopen),
    ]
}

// ── the world under test ────────────────────────────────────────────────────────────────

static SEQUENCE: AtomicU64 = AtomicU64::new(0);

struct World {
    s: Session,
    /// Every object this sequence created. Some no longer exist — removed, or undone away — and
    /// the invariant check skips those rather than treating absence as a failure.
    created: Vec<Object>,
    path: std::path::PathBuf,
}

impl World {
    fn new() -> Self {
        let n = SEQUENCE.fetch_add(1, Ordering::SeqCst);
        World {
            s: Session::new().expect("session"),
            created: Vec::new(),
            path: std::env::temp_dir().join(format!("cad-seq-{}-{n}.vcad", std::process::id())),
        }
    }

    /// The object at `index`, wrapped around the live list. None when nothing has been created.
    fn pick(&self, index: usize) -> Option<Object> {
        if self.created.is_empty() {
            return None;
        }
        Some(self.created[index % self.created.len()])
    }

    /// Applies one action. Every failure is swallowed on purpose: a fillet that cannot be built
    /// and a cut that removes everything are legitimate outcomes, and the invariants below are
    /// what says whether the document survived them honestly.
    fn apply(&mut self, action: &Action) {
        match action {
            Action::AddBox(x, y, z) => {
                if let Ok(o) = self.s.add_box(*x as f64, *y as f64, *z as f64) {
                    self.created.push(o);
                }
            }
            Action::AddCylinder(r, h) => {
                if let Ok(o) = self.s.add_cylinder(*r as f64, *h as f64) {
                    self.created.push(o);
                }
            }
            Action::Translate(i, x, y, z) => {
                if let Some(base) = self.pick(*i) {
                    if let Ok(o) =
                        self.s.add_translate(base, *x as f64, *y as f64, *z as f64)
                    {
                        self.created.push(o);
                    }
                }
            }
            Action::Cut(a, b) => {
                if let (Some(base), Some(tool)) = (self.pick(*a), self.pick(*b)) {
                    if base != tool {
                        if let Ok(o) = self.s.add_cut(base, tool) {
                            self.created.push(o);
                        }
                    }
                }
            }
            Action::Fillet(i, r) => {
                if let Some(base) = self.pick(*i) {
                    let edge = self.s.box_edge_between(base, BoxFace::ZMax, BoxFace::YMin);
                    if !edge.is_empty() {
                        if let Ok(o) = self.s.add_fillet(base, &edge, *r as f64) {
                            self.created.push(o);
                        }
                    }
                }
            }
            Action::SetLength(i, which, v) => {
                if let Some(o) = self.pick(*i) {
                    // Whatever property this feature happens to have. A miss is refused by the
                    // core, which is itself worth exercising: the shell will send exactly this
                    // when a user edits a field that a retyped feature no longer has.
                    let prop = ["dx", "dy", "dz", "radius", "height", "distance"]
                        [*which as usize % 6];
                    let _ = self.s.set_length(o, prop, *v as f64);
                }
            }
            Action::Remove(i) => {
                if let Some(o) = self.pick(*i) {
                    let _ = self.s.remove(o);
                }
            }
            Action::Undo => {
                let _ = self.s.undo();
            }
            Action::Redo => {
                let _ = self.s.redo();
            }
            Action::Recompute => {
                // The call itself must not error. Individual features failing is normal and is
                // reported in the counts; the recompute REFUSING to run is a core failure.
                if let Err(e) = self.s.recompute() {
                    assert!(
                        !e.message.trim().is_empty(),
                        "recompute failed with an empty message"
                    );
                }
            }
            Action::Rollback(i) => {
                let target = i.and_then(|i| self.pick(i));
                let _ = self.s.set_rollback(target);
            }
            Action::SaveReopen => {
                let path = self.path.to_str().expect("utf-8 temp path");
                if self.s.save(path).is_ok() {
                    // Reopened into the SAME session, which is what File > Open does. A fresh
                    // session would test less: this way the reopen has to survive replacing a
                    // document that is already loaded.
                    let _ = self.s.open(path);
                }
            }
        }
    }

    /// The invariants. Checked after every single step, so a violation names the step that caused
    /// it rather than the end of a forty-step sequence.
    fn check_invariants(&self, after: &Action, step: usize) {
        for (n, &o) in self.created.iter().enumerate() {
            // Gone — removed, or undone away. Not a failure; the model tracks what it created,
            // not what still exists.
            let Ok(state) = self.s.state(o) else { continue };

            match state {
                State::Failed | State::Blocked => {
                    let message = self.s.object_error(o);
                    assert!(
                        !message.trim().is_empty(),
                        "step {step} ({after:?}): object #{n} is {state:?} with an EMPTY message. \
                         A refusal the user cannot read is indistinguishable from a crash."
                    );
                }
                State::Clean => {
                    // Claims success, so everything downstream must agree. A Clean object with an
                    // invalid shape is the worst outcome available: it propagates into booleans,
                    // meshes and mass properties, and nothing downstream re-checks it.
                    let valid = self.s.is_valid_shape(o).unwrap_or(false);
                    assert!(
                        valid,
                        "step {step} ({after:?}): object #{n} is Clean but its shape is not valid"
                    );
                    // ASSERTED, not skipped when it fails. Written as `if let Ok(v)` this was an
                    // invariant that quietly turned itself off: a Clean object whose volume could
                    // not be computed passed without comment, which is precisely the "reports
                    // Clean over geometry it cannot produce" case this file exists to catch.
                    //
                    // A Clean feature has a valid solid -- the assertion above just established
                    // that -- so the volume query has no licence to fail. If a legitimate reason
                    // ever appears (a boolean reducing a solid to a shell, say), it belongs here
                    // as a named exception rather than as a silent skip.
                    let volume = self.s.volume(o);
                    assert!(
                        volume.is_ok(),
                        "step {step} ({after:?}): object #{n} is Clean and its shape is valid, \
                         but its volume could not be computed"
                    );
                    let v = volume.unwrap();
                    assert!(
                        v.is_finite() && v >= 0.0,
                        "step {step} ({after:?}): object #{n} is Clean with volume {v}"
                    );
                }
                _ => {}
            }
        }
    }
}

impl Drop for World {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

// ── the properties ──────────────────────────────────────────────────────────────────────

proptest! {
    // Modest case count: every step drives OCCT, and a suite slow enough to skip is a suite that
    // gets skipped. Sequences are long rather than numerous, because depth is where the bugs are.
    #![proptest_config(ProptestConfig {
        cases: 48,
        max_shrink_iters: 2048,
        ..ProptestConfig::default()
    })]

    /// No order of editing actions leaves the document lying about itself.
    #[test]
    fn no_sequence_of_edits_makes_the_document_dishonest(
        actions in prop::collection::vec(action_strategy(), 1..40)
    ) {
        let mut world = World::new();
        for (step, action) in actions.iter().enumerate() {
            world.apply(action);
            world.check_invariants(action, step);
        }
    }

    /// Undo to the bottom, then redo to the top, and the document is the one you started with.
    ///
    /// The digest is the document's own content hash, so this compares what the document IS rather
    /// than what it reports about itself. History that loses or duplicates a step shows up here
    /// and essentially nowhere else — a user only finds it after undoing past a mistake and
    /// redoing forward, by which point they cannot say what went wrong.
    #[test]
    fn undo_to_the_bottom_and_redo_to_the_top_round_trips(
        actions in prop::collection::vec(action_strategy(), 1..25)
    ) {
        let mut world = World::new();
        for action in &actions {
            world.apply(action);
        }
        let _ = world.s.recompute();
        let before = world.s.document_digest().expect("digest");

        let mut undos = 0;
        while world.s.undo().unwrap_or(false) {
            undos += 1;
            prop_assert!(undos < 500, "undo did not terminate");
        }

        // Exactly as many redos as undos, NOT "redo until it stops".
        //
        // The first version drained both, and proptest shrank it to two steps: AddBox, Undo. The
        // sequence itself had already consumed an undo, so draining redo climbed past the
        // starting point to the TOP of history — which is correct behaviour and a wrong
        // assertion. Redo goes to the top; it does not go back to where you were. The property
        // worth having is that the two are inverse over the same distance.
        for i in 0..undos {
            prop_assert!(
                world.s.redo().unwrap_or(false),
                "redo ran out after {i} of {undos} steps; undo and redo disagree about the \
                 history's depth"
            );
        }

        let after = world.s.document_digest().expect("digest");
        prop_assert_eq!(
            before, after,
            "undoing {} steps and redoing {} steps did not return the same document",
            undos, undos
        );
    }

    /// Saving and reopening preserves the document.
    ///
    /// This is the one with the worst consequence when it breaks: the user finds out after saving
    /// over the original. Compared by digest for the same reason as above.
    #[test]
    fn save_and_reopen_preserves_the_document(
        actions in prop::collection::vec(action_strategy(), 1..25)
    ) {
        let mut world = World::new();
        for action in &actions {
            world.apply(action);
        }
        let _ = world.s.recompute();

        let path = world.path.to_str().expect("utf-8 temp path").to_string();
        prop_assume!(world.s.save(&path).is_ok());
        let before = world.s.document_digest().expect("digest");
        let count_before = world.s.object_count().expect("count");

        let mut reopened = Session::new().expect("session");
        prop_assert!(reopened.open(&path).is_ok(), "a file we just wrote must reopen");

        prop_assert_eq!(
            reopened.object_count().expect("count"), count_before,
            "reopening lost or gained objects"
        );
        prop_assert_eq!(
            reopened.document_digest().expect("digest"), before,
            "reopening produced a different document"
        );
    }

    /// Recomputing twice in a row changes nothing.
    ///
    /// Cheap to state and it catches a whole class of bug: a feature that is not deterministic, a
    /// cache key that misses its own result, an invalidation that never settles. If this fails,
    /// the document's cost per edit is unbounded and nobody notices until a large part is slow.
    #[test]
    fn recompute_reaches_a_fixed_point(
        actions in prop::collection::vec(action_strategy(), 1..25)
    ) {
        let mut world = World::new();
        for action in &actions {
            world.apply(action);
        }

        let _ = world.s.recompute();
        let first = world.s.document_digest().expect("digest");
        let second_report = world.s.recompute();
        prop_assume!(second_report.is_ok());
        let second = world.s.document_digest().expect("digest");

        prop_assert_eq!(
            first, second,
            "a second recompute over an unchanged document changed it"
        );
        prop_assert_eq!(
            second_report.unwrap().computed, 0,
            "a second recompute recomputed features that were already clean"
        );
    }
}

// ── does this suite actually reach anything? ────────────────────────────────────────────

/// Runs a deterministic campaign and reports what states it reached.
///
/// A green property suite proves nothing if every sequence produced an empty document. This is
/// the check on the check: it fails when the generator stops reaching failures, blocked features
/// or real geometry, which is how a suite silently stops testing after an unrelated change to the
/// action weights or to the core's tolerances.
///
/// Deterministic on purpose — a coverage floor that itself flickers is worse than none.
#[test]
fn the_generator_reaches_interesting_states() {
    // xorshift64 rather than proptest's RNG: this needs to be reproducible run to run, and
    // proptest deliberately reseeds.
    let mut seed = 0x2545_F491_4F6C_DD1Du64;
    let mut next = move || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };

    let (mut clean, mut failed, mut blocked, mut created, mut checks) = (0u32, 0u32, 0u32, 0u32, 0u32);

    for _ in 0..120 {
        let mut world = World::new();
        for _ in 0..24 {
            let r = next();
            let action = match r % 12 {
                0 | 1 => Action::AddBox((r >> 8) as u16 % 60 + 1, (r >> 16) as u16 % 60 + 1,
                                        (r >> 24) as u16 % 60 + 1),
                2 => Action::AddCylinder((r >> 8) as u16 % 30 + 1, (r >> 16) as u16 % 60 + 1),
                3 => Action::Translate((r >> 8) as usize, 10, 0, 0),
                4 => Action::Cut((r >> 8) as usize, (r >> 24) as usize),
                5 => Action::Fillet((r >> 8) as usize, (r >> 24) as u16 % 25 + 1),
                6 | 7 => Action::SetLength((r >> 8) as usize, (r >> 20) as u8,
                                           (r >> 32) as u16 % 80 + 1),
                8 => Action::Remove((r >> 8) as usize),
                9 => Action::Undo,
                10 => Action::Recompute,
                _ => Action::SaveReopen,
            };
            world.apply(&action);
        }
        let _ = world.s.recompute();

        created += world.created.len() as u32;
        for &o in &world.created {
            let Ok(state) = world.s.state(o) else { continue };
            checks += 1;
            match state {
                State::Clean => clean += 1,
                State::Failed => failed += 1,
                State::Blocked => blocked += 1,
                _ => {}
            }
        }
    }

    println!(
        "campaign: {created} objects created, {checks} still present — \
         {clean} Clean, {failed} Failed, {blocked} Blocked"
    );

    // PROPORTIONS, not absolute counts. Absolute floors were correct against a fixed campaign but
    // lost their force the moment anyone grew it: `failed + blocked > 10` is trivially true over
    // ten thousand objects, so the check would stop meaning anything exactly when the campaign got
    // more thorough. Expressed as fractions, they hold whatever the campaign size becomes.
    assert!(created > 500, "the generator built almost nothing: {created} objects");
    assert!(
        clean > created / 4,
        "only {clean} of {created} objects computed successfully; the generator is producing \
         mostly rubble and the Clean invariant is barely exercised"
    );
    assert!(
        failed + blocked > created / 50,
        "only {failed} Failed and {blocked} Blocked out of {created} — the 'a failure says why' \
         invariant is barely exercised, so the suite is greener than it is strong"
    );
}
