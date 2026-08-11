# Testing

One command:

```bash
tools/run-tests.sh
```

`--quick` skips the property tier; `--asan` runs the C++ tiers under AddressSanitizer +
UBSan from `build-asan/`.

## Why the suite is split across two languages

The C++ tests exercise the core's **internals**. The Rust tests exercise the **C ABI** —
`core/abi/include/cad/abi/cad_plugin_abi.h` — which is the surface that third-party plugins,
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

Planned and not yet built, in the order they earn their place:

| Tier | Trigger to build it |
|---|---|
| **Golden geometry** | When we have file import (M2 io). Compare content hashes of imported reference parts against a committed manifest; a diff means a kernel or naming change moved geometry. |
| **Performance** | When the renderer lands (M3). `criterion` benchmarks over recompute latency and cache hit rate, gated in CI on a regression threshold rather than an absolute number. |
| **Fuzzing** | When file import lands. `cargo-fuzz` against the ABI's string-taking entry points and the STEP/3MF readers — parsers of foreign files are where malformed input meets C++. |
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
- Nothing throws across the boundary. `Session.cpp` wraps every export.
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

`.github/workflows/ci.yml` runs the whole thing on macos-14, ubuntu-24.04 and windows-2022,
plus a separate ASan + UBSan job. The vcpkg commit is pinned there and **must match
`builtin-baseline` in `vcpkg.json`** — otherwise CI silently tests a different OCCT than
developers do.
