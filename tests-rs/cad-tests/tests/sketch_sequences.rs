//! Permutation testing for the sketcher: random sequences of drawing, constraining and solving,
//! with invariants checked after every step.
//!
//! # Why the sketcher specifically
//!
//! `sequences.rs` permutes DOCUMENT actions — add, cut, fillet, undo, rollback, save. It contains
//! no sketch actions at all, and the sketcher is where a professional spends most of their day.
//! It is also the part of this codebase with the most ways to be subtly wrong:
//!
//!   * a third-party solver (planegcs) whose convergence test is a floating-point comparison,
//!   * a revert-on-non-finite path that has to restore the sketch exactly as it was,
//!   * degrees of freedom and conflict reporting, which are the only signal a user gets about
//!     whether their sketch is finished,
//!   * a text serialisation that must round-trip through the saved document without drift.
//!
//! Every one of those is a *sequence* property. A single solve of a single line proves nothing.
//!
//! # The invariants
//!
//! 1. **No coordinate is ever non-finite.** This is the strongest one and the reason this file
//!    exists. A NaN reaching planegcs makes it report SOLVED over nonsense, because every
//!    comparison with NaN is false and its convergence test is a comparison. The core reverts a
//!    solve that produces one — this asserts the revert actually holds, over sequences rather
//!    than over the single case that motivated it.
//! 2. **A solve that fails says something.** Reporting neither success nor a reason leaves a user
//!    with a sketch that will not close and no way to learn why.
//! 3. **Degrees of freedom are never negative.** A negative DOF is arithmetic nonsense and means
//!    the constraint count has been double-applied somewhere.
//! 4. **Text round-trips.** Serialise and reparse must preserve geometry count, kinds, and
//!    coordinates. Sketches are stored as text in `.vpart`, so drift here is silent data loss.
//! 5. **Geometry is never lost by solving.** Solving changes coordinates; it must never change
//!    how many curves exist.
//!
//! What is deliberately NOT asserted is any particular solution. A random constraint set has no
//! expected geometry, and asserting one would mean reimplementing the solver in the test.
//! Over-constrained and conflicting sketches are legitimate outcomes — what is illegitimate is a
//! sketch that lies about its own state.

use cad::{Con, Geo, Plane, Pt, Session, Sketch};

/// One editing action. Indices are taken modulo what exists, so a generated sequence is always
/// applicable rather than mostly skipped.
#[derive(Debug, Clone)]
enum Act {
    Line(i16, i16, i16, i16),
    Circle(i16, i16, u16),
    Arc(i16, i16, u16, u16, u16),
    Horizontal(usize),
    Vertical(usize),
    Parallel(usize, usize),
    Perpendicular(usize, usize),
    EqualLength(usize, usize),
    Radius(usize, u16),
    Coincident(usize, usize),
    LockX(usize, i16),
    Distance(usize, usize, u16),
    Solve,
    RoundTrip,
}

struct World {
    s: Session,
    sk: Sketch,
    geo: Vec<Geo>,
    /// Kinds, so the round-trip check can compare structure and not just count.
    steps: usize,
}

impl World {
    fn new() -> Self {
        let mut s = Session::new().expect("session");
        let sk = s.new_sketch(Plane::Xy).expect("sketch");
        World { s, sk, geo: Vec::new(), steps: 0 }
    }

    fn pick(&self, i: usize) -> Option<Geo> {
        if self.geo.is_empty() { None } else { Some(self.geo[i % self.geo.len()]) }
    }

