//! One set of FFI declarations for the C ABI, and a check that it stays one.
//!
//! # The bug this exists to prevent
//!
//! `plugin_host.rs` used to carry its own `extern "C"` blocks alongside the ones in `cad-sys`, and
//! in them `cad_mesh_element_name` was declared as returning `CadStr` when the C function returns
//! `const char*`.
//!
//! That is undefined behaviour, and its shape is worth understanding because it explains why no
//! amount of care would have caught it. A pointer comes back in the first return register; a
//! two-word struct comes back in the first two. So `.data` picked up the real pointer and `.len`
//! picked up whatever the callee happened to leave in the second register. On x86-64 and on macOS
//! arm64 that value was non-zero, so the assertion `len > 0` passed. On Linux arm64 with gcc it was
//! zero, and the test failed — the first machine in the matrix to read the truth.
//!
//! Underneath it, the same test never tessellated, so the string it was checking was empty on every
//! platform, always. The assertion had never once tested what it claimed. A wrong FFI signature had
//! been holding a broken test upright.
//!
//! # Why a lint and not a code review rule
//!
//! Drift between two declarations of the same C function is **not a compile error in any language
//! involved**. Rust believes whatever the `extern` block says; C exports whatever it exports; the
//! linker matches on the symbol name alone and is satisfied. The mismatch becomes visible only as
//! undefined behaviour at the call site, on some platforms, sometimes.
//!
//! So the rule cannot be "be careful". It has to be "there is only one place these are written
//! down", enforced by something that fails.

use std::path::{Path, PathBuf};

fn repo_root() -> PathBuf {
    // tests-rs/cad-tests/tests -> tests-rs -> repo
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("repo root")
        .to_path_buf()
}

/// Every `.rs` file under `tests-rs`, except `cad-sys` itself.
fn rust_sources_outside_cad_sys() -> Vec<PathBuf> {
    let root = repo_root().join("tests-rs");
    let mut found = Vec::new();
    let mut stack = vec![root.clone()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name();
            // `target` is build output and `cad-sys` is the one place allowed to declare these.
            if name == "target" || name == "cad-sys" {
                continue;
            }
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().is_some_and(|e| e == "rs") {
                found.push(path);
            }
        }
    }
    found
}

#[test]
fn no_duplicate_abi_declarations() {
    let mut offenders = Vec::new();

    for path in rust_sources_outside_cad_sys() {
        let Ok(text) = std::fs::read_to_string(&path) else {
            continue;
        };

        // Deliberately crude: any `fn cad_...` inside an `extern` block. A real parser would be
        // more precise and would also be a thing to maintain, and the precision is not needed --
        // there is exactly one correct number of these outside cad-sys, and it is zero.
        let mut depth = 0usize;
        let mut in_extern = false;
        for (number, line) in text.lines().enumerate() {
            let trimmed = line.trim();
            if trimmed.starts_with("extern \"C\"") && trimmed.contains('{') {
                in_extern = true;
                depth = 0;
            }
            if in_extern {
                depth += line.matches('{').count();
                depth -= line.matches('}').count().min(depth);
                if let Some(rest) = trimmed
                    .strip_prefix("pub fn ")
                    .or(trimmed.strip_prefix("fn "))
                {
                    if rest.starts_with("cad_") {
                        let name = rest.split('(').next().unwrap_or(rest);
                        offenders.push(format!(
                            "{}:{}  {}",
                            path.strip_prefix(repo_root()).unwrap_or(&path).display(),
                            number + 1,
                            name
                        ));
                    }
                }
                if depth == 0 {
                    in_extern = false;
                }
            }
        }
    }

    assert!(
        offenders.is_empty(),
        "these declare C ABI functions outside cad-sys:\n  {}\n\n\
         Declare them in tests-rs/cad-sys/src/lib.rs and import them from there. A second \
         declaration of the same C function cannot be kept correct by review: a mismatched return \
         type or argument is not a compile error in Rust, not a compile error in C, and not a link \
         error either -- the linker matches on the symbol name alone. It is undefined behaviour at \
         the call, on some platforms, sometimes. cad_mesh_element_name was declared as returning \
         CadStr instead of *const c_char and passed on four platforms for exactly that reason.",
        offenders.join("\n  ")
    );
}

/// The one crate allowed to declare them must actually be doing so, or the check above passes by
/// being vacuous.
#[test]
fn cad_sys_is_where_the_declarations_live() {
    let sys = repo_root().join("tests-rs/cad-sys/src/lib.rs");
    let text = std::fs::read_to_string(&sys).expect("cad-sys source");
    let count = text.matches("pub fn cad_").count();
    assert!(
        count > 50,
        "cad-sys declares only {count} C functions, which is too few to be the single source the \
         test above assumes. Either the declarations moved, or that test is now checking nothing."
    );
}
