# Stack — verified August 2026

Everything here was checked against upstream docs/releases in Aug 2026. Re-verify before
each major dependency bump. Do not trust this file after ~6 months.

## Geometry kernel — OCCT 8.0.1

- Latest: **8.0.1**, released 2026-07-30 (first maintenance release of the 8.0 series).
  8.0.0 shipped 2026-05-07. LGPL-2.1 with linking exception.
- **Minimum C++17.** OCCT 8.x uses `if constexpr`, `std::optional`, `std::variant`,
  `std::string_view`, `std::shared_mutex`, structured bindings, fold expressions internally.
  We build at C++20.
- **`Standard_Failure` now inherits `std::exception`.** This directly shapes our kernel
  guard: a single `catch (const std::exception&)` covers OCCT throws and ours.
  Do NOT write the old `OCC_CATCH_SIGNALS` / `Standard_Failure`-only pattern.
- `Standard_Mutex` is gone, replaced by `std::mutex`. Legacy `Sin`/`Cos`/`Sqrt` deprecated.
- **Source tree reorganized** to `src/Module/Toolkit/Package/File`; resources moved to a
  top-level `/resource`. Modules: FoundationClasses, ModelingData, ModelingAlgorithms,
  DataExchange, Visualization, ApplicationFramework, Draw.
- **CMake overhauled**: validated on CMake 3.10+, standards-compliant layout,
  **first-class ARM64 on macOS and Windows**, **vcpkg-compatible**. This is why we use vcpkg.
- **VTK is no longer enabled by default** (`USE_VTK=ON` to opt in). We do not want it in
  core anyway.
- New containers worth using: `NCollection_FlatMap`, `NCollection_OrderedMap`,
  `NCollection_KDTree`. `Size()` returns `size_t` on most collections now.
- Inspector and ExpToCas were split into separate repositories.
- **Data-exchange toolkits were renamed in 7.8** and remain renamed: `TKSTEP` → `TKDESTEP`,
  plus `TKDEIGES`, `TKDESTL`, `TKDEGLTF`, `TKDEOBJ`, `TKDEVRML`, `TKDEPLY`.
  Anything you copy from a pre-7.8 tutorial will not link.
- **`DE_Wrapper` plugin system** (7.8+): OCCT already has a registry where format providers
  self-register and can be loaded without recompiling. Our `Importer`/`Exporter` extension
  point should mirror it, and for OCCT-native formats, delegate to it.
- STEP reading is up to **75% faster than 7.7**; the same work sped up DXF/SAT/Parasolid readers.
- **Build gotcha:** `OpenCASCADEConfig.cmake` ships an EXACT-only version file, so
  `find_package(OpenCASCADE 8.0)` *rejects* 8.0.1. Call `find_package(OpenCASCADE REQUIRED)`
  with no version and compare `OpenCASCADE_VERSION` yourself. Same trap with Eigen below.
- **Verified working**: vcpkg `opencascade` port at 8.0.1, arm64-osx, built from source
  (~40 min). STEP write/read round-trip confirmed by `spikes/occt_smoke`.

## Renderer — bgfx, NOT Diligent

**Decision reversed from the earlier plan.** Diligent Engine is Apache-2.0 for
D3D11/D3D12/Vulkan/OpenGL — but the **Metal backend is commercial-license-only**
(contact Diligent Graphics). Metal is mandatory for both macOS desktop and the future iPad
build, so Diligent would put a commercial dependency on our two most important platforms
before we have revenue.

**bgfx** (BSD-2-Clause) includes the Metal backend with no strings, and its macOS/iOS Metal
support is mature. We take bgfx. Revisit only if we hit a concrete wall bgfx cannot clear.

See `docs/decisions/0002-renderer-bgfx.md`.

## Constraint solver — planegcs

- Extracted from FreeCAD `src/Mod/Sketcher/App/planegcs/`. LGPL-2+.
- FreeCAD stable is **1.1** (released 2026-03-24), 1.2 in preview. Pin the extraction to a
  tagged commit; do not track `main`.
- Validation that this is the right pick: **KiCad announced it is adopting planegcs for
  v11.** It is becoming the de-facto open 2D constraint solver.
- Upstream is actively changing solver behaviour (driven-constraint handling reworked
  Feb 2026, self-tangency guards Jul 2026). Track upstream deliberately, on our schedule.

## GUI — Qt 6.8 LTS (desktop only)

