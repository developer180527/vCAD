# 0004 — assetlib DDC is the recompute cache

Status: accepted (Aug 2026)
Consumers: `core/recompute`, `render` (the mesh cache's blob store), and the `abi` session's cache tiers.

## Mapping

| assetlib | CAD core |
|---|---|
| `ICooker` | `Feature::compute()` — pad, fillet, loft |
| DDC key inputs | feature type + version + property hash + input shape keys + element-map identity |
| `declaredInputs()` | referenced external files (imported STEP, library part) |
| `ctx.addDependency` (stores the dep's *source* hash) | DAG edges between features |
| Registry UUIDs / cook state | document object IDs / recompute state |
| Cooked blob | serialized shape (BinTools) + tessellation + element map |
| Shared mount | **team cache** — CI or a colleague already rebuilt this assembly |

The assetlib rule that a dependency is recorded as the dependency's *source* hash rather than
its cooked key — "a material depends on a shader's declared INTERFACE, not its stage sources"
— maps directly and is better than what FreeCAD does: a fillet depends on its input shape's
key, so an upstream change producing identical geometry does not invalidate downstream work.

## Three required modifications

1. **Latency.** Cooking tolerates process spawn and SQLite round-trips; dragging a dimension
   does not (~16–50 ms budget). Add an **in-memory L0 tier** (hash → live shape, no
   serialization) in front of the existing local L1 disk tier; write through to L1 on a
   background thread. The `dispatch`/`worker_*` out-of-process path is **wrong** for feature
   recompute and **right** for FEA and heavy import/tessellation. Keep both.

2. **Element identity in the key.** A fillet's key is not "input shape hash + radius" but
   "+ *which edges*", and which-edges is an `ElementName`, not an index. Two shapes that hash
   identically as geometry may carry different element maps from different histories.
   Decision: the shape content hash **includes** the element map (safer, fewer hits).
   Revisit if hit rates disappoint.

3. **Pinning under interactive use.** Disk pinning via link-count > 1 still works. L0 needs
   its own: anything reachable from the open document's live DAG is unevictable, and eviction
   must respect iOS jetsam pressure notifications, not only a byte budget.

## Implementation notes (Aug 2026)

Landed as `cad::recompute::DdcCache` over `assetlib::DdcStore`, behind `TieredCache`
(MemoryCache L0 -> DdcCache L1). assetlib vendored as a submodule at `modules/engine`;
it is genuinely standalone, with BLAKE3 and SQLite vendored inside it, so spike 0.5's
assumption held.

Three things the implementation forced that the design note did not anticipate:

1. **The element map has to be serialised with the shape.** A cached shape without its
   names is *worse* than a cache miss: every downstream reference into it fails to resolve,
   so the cache silently breaks the guarantee M1 exists to provide. `cad::io::serialize`
   writes both; `a_cached_shape_keeps_its_element_names` is the regression test.

2. **The serialisation version belongs in the KEY, not only in the blob header.** With it
   only in the header, every client decodes-and-discards on the first access after a format
   change. In the key, old blobs simply become unreachable.

3. **A blob that fails to decode must be a MISS, never an error.** A cache is an
   optimisation; it must not be able to fail a build. `DdcCache::get` counts these
   separately (`unreadable()`) and evicts the local copy, leaving the shared tier alone —
   other machines may be serving from it, and deleting there is not ours to do.

Still open: the L1 write is synchronous. ADR 0004 calls for a background writer once the
interactive path is measured; that needs its own lifetime and ordering rules and should be
bought with evidence rather than assumption.

---

## Amendment (12 Sep 2026) — content addressing lost to naming, and how to get it back

This ADR's premise is that a result is keyed by its CONTENT, so anything computed once is served
everywhere it recurs. That premise is currently false, deliberately and for a good reason, and the
cost was not recorded when it was paid.

`a91b3d4` mixed the object's own id into `Engine::cacheKeyOf`. That was a real fix: the id IS
`ComputeContext::namingSerial`, the compute stamps it into every element name it produces, and
without it two identical boxes shared one cached `Output` — all 26 element names came back
identical, a reference to one body's face resolved in the other, and every boolean between two
identical bodies failed with a naming collision. `tests/acceptance/cache_key_identity.cpp` holds
that line.

**The consequence: two features that differ only by identity can no longer share anything.** With
the id in the key they are not identical. The same follows one layer down, because a mesh is keyed
by content that includes its element names — so N identical parts tessellate N times.

What that costs is this project's central scale claim. Three tests in the Rust suite assert the lost
property and are now `#[ignore]`d, each naming this amendment:

- `m2_recompute::identical_geometry_shares_cache_entries_across_object_ids`
- `m3_tessellation::identical_parts_tessellate_once` — which calls itself "THE scale assertion"
- `plugin_host::identical_plugin_features_share_a_cache_entry`

They are ignored rather than rewritten because the property is still wanted. **Removing those three
ignores is the definition of done.**

The fix is not to choose between the two. It is to stop them being one question, using the trick
the renderer already uses a layer down — `render/include/cad/render/Backend.h`: "the mesh stores
element SLOTS; the instance stores its base." Cache the geometry and a RELATIVE element map, keyed
by content with no serial in it, and stamp the retrieving object's serial onto the names at
retrieval. Identical boxes then share one computation and still get distinct names.

That touches naming, the cache key and the mesh cache together, which is why it is written down
here rather than attempted alongside the change that found it.
