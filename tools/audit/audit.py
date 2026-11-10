#!/usr/bin/env python3
"""The architecture audit: structural rules, checked by machine, against a committed baseline.

    python3 tools/audit/audit.py [repo] [--json report.json]

Exit status is 0 when nothing NEW is wrong and 1 otherwise, so it is usable as a ctest entry and as
the gate of a nightly job.

# Why this exists

`app::Controller` and `abi::Session` each assembled the document-computing stack by hand -- feature
registry, recompute cache, blob store, mesh cache -- and nothing recorded that there were two. They
drifted in BOTH directions: the shells had no disk tier at all, and the first shared version would
have taken the memory tier away from the ABI. Nobody decided either. It was found only because
someone extracted `cad::runtime::DocumentRuntime` by hand (8ed1262, 5de7fa8).

That extraction consolidated two copies, and four more were still there: the Python bindings and
three spikes each assemble their own, and the refactor that fixed the drift could not see them -- the
audit found them on its first run. A careful human fix missing copies is the argument for a machine
check, not a failure of the person.

Every check here is shaped by a drift this repository actually had:

  single_assembly  -- the stack is built in cad::runtime and nowhere else.       (Controller/Session)
  layer_graph      -- cad-internal dependencies follow a declared ALLOW-list.    (the runtime gap)
  ffi_mirror       -- a hand-written Rust copy of a C struct matches its order.   (the CadHost SIGSEGV)

# The baseline is a ratchet, not an allow-list that rots

`tools/audit/baseline.json` lists known violations, each with a reason. A finding in the baseline is
reported as tracked debt and does not fail. A finding NOT in the baseline fails. A baseline entry that
no longer matches a finding also fails -- it is stale, and a one-way allow-list only moves the rot.
So debt can be paid down, and can never be quietly added to.

# How it cannot pass vacuously

Every check counts what it examined, and a floor fails the run when that count collapses: a moved
directory or a broken parser must break the audit, not read as "all clear". The patterns themselves
are exercised by tools/audit/selftest.py, which builds synthetic trees that MUST produce findings.
Checks that were never seen failing are how this project got a layering rule that let a layer be
included from below.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Iterator

SOURCE_SUFFIXES = {".h", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".mm"}
SKIP_PARTS = {".git", "vcpkg_installed", "third_party", "target", "__pycache__", "node_modules"}


@dataclass(frozen=True)
class Finding:
    check: str
    key: str        # stable identity: no line numbers, so an unrelated edit does not churn it
    message: str
    where: str      # file:line, for a human to jump to


@dataclass
class CheckResult:
    name: str
    findings: list[Finding] = field(default_factory=list)
    examined: int = 0


# ── source helpers ──────────────────────────────────────────────────────────────────────────


def _skipped(rel: Path) -> bool:
    return any(part in SKIP_PARTS or part.startswith("build") for part in rel.parts)


def source_files(root: Path, suffixes: set[str] = SOURCE_SUFFIXES) -> Iterator[tuple[Path, Path]]:
    for path in sorted(root.rglob("*")):
        if path.suffix not in suffixes or not path.is_file():
            continue
        rel = path.relative_to(root)
        if not _skipped(rel):
            yield rel, path


def strip_comments(text: str) -> str:
    """C/C++ comments and string literals blanked, newlines kept so line numbers still line up.

    Without this every doc comment that NAMES a forbidden construction reads as one -- and this
    codebase documents its reasons at length, so that would be most of the findings.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i += 2
        elif c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                out.append("\n" if i < n and text[i] == "\n" else " ")
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def top_layer(rel: Path) -> str:
    """The layer a path belongs to: its top directory, or two levels for modules/ and bindings/."""
    parts = rel.parts
    if parts and parts[0] in {"modules", "bindings"} and len(parts) > 1:
        return f"{parts[0]}/{parts[1]}"
    return parts[0] if parts else ""


