---
status: as-built
tier: hardened
verified: 2026-08-10
parses-external-input: true
covers:
  - modules/assetlib/
tests:
  - tests/cook_infra_test.cpp
  - tests/fuzz_ddc_manifest_test.cpp
  - tests/fuzz_mesh_loader_test.cpp
  - tests/fuzz_scene_loader_test.cpp  # loadScene, hostile bytes
  - tests/cook_hardening_test.cpp   # result framing, DDC GC, schema versioning
  - tests/cook_deps_test.cpp        # declared inputs must move the cook key
---
# assetlib

## Purpose
Standalone asset-database module: SQLite-backed asset registry (UUIDs,
content hashes, cook state), the cook pipeline, dependency tracking, and file
watching. Built as its own CMake library — it has no dependency on the engine
and already follows the include/src public-header split the rest of the SDK
is moving toward.

## Architecture
- **`AssetRegistry`** — `registry.db` (SQLite, WAL journal). `scan(root)`
  assigns UUIDs to new files and re-hashes changed ones (BLAKE3-256; legacy
  FNV hashes upgraded in place on scan); records carry source path, content
  hash, cooked path, DDC key of the last cook attempt, and state (used by
  the asset browser's badges).
### Cooked asset formats
`mesh_asset` / `texture_asset` / `scene_asset` / `shader_asset` / `material_asset`
are the on-disk containers cookers write and the runtime reads. They live here
rather than in the engine because a cooked format is a contract between the
offline and online halves, and `engine_core` (which hosts the cookers) must stay
GPU-free.

`loadScene` was hardened by `fuzz_scene_loader_test` against the exact two bugs
`loadMesh` had already been fixed for — allocation sized straight from a header
count (2.7 GB from a 3 482-byte file; a 20-iteration run was OOM-killed), and
`return f.good() || f.eof()` accepting a truncated file as success. It carried
both verbatim, which is the lesson worth keeping: a hardening that lands in one
deserializer and not its sibling is a coincidence, not a policy. A third was
unique to it — `stringTableRead` computed `offset + length` in 32-bit, so the
bound wrapped and the read ran off the heap.

`material_asset` v3 added `MaterialTexture::cooked`: a CACHE-RELATIVE path to
the cooked texture, beside the source path the author wrote. It exists because a
shipped dist has no `registry.db`, so a source path resolves to nothing there
and every textured material bound its white fallback. The field is filled by
`engine_build`, never by the cooker — a cooker can run in a worker PROCESS that
receives only source/output/uuid on argv, so it has no registry to resolve
against. Bumping the format version alone would NOT have been enough:
`loadMaterial` rejects an unknown version, so `MaterialCooker::kVersion` was
bumped too, or the DDC would have served unreadable v2 files as up to date.

Every loader treats its input as **untrusted**: cooked blobs travel through a
SHARED DDC, so "another machine wrote this" is the threat model, not a
hypothetical. String lengths are capped before allocating, and offsets into a
payload are bounds-checked against it — a `.cshader` variant slice pointing past
its blob is rejected rather than handed to a GPU driver.

`MeshAsset` carries an optional chain of coarser **LOD levels** (v4; v5 added a
per-level submesh range table). The level count leads the LOD *section* rather than
sitting in `MeshHeader`: the header is a fixed-size block the reader maps directly,
and `static_assert(sizeof(MeshHeader) == 80)` exists to catch exactly the growth that
would silently invalidate every cooked mesh on disk. A level carries its own vertex
and index payload, because a level that shared the parent's buffers would be cheaper
to draw and no cheaper to store — and VRAM is the tighter budget.

**v4 STAYS READABLE.** v5's extra field is written and read behind a version check
rather than sniffed, and a v4 level simply has no range table — which reads as one
range over the whole buffer, exactly what v4 meant. Rejecting those files would break
every project with a populated cache until a full re-cook, for nothing.

**THE LOD SECTION IS THE ONE PLACE A COUNT AND ITS BYTE SIZE ARE STORED SEPARATELY.**
Everywhere else the bytes are derived from the count
(`vertexData.resize(vertexCount * vertexStride)`), so the two cannot disagree; a level
stores `vcount`/`vbytes` and `icount`/`ibytes` as four independent fields. That
shipped unvalidated and unfuzzed, and it mattered because `AssetService` builds
`Mesh lm(lvb, lib, lvl.indexCount)` — handing the count to bgfx as a draw range, so a
level claiming a billion indices over twelve bytes of buffer was a GPU read off the
end. The loader now requires `count * stride == bytes` exactly and clamps every level
range to the level's own index count; `fuzz_mesh_loader_test` generates v4/v5 levels
and has `InflateLod*` corruptions aimed at these fields, because the generic
header-field corruptions cannot reach them.

### The cook layer — one concern per TU
Design doc: **`docs/architecture/asset-cook-architecture.md`** — the key recipe, the
invariants that are load-bearing (and silent when broken), the
transformation-graph target, and the decisions deliberately not taken. Read §5
(invariants) and §6.2 (stage-boundary economics) before changing cook code.

`CookPipeline` orchestrates; each mechanism sits behind its own seam, so a
change to (say) the cache record format can't disturb scheduling or registry
policy. Public headers are `cooker.h` (the cooker contract), `cook_pipeline.h`,
`ddc.h`, `ddc_manifest.h`, `cook_result_file.h`, `task_graph.h`; anything under
`src/` is internal.

## Source layout
**Directory = layer, file = concern.** A reader asking "how does staleness work"
or "where is the Windows spawn" should not have to grep a 700-line file. The four
files that used to answer several questions each (`asset_registry.cpp` 675 lines,
`cook_pipeline.cpp` 469, `cook_dispatch.cpp` 457, `ddc.cpp` 399) are split along
the seams they already had internally — mechanically, with cook keys unchanged.

| unit | concern |
|---|---|
| `registry/schema.cpp` | tables, additive migrations, `PRAGMA user_version`, `open`/`close`. Edited rarely, reviewed carefully — a schema mistake is the one registry error that re-running cannot fix. |
| `registry/records.cpp` | `AssetRecord` ⇄ rows, CRUD, single-record queries. `kCols` and `rowToRecord` sit side by side because they must agree positionally. |
| `registry/dependencies.cpp` | asset→asset edges, cycle rejection, and the dependency SOURCE HASHES that feed cook keys. Explicitly *not* staleness. |
| `registry/scanner.cpp` | the filesystem walk, stat/hash change detection, move detection by content hash. The only registry code that touches the filesystem, and the hot path of a warm editor start. **One registry holds records from several roots** (project assets + engine defaults), so the deleted-file sweep is scoped to the root just walked — an unscoped one had every scan declaring the other root's assets Missing. |
| `registry/asset_names.cpp` | `AssetType` ⇄ name/extension. Pure, no SQLite — what you edit to add a format. |
| `formats/*.cpp` | the on-disk asset containers (mesh, texture, scene, material, shader): read/write, versioned headers, bounds-checked parse. |
| `ddc/hash.cpp` | BLAKE3 over bytes/files, and the ONE function composing a cook key. Pure — what a key covers fits on a screen. |
| `ddc/store.cpp` | the two-tier store: roots, atomic ingest, hardlink materialization, shared→local promotion. Only ever about moving bytes safely. |
| `ddc/gc.cpp` | budget + LRU eviction of the LOCAL tier. Content-addressed blobs have no referrer, so they cannot be reference counted. |
| `ddc/fs_util.cpp` | read-only blob file ops and collision-proof temp naming, shared by store and gc (a second, subtly different `uniqueTempPath` is how two writers land on one file). |
| `ddc/manifest.cpp` | **cached-output record format** — a cook's output set (primary + sibling `.ctex`) as a manifest of per-member content-hashed blobs; all-or-nothing fetch, so a hit never yields a mesh missing its textures. |
| `cook/pipeline.cpp` | per-record machinery: `resolve()` → (cooker, key, paths), `placeOutput()` the single temp→DDC→cache placement, `commitResult()` the single registry writer, plus `cookOne`/`forceRecook`. All caller-thread. |
| `cook/pipeline_batch.cpp` | `cookAll`/`cookGraph` — where stale assets become a cost-weighted task graph, and where the one-query dependency-hash snapshot is taken. |
| `cook/key.{h,cpp}` | **identity + staleness** — DDC key = source hash ⊕ cooker id ⊕ version ⊕ settings ⊕ import settings ⊕ **declared input files** ⊕ **dependency source hashes**; `cookIsStale()` is the whole "is the cooked output already correct?" policy, testable on its own. |
| `cook/dispatch.{h,cpp}` | **execution mode** — isolated child vs in-process behind the exception net. `dispatchCook()` is the seam every cook passes through, and the natural hook for remote/farm execution. |
| `cook/worker_posix.cpp` | the POSIX child: `fork` + `setrlimit` + `execv`, reap with a deadline. Compiles to nothing on Windows. |
| `cook/worker_win32.cpp` | the Windows child: `CreateProcessW` with CommandLineToArgvW-correct quoting, memory cap via a job object applied while suspended. |
| `cook/result_file.cpp` | the sidecar protocol's parse side; framing is shared with the writer in `cook_result_file.h`. |
| `task_graph.*` | **scheduling** — cost-weighted DAG: max-heap ready queue on estimated bytes (longest-first dispatch), dependency edges, memory-budget admission + QoS-demoted workers (the thermal levers live here), a serialized drain lane on the caller thread for `done()` callbacks, cancellation, cycle detection. Dependents release when a task DRAINS (success or failure) — a failed asset never wedges the scenes referencing it. |
| `cook/env.h` | the `COOK_*` env-knob reader shared by the above. |

Both `worker_*.cpp` are listed unconditionally in CMake and guard their own
contents, so neither can rot unnoticed behind a platform nobody builds locally.
- **`DdcStore`** (`ddc.h`) — two-tier content-addressed Derived Data Cache:
  local `~/.engine/ddc` (`ENGINE_DDC`) + optional shared mount
  (`ENGINE_DDC_SHARED`), BLAKE3-256 keys (vendored `third_party/blake3`,
  portable + NEON). Immutable read-only blobs, atomic temp+rename ingest,
  hardlink materialization, shared→local promotion on hit. Multi-output
  cooks are manifests of member blobs (see `src/assets/info.md`).
  **Collected by budget, not by reference.** Keys derive from inputs, so every
  source edit, cooker bump or settings change mints a new key and orphans the
  old blob with no referrer left to notice — reference counting cannot collect
  that, so `collectGarbage(maxBytes, prune)` evicts LRU by mtime (which `fetch`
  touches on a local hit, making the order reflect USE rather than ingest).
  Budget is `ENGINE_DDC_MAX_MB`, default 20 GB; `0` means unbounded.
  Two invariants: a blob hardlinked into a live `.cache` (link count > 1) is
  PINNED and never evicted, because unlinking the store's copy frees zero bytes
  and would re-ingest something in active use; and the SHARED tier is never
  collected from a client, because no client can know what another machine still
  needs. `engine_cook --gc` runs it AFTER the project cache sweep, so dropped
  hardlinks un-pin blobs in the same invocation.
- **`DependencyGraph`** — asset→asset dependencies so cooking can cascade.
- **`FileWatcher`** — change notifications driving re-scan requests.
- **`uuid`** — stable asset identity that survives renames/moves.

## Concurrency Model
WAL mode: exactly one writer connection (the cook thread / CLI) plus any
number of read connections (main thread, panels). Never share one connection
across threads.

## Schema versioning
`PRAGMA user_version` (`kSchemaVersion`). The additive-ALTER migration list is
idempotent — each statement is its own `exec`, so one failing cannot skip the
next — but only the exact `duplicate column name` failure is benign; anything
else is a real error and `open()` now FAILS on it rather than handing back a
registry that silently drops writes. A database whose `user_version` exceeds this
build's is refused outright: a newer engine may have added columns this build
cannot see, and writing would discard them on every `update()`. That matters
specifically when a cache is shared between machines on different builds.

## Consumers
- Engine runtime: read connection opened in `EngineRuntime::init`.
- `CookService` (`src/io`): write connection on the cook thread.
- `engine_cook` CLI: synchronous one-shot cook.

## Dependency invalidation
**The key is the only mechanism.** `cookIsStale` decides staleness from the DDC
key alone and never reads `rec.state` (except to keep a Failed record failed), so
a registry-side "mark dependents stale" cascade would be a no-op — which is why
`transitiveDependents()` is documented as a QUERY, not an invalidation path. A key
is also the only thing that is correct across a SHARED DDC: a cascade is local to
one machine's registry, while a content-derived key means the same thing on every
machine.

Two sources feed `DdcKeyInputs::depHashes`, and both are now populated:

- **`ICooker::declaredInputs()`** — extra FILES a cook reads, hashed by the
  pipeline before the cook. This is the seam that was missing:
  `CookContext::addDependency` takes a UUID and cookers have no registry lookup,
  so a cooker whose extra input was a plain file (a shader's `.sc` sources, the
  `.shader` manifest a material resolves against) had to hand-roll
  `blake3File()` into `settingsFingerprint` — and a cooker that simply forgot was
  indistinguishable from one with no extra inputs. `cook_deps_test` now perturbs
  every declared input of every registered cooker and requires the key to move.
- **Recorded UUID dependencies** (`ctx.addDependency` → the `dependencies` table),
  folded in as the dependency's SOURCE hash. Effective from the SECOND cook, since
  they are discovered during cooking; `declaredInputs` is the pre-first-cook path.
  Snapshotted in ONE query per batch (`allDependencySourceHashes`), never a query
  per asset.

Source hashes rather than the dependency's cooked key, deliberately: folding
cooked identity in would make the fold transitive and drag in things the dependent
provably does not read — a material depends on a shader's declared INTERFACE, not
on its stage sources, and keying materials on those would recook every material in
the project on every shading-code edit. The cost is that a change does not
propagate through two hops on its own; a cooker needing that must declare the far
input too.
