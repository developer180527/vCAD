# Pick up here

Written 14 Aug 2026 at commit `5c22370`, at the end of a long session. Everything below was
verified on that commit, not remembered.

**The tree is green and every promise currently marked (RESOLVED) is genuinely tested.** Nothing is
half-wired. You can start clean.

---

## Verified state

| Check | Result |
|---|---|
| C++ tests (`ctest --test-dir build`) | **27 / 27** |
| Rust tests (`cd tests-rs && cargo test --workspace`) | **114 passing, 0 failing** |
| Layering (`cmake --build build`) | Layering OK |
| Qt shell (`cmake --build build-qt --target vcad`) | builds clean |
| Scale spike (`spike_scale 4096 20`) | all claims hold |
| ABI golden snapshot | no drift |

Two build directories, on purpose: `build` (core, tests, spikes) and `build-qt` (renderer + Qt
shell). Rust links the library from `build/abi`, so **run `cmake --build build` before
`cargo test`** or the suite tests a stale library.

---

## Start here: `shape_faces` / `shape_edges`

**Why first:** it is small, it unblocks tests that had to be deleted, and step 3 needs it anyway.

Right now `element_resolve` and `element_name_of` are wired and tested — but only for their
*negatives*. A plugin has **no way to obtain a sub-shape handle**, so it cannot name a face in the
first place. Until this lands, the naming guarantee is theoretical, which is FreeCAD's failure in a
different costume. `PLUGIN_CONTRACT.md` §3.4 is marked **(PARTIAL)** for exactly this.

What to add to `CadHost` in `abi/include/cad/abi/cad_plugin_abi.h`:

```c
CadStatus (*shape_sub_count)(void* ctx, CadShape s, uint32_t kind, uint32_t* out);
CadStatus (*shape_sub_at)(void* ctx, CadShape s, uint32_t kind, uint32_t index, CadShape* out);
```

Count-then-index, matching every other list in this ABI, so neither side owns or frees an array.
`kind` wants `CAD_SUB_FACE` / `CAD_SUB_EDGE` / `CAD_SUB_VERTEX` with **explicit numeric values** —
they will end up in documents.

Implement in `abi/src/Session.cpp` beside `hostElementNameOf`. The stored shape already carries its
`ElementMap` (`Session::StoredShape`), so a face obtained this way can be named immediately.

Then **restore the three tests deleted from `tests-rs/cad-tests/tests/plugin_host.rs`**: name a
face, resolve it back, and resolve it by its text form alone with the digest zeroed. Their bodies
are in the commit message of `50539f8` if you want the original phrasing.

---

## Then: step 3b — `register_feature` and the compute accessors

The last thing blocking a plugin that does something. Largest piece so far; do not start it in a
tired session.

**Design already settled** (`PLUGIN_CONTRACT.md` §7.2) — do not re-derive it:

- `CadComputeCtx` is an opaque handle indexing `Session::computes`, **not** a pointer to a
  `ComputeContext`. A plugin that stores one and uses it later must get a clean rejection, not a
  dangling reference into a frame that has returned. The map and the `ActiveCompute` struct already
  exist in `Session.cpp`, unused.
- **The compute context is read-only by construction.** No `compute_set_param`, no document handle,
  no transaction reachable from it. §4.1's determinism rule is enforced by the *shape of the API*,
  not by asking politely. Do not add a convenience that breaks this.
- `compute_fail(cc, message, detail)` exists because a `CadStatus` is a code and a code cannot say
  "the flange radius is larger than the material allows". Route it into the same Failed/Blocked
  path built-ins use, so a plugin's failures are as legible as a built-in's.
- Parameters arrive with the feature: `register_feature(ctx, desc, params, param_count)`.

**Bridge shape:** `register_feature` builds a `recompute::FeatureType` whose `compute` lambda opens
an `ActiveCompute`, calls the plugin's function pointer, then takes the output handle and names it.
`FeatureType::externalInputs` should be fed from the descriptor's new `external_inputs` callback —
that wiring is why step 2 was done first.

**Naming the output:** built-ins use `naming::NamingContext(ctx.namingSerial, 0).nameprimitive(...)`
— see `computeBox` in `core/recompute/src/Features.cpp:89`. A plugin's compute output should take
its feature's serial, not a per-shape one (`hostMakeBox` currently uses a per-shape serial because a
host-built shape has no feature to belong to; that is a deliberate stopgap, noted in the code).

**Finish with an in-process fake plugin** in the Rust suite: register a feature, compute a box from
a parameter, and assert the output lands in the document *with names attached*. That is the first
end-to-end proof the plugin stack works.

---

## After that

4. **Unknown-feature preservation** (§4A) — a `.vpart` opens without the plugin that made it and
   loses nothing. Failure must not nuke the user's *data*. Independent of the loader.
5. **Error containment** (§5).
6. **The loader** — discovery, manifest, `dlopen` with `RTLD_LOCAL`, lifecycle. Plus the
   compatibility museum and the hostile-plugin test. Built last, deliberately: the loader is the
   first client and a client freezes the design it is built against.
7. **WASM sandbox ADR** — decided in principle (`PLUGIN_CONTRACT.md` §9). Needs: memory ownership
   across the linear-memory boundary, what a handle means when the guest cannot hold a host
   pointer, and the per-recompute performance cost.

---

## Things a fresh session will otherwise rediscover the hard way

- **`extern "C" {` wraps the whole ABI header.** Any parser that tracks brace depth never returns to
  zero. This silently gave the golden snapshot 42 macros and *zero functions*, and it passed a
  `len() > 20` guard. The extractor now counts by kind.
- **Kernel API names**, all of which cost a compile cycle to find: `BoxResult` has `.op` (use
  `.op.shape()`), booleans are `booleanCut`/`booleanFuse`, validity is `shape.validate()` returning
  `Result<void>`, log level is `Warning` not `Warn`.
- **`nameprimitive` binds the FACES, not the solid.** `nameOf(solid)` correctly returns nothing.
  Three tests were written on the wrong assumption.
- **`Session` already has a `scratch` string** for strings returned across the boundary. Reuse it;
  adding a second is a duplicate-member compile error.
- **Recompute skips objects that are Clean with an output** (`Engine.cpp:296`). This is why the
  Import cache fix is only half a fix: the key is now correct, but nothing *notices* a file changed.
- **`--shot` has never worked on this machine**, before or after the renderer landed. There is a
  second cause still unfound. It forces the offscreen path (`Viewport::setForceOffscreen`), so
  screenshots never exercise the native presentation route.

---

## Open, and unconfirmed by a human

**Does the native Metal surface actually render on screen?** The code landed, the status bar should
read `readback 0.0`, and nobody has looked. If it does not, the offscreen fallback is automatic, so
the shell is not broken either way — but the 27 fps ceiling is still there until someone confirms.

`docs/STATUS.md` was last audited 13 Aug at commit `56e16e4` and is now **ten commits stale**,
missing the entire renderer and plugin threads. Its own header warns that reading it without
re-auditing is how it starts lying. Update it after step 3.