# ── check: single_assembly ──────────────────────────────────────────────────────────────────

# The pieces of the document-computing stack. Constructing one is ASSEMBLING the stack.
STACK_COMPONENTS = ("FeatureRegistry", "MemoryCache", "TieredCache", "DdcCache",
                    "MemoryBlobStore", "MeshCache")

# Where construction is legitimate: the one assembly point, the layers that DEFINE the pieces, and
# the tests that exercise them in isolation. Deliberately short -- spikes and bindings are NOT here,
# because a benchmark measuring a stack the product does not ship measures the wrong thing.
ASSEMBLY_ALLOWED = {"runtime", "core", "tests", "tests-rs"}

_COMP = "|".join(STACK_COMPONENTS)
_QUAL = r"(?:[A-Za-z_]\w*::)*"
_ASSEMBLY_PATTERNS = [
    # std::make_unique<recompute::TieredCache>(...)
    re.compile(rf"make_(?:unique|shared)\s*<\s*{_QUAL}({_COMP})\s*>"),
    # a value member or local: `recompute::MemoryCache cache;`, `MeshCache meshes(blobs);`
    # References, pointers and smart-pointer members are USES of an assembled stack, not assembly,
    # and the lookbehind keeps them out.
    re.compile(rf"(?<![\w:&*<]){_QUAL}({_COMP})\s+[A-Za-z_]\w*\s*(?:[;({{]|=(?!=))"),
]
# Building a registry of the FEATURE builtins is assembling one: `features::builtins()`, a bare
# `builtins()` under a using-directive, or `registerBuiltins(`. NOT `io::FormatRegistry::builtins()`,
# which is the file-format registry and no part of the document stack -- the first version of this
# pattern flagged three of those, including the import harness, and would have taught everyone to
# ignore the check.
_REGISTRY_BUILD = re.compile(r"\bfeatures::builtins\s*\(|(?<![:\w])builtins\s*\(|\bregisterBuiltins\s*\(")


def check_single_assembly(root: Path) -> CheckResult:
    result = CheckResult("single_assembly")
    seen: set[str] = set()
    for rel, path in source_files(root):
        layer = top_layer(rel)
        result.examined += 1
        if layer in ASSEMBLY_ALLOWED:
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        hits: list[tuple[str, int]] = []
        for pattern in _ASSEMBLY_PATTERNS:
            hits += [(m.group(1), m.start()) for m in pattern.finditer(text)]
        hits += [("FeatureRegistry", m.start()) for m in _REGISTRY_BUILD.finditer(text)]
        for component, index in sorted(hits, key=lambda h: h[1]):
            key = f"{rel.as_posix()}:{component}"
            if key in seen:
                continue
            seen.add(key)
            result.findings.append(Finding(
                "single_assembly", key,
                f"builds a {component} -- the document stack is assembled in cad::runtime only; "
                f"use cad::runtime::DocumentRuntime",
                f"{rel.as_posix()}:{line_of(text, index)}"))
    return result


# ── check: layer_graph ──────────────────────────────────────────────────────────────────────

