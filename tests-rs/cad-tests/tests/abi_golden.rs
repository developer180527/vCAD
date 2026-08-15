//! The golden ABI snapshot: adding to the plugin boundary is legal, editing it is not.
//!
//! ADR 0011 commits vCAD to supporting a compiled plugin for a decade. Rule 2 of that ADR --
//! additive only, never remove a function, never change a signature, never reorder a struct member
//! -- is what every project intends and what discipline alone never delivers. This turns it into a
//! build failure.
//!
//! Each declaration in `cad_plugin_abi.h` is extracted and hashed, and the hashes are checked
//! against `abi/abi_golden.txt`. Changing an existing declaration fails. Removing one fails. Adding
//! one passes, and the golden file is then regenerated deliberately:
//!
//!     CAD_ABI_GOLDEN_UPDATE=1 cargo test -p cad-tests --test abi_golden
//!
//! That the update is a deliberate act, in its own commit, is the point: a reviewer sees the golden
//! file change and knows exactly what the boundary gained.
//!
//! COMMENTS AND FORMATTING ARE NORMALISED AWAY on purpose. A snapshot that failed when someone
//! improved a comment would be deleted by the third person it inconvenienced, and a test nobody
//! trusts protects nothing. It hashes what the compiler sees, not what the file looks like.

use std::collections::BTreeMap;
use std::path::PathBuf;

fn header_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .expect("cad-tests must live at <repo>/tests-rs/cad-tests")
        .join("abi/include/cad/abi/cad_plugin_abi.h")
}

fn golden_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .expect("cad-tests must live at <repo>/tests-rs/cad-tests")
        .join("abi/abi_golden.txt")
}

/// FNV-1a. Change detection, not cryptography: this guards against accident, and anyone able to
/// craft a collision could simply edit the golden file instead.
fn hash(text: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in text.as_bytes() {
        h ^= u64::from(*b);
        h = h.wrapping_mul(0x1000_0000_01b3);
    }
    h
}

