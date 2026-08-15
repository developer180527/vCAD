# Pick up here

Written 15 Aug 2026, at the end of the session that finished step 3b. Everything below was
verified on that commit, not remembered.

**The tree is green and every promise currently marked (RESOLVED) is genuinely tested.** Nothing is
half-wired. You can start clean.

---

## Verified state

| Check | Result |
|---|---|
| C++ tests (`ctest --test-dir build`) | green |
| Rust tests (`cd tests-rs && cargo test --workspace`) | green, `plugin_host.rs` now 25 |
| Layering (`cmake --build build`) | Layering OK |
| Qt shell (`cmake --build build-qt --target vcad`) | builds clean |
| ABI golden snapshot | no drift, regenerated for 1.14 |

Two build directories, on purpose: `build` (core, tests, spikes) and `build-qt` (renderer + Qt
shell). Rust links the library from `build/abi`, so **run `cmake --build build` before
`cargo test`** or the suite tests a stale library.

**Run the Rust suite in PARALLEL at least once before believing it.** A harness bug that only
appears under concurrency passed cleanly under `--test-threads=1` and aborted the process the
moment the full suite ran — see the note at the bottom.

---

## Start here: step 4, unknown-feature preservation (§4A)

**Why first:** it is the last thing that can lose a user's DATA, and it is independent of the
loader — a `.vpart` containing a plugin's feature must open on a machine where that plugin is not
installed, and must not lose the plugin's parameters.

Now genuinely reachable: as of step 3b a plugin feature can exist in a document, so a document
containing one can be saved and reopened without its plugin. Before, there was nothing to preserve.

The shape of it: an unknown feature type keeps its stored parameters verbatim, computes nothing,
and is marked as needing the plugin rather than as broken. Saving must round-trip those parameters
untouched — losing them silently is worse than refusing to open the file, because the user only
finds out after saving over the original.

---

## Then

5. **Error containment** (§5).
6. **The loader** — discovery, manifest, `dlopen` with `RTLD_LOCAL`, lifecycle. Plus the
   compatibility museum and the hostile-plugin test. Built last, deliberately: the loader is the
   first client and a client freezes the design it is built against.
7. **WASM sandbox ADR** — decided in principle (`PLUGIN_CONTRACT.md` §9). Needs: memory ownership
   across the linear-memory boundary, what a handle means when the guest cannot hold a host
   pointer, and the per-recompute performance cost.

Also open, from the performance work and not on the plugin thread at all:

- **`Document::add` is quadratic** (n^1.85 measured). Profiled with samply: the cost is
  `set_length` → `Engine::invalidate` → `Document::replace`, and every `replace` deep-copies the
  whole `std::map` of objects. `Document::dependents()` is an O(n) scan called once per invalidate.
  Both are in `tests-rs/cad-bench/tests/scaling.rs`, `#[ignore]`d with the measurements.
- **The sketch solver is cubic** (n^2.9). 98% of it is `GCS::System::diagnose` running a DENSE
  full-pivot QR. `GCS.h` already offers `EigenSparseQR` and we never set it; and `diagnose()` runs
  on every solve when the constraint set rarely changes between drags.

---

## Things a fresh session will otherwise rediscover the hard way

- **`extern "C" {` wraps the whole ABI header.** Any parser that tracks brace depth never returns to
  zero. This silently gave the golden snapshot 42 macros and *zero functions*, and it passed a
  `len() > 20` guard. The extractor now counts by kind.
- **Add to `CadHost` at the END, never in the middle.** A plugin compiled against an older minor
  computes every earlier member's offset from the layout it saw, so an inserted member shifts all
  of them and silently calls the wrong function pointer. The golden snapshot catches it — it
  reported "3 new declarations — legal" beside "CHANGED: struct CadHost" — but only if you read
  which of the two it said.
- **`withError` forces `Failed`.** `withState(Blocked).withError(e)` therefore produces a *Failed*
  object; only the reverse order gives a blocked one with a message. `withBlocked(e)` exists now so
  nobody has to know that.
- **Every comparison with NaN is false.** A guard written as `if (v <= 0.0) reject;` therefore
  ACCEPTS NaN, and a convergence test written as a threshold declares victory on it. Both bugs
  existed here, in five kernel sites and in the sketch solver. Use `isPositiveFinite` / `isFinite`
  from `kernel/Guard.h`.
- **Kernel API names**, all of which cost a compile cycle to find: `BoxResult` has `.op` (use
  `.op.shape()`), booleans are `booleanCut`/`booleanFuse`, validity is `shape.validate()` returning
  `Result<void>`, `ElementMap` has `size()` and no `empty()`, log level is `Warning` not `Warn`.
- **`nameprimitive` binds the FACES, not the solid.** `nameOf(solid)` correctly returns nothing.
- **`Session` already has a `scratch` string** for strings returned across the boundary. Reuse it.
- **Recompute skips objects that are Clean with an output** (`Engine.cpp`). This is why the Import
  cache fix is only half a fix: the key is now correct, but nothing *notices* a file changed.
- **A test harness must not keep host state in a `static mut`.** The fake plugin did, passed under
  `--test-threads=1`, and SIGABRT'd under the parallel suite. `plugin_ctx` is what a plugin carries
  its state in; a harness that cheats around it is not testing the boundary it claims to.

---

## Open, and unconfirmed by a human

**Does the native Metal surface actually render on screen?** The code landed, the status bar should
read `readback 0.0`, and nobody has looked. If it does not, the offscreen fallback is automatic, so
the shell is not broken either way.

**`--shot` hangs, but only with a document open.** Measured on `f2e5447`: `--shot --home` exits 0
and writes the PNG; `--shot` with a document times out. So the cause is in the viewport render
path, not in the screenshot code — an earlier note here said it had "never worked", which is too
strong and made the bug look unfindable. Evidence: the offscreen path repaints continuously (paints
1–4 render, 5+ arrive with nothing dirty), and Qt drains newly-posted events inside the same
`sendPostedEvents` pass, so the pending zero-timer that calls `grab()` never runs. `sample` puts the
main thread in an ordinary Qt repaint, not in the grab.

`docs/STATUS.md` was last audited 13 Aug and is now well behind, missing the renderer and plugin
threads entirely. Its own header warns that reading it without re-auditing is how it starts lying.