# The DECLARED dependency graph, as an allow-list: a layer may include its own headers and those of
# the layers named here, and nothing else from this repository.
#
# An allow-list and not per-layer deny-lists, because deny-lists are how `runtime` came to be
# includable from `core`: adding a layer meant remembering to forbid it in every layer below, and
# nobody did. Here a new layer is forbidden everywhere until something is deliberately allowed to use
# it, and a new top-level directory is a finding until it is declared at all.
#
# Third-party includes (Qt in core, bgfx outside render) are tools/check_layering.py's job.
ANY = "*"
LAYERS: dict[str, dict[str, str]] = {
    "core": {"modules/assetlib": "content-addressed cache", "modules/planegcs": "sketch solver"},
    "render": {"core": ""},
    "runtime": {"core": "", "render": "the mesh cache"},
    "abi": {"core": "", "render": "", "runtime": "the shared document stack"},
    "app": {"core": "", "render": "", "runtime": "the shared document stack",
            "abi": "About reads the ABI version macros"},
    "shell_qt": {"app": "", "abi": "version macros and plugin hosting", "core": "", "render": "",
                 "runtime": "", "modules/proshell": ""},
    "shell_ios": {"app": "", "core": "", "render": "", "runtime": ""},
    "shell_libgui": {"app": "", "core": "", "render": "the scene, and the UiPass it draws the interface with"},
    "bindings/python": {"core": "", "render": "", "runtime": ""},
    "modules/proshell": {},
    "modules/assetlib": {},
    # A HEADER-restricted edge: planegcs may use cad/log/ and nothing else of core. At whole-layer
    # granularity this read as a cycle (core -> planegcs -> core), which it is not, because cad/log
    # is a leaf -- and the audit verifies that it stays one rather than taking it on trust.
    "modules/planegcs": {"header:cad/log/": "the shim routes solver logging into cad::log (cd5df9a)"},
    "rust": {},
    "tools": {"core": "", "render": "", "runtime": "", "app": "", "abi": "",
              "modules/assetlib": ""},
    "spikes": {"core": "", "render": "", "runtime": "", "app": "", "modules/assetlib": ""},
    "tests": {ANY: "tests may exercise any layer"},
}

_INCLUDE = re.compile(r'^[ \t]*#[ \t]*(?:include|import)[ \t]*[<"]([^>"]+)[>"]', re.M)


def include_roots(root: Path) -> dict[str, str]:
    """Header prefix -> owning layer, DERIVED from where headers live rather than listed.

    `core/document/include/cad/document` makes `cad/document/` a core header; a new core package is
    covered the day it is created, with nothing here to update.
    """
    roots: dict[str, str] = {}
    for inc in sorted(root.rglob("include")):
        rel = inc.relative_to(root)
        if not inc.is_dir() or _skipped(rel):
            continue
        layer = top_layer(rel)
        for child in sorted(inc.iterdir()):
            if not child.is_dir():
                continue
            if child.name == "cad":
                for package in sorted(child.iterdir()):
                    if package.is_dir():
                        roots[f"cad/{package.name}/"] = layer
            else:
                roots[f"{child.name}/"] = layer
    return roots


def _cycle(graph: dict[str, set[str]]) -> list[str] | None:
    state: dict[str, int] = {}
    stack: list[str] = []

    def visit(node: str) -> list[str] | None:
        state[node] = 1
        stack.append(node)
        for nxt in sorted(graph.get(node, ())):
            if state.get(nxt) == 1:
                return stack[stack.index(nxt):] + [nxt]
            if nxt not in state and (found := visit(nxt)):
                return found
        stack.pop()
        state[node] = 2
        return None

    for node in sorted(graph):
        if node not in state and (found := visit(node)):
            return found
    return None


