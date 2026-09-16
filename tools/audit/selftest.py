#!/usr/bin/env python3
"""Proves every architecture check can FAIL, on synthetic trees built to break each rule.

    python3 tools/audit/selftest.py

A check that has only ever been seen passing is not a check. This project has shipped several: a
layering rule that let a new layer be included from below, a selection assertion that was trivially
true because the offscreen probe never produced an element, and an ABI golden that was regenerated in
the same commit as the break it existed to catch. So every rule in audit.py is exercised here in both
directions -- a tree that violates it must produce a finding, and a near-miss that does not violate it
must not. The near-misses matter as much: a check that fires on doc comments or on the file-format
registry gets ignored, and an ignored check is a disabled one.

Runs under ctest beside the audit itself, so the audit cannot quietly lose its teeth.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit  # noqa: E402

failures: list[str] = []


def expect(condition: bool, what: str) -> None:
    print(f"{'PASS' if condition else 'FAIL'}  {what}")
    if not condition:
        failures.append(what)


def tree(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return tmp


def keys(result: audit.CheckResult) -> set[str]:
    return {f.key for f in result.findings}


# ── single_assembly ─────────────────────────────────────────────────────────────────────────

with tree({
    # The drift itself: a consumer outside runtime building the stack.
    "app/src/controller.cpp": "auto c = std::make_unique<recompute::TieredCache>(a, b);\n",
    "bindings/python/src/module.cpp": "recompute::FeatureRegistry registry_ = features::builtins();\n",
    "spikes/bench/main.cpp": "const auto registry = features::builtins();\nrender::MeshCache meshes(blobs);\n",
    # The one legitimate home.
    "runtime/src/DocumentRuntime.cpp": "cache_ = std::make_unique<recompute::MemoryCache>();\n",
    # Near-misses that must NOT fire.
    "abi/src/Session.cpp": (
        "// app::Controller used to build a MemoryCache cache; here, by hand.\n"
        "/* recompute::DdcCache ddc(dir); */\n"
        "const char* s = \"MemoryBlobStore blobs;\";\n"
        "cad::io::FormatRegistry formats = cad::io::FormatRegistry::builtins();\n"
        "void f(const recompute::FeatureRegistry& registry, render::MeshCache* meshes);\n"
        "std::unique_ptr<recompute::TieredCache> cache_;\n"
        "recompute::Engine engine(runtime.registry(), runtime.cache());\n"),
}) as tmp:
    found = keys(audit.check_single_assembly(Path(tmp)))
    expect("app/src/controller.cpp:TieredCache" in found, "make_unique of a cache outside runtime is found")
    expect("bindings/python/src/module.cpp:FeatureRegistry" in found,
           "a registry member built from features::builtins() is found")
    expect("spikes/bench/main.cpp:FeatureRegistry" in found, "an auto registry from builtins() is found")
    expect("spikes/bench/main.cpp:MeshCache" in found, "a MeshCache value built outside runtime is found")
    expect(not any(k.startswith("runtime/") for k in found), "building the stack IN runtime is allowed")
    expect(not any(k.startswith("abi/") for k in found),
           "comments, strings, references, pointers, unique_ptr members, Engine, and "
           "io::FormatRegistry::builtins() are not assembly")


# ── layer_graph ─────────────────────────────────────────────────────────────────────────────

LAYOUT = {
    "core/log/include/cad/log/Log.h": "",
    "core/document/include/cad/document/Document.h": "",
    "render/include/cad/render/Scene.h": "",
    "runtime/include/cad/runtime/DocumentRuntime.h": "",
    "app/include/cad/app/Controller.h": "",
    "modules/planegcs/shim/Console.h": '#include "cad/log/Log.h"\n',
}

with tree({
    **LAYOUT,
    # The gap tools/check_layering.py had: nothing forbade a layer from below.
    "core/document/src/Document.cpp": '#include "cad/runtime/DocumentRuntime.h"\n',
    # Reaching up.
    "runtime/src/DocumentRuntime.cpp": '#include "cad/app/Controller.h"\n',
    # A directory nobody declared.
    "newlayer/src/thing.cpp": '#include "cad/document/Document.h"\n',
    # Allowed, and must stay quiet.
    "render/src/Scene.cpp": '#include "cad/document/Document.h"\n',
    "app/src/x.cpp": '// #include "cad/shell/Nope.h"\n#include "cad/runtime/DocumentRuntime.h"\n',
}) as tmp:
    found = keys(audit.check_layer_graph(Path(tmp)))
    expect("core->runtime:core/document/src/Document.cpp" in found,
           "core including runtime (a layer ABOVE it) is found")
    expect("runtime->app:runtime/src/DocumentRuntime.cpp" in found, "runtime reaching up into app is found")
    expect("undeclared-layer:newlayer" in found, "a new top-level directory must be declared")
    expect(not any(k.startswith("render->") for k in found), "an allowed edge is quiet")
    expect(not any(k.startswith("app->") for k in found), "an allowed edge and a commented include are quiet")
    expect(not any("planegcs" in k for k in found), "planegcs may use the leaf cad/log/")

with tree({**LAYOUT, "core/log/src/Log.cpp": '#include "cad/document/Document.h"\n'}) as tmp:
    found = keys(audit.check_layer_graph(Path(tmp)))
    expect("not-a-leaf:cad/log/" in found,
           "a header-restricted edge fails the moment its package stops being a leaf")

cyclic = {**audit.LAYERS, "core": {**audit.LAYERS["core"], "app": "a mistake"}}
with tree(LAYOUT) as tmp:
    found = keys(audit.check_layer_graph(Path(tmp), cyclic))
    expect(any(k.startswith("cycle:") for k in found), "a cycle in the DECLARED graph is found")


# ── ffi_mirror ──────────────────────────────────────────────────────────────────────────────

HEADER = """
typedef struct { uint32_t struct_size; uint32_t edgePolylines; float p[3]; } CadInfo;
struct CadHost {
    uint32_t struct_size;
    CadStatus (*register_feature)(void* ctx, const CadFeatureDesc* desc);
    CadStatus (*register_command)(void* ctx, const CadCommandDesc* desc);
};
"""

with tree({
    "abi/include/cad/abi/cad_plugin_abi.h": HEADER,
    "tests-rs/cad-sys/src/lib.rs": """