    fn apply(&mut self, a: &Act) {
        self.steps += 1;
        match a {
            Act::Line(x0, y0, x1, y1) => {
                let (a0, b0) = (f64::from(*x0) / 8.0, f64::from(*y0) / 8.0);
                let (a1, b1) = (f64::from(*x1) / 8.0, f64::from(*y1) / 8.0);
                // A zero-length line is refused by the core, and that refusal is not a test
                // failure — it is the core declining to create geometry it cannot constrain.
                if let Ok(g) = self.s.add_line(self.sk, (a0, b0), (a1, b1)) {
                    self.geo.push(g);
                }
            }
            Act::Circle(x, y, r) => {
                let r = f64::from(*r % 200 + 1) / 8.0;
                if let Ok(g) =
                    self.s.add_sketch_circle(self.sk, (f64::from(*x) / 8.0, f64::from(*y) / 8.0), r)
                {
                    self.geo.push(g);
                }
            }
            Act::Arc(x, y, r, a0, a1) => {
                let r = f64::from(*r % 200 + 1) / 8.0;
                let start = f64::from(*a0 % 360);
                let end = f64::from(*a1 % 360);
                if let Ok(g) = self.s.add_sketch_arc(
                    self.sk,
                    (f64::from(*x) / 8.0, f64::from(*y) / 8.0),
                    r,
                    start,
                    end,
                ) {
                    self.geo.push(g);
                }
            }
            Act::Horizontal(i) => self.con(self.pick(*i).map(Con::Horizontal)),
            Act::Vertical(i) => self.con(self.pick(*i).map(Con::Vertical)),
            Act::Parallel(i, j) => {
                let c = match (self.pick(*i), self.pick(*j)) {
                    (Some(a), Some(b)) if a != b => Some(Con::Parallel(a, b)),
                    _ => None,
                };
                self.con(c);
            }
            Act::Perpendicular(i, j) => {
                let c = match (self.pick(*i), self.pick(*j)) {
                    (Some(a), Some(b)) if a != b => Some(Con::Perpendicular(a, b)),
                    _ => None,
                };
                self.con(c);
            }
            Act::EqualLength(i, j) => {
                let c = match (self.pick(*i), self.pick(*j)) {
                    (Some(a), Some(b)) if a != b => Some(Con::EqualLength(a, b)),
                    _ => None,
                };
                self.con(c);
            }
            Act::Radius(i, r) => {
                let value = f64::from(*r % 200 + 1) / 8.0;
                self.con(self.pick(*i).map(|g| Con::Radius(g, value)));
            }
            Act::Coincident(i, j) => {
                let c = match (self.pick(*i), self.pick(*j)) {
                    (Some(a), Some(b)) if a != b => {
                        Some(Con::Coincident(a, Pt::End, b, Pt::Start))
                    }
                    _ => None,
                };
                self.con(c);
            }
            Act::LockX(i, v) => {
                let value = f64::from(*v) / 8.0;
                self.con(self.pick(*i).map(|g| Con::LockX(g, Pt::Start, value)));
            }
            Act::Distance(i, j, d) => {
                let value = f64::from(*d % 400 + 1) / 8.0;
                let c = match (self.pick(*i), self.pick(*j)) {
                    (Some(a), Some(b)) if a != b => {
                        Some(Con::Distance(a, Pt::Start, b, Pt::Start, value))
                    }
                    _ => None,
                };
                self.con(c);
            }
            Act::Solve => {
                let before = self.geometry_len();
                let report = self.s.solve(self.sk);

                if let Ok(r) = report {
                    // INVARIANT 3. A negative DOF is arithmetic nonsense: it means constraints
                    // were counted twice, and the number a user reads to know whether a sketch is
                    // finished would be meaningless.
                    assert!(
                        r.dofs >= 0,
                        "step {}: solve reported {} degrees of freedom",
                        self.steps,
                        r.dofs
                    );

                    // INVARIANT 2. Not solved, so it must account for itself: conflicts,
                    // redundancy, or remaining freedom. Silence here is a sketch that will not
                    // close with nothing to tell the user why.
                    if r.solved == 0 {
                        assert!(
                            r.conflicting > 0 || r.redundant > 0 || r.dofs > 0,
                            "step {}: solve failed but reported no conflicts, no redundancy and \
                             zero degrees of freedom — the user is told nothing",
                            self.steps
                        );
                    }
                }

                // INVARIANT 5. Solving moves points; it must never lose a curve.
                assert_eq!(
                    self.geometry_len(),
                    before,
                    "step {}: solving changed the number of curves",
                    self.steps
                );
            }
            Act::RoundTrip => {
                // INVARIANT 4. Sketches live in `.vpart` as text, so any drift here is silent
                // data loss on save and reopen.
                let text = self.s.sketch_text(self.sk);
                if text.is_empty() {
                    return;
                }
                let before = self.s.sketch_geometry(self.sk).unwrap_or_default();
                let reparsed = match self.s.sketch_from_text(&text) {
                    Ok(sk) => sk,
                    Err(e) => panic!("step {}: a sketch we just serialised will not parse: {e:?}",
                                     self.steps),
                };
                let after = self.s.sketch_geometry(reparsed).unwrap_or_default();
                assert_eq!(
                    before.len(),
                    after.len(),
                    "step {}: round trip changed the geometry count",
                    self.steps
                );
                for (n, (a, b)) in before.iter().zip(after.iter()).enumerate() {
                    assert_eq!(a.kind, b.kind, "step {}: curve {n} changed kind", self.steps);
                    for k in 0..5 {
                        assert!(
                            (a.p[k] - b.p[k]).abs() < 1e-9,
                            "step {}: curve {n} coordinate {k} drifted {} -> {}",
                            self.steps,
                            a.p[k],
                            b.p[k]
                        );
                    }
                }
                self.s.release_sketch(reparsed);
            }
        }

        self.check_finite(a);
    }

    fn con(&mut self, c: Option<Con>) {
        // A refused constraint is a legitimate outcome — over-constrained, or referring to a point
        // a curve does not have. What matters is what the sketch looks like afterwards.
        if let Some(c) = c {
            let _ = self.s.constrain(self.sk, c);
        }
    }

