//! Measurement harness for the CAD core, driven through the C ABI.
//!
//! # Why this exists, and what it refuses to do
//!
//! Every performance claim this project has made was wrong at least once, and each time the
//! failure was the same shape: a number that agreed with itself. The renderer reported 100k
//! instances submitted while drawing one transform a hundred thousand times. A spike asserted a
//! no-op update cost under 25% of a build, then failed the moment the build got fast. A pixel
//! floor of "at least a tenth of the frame is lit" passed on a frame containing one box.
//!
//! So this harness holds two rules.
//!
//! **1. Assert on complexity, not on wall-clock.** A threshold in milliseconds encodes the
//! machine it was written on. It fails on a slow CI box, passes on a fast one while an algorithm
//! silently goes quadratic, and gets bumped rather than investigated. `Scaling` instead measures
//! a workload at several sizes and checks the GROWTH, which is a property of the code. Absolute
//! times are reported for humans and asserted on by nothing.
//!
//! **2. Prefer a counter the work cannot fake.** Where the core exposes one — cache hits, mesh
//! uploads, recompute counts — assert on that, because "the second tessellation was a cache hit"
//! is not a claim about speed and cannot be satisfied by a fast machine. Timing is the fallback
//! for work that exposes no such counter, never the first choice.

pub mod workloads;

use std::time::{Duration, Instant};

/// Timings for one workload at one size, with the outliers kept rather than hidden.
#[derive(Debug, Clone)]
pub struct Sample {
    pub label: String,
    pub n: usize,
    pub runs: Vec<Duration>,
}

impl Sample {
    pub fn median(&self) -> Duration {
        let mut v = self.runs.clone();
        v.sort();
        v[v.len() / 2]
    }

    /// The slowest run, reported rather than trimmed. A hitch is a real user-visible event —
    /// an OCCT healing pass, a cache spill — and averaging it away is how a stutter becomes
    /// invisible to the suite that is supposed to catch it.
    pub fn worst(&self) -> Duration {
        *self.runs.iter().max().expect("a sample has at least one run")
    }

    pub fn ms(&self) -> f64 {
        self.median().as_secs_f64() * 1000.0
    }
}

/// Runs `body` `runs` times and keeps every timing.
///
/// One warm-up pass first, discarded: the first call through any path pays for lazy statics, the
/// DDC opening its store, and OCCT's own one-off setup. Including it measures startup and calls
/// it throughput.
pub fn measure<F>(label: &str, n: usize, runs: usize, mut body: F) -> Sample
where
    F: FnMut(usize),
{
    body(n);
    let mut timings = Vec::with_capacity(runs);
    for _ in 0..runs {
        let t0 = Instant::now();
        body(n);
        timings.push(t0.elapsed());
    }
    Sample { label: label.to_string(), n, runs: timings }
}

/// Times only `body`, with `setup` run outside the clock before each run.
///
/// The reason this exists: the first version of the report timed the workload builder along with
/// the operation, so "full recompute of a chain of 512" was really "build 512 features AND
/// recompute them" — and since building is itself superlinear, the recompute row inherited a
/// growth curve that was not recompute's. A measurement that includes its own setup is measuring
/// the setup, which is the whole failure mode this harness was written against.
pub fn measure_prepared<S, T, F>(label: &str, n: usize, runs: usize, mut setup: S, mut body: F)
    -> Sample
where
    S: FnMut(usize) -> T,
    F: FnMut(&mut T),
{
    let mut warm = setup(n);
    body(&mut warm);

    let mut timings = Vec::with_capacity(runs);
    for _ in 0..runs {
        let mut state = setup(n);
        let t0 = Instant::now();
        body(&mut state);
        timings.push(t0.elapsed());
    }
    Sample { label: label.to_string(), n, runs: timings }
}

/// How a workload's cost grows with its size.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Growth {
    /// Cost independent of n. A no-op update, a cache hit.
    Constant,
    /// Cost proportional to n. Recompute over a chain, scene build over placements.
    Linear,
    /// Between linear and quadratic — n log n sorts, spatial builds.
    Loglinear,
}

