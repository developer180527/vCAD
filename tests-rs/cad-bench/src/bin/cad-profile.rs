//! One workload, run hard, for a profiler to sample. `cad-profile <workload> [seconds]`
//!
//! Deliberately not the full report. A profile of `cad-perf` would be a profile of everything it
//! measures, and the interesting stacks would be a few percent each. This runs ONE thing in a
//! loop so the flame graph is almost entirely the thing under investigation.
//!
//! Setup is inside the timed loop here, unlike in the bench harness — because for `build` the
//! setup IS the workload, and for `solve` the profiler can tell the two apart by function, which
//! a wall-clock measurement cannot.

use cad_bench::workloads::{sketch_chain, translate_chain};
use std::time::{Duration, Instant};

fn main() {
    let mut args = std::env::args().skip(1);
    let workload = args.next().unwrap_or_else(|| "build".to_string());
    let seconds: u64 = args.next().and_then(|s| s.parse().ok()).unwrap_or(10);
    let budget = Duration::from_secs(seconds);

    eprintln!("profiling '{workload}' for {seconds}s");
    let start = Instant::now();
    let mut iterations = 0u64;

    match workload.as_str() {
        // The quadratic one: n add() calls, no recompute, no geometry.
        "build" => {
            while start.elapsed() < budget {
                let _ = translate_chain(512);
                iterations += 1;
            }
        }
        // The cubic one. n=128 is where it reaches ~0.9 s, so a handful of iterations already
        // gives a dense profile.
        "solve" => {
            while start.elapsed() < budget {
                let (mut s, sk) = sketch_chain(128);
                s.solve(sk).expect("solve");
                iterations += 1;
            }
        }
        other => {
            eprintln!("unknown workload '{other}'; expected 'build' or 'solve'");
            std::process::exit(2);
        }
    }

    eprintln!(
        "{iterations} iterations in {:.1}s",
        start.elapsed().as_secs_f64()
    );
}
