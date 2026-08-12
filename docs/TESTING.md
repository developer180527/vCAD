# Testing

One command:

```bash
tools/run-tests.sh
```

`--quick` skips the property tier; `--asan` runs the C++ tiers under AddressSanitizer +
UBSan from `build-asan/`.

## Why the suite is split across two languages

The C++ tests exercise the core's **internals**. The Rust tests exercise the **C ABI** —
`abi/include/cad/abi/cad_plugin_abi.h` — which is the surface that third-party plugins,
the future SwiftUI iPad shell, and any language binding will use.

That split is the point, not an accident of tooling. A test suite that only ever calls C++
directly will not notice the day the ABI stops working, and the people who find out will be
plugin authors. Driving the acceptance tier from another language means:

- an ABI regression fails our own tests first;
- the ABI is exercised by someone who cannot cheat — Rust genuinely cannot reach into a C++
  type, so anything the tests need must be properly exposed;
- the safe wrapper in `tests-rs/cad/` doubles as the reference for how a binding should
  consume the ABI. If writing a test there is awkward, the ABI is wrong.

## Tiers

Cheapest first, so failures surface early. `tools/run-tests.sh` runs them in this order.

| # | Tier | Where | Runtime | What it protects |
|---|---|---|---|---|
| 1 | **Layering** | `tools/check_layering.py` | ~0.1 s | `core/` never includes Qt, bgfx or Metal. One violation kills the iPad port and the plugin ABI. |
| 2 | **C++ unit / acceptance** | `tests/` (Catch2) | ~3 s | Kernel wrapper, topological naming, units. Internals with no ABI representation. |
| 3 | **Rust acceptance** | `tests-rs/cad-tests/tests/m2_*.rs` | ~0.2 s | Document DAG, recompute, cache behaviour, undo — through the ABI. |
| 4 | **Rust property** | `tests-rs/cad-tests/tests/prop_*.rs` | ~3 s | Invariants over generated inputs, with shrinking. |
| 5 | **Python bindings** | `bindings/python/tests/` (pytest) | ~1 s | The Python surface only — that errors arrive as exceptions, that the objects are idiomatic, that the docstring example is true. |

Tier 5 is the one exception to "new tests go in Rust", and it is a necessary one: a Python
API cannot be exercised from Rust. It deliberately does **not** re-test geometry or
recompute semantics — those are covered once, in Rust, through the ABI. What it tests is
that the *binding* behaves like Python: a failed call raises rather than returning a code,
and a failed **feature** does not raise, because a partly-broken model must stay openable.

Planned and not yet built, in the order they earn their place:

| Tier | Trigger to build it |
|---|---|
| **Golden geometry** | Now possible — import landed. Compare content hashes of imported reference parts against a committed manifest; a diff means a kernel or naming change moved geometry. Blocked only on choosing licence-clean reference files. |
| **Performance** | When the renderer lands (M3). `criterion` benchmarks over recompute latency and cache hit rate, gated in CI on a regression threshold rather than an absolute number. |
| **Fuzzing** | Now possible — import landed. `cargo-fuzz` against the ABI's string-taking entry points and the STEP/IGES readers. Parsers of foreign files are where malformed input meets C++, and `cad_import_probe` is a ready-made entry point. |
| **Render regression** | With M3. Golden images with a perceptual diff. |
| **UI** | With M4. Deliberately last and deliberately thin. |

## Writing tests

**New tests go in Rust unless they cannot.** They cannot when the thing under test has no
ABI representation — the element-map internals, the `Result`/`guard` behaviour, `Quantity`'s
compile-time dimension checking. Those stay in Catch2.

If you find yourself wanting to test something through the ABI and the ABI does not expose
it, extend the ABI. That is usually the right answer: if a test needs it, a plugin author
will too.

### Property tests

Assert **invariants**, never specific values. `volume(cut) <= volume(base)` is a property;
`volume == 240000` is an example. The rule that matters most in a geometry system:

