# 0007 — Renderer architecture and the swap seam

Status: accepted (Aug 2026), amended — see "Amendment" at the end

## Context

M3 needs a viewport. The MVP will later be replaced by the renderer in
`developer180527/engine` (`src/render/`), which is clustered-forward, targets integrated
GPUs, and is considerably more capable than anything we would write here.

Three facts from reading that renderer changed this design:

1. **It is already bgfx.** `vertex.h`, `mesh.h` and `render_view.h` all include
   `<bgfx/bgfx.h>`. vCAD independently chose bgfx in [ADR 0002](0002-renderer-bgfx.md)
   because Diligent's Metal backend is commercial-only. Same RHI on both sides.
2. **It already consumes assetlib.** `src/render/asset_registry.h`, `cooked_texture.h`
   and `gpu_resource_cache.h` sit alongside the renderer. Our DDC integration
   ([ADR 0004](0004-ddc-recompute.md)) is the same cache.
3. **Its own architecture doc identifies the seam trap**, and it is the exact trap this ADR
   has to avoid: *"the only seam is `IRenderPipeline`: swap it and you inherit nothing — no
   culling, no sorting, no light handling. That is a facade."*

## Decision 1 — the MVP renderer is C++, not Rust

**No Rust for the renderer.** Rust stays where it already earns its place: the test suite,
driving the core through the C ABI ([ADR 0006](0006-testing-tiers.md)).

The deciding argument is not language preference, it is what survives the swap:

| | C++/bgfx MVP | Rust/wgpu MVP |
|---|---|---|
| RHI shared with destination | **yes** — same bgfx | no — wgpu vs bgfx |
| Vertex layouts, buffer mgmt, shader conventions | **carry over** | discarded |
| Swap shape | incremental, subsystem by subsystem | wholesale rewrite |
| Tessellation data path | direct span over OCCT-owned memory | copy or unsafe view per frame |
| Release build story | unchanged | cargo becomes a release dependency |

A Rust MVP would share *nothing* with its replacement — different language, different RHI,
different vertex layout. Everything written would be thrown away. A C++/bgfx MVP lets the
engine renderer be adopted **piece by piece**: its `Vertex`/`Mesh` shapes, its
`gpu_resource_cache`, then its pipeline, while the CAD-specific parts we must own regardless
(edge rendering, ID-buffer picking, section planes) stay put.

wgpu is genuinely better technology than bgfx in isolation. It is the wrong choice *here*
because of where we are going.

## Decision 2 — the seam is three narrow interfaces, not one god-interface

Learning directly from the engine's own critique. A single `IRenderer::render(scene)` is a
facade: whoever implements it inherits nothing and reimplements everything.

Instead the responsibilities split by **who owns the data**:

```
core (document, kernel)                 — owns shapes and element names
  │
  ▼
render/tessellate   Shape -> RenderMesh  — OURS, always. Content-addressed, DDC-cached.
  │                                        Pure CPU. No GPU types.
  ▼
render/scene        RenderMesh -> Frame  — OURS, always. Flat POD draw streams,
  │                                        camera, selection, section planes.
  ▼
IGpuResources  │  IFrameSink  │  IPicker  — THE SEAM. Three interfaces, POD only.
  │                                          MVP implements all three over bgfx.
  ▼                                          Engine renderer implements them later.
GPU
```

- **`IGpuResources`** — upload/release vertex, index and texture buffers. Keyed by content
  hash, so an unchanged mesh is never re-uploaded. This is the interface the engine's
  `gpu_resource_cache` already almost is.
- **`IFrameSink`** — consume one `SceneFrame` and present. The only per-frame call.
- **`IPicker`** — resolve a screen position (or rectangle) to element ids. Separate because
  it needs an offscreen target and a readback, which is a GPU concern the scene layer
  cannot express.

Everything above the seam is ours permanently and is where the CAD-specific value lives.
Everything below is replaceable. **Tessellation and scene assembly are explicitly NOT in the
seam** — that is the mistake `IRenderPipeline` made.

## Decision 3 — no shared-memory IPC; share by immutability instead

"Shared memory" is the right instinct about *avoiding copies*, and the wrong mechanism for
*this* problem.

In-process, the requirement is that tessellation output is produced once, lives in one place,
and reaches the GPU once. That is satisfied by `std::shared_ptr<const RenderMesh>` plus
upload-by-content-hash — zero copies, no IPC, no lifetime ambiguity. The engine renderer,
being in the same process, takes a `std::span` over the same bytes.

Out-of-process rendering is deliberately rejected for an interactive viewport: it adds a
frame of input latency to a tool where pointer latency is the product, complicates GPU
context ownership, and buys isolation we do not need.

If it is ever wanted anyway, the content-addressed design already permits it: a `RenderMesh`
is a pure function of (shape hash, tessellation settings), so the DDC blob *is* the transport.
The design allows this; we are not building it.

## Decision 4 — our own vertex layout, converted at upload

CAD geometry is large, static and untextured. The engine's `Vertex` is 48 bytes
(position, normal, tangent, uv); tangent and uv are dead weight for us — half the bandwidth
on the one thing we have a lot of.

Ours:

```
CadVertex { float position[3]; float normal[3]; uint32_t element; }   // 28 bytes
```

`element` is the index into the frame's element table, which is what makes GPU ID-buffer
picking work — and it has no equivalent in a game renderer.

