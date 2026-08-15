//! Fuzzing the DXF importer: mutated and truncated files, and the rule that it must refuse them.
//!
//! # Why the importer and nothing else
//!
//! This is the one place in vCAD where **an attacker controls the input**. Every other surface
//! takes values from our own UI or our own saved files; an importer takes bytes from a stranger,
//! and importers are the largest CVE source in this industry. A parser that trusts its input is
//! a remote code execution waiting for a colleague to email a part file.
//!
//! # The contract being fuzzed
//!
//! For ANY sequence of bytes, `import_dxf` must:
//!
//!   1. **not crash** — no segfault, no abort, no uncaught exception crossing the C boundary;
//!   2. **not hang** — it must terminate, and quickly enough that a UI is not wedged;
//!   3. **not allocate unboundedly** — a corrupt count field must not become a gigabyte;
//!   4. **either import something sensible, or return an error.**
//!
//! Note what is NOT required: that a mutated file fails. Many mutations produce a *valid* DXF that
//! simply describes different geometry, and demanding failure would be asserting that our parser is
//! bad at its job. The requirement is that whatever it decides, it decides safely.
//!
//! # Stable Rust, no cargo-fuzz
//!
//! `cargo-fuzz` needs nightly and a separate build; this runs in the ordinary suite on every
//! platform. That trades coverage-guided exploration for reach, which is the right trade for a
//! first harness: the bugs a naive mutator finds in a parser that has never been fuzzed are the
//! shallow ones, and those are the ones that ship.
//!
//! In-process is also a deliberate limit worth naming: a true segfault takes the test process with
//! it. That IS the failure being reported — cargo shows the signal — but there is no shrinking and
//! no per-case attribution, so the harness prints each case's seed before running it. When the
//! process dies, the last line printed is the input that killed it.

use cad::*;
use std::io::Write;
use std::time::{Duration, Instant};

/// A valid DXF, as the seed corpus. Read from the repository fixture so the corpus and the
/// importer's own acceptance tests cannot drift apart.
fn seed() -> Vec<u8> {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../tests/data/sketch_profile.dxf"
    );
    std::fs::read(path).unwrap_or_else(|e| panic!("seed corpus missing at {path}: {e}"))
}

/// A second seed, built rather than stored: the minimum DXF the importer accepts.
///
/// Hand-written because mutating a large file mostly produces garbage far from the parser's
/// interesting paths, while mutating a small one lands on structure — section markers, group
/// codes, counts — which is where a parser breaks.
fn minimal_seed() -> Vec<u8> {
    let mut out = String::new();
    out.push_str("0\nSECTION\n2\nENTITIES\n");
    out.push_str("0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n10\n");
    out.push_str("0\nCIRCLE\n8\n0\n10\n5\n20\n5\n40\n2.5\n");
    out.push_str("0\nENDSEC\n0\nEOF\n");
    out.into_bytes()
}

/// xorshift64. Deterministic, so a failure reported by seed is reproducible forever — which a
/// fuzzer whose findings cannot be replayed is not worth much.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            (self.next() % n as u64) as usize
        }
    }
}