> An operation is allowed to **fail**. It is not allowed to **succeed and produce an invalid
> shape**, or to succeed and lose a name.

A silently invalid solid poisons everything downstream and is very hard to trace back to its
origin, so the property tests check the success path's validity rather than merely its
existence. Every failure path is checked to produce a legible message, because those same
messages reach users.

Keep `cases` modest (64 is the current default) — each case drives OCCT, and a suite nobody
runs before committing protects nothing.

## The ABI contract, for test authors

- Handles are opaque `u64`. Zero is null. The core validates them; passing a stale handle
  gets `CAD_ERR_BAD_HANDLE`, not a crash.
- Returned strings are **valid only until the next call on that session**. The safe wrapper
  copies immediately; do not hold a `*const c_char`.
- Nothing throws across the boundary. `abi/src/Session.cpp` wraps every export.
- `tests-rs/cad-sys/src/lib.rs` is hand-written, not bindgen-generated. Change it and the C
  header in the **same commit**, and bump `CAD_ABI_VERSION_MINOR`. That is deliberate: an
  accidental header change should be a compile error, not a silent regeneration.

## Build order

Cargo does **not** invoke CMake. Driving a 40-minute OCCT build from `cargo test` makes the
Rust suite hostage to the C++ build's state and produces incomprehensible failures. The
contract is explicit:

```bash
cmake --build build -j     # first
cd tests-rs && cargo test  # then
```

`cad-sys/build.rs` fails with that exact instruction if `libcad_abi` is missing. Point
`CAD_BUILD_DIR` at a different build directory to test another configuration.

On macOS the dylib carries an absolute install name so test binaries find it without any
environment setup. Linux and Windows need `LD_LIBRARY_PATH` / `PATH` — `tools/run-tests.sh`
and the CI workflow handle it.

## CI

`.github/workflows/ci.yml`, three jobs:

- **lint** (seconds, no compiler) — layering, `cargo fmt`, and two consistency checks that
  exist because they are otherwise invisible until something breaks far away: the C header's
  `CAD_ABI_VERSION_MINOR` must equal `cad-sys`'s, and `vcpkg.json`'s `builtin-baseline` must
  equal the workflow's pin. Both run first so a five-second failure does not wait behind an
  hour-long OCCT build.
- **test** — the full suite on ubuntu-24.04 (gcc), windows-2022 (MSVC) and macos-14 (arm64).
  `fail-fast: false`, so one platform failing does not hide the others.
- **sanitizers** — ASan + UBSan on Linux. `detect_leaks=0`: OCCT allocates plenty it never
  frees at exit, and leak reports would drown the signal we actually want.

The vcpkg binary cache is **load-bearing, not an optimisation**. A cold OCCT build is 30–60
minutes; without the cache this workflow is unusable.

### Platform notes that cost real time

- **Shared-library lookup differs three ways.** macOS finds `libcad_abi` via the absolute
  install name baked into the dylib; Linux needs `LD_LIBRARY_PATH`; Windows needs the DLL's
  directory on `PATH`. This lives in the workflow rather than `cad-sys/build.rs` because
  Cargo's `rustc-link-arg` only applies to the crate whose build script emits it — it never
  reaches a downstream crate's test binary.
- **Windows exports nothing by default.** ELF and Mach-O export all symbols; PE exports none.
  Hence `CAD_API` in `cad_plugin_abi.h` and `CAD_ABI_BUILD` on the library target. Without
  them the DLL builds fine and every consumer fails to link.
- **`submodules: recursive` in checkout is mandatory** — `modules/engine` provides assetlib,
  and configure fails without it.
- **`M_PI` is not standard C++.** MSVC hides it behind `_USE_MATH_DEFINES`. `core/units` uses
  `std::numbers::pi`.
- **`popen`/`pclose` are `_popen`/`_pclose` on MSVC**, and `cmd.exe` has no
  `VAR=value command` prefix — the cross-process determinism test sets the variable in its
  own environment instead.