    fn geometry_len(&self) -> usize {
        self.s.sketch_geometry(self.sk).map(|g| g.len()).unwrap_or(0)
    }

    /// INVARIANT 1, checked after EVERY action.
    ///
    /// The one that matters most. A NaN reaching planegcs makes it report SOLVED over nonsense,
    /// because its convergence test is a comparison and every comparison with NaN is false. From
    /// there the NaN spreads to the DXF export, the saved document and any extrude built on the
    /// profile, each failing far from the cause.
    fn check_finite(&self, after: &Act) {
        let Ok(geometry) = self.s.sketch_geometry(self.sk) else { return };
        for (n, g) in geometry.iter().enumerate() {
            for (k, v) in g.p.iter().enumerate() {
                assert!(
                    v.is_finite(),
                    "step {} ({after:?}): curve {n} coordinate {k} is {v} — a non-finite value in \
                     the sketch means the solver reported success over nonsense, and it will \
                     reach the document, the export and every feature built on this profile",
                    self.steps
                );
            }
        }
    }
}

// ── the campaign ────────────────────────────────────────────────────────────────────────

/// Deterministic xorshift64, so a failure is reproducible from the seed printed below.
fn campaign(seed: u64, sequences: usize, depth: usize) -> (u32, u32, u32) {
    let mut state = seed;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };

    let (mut curves, mut solves, mut unsolved) = (0u32, 0u32, 0u32);

    for _ in 0..sequences {
        let mut world = World::new();
        for _ in 0..depth {
            let r = next();
            let act = match r % 16 {
                0 | 1 | 2 => Act::Line(
                    (r >> 8) as i16 % 200,
                    (r >> 20) as i16 % 200,
                    (r >> 32) as i16 % 200,
                    (r >> 44) as i16 % 200,
                ),
                3 => Act::Circle((r >> 8) as i16 % 200, (r >> 20) as i16 % 200, (r >> 32) as u16),
                4 => Act::Arc(
                    (r >> 8) as i16 % 200,
                    (r >> 20) as i16 % 200,
                    (r >> 32) as u16,
                    (r >> 40) as u16,
                    (r >> 48) as u16,
                ),
                5 => Act::Horizontal((r >> 8) as usize),
                6 => Act::Vertical((r >> 8) as usize),
                7 => Act::Parallel((r >> 8) as usize, (r >> 24) as usize),
                8 => Act::Perpendicular((r >> 8) as usize, (r >> 24) as usize),
                9 => Act::EqualLength((r >> 8) as usize, (r >> 24) as usize),
                10 => Act::Radius((r >> 8) as usize, (r >> 24) as u16),
                11 => Act::Coincident((r >> 8) as usize, (r >> 24) as usize),
                12 => Act::LockX((r >> 8) as usize, (r >> 24) as i16 % 200),
                13 => Act::Distance((r >> 8) as usize, (r >> 24) as usize, (r >> 40) as u16),
                14 => Act::RoundTrip,
                _ => Act::Solve,
            };

            if matches!(act, Act::Solve) {
                solves += 1;
                if let Ok(r) = world.s.solve(world.sk) {
                    if r.solved == 0 {
                        unsolved += 1;
                    }
                }
            }
            world.apply(&act);
        }
        curves += world.geometry_len() as u32;
    }

    (curves, solves, unsolved)
}

#[test]
fn no_sequence_of_sketch_edits_produces_a_non_finite_coordinate() {
    let (curves, solves, _) = campaign(0x9E37_79B9_7F4A_7C15, 60, 40);
    assert!(curves > 200, "the generator drew almost nothing: {curves} curves");
    assert!(solves > 50, "the generator barely solved: {solves} solves");
}

#[test]
fn a_second_seed_finds_the_same_answer() {
    // A different walk through the same state space. One seed passing is one sample; two
    // independent seeds passing is weak evidence the property holds rather than that the first
    // seed happened to avoid the bug.
    let (curves, _, _) = campaign(0x2545_F491_4F6C_DD1D, 60, 40);
    assert!(curves > 200, "the generator drew almost nothing: {curves} curves");
}

#[test]
fn the_generator_reaches_unsolved_sketches() {
    // The check on the check. If every sketch this campaign produced solved cleanly, then
    // invariant 2 — "a solve that fails says something" — was never once exercised, and the suite
    // would be green because it asked nothing rather than because the sketcher is correct.
    let (curves, solves, unsolved) = campaign(0x1234_5678_9ABC_DEF1, 60, 40);
    println!("sketch campaign: {curves} curves, {solves} solves, {unsolved} unsolved");

    assert!(
        unsolved > solves / 50,
        "only {unsolved} of {solves} solves failed to converge — the 'a failed solve says why' \
         invariant is barely being exercised"
    );
}