def check_layer_graph(root: Path, layers: dict[str, dict[str, str]] = LAYERS) -> CheckResult:
    result = CheckResult("layer_graph")
    graph = {name: {d for d in deps if d != ANY and not d.startswith("header:")}
             for name, deps in layers.items()}
    if cycle := _cycle(graph):
        result.findings.append(Finding("layer_graph", "cycle:" + "->".join(cycle),
                                       "the declared layer graph has a cycle: " + " -> ".join(cycle),
                                       "tools/audit/audit.py"))
    for name, deps in layers.items():
        for dep in deps:
            if dep.startswith("header:"):
                continue
            if dep != ANY and dep not in layers:
                result.findings.append(Finding("layer_graph", f"undeclared-dep:{name}->{dep}",
                                               f"{name} is allowed to use {dep}, which is not a layer",
                                               "tools/audit/audit.py"))

    roots = include_roots(root)

    # A header-restricted edge is acyclic only while that package is a LEAF. Verified, not assumed:
    # the day cad/log includes another cad package, the edge planegcs -> cad/log becomes a real path
    # back into core, and this says so.
    for name, deps in layers.items():
        for dep in deps:
            if not dep.startswith("header:"):
                continue
            prefix = dep[len("header:"):]
            package_dirs = [d for d in root.glob(f"*/{prefix.rstrip('/').split('/')[-1]}")
                            if d.is_dir() and (d / "include" / prefix).is_dir()]
            for pkg in package_dirs:
                for rel, path in source_files(pkg):
                    text = path.read_text(encoding="utf-8", errors="replace")
                    for m in _INCLUDE.finditer(text):
                        inc = m.group(1)
                        if any(inc.startswith(p) for p in roots) and not inc.startswith(prefix):
                            where = (pkg / rel).relative_to(root).as_posix()
                            result.findings.append(Finding(
                                "layer_graph", f"not-a-leaf:{prefix}",
                                f"{name} is allowed {prefix} only because it is a leaf, and it now "
                                f"includes {inc} -- that edge may close a cycle", where))

    undeclared: set[str] = set()
    for rel, path in source_files(root):
        layer = top_layer(rel)
        if layer not in layers:
            if layer not in undeclared:
                undeclared.add(layer)
                result.findings.append(Finding(
                    "layer_graph", f"undeclared-layer:{layer}",
                    f"'{layer}' contains sources but is not a declared layer -- declare what it may "
                    f"use in tools/audit/audit.py LAYERS", rel.as_posix()))
            continue
        allowed = layers[layer]
        text = strip_comments_keep_includes(path.read_text(encoding="utf-8", errors="replace"))
        for m in _INCLUDE.finditer(text):
            target = next((lay for prefix, lay in roots.items() if m.group(1).startswith(prefix)), None)
            if target is None:
                continue          # third-party or system: not this check's question
            result.examined += 1
            if target == layer or ANY in allowed or target in allowed:
                continue
            if any(d.startswith("header:") and m.group(1).startswith(d[len("header:"):])
                   for d in allowed):
                continue
            result.findings.append(Finding(
                "layer_graph", f"{layer}->{target}:{rel.as_posix()}",
                f"{layer} includes {m.group(1)} from {target}, which {layer} is not allowed to use",
                f"{rel.as_posix()}:{line_of(text, m.start())}"))
    return result


def strip_comments_keep_includes(text: str) -> str:
    """Comments removed, but the quoted path of an #include kept -- it is the thing being checked."""
    lines = text.split("\n")
    kept = {i: line for i, line in enumerate(lines) if re.match(r'^\s*#\s*(?:include|import)', line)}
    stripped = strip_comments(text).split("\n")
    return "\n".join(kept.get(i, line) for i, line in enumerate(stripped))


# ── check: ffi_mirror ───────────────────────────────────────────────────────────────────────

C_HEADER = "abi/include/cad/abi/cad_plugin_abi.h"
RUST_ROOTS = ("tests-rs", "rust")
PREFIX_SUFFIX = "Prefix"   # `CadHostPrefix` mirrors the leading members of `CadHost`