#[repr(C)]
pub struct CadInfo { pub struct_size: u32, pub edge_polylines: u32, pub p: [f32; 3] }

#[repr(C)]
#[derive(Clone)]
pub struct CadHostPrefix {
    pub struct_size: u32,
    pub register_feature: Option<unsafe extern "C" fn(ctx: *mut c_void,
                                                      desc: *const CadFeatureDesc) -> i32>,
}
""",
    # The CadHost SIGSEGV, in miniature: a prefix mirror whose second slot is the wrong function.
    "tests-rs/cad-tests/tests/host.rs": """
#[repr(C)]
pub struct CadHostPrefix {
    pub struct_size: u32,
    pub register_command: Option<unsafe extern "C" fn(ctx: *mut c_void) -> i32>,
}
#[repr(C)]
pub struct NotInTheHeader { pub a: u32 }
""",
}) as tmp:
    result = audit.check_ffi_mirror(Path(tmp))
    found = keys(result)
    expect("tests-rs/cad-tests/tests/host.rs:CadHostPrefix" in found,
           "a prefix mirror with the wrong function in a slot is found")
    expect("tests-rs/cad-tests/tests/host.rs:NotInTheHeader:orphan" in found,
           "a repr(C) struct mirroring nothing is found")
    expect("tests-rs/cad-sys/src/lib.rs:CadInfo" not in found,
           "edge_polylines and edgePolylines are the same slot")
    expect("tests-rs/cad-sys/src/lib.rs:CadHostPrefix" not in found,
           "a correct prefix mirror is quiet, and fn-pointer PARAMETERS are not read as fields")
    expect(result.examined == 3, "three mirrors examined (the orphan is reported, not counted)")


# ── the runner: baseline and floors ─────────────────────────────────────────────────────────

# The C header is present so that ffi_mirror has nothing to say: without it these runs fail on "header
# missing", and the first version of this block did exactly that -- its "an unbaselined finding fails"
# passed for the wrong reason, and the debt case failed for it. Each assertion below names the
# finding it depends on so that cannot happen quietly again.
with tree({"app/src/a.cpp": "auto c = std::make_unique<recompute::MemoryCache>();\n",
           "abi/include/cad/abi/cad_plugin_abi.h": "typedef struct { uint32_t a; } CadX;\n"}) as tmp:
    root = Path(tmp)
    key = ("single_assembly", "app/src/a.cpp:MemoryCache")
    unbaselined = audit.run(root, {}, apply_floors=False)
    expect(not unbaselined["ok"] and [(i["check"], i["key"]) for i in unbaselined["new"]] == [key],
           "an unbaselined finding fails the run, and it is the ONLY reason")
    accepted = audit.run(root, {key: "known"}, apply_floors=False)
    expect(accepted["ok"] and len(accepted["debt"]) == 1, "a baselined finding is reported as debt and passes")
    stale = audit.run(root, {key: "known", ("single_assembly", "gone.cpp:MeshCache"): "paid"},
                      apply_floors=False)
    expect(not stale["ok"] and len(stale["stale"]) == 1, "a baseline entry matching nothing is STALE and fails")
    floored = audit.run(root, {key: "known"}, apply_floors=True)
    expect(not floored["ok"] and floored["problems"],
           "a tree too small to mean anything fails its floors instead of reading as clean")

with tree({"tools/audit/baseline.json": '{"accepted": [{"check": "x", "key": "y", "reason": " "}]}'}) as tmp:
    _, problems = audit.load_baseline(Path(tmp))
    expect(bool(problems), "a baseline entry without a reason is a problem")

print(f"\n{'audit selftest OK' if not failures else f'audit selftest FAILED ({len(failures)})'}")
sys.exit(1 if failures else 0)