- Qt 6.11 is current (released 2026-03-23). Qt 6.10 is supported until 2026-10-07.
- **6.10 and 6.11 are NOT LTS.** Nearest LTS releases are **Qt 6.8 LTS** and 6.5 LTS,
  and LTS patch releases are commercial-only after the initial open-source phase.
- We target **Qt 6.8 LTS**, dynamically linked and bundled in our installers
  (LGPLv3 relink requirement satisfied by shared libs — see `docs/decisions/0001-qt-linking.md`).
- Qt is used **only** in `shell_qt/`. Never in `core/`, `render/`, or `app/`.

## Eigen — 5.0.1, not 3.x

The pinned vcpkg baseline carries **Eigen 5.0.1**. Its config uses SameMajorVersion
compatibility, so `find_package(Eigen3 3.4)` hard-rejects it. We ask for no version. Nothing
in core depends on Eigen yet; **re-check the Eigen 3 → 5 migration notes before core/sketch
and the solver land** rather than assuming API compatibility.

## Recompute cache — assetlib (ours)

Vendored at `modules/assetlib` as a submodule of the engine repo. BLAKE3-256 content
addressing, two-tier local + shared DDC, atomic ingest with hardlink materialization,
budget LRU eviction by mtime, link-count pinning, SQLite/WAL single-writer registry,
staleness derived exclusively from the key. See `docs/decisions/0004-ddc-recompute.md`
for the CAD-specific modifications (L0 in-memory tier, element-map in the key,
interactive latency budget).

## Simulation — desktop only, out of process

CalculiX (GPL) + Gmsh (GPL) or Netgen (LGPL), driven as child processes so the GPL stays
out of our binary. Not available on iPad (no `fork`/`exec`), which is fine — iPad is the
3D-printing product.

## Platform notes

- macOS/iOS arm64 is first-class in OCCT 8.x CMake — the iPad port is much cheaper than it
  would have been on 7.x.
- iPad plugins, when we get there: JS in a `WKWebView`. That is the only place Apple grants
  JIT on iOS, and it sidesteps App Store §2.5.2 (no downloaded native code).

## CI build time — what actually mattered (Aug 2026)

OCCT compiles from source on every platform; there is no prebuilt 8.0.1 in apt or Homebrew
(Ubuntu 24.04 ships 7.x, Homebrew 7.9.3). That makes the cold build 30–60 minutes per
platform, so the binary cache is not an optimisation — it is the difference between a
workflow people wait for and one they ignore.

**The bottleneck was not OCCT. It was a silently broken cache.**

`VCPKG_BINARY_SOURCES: clear;x-gha,readwrite` looks right and is what most guides recommend,
but `x-gha` talks to the *legacy* Actions Cache API, which GitHub has retired. It fails
silently: no error, no warning, just a full rebuild every run. It went unnoticed through
several runs because a slow CI is indistinguishable from a correctly-slow first run.

Diagnosis: `gh api /repos/OWNER/REPO/actions/cache/usage` reported **0 caches** after
multiple complete runs. That single number is the check worth remembering.

Fix: a plain `files` binary source plus `actions/cache@v4`, which speaks the current cache
service. The archive directory is also inspectable, so the workflow now prints package count
and size after configure — a silent regression here should be visible in the log rather than
inferred from the wall clock an hour later.

Three smaller levers, in descending order of value:

1. **Release-only dependencies** (`cmake/triplets/`). vcpkg builds every port debug *and*
   release by default; we link only release. Roughly halves the cold build.
2. **Scope.** Docs-only pushes skip CI entirely; the sanitizer job does not run on pull
   requests, where it would race the Linux job for the same cold cache.

Two things we deliberately did NOT do, both for the same reason — they only shorten the
*cold* build, which a working cache makes a once-per-baseline-bump event:

- **Dropping OCCT's `freetype` default feature.** It would shed fontconfig, freetype, brotli,
  libpng and gperf. But those are minutes against OCCT's own 30–60, and `USE_FREETYPE=OFF` is
  unverified here: it would fail deep into the OCCT compile, costing a full cycle on three
  platforms per attempt.
- **An overlay port disabling `BUILD_MODULE_Visualization`.** This is where the remaining cold
  time actually lives, but DataExchange's glTF/VRML readers link `TKService`, so it is a real
  experiment rather than a flag flip, and it is maintenance we would carry across every vcpkg
  bump.

Both are worth revisiting if cold builds ever become frequent. Measure before either.
