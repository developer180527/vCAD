#!/usr/bin/env python3
"""Fail the build if a layer includes something it must not.

The rule this protects:

    core    -> OCCT / planegcs / assetlib / Eigen / stdlib only
    render  -> core + RHI
    app     -> core + render
    shell_* -> app + its own toolkit

Violating it is not a style problem. `core` is what compiles for iPadOS, what the plugin
C ABI is carved out of, and what the headless test suite exercises. A single Qt include in
`core` costs months later.

Run standalone:  python3 tools/check_layering.py .
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".h", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Forbidden include prefixes per layer directory.
FORBIDDEN: dict[str, list[str]] = {
    "core": [
        "Q",            # Qt: QtCore/, QWidget, QString...
        "bgfx/", "bx/", "Diligent",
        "Metal/", "MetalKit/", "QuartzCore/",
        "vulkan/", "GL/", "GLFW/", "d3d11", "d3d12",
        "vtk", "imgui",
        "cad/render/", "cad/app/", "cad/shell",
    ],
    "render": [
        "Q",
        "cad/app/", "cad/shell",
    ],
    "app": [
        "Q",
        "bgfx/", "bx/",   # app talks to render through cad/render, not the RHI directly
        "cad/shell",
    ],
    # The C facade over core + render. May use both; may not reach a UI toolkit or the RHI,
    # because everything it exposes has to be callable from Swift and from a plugin.
    "abi": [
        "Q",
        "bgfx/", "bx/", "Diligent",
        "cad/shell",
    ],
}

# Narrow, deliberate exemptions. Each needs a comment justifying it.
EXEMPT_FILES: set[str] = set()


def layer_of(path: Path, root: Path) -> str | None:
    try:
        rel = path.relative_to(root)
    except ValueError:
        return None
    parts = rel.parts
    if not parts:
        return None
    top = parts[0]
    if top == "core":
        return "core"
    if top == "render":
        return "render"
    if top == "app":
        return "app"
    if top == "abi":
        return "abi"
    return None


def main(root_arg: str) -> int:
    root = Path(root_arg).resolve()
    violations: list[str] = []

    for path in root.rglob("*"):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        if any(part in {"build", "third_party", ".git", "modules"} for part in path.parts):
            continue
        layer = layer_of(path, root)
        if layer is None:
            continue
        rel = str(path.relative_to(root))
        if rel in EXEMPT_FILES:
            continue

        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for inc in INCLUDE_RE.findall(text):
            for bad in FORBIDDEN.get(layer, []):
                # "Q" is a prefix rule for Qt headers, which are Q-prefixed or QtFoo/.
                if bad == "Q":
                    if re.match(r"^Q[A-Z]", inc) or inc.startswith("Qt"):
                        violations.append(f"{rel}: layer '{layer}' includes Qt header <{inc}>")
                    continue
                if inc.startswith(bad):
                    violations.append(f"{rel}: layer '{layer}' includes forbidden <{inc}>")

    if violations:
        print("Layering violations:", file=sys.stderr)
        for v in sorted(set(violations)):
            print(f"  {v}", file=sys.stderr)
        print(
            "\nSee cmake/CadLayering.cmake for the rule and why it exists.",
            file=sys.stderr,
        )
        return 1

    print("Layering OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
