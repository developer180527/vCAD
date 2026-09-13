#!/usr/bin/env python3
"""Fail if an exported C ABI function has no exception firebreak.

The rule, from the first lines of abi/src/Session.cpp:

    Every exported function here is a firebreak: nothing may escape as an exception, no C++ type
    may appear in a signature, and every handle is validated before use.

A C++ exception unwinding into a C, Rust or Swift caller is undefined behaviour -- in practice a
crash in a plugin's frame, reported against the plugin. `withSession` and friends enforce the rule,
but nothing forced an export to USE them, and four did not. Two could leak `bad_alloc`; all four read
or wrote per-session state under the global mutex instead of the session's own, so they raced with
every call that writes that state under the right lock.

# Why a source check, when feature_reachability's source check was called a weakness

That guard reads source to answer a RUNTIME question -- is a command reachable from a button -- and
it passes or fails on how code is spelled rather than on what the application does. This question is
structural: "does every export's body pass through a firebreak". There is no runtime observation that
proves an exception cannot escape, so reading the body is the honest check, not a proxy for one.

# How it cannot pass vacuously

- The export list comes from the HEADER, not from what the parser happens to find, so a definition
  the parser cannot see is reported as missing instead of silently skipped.
- Every exemption must name a reason, and an exemption for an export that has since gained a
  firebreak -- or no longer exists -- is stale and fails. A one-way allow-list only moves the rot.
- A floor on the number of exports, so an emptied or moved header does not read as "all clear".

Run standalone:  python3 tools/check_abi_firebreak.py .
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HEADER = "abi/include/cad/abi/cad_plugin_abi.h"
SOURCES = "abi/src"

# What counts as a firebreak inside an export's body.
#
# Patterns, not substrings. The first version tested `"try" in body`, which every body mentioning
# `registry`, `entry` or `retry` satisfied -- so an unguarded export touching the feature registry
# would have been waved through by the checker written to catch it.
FIREBREAKS = (
    re.compile(r"\bwithSession\s*\("),
    re.compile(r"\bwithSessionStr\s*\("),
    re.compile(r"\breadSessionStr\s*\("),
    re.compile(r"\btry\s*\{"),
)

# Exports that need no firebreak, each with the reason it cannot throw. Verified by reading the body;
# the checker then holds the reason to account by failing if the body ever gains a wrapper (the
# exemption is stale) or the export disappears.
EXEMPT: dict[str, str] = {
    "cad_abi_version": "writes two integers through out-pointers and allocates nothing",
    "cad_abi_accepts": "compares integers and hands back string literals; allocates nothing",
    "cad_session_create": "only delegates to createSession, whose whole body is inside a try; "
                          "the empty std::string it passes is noexcept to construct",
    "cad_session_release": "erases from a map under a lock; erase and the destructors it runs are "
                           "noexcept",
}

# Declared in the header, but defined by a PLUGIN rather than by the host -- so the host must not
# define them, and finding one here would itself be a mistake worth reporting.
PLUGIN_DEFINED: dict[str, str] = {
    "cad_plugin_main": "the entry point every plugin exports; the host only calls it",
}

# A handful of exports is a moved or truncated header, not a small ABI.
MIN_EXPORTS = 80

# `#define NAME(fn, ...)` whose body defines a function called `fn` -- how the typed setters are
# generated. Each invocation `NAME(cad_x, ...)` is then checked against the macro's body, because a
# macro is one body however many exports it stamps out.
MACRO_DEF = re.compile(r"^#define\s+([A-Z_][A-Z0-9_]*)\(\s*([a-zA-Z_]\w*)\s*,(.*?)(?<!\\)\n",
                       re.MULTILINE | re.DOTALL)
MACRO_USE = re.compile(r"^([A-Z_][A-Z0-9_]*)\(\s*(cad_[a-z0-9_]+)\s*,", re.MULTILINE)

EXPORT_DECL = re.compile(r"^CAD_API\b[^;(]*?\b(cad_[a-z0-9_]+)\s*\(", re.MULTILINE)
DEFINITION = re.compile(r"^[A-Za-z_][\w:<>*& ]*?\b(cad_[a-z0-9_]+)\s*\(", re.MULTILINE)


def exported_names(root: Path) -> list[str]:
    text = (root / HEADER).read_text()
    return sorted(set(EXPORT_DECL.findall(text)))


def definitions(root: Path) -> dict[str, list[str]]:
    """Every function body, by name, across the ABI's sources. A list so a duplicate shows up."""
    found: dict[str, list[str]] = {}
    for path in sorted((root / SOURCES).glob("*.cpp")):
        text = path.read_text()
        for match in DEFINITION.finditer(text):
            name = match.group(1)
            # Past the parameter list, then decide: a ';' first is a declaration, a '{' a body.
            depth, i = 0, match.end() - 1
            while i < len(text):
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            rest = text[i + 1:]
            brace, semi = rest.find("{"), rest.find(";")
            if brace < 0 or (0 <= semi < brace):
                continue
            start = i + 1 + brace
            depth, j = 0, start
            while j < len(text):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            found.setdefault(name, []).append(text[start:j + 1])

        # Macro-generated exports: the macro's whole body stands in for each one it stamps out.
        macros: dict[str, str] = {}
        for m in MACRO_DEF.finditer(text):
            macro, param, body = m.group(1), m.group(2), m.group(3)
            if re.search(rf"\b{re.escape(param)}\s*\(", body):
                macros[macro] = body
        for m in MACRO_USE.finditer(text):
            if m.group(1) in macros:
                found.setdefault(m.group(2), []).append(macros[m.group(1)])
    return found


def main(root_arg: str) -> int:
    root = Path(root_arg).resolve()
    exports = exported_names(root)
    bodies = definitions(root)
    problems: list[str] = []

    if len(exports) < MIN_EXPORTS:
        problems.append(f"only {len(exports)} exports found in {HEADER} (floor {MIN_EXPORTS}); "
                        "has the header moved?")

    for name in exports:
        if name in PLUGIN_DEFINED:
            if name in bodies:
                problems.append(f"{name}: a plugin defines this, but the host defines it too")
            continue
        found = bodies.get(name, [])
        if not found:
            # Missing is a failure, not a skip: an export this cannot read is an export it cannot
            # vouch for.
            problems.append(f"{name}: declared in the header but no definition found in {SOURCES}")
            continue
        guarded = any(any(f.search(body) for f in FIREBREAKS) for body in found)
        if name in EXEMPT:
            if guarded:
                problems.append(f"{name}: exempt, but its body now has a firebreak -- remove the "
                                "stale exemption")
            elif not EXEMPT[name].strip():
                problems.append(f"{name}: exempt with no reason")
            continue
        if not guarded:
            problems.append(f"{name}: no withSession, withSessionStr, readSessionStr or try -- an "
                            "exception here unwinds into a C caller")

    for name in PLUGIN_DEFINED:
        if name not in exports:
            problems.append(f"{name}: listed as plugin-defined but no longer declared")

    for name in EXEMPT:
        if name not in exports:
            problems.append(f"{name}: exempt but no longer exported -- remove the stale exemption")

    if problems:
        print("ABI firebreak violations:")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"ABI firebreak OK: {len(exports)} exports, {len(EXEMPT)} exempt with reasons")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
