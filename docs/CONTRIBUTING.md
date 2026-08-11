# Contributing

## First checkout

```bash
git clone --recurse-submodules <url> CAD
cd CAD
```

If you forgot `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

`modules/engine` is a submodule and is **required** — `core/recompute/DdcCache` builds
against `assetlib` inside it. CMake will fail at configure time without it.

Then:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
tools/run-tests.sh
```

The first configure builds OCCT from source — roughly 40 minutes. Subsequent ones are
instant. Python bindings additionally need `pip install pybind11 pytest`.

## What is and is not committed

**Committed:** source, CMake, docs, `vcpkg.json` (with its pinned `builtin-baseline`),
`Cargo.lock`, CI config, and any test fixtures under `tests/fixtures/`.

**Never committed:** `build*/`, `vcpkg_installed/`, `tests-rs/target/`, compiled Python
modules, and any geometry a test writes.

`vcpkg_installed/` is the one worth spelling out: it is a complete dependency tree, and for
this project that means OCCT — hundreds of megabytes of static archives, including a single
438 MB `libTKDESTEP.a` in a debug build. It is fully reproducible from `vcpkg.json` plus the
pinned baseline, so committing it buys nothing.

It *was* committed, from the first commit through M2, and the history had to be rewritten
with `git filter-repo` before the repo could be pushed at all: GitHub rejects any file over
100 MB. The repository went from 802 MB to 24 MB. If you are reading this because you cloned
before that rewrite, re-clone rather than merge.

### Global vs. repo gitignore

Keep the distinction sharp, because getting it backwards is how the above happens again:

- **`.gitignore` (repo)** — things about the *project*: its build directory, its dependency
  tree, its generated artifacts. Every contributor has these, so they must be ignored for
  everyone.
- **`~/.gitignore_global`** — things about *you and your machine*: your editor's droppings,
  OS metadata, local tool caches. Other contributors do not have them, so the repo has no
  business listing them.

A build directory belongs in the repo's file, never in a personal one. Relying on a personal
file means the next contributor commits their build output.

Enable a global file with:

```bash
git config --global core.excludesfile ~/.gitignore_global
```

## Before you push

```bash
tools/run-tests.sh
```

Five tiers; see [TESTING.md](TESTING.md). `--quick` skips the property tier for a faster
inner loop, but run the full thing before pushing.

New tests go in **Rust**, driven through the C ABI, unless the thing under test has no ABI
representation (then Catch2) or is the Python binding itself (then pytest). The reasoning is
in [ADR 0006](decisions/0006-testing-tiers.md).

## Things that will bite you

- **OCCT is a static archive.** Internal modules are therefore `STATIC` — never make one
  `SHARED`. Linking OCCT into several shared libraries gives each its own copy of OCCT's
  global state, and the symptom is unrelated-looking geometry failures. See the comment on
  `cad_core_library` in `cmake/CadLayering.cmake`; this cost real debugging time once.
- **`core/` must not include Qt, bgfx or Metal.** Enforced by `tools/check_layering.py` on
  every build. One violation kills the iPad port and the plugin ABI.
- **The C header and `tests-rs/cad-sys/src/lib.rs` change together**, in the same commit,
  with `CAD_ABI_VERSION_MINOR` bumped. The FFI declarations are hand-written on purpose.
- **CI's pinned vcpkg commit must match `builtin-baseline` in `vcpkg.json`**, or CI silently
  tests a different OCCT than you do.