The backend declares the layout it wants; the scene layer converts once at upload and the
result is cached by content hash. Adapting to the engine's `Vertex` later is therefore a
conversion function, not a redesign.

## Decision 5 — edges are a first-class draw stream

The single biggest visual difference between "a 3D view" and "a CAD viewport" is crisp
feature and silhouette edges. They are not a post-process: OCCT gives us exact edge
polylines, and drawing them as line primitives is both faster and vastly better-looking than
any screen-space edge detection.

A game renderer has no concept of this. It stays ours permanently, above the seam.

## Consequences

- The MVP is throwaway *below* the seam and permanent *above* it. That ratio is the point.
- We must keep bgfx as the RHI. Switching to wgpu later would break the shared-RHI argument
  that makes the swap cheap.
- CI has no GPU. A `NullBackend` implementing the three interfaces by recording draw calls
  makes the scene layer testable in Rust through the ABI without a GPU — see
  [docs/M3.md](../M3.md). Pixel-level regression needs a software rasteriser and is deferred.
- The iPad shell will need the seam reachable from Swift. It is POD-only by construction, so
  a C facade over it is mechanical. Not built yet.


---

# Amendment (Aug 2026): swap requirement dropped, scale requirement added

Two changes from the original brief. The first invalidates an argument this ADR leaned on;
the second changes the data model.

## The engine-swap requirement is withdrawn

"No need to make it swappable by engine's renderer — we could develop the renderers
independently."

That removes the main justification for Decision 2. Rather than quietly keep a design
defended by a dead requirement, here is what actually survives and why:

- **Decision 1 (C++, not Rust) — still holds, weaker reasons.** The shared-bgfx argument is
  gone. What remains: tessellation output lives in OCCT-owned memory, so a Rust renderer needs
  a copy or an unsafe view per frame at the hottest path; and cargo would become a release
  dependency rather than a test one. Real, but no longer decisive. If the renderer were being
  started from scratch today with no other constraints, Rust + wgpu would be a defensible
  choice. We are not restarting.
- **Decision 2 (three narrow interfaces) — still holds, different reasons.** Not for swapping
  in the engine any more, but because:
  1. **CI has no GPU.** A `NullBackend` behind these interfaces is the only way the scene
     layer — where the logic bugs live — gets tested at all.
  2. **iPad needs a second real backend.** Metal behind SwiftUI is a different backend to the
     desktop one whatever we do.
  3. Two implementations existing from day one is what keeps an abstraction honest.
- **Decisions 3, 4, 5 (no IPC, own vertex layout, edges first-class) — unaffected.** Decision 4
  gets *stronger*: with no engine layout to converge on, `CadVertex` is free to be exactly
  what CAD wants.
- **Decision 2's adoption path (M3.4) — deleted.** No longer a goal.

## Scale: 100k–1M parts

This is the requirement that shapes the data model, and the original design fails it.

A `DrawItem` per part means 100k draw calls per frame. That is not a budget problem, it is a
category error — the CPU cannot submit them, let alone at 60fps.

### What replaces it: instanced batches

```
Batch { BufferId vertices, indices; uint32 indexOffset, indexCount; span<Instance> }
Instance { float transform[12]; uint8 colour[4]; uint32 elementBase; }   // 56 bytes exactly
```

One draw call per *unique mesh*, instanced across every part that uses it. `Instance` is
deliberately 56 bytes with a 4x3 affine transform (the fourth row is always 0,0,0,1) and
packed colour: at 1M instances that is the difference between 56 MB and 96 MB per frame of
instance data.

### Why this works at all: content-addressed dedupe

The single biggest win, and it is already in the design rather than bolted on. A `RenderMesh`
is keyed by *shape content hash*, so **identical parts collapse to one mesh automatically.**

Large assemblies are overwhelmingly repeats — fasteners, brackets, fittings. A 100k-part
assembly plausibly has 500–2000 unique shapes. That turns:

| | naive | content-addressed |
|---|---|---|
| meshes tessellated | 100,000 | ~1,000 |
| GPU buffers | 100,000 | ~1,000 |
| draw calls / frame | 100,000 | ~1,000 (instanced) |

Dedupe is not an optimisation pass here; it is a consequence of keying on content, which we
were doing anyway for the DDC. The 50,000 identical bolts tessellate once.

### Consequences for M3.1

1. **Tessellation must be per-unique-shape, not per-part.** The cache lookup happens before
   any work, and a hit costs a hash lookup.
2. **Tessellation must be parallel across shapes.** ~1000 unique shapes at tens of
   milliseconds each is a minute single-threaded. Independent shapes, embarrassingly parallel.
3. **A memory budget is mandatory, not a nicety.** 1000 unique meshes is fine; a pathological
   assembly with 100k unique shapes is not, and must degrade (coarser LOD, evict, proxy boxes)
   rather than exhaust memory. The DDC's budget eviction already covers the disk tier; the
   live tier needs its own.
4. **Element tables must be per-instance, not per-mesh.** One mesh shared by 50,000 bolts
   cannot carry 50,000 element names. Hence `Instance::elementBase`: the mesh stores element
   *slots*, the instance stores where its names start in the frame's element table. A GPU pick
   returns (instance, slot) and resolves to a name through that.

Deferred to M3.3, recorded so the data model does not preclude them: GPU-driven frustum and
occlusion culling, LOD selection, and small-feature culling (a part under a few pixels draws
as a box or not at all).