def _match_brace(text: str, open_index: int) -> int:
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _split_top(body: str, sep: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in body:
        if ch in "({[<":
            depth += 1
        elif ch in ")}]>":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [p for p in parts if p.strip()]


def c_structs(text: str) -> dict[str, list[str]]:
    """Field names, in declaration order, of every struct with a body."""
    text = strip_comments(text)
    structs: dict[str, list[str]] = {}
    for m in re.finditer(r"\b(typedef\s+)?struct\s*(\w*)\s*\{", text):
        close = _match_brace(text, m.end() - 1)
        if close < 0:
            continue
        after = re.match(r"\s*(\w+)?\s*;", text[close + 1:])
        name = (after.group(1) if m.group(1) and after and after.group(1) else m.group(2))
        if not name:
            continue
        fields: list[str] = []
        body = text[m.end():close]
        # Statements, with nested braces flattened so an inline union does not split mid-way.
        for stmt in _split_top(body.replace("{", "(").replace("}", ")"), ";"):
            stmt = " ".join(stmt.split())
            if fp := re.search(r"\(\s*\*\s*(\w+)\s*\)", stmt):
                fields.append(fp.group(1))
                continue
            declarators = _split_top(stmt, ",")
            for j, decl in enumerate(declarators):
                decl = re.sub(r"\[[^\]]*\]|:\s*\d+\s*$", "", decl).strip()
                ids = re.findall(r"[A-Za-z_]\w*", decl)
                if ids:
                    fields.append(ids[-1])
        structs[name] = fields
    return structs


def rust_repr_c_structs(text: str) -> dict[str, tuple[list[str], int]]:
    """`#[repr(C)]` struct name -> (field names in order, index), parsing depth so a function-pointer
    type spanning lines does not have its PARAMETERS read as fields."""
    found: dict[str, tuple[list[str], int]] = {}
    for m in re.finditer(r"#\[repr\(C\)\]\s*(?:#\[[^\]]*\]\s*)*(?:pub(?:\([^)]*\))?\s+)?struct\s+(\w+)\s*\{",
                         text):
        close = _match_brace(text, m.end() - 1)
        if close < 0:
            continue
        body = re.sub(r"//[^\n]*", "", text[m.end():close])
        body = re.sub(r"#\[[^\]]*\]", "", body)
        names = []
        for piece in _split_top(body, ","):
            if fm := re.match(r"\s*(?:pub(?:\([^)]*\))?\s+)?(?:r#)?([A-Za-z_]\w*)\s*:", piece):
                names.append(fm.group(1).rstrip("_"))
        found[m.group(1)] = (names, m.start())
    return found


def check_ffi_mirror(root: Path) -> CheckResult:
    result = CheckResult("ffi_mirror")
    header = root / C_HEADER
    if not header.is_file():
        result.findings.append(Finding("ffi_mirror", "header-missing", f"{C_HEADER} not found", C_HEADER))
        return result
    cdefs = c_structs(header.read_text(encoding="utf-8", errors="replace"))
    for base in RUST_ROOTS:
        base_dir = root / base
        if not base_dir.is_dir():
            continue
        for path in sorted(base_dir.rglob("*.rs")):
            rel = path.relative_to(root)
            if _skipped(rel):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for name, (rust_fields, index) in rust_repr_c_structs(text).items():
                prefix = name.endswith(PREFIX_SUFFIX) and name[: -len(PREFIX_SUFFIX)] in cdefs
                cname = name[: -len(PREFIX_SUFFIX)] if prefix else name
                where = f"{rel.as_posix()}:{line_of(text, index)}"
                if cname not in cdefs:
                    result.findings.append(Finding(
                        "ffi_mirror", f"{rel.as_posix()}:{name}:orphan",
                        f"#[repr(C)] {name} mirrors no struct in {C_HEADER}", where))
                    continue
                result.examined += 1
                cfields = [f.rstrip("_") for f in cdefs[cname]]
                expected = cfields[: len(rust_fields)] if prefix else cfields
                # Compared by SPELLING-INDEPENDENT name: Rust says edge_polylines where C says
                # edgePolylines, and that is the same slot. What must match is which field sits in
                # which position -- a reorder changes that however either side spells its names.
                def norm(names: list[str]) -> list[str]:
                    return [n.replace("_", "").lower() for n in names]
                if norm(rust_fields) != norm(expected):
                    first = next((i for i, (a, b) in enumerate(zip(norm(rust_fields), norm(expected)))
                                  if a != b),
                                 min(len(rust_fields), len(expected)))
                    got = rust_fields[first] if first < len(rust_fields) else "(end)"
                    want = expected[first] if first < len(expected) else "(end)"
                    result.findings.append(Finding(
                        "ffi_mirror", f"{rel.as_posix()}:{name}",
                        f"{name} does not match C {cname}{' (prefix)' if prefix else ''}: field {first} "
                        f"is '{got}' in Rust and '{want}' in C -- a caller would read the wrong slot",
                        where))
    return result


# ── the runner ──────────────────────────────────────────────────────────────────────────────

CHECKS: dict[str, Callable[[Path], CheckResult]] = {
    "single_assembly": check_single_assembly,
    "layer_graph": check_layer_graph,
    "ffi_mirror": check_ffi_mirror,
}

# Collapse floors: below these the check examined too little to mean anything. Set well under
# today's counts (see the report's `examined`) so growth never trips them and a vanished tree does.
FLOORS = {"single_assembly": 300, "layer_graph": 300, "ffi_mirror": 9}

BASELINE = "tools/audit/baseline.json"


def load_baseline(root: Path) -> tuple[dict[tuple[str, str], str], list[str]]:
    path = root / BASELINE
    if not path.is_file():
        return {}, []
    data = json.loads(path.read_text(encoding="utf-8"))
    accepted, problems = {}, []
    for entry in data.get("accepted", []):
        check, key, reason = entry.get("check", ""), entry.get("key", ""), entry.get("reason", "")
        if not reason.strip():
            problems.append(f"baseline entry {check}:{key} has no reason")
        accepted[(check, key)] = reason
    return accepted, problems


def run(root: Path, baseline: dict[tuple[str, str], str] | None = None,
        apply_floors: bool = True) -> dict:
    baseline = {} if baseline is None else baseline
    report: dict = {"checks": {}, "new": [], "stale": [], "debt": [], "problems": []}
    produced: set[tuple[str, str]] = set()
    for name, check in CHECKS.items():
        result = check(root)
        entry = {"examined": result.examined, "findings": [asdict(f) for f in result.findings]}
        report["checks"][name] = entry
        if apply_floors and result.examined < FLOORS.get(name, 0):
            report["problems"].append(
                f"{name} examined {result.examined}, below its floor of {FLOORS[name]} -- "
                f"a moved directory or a broken parser, not a clean tree")
        for f in result.findings:
            produced.add((f.check, f.key))
            bucket = "debt" if (f.check, f.key) in baseline else "new"
            item = asdict(f)
            if bucket == "debt":
                item["reason"] = baseline[(f.check, f.key)]
            report[bucket].append(item)
    report["stale"] = [{"check": c, "key": k} for (c, k) in baseline if (c, k) not in produced]
    report["ok"] = not (report["new"] or report["stale"] or report["problems"])
    return report


def _git_commit(root: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("repo", nargs="?", default=".")
    parser.add_argument("--json", help="also write the full report here")
    args = parser.parse_args(argv)
    root = Path(args.repo).resolve()

    baseline, problems = load_baseline(root)
    report = run(root, baseline)
    report["problems"] = problems + report["problems"]
    report["ok"] = report["ok"] and not problems
    report["commit"] = _git_commit(root)
    report["time"] = _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")

    for name, entry in report["checks"].items():
        print(f"{name:16} examined {entry['examined']:5}   findings {len(entry['findings'])}")
    for title, items, fmt in (
        ("NEW -- not in the baseline, fails the run", report["new"],
         lambda i: f"  {i['where']}: {i['message']}"),
        ("STALE baseline entries -- the debt was paid; remove them", report["stale"],
         lambda i: f"  {i['check']}: {i['key']}"),
        ("PROBLEMS", [{"p": p} for p in report["problems"]], lambda i: f"  {i['p']}"),
        ("tracked debt (in the baseline)", report["debt"],
         lambda i: f"  {i['where']}: {i['message']}\n      reason: {i['reason']}"),
    ):
        if items:
            print(f"\n{title} ({len(items)})")
            for item in items:
                print(fmt(item))
    print("\narchitecture audit " + ("OK" if report["ok"] else "FAILED"))

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