/// One mutation of `input`. The kinds are chosen for what breaks PARSERS rather than for variety.
fn mutate(rng: &mut Rng, input: &[u8]) -> Vec<u8> {
    let mut out = input.to_vec();
    if out.is_empty() {
        return out;
    }
    match rng.below(7) {
        // Flip bits. The classic, and what turns "10" into a group code nobody handles.
        0 => {
            for _ in 0..(1 + rng.below(8)) {
                let i = rng.below(out.len());
                out[i] ^= 1u8 << rng.below(8);
            }
        }
        // TRUNCATE. The single most productive DXF mutation: the format is a stream of paired
        // lines, so cutting anywhere leaves a group code whose value never arrives. A parser that
        // reads the pair without checking for end-of-input walks off the end.
        1 => {
            let n = rng.below(out.len());
            out.truncate(n);
        }
        // Splice out a run, so the structure stays plausible while the content does not.
        2 => {
            let start = rng.below(out.len());
            let len = rng.below(out.len() - start);
            out.drain(start..start + len);
        }
        // Duplicate a run. Repeated section markers, repeated vertex counts.
        3 => {
            let start = rng.below(out.len());
            let len = 1 + rng.below((out.len() - start).max(1));
            let slice = out[start..start + len].to_vec();
            let at = rng.below(out.len());
            out.splice(at..at, slice);
        }
        // Absurd numbers where a count or a coordinate goes. This is the unbounded-allocation
        // probe: LWPOLYLINE carries a vertex count in group 90, and a parser that reserves what
        // the file claims before reading it will try to allocate four billion vertices.
        4 => {
            let payloads: [&[u8]; 6] = [
                b"999999999999999999999",
                b"-999999999999999999999",
                b"1e309",
                b"nan",
                b"inf",
                b"0x7fffffff",
            ];
            let payload = payloads[rng.below(payloads.len())];
            let at = rng.below(out.len());
            out.splice(at..at, payload.iter().copied());
        }
        // Interior NUL and other bytes a text parser may treat as a terminator.
        5 => {
            let i = rng.below(out.len());
            out[i] = [0u8, b'\n', b'\r', 0xFF, 0x1A][rng.below(5)];
        }
        // Replace an entity name, so dispatch sees something it has no case for.
        _ => {
            if let Some(pos) = find(&out, b"LINE") {
                out.splice(pos..pos + 4, b"ZZZZ".iter().copied());
            } else {
                let i = rng.below(out.len());
                out[i] = b'Z';
            }
        }
    }
    out
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// Imports one candidate and enforces the contract. Returns whether it parsed.
fn import_and_check(bytes: &[u8], label: &str) -> bool {
    let path = std::env::temp_dir().join(format!(
        "cad-fuzz-{}-{}.dxf",
        std::process::id(),
        label.replace('/', "_")
    ));
    {
        let mut f = std::fs::File::create(&path).expect("temp file");
        f.write_all(bytes).expect("write");
    }

    // Printed BEFORE the call. In-process fuzzing cannot survive a segfault, so when the process
    // dies the last line on stdout is the input that killed it. Without this a crash reports only
    // "signal 11" and the case is lost.
    // FLUSHED, not just printed. println! is block-buffered to a pipe, so on a process abort the
    // buffer dies with the process and the case that killed it is lost — which is exactly when the
    // line is needed. Costs a syscall per case and buys the only attribution in-process fuzzing
    // gets. Run with --nocapture, or cargo swallows it too.
    println!("fuzz case {label} ({} bytes)", bytes.len());
    let _ = std::io::stdout().flush();

    let started = Instant::now();
    let mut s = Session::new().expect("session");
    let result = s.import_dxf(path.to_str().expect("utf-8 path"), Plane::Xy, 1.0);
    let elapsed = started.elapsed();

    // 2. Must not hang. A real hang wedges the suite and shows up as a CI timeout; this catches
    //    the near-misses — a quadratic blow-up on a duplicated section, say — while they are still
    //    only slow, which is when they are cheap to fix.
    assert!(
        elapsed < Duration::from_secs(5),
        "case {label}: import took {elapsed:?} on {} bytes; a corrupt file must not wedge the UI",
        bytes.len()
    );

    let parsed = match result {
        Ok(sk) => {
            // 3. Must not allocate unboundedly. The seeds hold a handful of entities, and no
            //    mutation of them describes a million — so a count far past the input's own size
            //    means a length field was believed rather than checked.
            let geometry = s.sketch_geometry(sk).unwrap_or_default();
            assert!(
                geometry.len() < 100_000,
                "case {label}: {} bytes produced {} geometries; a count field was trusted",
                bytes.len(),
                geometry.len()
            );
            // 4a. What it did import must be finite. A NaN coordinate reaching the solver makes
            //     it report SOLVED over nonsense (every comparison with NaN is false).
            {
                for (i, g) in geometry.iter().enumerate() {
                    for (j, v) in g.p.iter().enumerate() {
                        assert!(
                            v.is_finite(),
                            "case {label}: imported geometry {i} parameter {j} is {v} — a \
                             non-finite coordinate from a file is a NaN in the solver later"
                        );
                    }
                }
            }
            s.release_sketch(sk);
            true
        }
        // 4b. A refusal is a perfectly good outcome, provided it says something.
        Err(e) => {
            assert!(
                !e.message.trim().is_empty(),
                "case {label}: the import failed with an empty message"
            );
            false
        }
    };

    let _ = std::fs::remove_file(&path);
    parsed
}

// ── the campaign ────────────────────────────────────────────────────────────────────────

/// Mutations of the repository's own DXF fixture.
#[test]
fn mutated_dxf_files_are_refused_or_imported_safely() {
    let seeds = [("fixture", seed()), ("minimal", minimal_seed())];
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    let mut parsed = 0;

    for (name, base) in &seeds {
        for case in 0..300 {
            let candidate = mutate(&mut rng, base);
            if import_and_check(&candidate, &format!("{name}/{case}")) {
                parsed += 1;
            }
        }
    }

    // Coverage floor, in the spirit of the sequence suite's meta-test: if every mutant were
    // rejected at the first byte, this file would be asserting almost nothing about the parser
    // and would still be green. Some mutants must get far enough in to produce geometry.
    assert!(
        parsed > 30,
        "only {parsed} of 600 mutants imported at all; the mutator is producing garbage that \
         never reaches the parser, so this campaign proves less than it appears to"
    );
    println!("{parsed} of 600 mutants imported; the rest were refused");
}

/// Truncation at EVERY offset of a small file.
///
/// Exhaustive rather than random, because truncation is the mutation DXF is most vulnerable to —
/// the format is a stream of code/value line pairs, so cutting anywhere leaves a code whose value
/// never arrives — and a small seed makes every offset affordable. Random truncation would sample
/// this space; enumerating it settles it.
#[test]
fn every_truncation_of_a_dxf_is_handled() {
    let base = minimal_seed();
    for cut in 0..base.len() {
        import_and_check(&base[..cut], &format!("truncate/{cut}"));
    }
}

/// Files that are not DXF at all, and files that are pathological rather than merely wrong.
#[test]
fn hostile_inputs_are_refused_legibly() {
    let cases: Vec<(&str, Vec<u8>)> = vec![
        ("empty", Vec::new()),
        ("nul only", vec![0u8; 64]),
        ("binary noise", (0..=255u8).cycle().take(4096).collect()),
        ("no newline at all", b"0SECTION2ENTITIES0EOF".to_vec()),
        // A group code with no value: the pairing assumption, violated at the last line.
        (
            "dangling group code",
            b"0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n".to_vec(),
        ),
        // Deep nesting. A recursive-descent parser without a depth limit runs out of stack, which
        // is a crash rather than an error.
        ("nested sections", {
            let mut v = Vec::new();
            for _ in 0..10_000 {
                v.extend_from_slice(b"0\nSECTION\n2\nENTITIES\n");
            }
            v.extend_from_slice(b"0\nEOF\n");
            v
        }),
        // A vertex count that lies, which is the allocation probe in its purest form.
        ("lying vertex count", {
            let mut v = Vec::new();
            v.extend_from_slice(b"0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n8\n0\n90\n");
            v.extend_from_slice(b"2000000000\n70\n0\n10\n0\n20\n0\n");
            v.extend_from_slice(b"0\nENDSEC\n0\nEOF\n");
            v
        }),
        // Very long single line: a parser reading a "line" into a fixed buffer, or one with no
        // bound at all.
        ("enormous single token", {
            let mut v = b"0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n".to_vec();
            v.extend(std::iter::repeat(b'9').take(1_000_000));
            v.extend_from_slice(b"\n0\nEOF\n");
            v
        }),
    ];

    for (name, bytes) in cases {
        import_and_check(&bytes, name);
    }
}

// ── the boundary the guard is built on ──────────────────────────────────────────────────

/// A DXF whose LINE has a layer name of exactly `len` bytes.
///
/// Group 8 is the layer, which is a free-text value — so it is the natural place to put a record
/// of a chosen length without the file being malformed in any other way. A long layer name is
/// unusual but not invalid, which is exactly what the accepted side of the boundary needs.
fn dxf_with_record_of_length(len: usize) -> Vec<u8> {
    let mut v = b"0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n".to_vec();
    v.extend(std::iter::repeat(b'A').take(len));
    v.extend_from_slice(b"\n10\n0\n20\n0\n11\n10\n21\n10\n0\nENDSEC\n0\nEOF\n");
    v
}

/// The length guard fires exactly at dime's buffer size, and not before.
///
/// `kDimeLineLimit` in Dxf.cpp duplicates dime's `DXF_MAXLINELEN`. A static_assert now catches the
/// two constants drifting apart at compile time, but that only proves the NUMBERS agree — it says
/// nothing about whether the guard is placed correctly around it. This walks the boundary and
/// asserts the behaviour changes on the right byte:
///
///   * 4095 characters — dime fills lineBuf[0..4094] and terminates at [4095], in bounds. Must be
///     accepted, or the guard is over-strict and rejects valid files.
///   * 4096 characters — dime fills [0..4095] and terminates at [4096], one past the end. Must be
///     refused, or the overflow is reachable again.
///
/// The 4095 case is the one that catches the dangerous direction of drift. If dime's limit ever
/// SHRANK, that record would overflow and this test would crash the process rather than fail
/// politely — which is the correct outcome, and is why it is worth running a case that is expected
/// to succeed rather than only cases expected to fail.
#[test]
fn the_record_length_guard_fires_exactly_at_dimes_buffer_size() {
    const LIMIT: usize = 4096;

    // Just inside. Must import; anything else means the guard rejects valid files.
    let inside = dxf_with_record_of_length(LIMIT - 1);
    let mut s = Session::new().expect("session");
    let path = std::env::temp_dir().join(format!("cad-bound-in-{}.dxf", std::process::id()));
    std::fs::write(&path, &inside).expect("write");
    let ok = s.import_dxf(path.to_str().expect("utf-8"), Plane::Xy, 1.0);
    assert!(
        ok.is_ok(),
        "a {}-byte record is within dime's buffer and must be accepted; the guard is \
         over-strict and would reject valid files: {:?}",
        LIMIT - 1,
        ok.err().map(|e| e.message)
    );
    let _ = std::fs::remove_file(&path);

    // Exactly at the limit, and past it. Both must be refused, and say why.
    for len in [LIMIT, LIMIT + 1] {
        let outside = dxf_with_record_of_length(len);
        let mut s = Session::new().expect("session");
        let path = std::env::temp_dir().join(format!("cad-bound-{len}-{}.dxf", std::process::id()));
        std::fs::write(&path, &outside).expect("write");
        let result = s.import_dxf(path.to_str().expect("utf-8"), Plane::Xy, 1.0);
        let err = result.err().unwrap_or_else(|| {
            panic!("a {len}-byte record reaches dime's one-past-the-end write and must be refused")
        });
        assert!(
            !err.message.trim().is_empty(),
            "the refusal at {len} bytes must say something"
        );
        let _ = std::fs::remove_file(&path);
    }
}