/// Strip comments, then collapse every run of whitespace to one space.
fn normalise(source: &str) -> String {
    let mut out = String::with_capacity(source.len());
    let bytes: Vec<char> = source.chars().collect();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == '/' && i + 1 < bytes.len() && bytes[i + 1] == '*' {
            i += 2;
            while i + 1 < bytes.len() && !(bytes[i] == '*' && bytes[i + 1] == '/') {
                i += 1;
            }
            i += 2;
            out.push(' ');
        } else if bytes[i] == '/' && i + 1 < bytes.len() && bytes[i + 1] == '/' {
            while i < bytes.len() && bytes[i] != '\n' {
                i += 1;
            }
            out.push(' ');
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    out
}

fn squeeze(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// Every declaration the boundary exposes, as name -> normalised text.
///
/// Duplicate names are concatenated rather than overwritten, because `CAD_API` is defined twice
/// under `#if defined(_WIN32)`. Keeping only one would make the snapshot depend on which platform
/// the extractor happened to notice first.
fn declarations(source: &str) -> BTreeMap<String, String> {
    let text = normalise(source);
    let mut out: BTreeMap<String, String> = BTreeMap::new();
    let mut add = |name: String, body: String| {
        out.entry(name)
            .and_modify(|existing| {
                existing.push_str(" || ");
                existing.push_str(&body);
            })
            .or_insert(body);
    };

    // #define NAME ..., continuation lines included.
    let raw_lines: Vec<&str> = text.lines().collect();
    let mut i = 0;
    while i < raw_lines.len() {
        let line = raw_lines[i].trim();
        if let Some(rest) = line.strip_prefix("#define ") {
            let mut body = rest.trim().to_string();
            while body.ends_with('\\') && i + 1 < raw_lines.len() {
                body.pop();
                i += 1;
                body.push(' ');
                body.push_str(raw_lines[i].trim());
            }
            let name = body
                .split(|c: char| c.is_whitespace() || c == '(')
                .next()
                .unwrap_or("")
                .to_string();
            if !name.is_empty() {
                add(format!("define {name}"), squeeze(&body));
            }
        }
        i += 1;
    }

    // Statements: everything between top-level semicolons, with brace-balanced bodies kept whole.
    //
    // Preprocessor lines are removed FIRST. Leaving them in the stream meant every statement
    // carried the `#define`s and `#if`s that preceded it, and the extractor found 42 macros and
    // zero functions -- silently, because a golden file full of macros looks perfectly plausible.
    let code: String = text
        .lines()
        .filter(|line| !line.trim_start().starts_with('#'))
        .collect::<Vec<_>>()
        .join("\n");

    // `extern "C" {` wraps the ENTIRE header, so brace depth never returns to zero and a
    // depth==0 test for the end of a statement never fires. Removing it leaves one unmatched
    // closing brace, which saturating_sub absorbs and which names nothing.
    let code = code.replace("extern \"C\" {", "");

    let mut statement = String::new();
    let mut depth = 0usize;
    for ch in code.chars() {
        statement.push(ch);
        match ch {
            '{' => depth += 1,
            '}' => depth = depth.saturating_sub(1),
            ';' if depth == 0 => {
                let s = squeeze(&statement);
                if let Some(name) = statement_name(&s) {
                    add(name, s);
                }
                statement.clear();
            }
            _ => {}
        }
    }
    out
}

/// The name a statement declares, or None for things that are not part of the boundary.
fn statement_name(statement: &str) -> Option<String> {
    let s = statement.trim();
    if s.is_empty() {
        return None;
    }

    // typedef struct { ... } Name;  /  typedef uint64_t Name;
    if s.starts_with("typedef ") {
        let before_semi = s.trim_end_matches(';').trim();
        let name = before_semi
            .rsplit(|c: char| c.is_whitespace() || c == '}')
            .next()?;
        let name = name.trim();
        if name.is_empty() {
            return None;
        }
        return Some(format!("type {name}"));
    }

    // struct CadHost { ... };
    if let Some(rest) = s.strip_prefix("struct ") {
        let name = rest.split(|c: char| c.is_whitespace() || c == '{').next()?;
        if !name.is_empty() {
            return Some(format!("struct {name}"));
        }
    }

    // CAD_API <ret> name(args);
    if s.contains("CAD_API") {
        let before_paren = s.split('(').next()?;
        let name = before_paren
            .trim()
            .rsplit(|c: char| c.is_whitespace() || c == '*')
            .next()?;
        if !name.is_empty() {
            return Some(format!("fn {name}"));
        }
    }
    None
}

#[test]
fn the_plugin_abi_is_additive_only() {
    let source = std::fs::read_to_string(header_path()).expect("cad_plugin_abi.h must be readable");
    let current = declarations(&source);
    // Counted BY KIND, because the first version of this parser produced 42 macros and zero
    // functions and sailed past a `len() > 20` check. A golden file full of macros looks
    // perfectly plausible, which is precisely what makes that failure dangerous: the test would
    // have gone green forever while guarding none of the 82 exported functions.
    let functions = current.keys().filter(|k| k.starts_with("fn ")).count();
    let types = current
        .keys()
        .filter(|k| k.starts_with("type ") || k.starts_with("struct "))
        .count();
    let macros = current.keys().filter(|k| k.starts_with("define ")).count();
    assert!(
        functions >= 50 && types >= 5 && macros >= 20,
        "the extractor is broken, not the header: {functions} functions, {types} types, \
         {macros} macros. The header exports over 80 functions; a snapshot missing them would \
         pass forever while protecting nothing."
    );

    let rendered: String = current
        .iter()
        .map(|(name, body)| format!("{name}\t{:016x}\n", hash(body)))
        .collect();

    if std::env::var("CAD_ABI_GOLDEN_UPDATE").is_ok() {
        std::fs::write(golden_path(), &rendered).expect("golden file must be writable");
        eprintln!("golden ABI snapshot updated: {}", golden_path().display());
        return;
    }

    let golden = match std::fs::read_to_string(golden_path()) {
        Ok(text) => text,
        Err(_) => panic!(
            "no golden ABI snapshot at {}. Create it with:\n\
             \n    CAD_ABI_GOLDEN_UPDATE=1 cargo test -p cad-tests --test abi_golden\n",
            golden_path().display()
        ),
    };

    let mut expected: BTreeMap<&str, &str> = BTreeMap::new();
    for line in golden.lines() {
        if let Some((name, digest)) = line.split_once('\t') {
            expected.insert(name, digest);
        }
    }

    let mut changed = Vec::new();
    let mut removed = Vec::new();
    for (name, digest) in &expected {
        match current.get(*name) {
            None => removed.push((*name).to_string()),
            Some(body) => {
                let now = format!("{:016x}", hash(body));
                if now != *digest {
                    changed.push((*name).to_string());
                }
            }
        }
    }
    let added: Vec<&String> = current
        .keys()
        .filter(|name| !expected.contains_key(name.as_str()))
        .collect();

    // Additions are legal and are reported, not failed. This is the whole asymmetry the ADR rests
    // on: the boundary may grow forever and must never change shape underneath a compiled plugin.
    if !added.is_empty() {
        eprintln!(
            "note: {} new declaration(s) -- legal. Regenerate the snapshot in its own commit:\n  {}",
            added.len(),
            added
                .iter()
                .map(|s| s.as_str())
                .collect::<Vec<_>>()
                .join("\n  ")
        );
    }

    assert!(
        changed.is_empty() && removed.is_empty(),
        "the plugin ABI changed in a way that breaks every compiled plugin.\n\
         \n\
         CHANGED (a signature, a struct member, or a macro's value):\n  {}\n\
         \n\
         REMOVED:\n  {}\n\
         \n\
         ADR 0011: the boundary is additive only. A mistake gets a NEW declaration beside the old \
         one; the old one keeps its behaviour forever. If this change is genuinely intended and \
         no plugin can yet exist to be broken by it, regenerate the snapshot deliberately:\n\
         \n    CAD_ABI_GOLDEN_UPDATE=1 cargo test -p cad-tests --test abi_golden\n",
        if changed.is_empty() {
            "(none)".into()
        } else {
            changed.join("\n  ")
        },
        if removed.is_empty() {
            "(none)".into()
        } else {
            removed.join("\n  ")
        },
    );
}