impl Growth {
    /// The largest ratio of cost between n and 2n that still counts as this class.
    ///
    /// Deliberately loose. These are wall-clock measurements on a machine doing other things,
    /// and the bug worth catching is a class change — linear to quadratic doubles to 4x and
    /// keeps going — not a 30% regression, which timing noise cannot distinguish anyway. A tight
    /// bound here would produce a flaky suite, and a flaky suite gets disabled.
    fn max_doubling_ratio(self) -> f64 {
        match self {
            Growth::Constant => 1.8,
            Growth::Linear => 3.0,
            Growth::Loglinear => 3.6,
        }
    }
}

/// A workload measured at doubling sizes, with the growth between them.
pub struct Scaling {
    pub label: String,
    pub samples: Vec<Sample>,
}

impl Scaling {
    /// Measures `body` at each size in `sizes`.
    pub fn measure<F>(label: &str, sizes: &[usize], runs: usize, mut body: F) -> Self
    where
        F: FnMut(usize),
    {
        let samples = sizes
            .iter()
            .map(|&n| measure(label, n, runs, &mut body))
            .collect();
        Scaling { label: label.to_string(), samples }
    }

    /// Measures at each size with the state built outside the clock. See [`measure_prepared`].
    pub fn measure_prepared<S, T, F>(label: &str, sizes: &[usize], runs: usize,
                                     mut setup: S, mut body: F) -> Self
    where
        S: FnMut(usize) -> T,
        F: FnMut(&mut T),
    {
        let samples = sizes
            .iter()
            .map(|&n| measure_prepared(label, n, runs, &mut setup, &mut body))
            .collect();
        Scaling { label: label.to_string(), samples }
    }

    /// Observed cost ratios between consecutive sizes, normalised to a doubling.
    ///
    /// Normalised because sizes are not always exact doublings: a step from 1000 to 4000 is two
    /// doublings, so its ratio is square-rooted before comparison. Without this the assertion
    /// would depend on how the sizes were chosen rather than on the code.
    pub fn doubling_ratios(&self) -> Vec<f64> {
        self.samples
            .windows(2)
            .map(|w| {
                let (a, b) = (&w[0], &w[1]);
                let cost = b.ms() / a.ms().max(f64::MIN_POSITIVE);
                let doublings = (b.n as f64 / a.n as f64).log2().max(1.0);
                cost.powf(1.0 / doublings)
            })
            .collect()
    }

    /// Fails when the growth is worse than `expected`.
    ///
    /// Skipped entirely when the smallest sample is under `floor_ms`. Below roughly a
    /// millisecond a "ratio" is comparing two piles of timer noise, and asserting on it produces
    /// exactly the flaky failure that teaches people to ignore this suite. Reporting that it was
    /// too fast to judge is the honest outcome.
    pub fn assert_growth(&self, expected: Growth, floor_ms: f64) {
        let smallest = self.samples.first().expect("a scaling has samples").ms();
        if smallest < floor_ms {
            println!(
                "  {} — too fast to judge growth ({:.3} ms at n={}), not asserted",
                self.label,
                smallest,
                self.samples[0].n
            );
            return;
        }
        let limit = expected.max_doubling_ratio();
        for (i, ratio) in self.doubling_ratios().iter().enumerate() {
            assert!(
                *ratio <= limit,
                "{}: cost grew {:.2}x per doubling between n={} ({:.2} ms) and n={} ({:.2} ms), \
                 which is worse than {:?} (limit {:.1}x). That is a complexity change, not noise.",
                self.label,
                ratio,
                self.samples[i].n,
                self.samples[i].ms(),
                self.samples[i + 1].n,
                self.samples[i + 1].ms(),
                expected,
                limit,
            );
        }
    }

    /// One row per size, plus the growth between them. Printed by the report binary and by the
    /// tests, so a failure carries the numbers that produced it.
    pub fn report(&self) {
        println!("  {}", self.label);
        let ratios = self.doubling_ratios();
        for (i, s) in self.samples.iter().enumerate() {
            let growth = if i == 0 {
                String::from("     —")
            } else {
                format!("{:5.2}x", ratios[i - 1])
            };
            println!(
                "    n={:<8} {:9.3} ms   worst {:9.3} ms   {}/doubling",
                s.n,
                s.ms(),
                s.worst().as_secs_f64() * 1000.0,
                growth
            );
        }
    }
}

/// Prints a section heading, so the report reads as a document rather than a log.
pub fn heading(text: &str) {
    println!("\n{}\n{}", text, "─".repeat(text.len()));
}
