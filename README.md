# vCAD

[![CI](https://github.com/developer180527/vCAD/actions/workflows/ci.yml/badge.svg)](https://github.com/developer180527/vCAD/actions/workflows/ci.yml)

Cross-platform parametric CAD. Desktop (Windows/macOS/Linux) is the full product; iPadOS is a
stripped-down 3D-printing client on the same core, built later.

**Current state: M1 and M2 complete.** Kernel, topological naming, units, document DAG,
recompute engine, the assetlib DDC, file interchange, the C ABI and Python bindings are all
implemented. Everything passes:

```bash
tools/run-tests.sh
```

Five tiers — layering, C++ unit (Catch2), Rust acceptance through the C ABI, Rust property
tests, Python bindings. See [docs/TESTING.md](docs/TESTING.md).

There is a headless kernel with a Python API, which was the point of M2:

```python
import cad
s = cad.Session(cache_dir="~/.cad-cache")   # on-disk DDC, shared across sessions
box = s.add("Box")
s.set_length(box, "dx", cad.Length.mm(100))
...
s.export_file(box, "part.step")
```

```bash
ctest --test-dir build --output-on-failure
```

## Read these first

| Doc | Why |
|---|---|
| [docs/STACK.md](docs/STACK.md) | Verified dependency facts as of Aug 2026. Re-verify before any major bump; don't trust it after ~6 months. |
| [docs/M0_M1.md](docs/M0_M1.md) | The current milestone checklist and exit criteria. |
| [docs/FORMATS.md](docs/FORMATS.md) | Industry format support, tiers, licensing traps, and the PMI constraint on the document model. |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | First checkout (submodules are required), what is and is not committed, and the traps that have already bitten once. |
| [docs/TESTING.md](docs/TESTING.md) | The four test tiers, why acceptance tests are in Rust, and how to add one. |
| [docs/decisions/](docs/decisions/) | Six ADRs. 0002, 0004, 0005 and 0006 are the load-bearing ones. |

## Layers — the one rule that matters

```
core     -> OCCT, planegcs, assetlib, Eigen.   Nothing else. Ever.
render   -> core + bgfx
app      -> core + render
shell_qt -> app + Qt 6.8 LTS     (desktop)
shell_ios-> app + SwiftUI/Metal  (later)
```

`core/` is what compiles for iPadOS, what the plugin C ABI is carved from, and what the
headless test suite exercises. One Qt include in `core/` kills the iPad port and makes the
plugin ABI unshippable. Enforced by [tools/check_layering.py](tools/check_layering.py),
which runs as part of every build.

```bash
python3 tools/check_layering.py .
```

## Stack

OCCT 8.0.1 · planegcs (FreeCAD 1.1, pinned) · assetlib (ours) · **bgfx** (not Diligent — see
[ADR 0002](docs/decisions/0002-renderer-bgfx.md)) · Qt 6.8 LTS · lib3mf · CalculiX
out-of-process · CPython + C ABI plugins (desktop only).

## Build

Requires a vcpkg checkout and CMake 3.24+.

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

No submodules — `assetlib` is vendored at `modules/assetlib`.

Python bindings need `pip install pybind11` — deliberately from the interpreter you intend
to bind to, not from vcpkg (whose port builds a whole CPython, and fails on arm64-osx).

Then:

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

The vcpkg baseline is pinned to `dc31f86a`, which supplies OCCT **8.0.1** and Eigen **5.0.1**.
Building OCCT from source takes roughly 40 minutes the first time.

Two version traps, both already handled in `CMakeLists.txt`: OCCT's and Eigen's CMake config
files reject a *newer* patch/major than requested, so neither `find_package` call carries a
version argument — the version is checked afterwards instead.

## Milestones

- **M0** de-risk (4–6 wk) — spikes in `spikes/`, incl. OCCT on a physical iPad and bgfx Metal
- **M1** kernel wrapper + topological naming (6–8 wk) — **the gate for the whole project**
- **M2** document + recompute + DDC (6–8 wk) — **complete.** Persistent document with
  free undo, recompute engine, two-tier cache over assetlib's DDC, STEP/IGES/STL
  interchange, the C ABI, and Python bindings.
- **M3** renderer (8–10 wk) · **M4** Qt shell (8–10 wk) · **M5** sketcher (10–12 wk) ·
  **M6** features + plugin ABI v1 (12+ wk)

M1 is done when the acceptance suite passes under ASan on all three desktop platforms.
See [docs/M0_M1.md](docs/M0_M1.md) for per-item status and the five design corrections the
tests forced.
