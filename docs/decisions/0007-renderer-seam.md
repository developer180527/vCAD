# 0007 — Renderer architecture and the swap seam

Status: accepted (Aug 2026), amended twice — see the two "Amendment" sections at the end

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
Instance { float transform[12]; uint8 colour[4]; uint32 elementBase, instanceId, reserved; }
```

One draw call per *unique mesh*, instanced across every part that uses it. `Instance` carries a
4x3 affine transform (the fourth row of a CAD placement is always 0,0,0,1) and packed RGBA8
colour: at 1M instances that is 64 MB per frame rather than the 96 MB a Mat4-and-floats layout
would cost.

**Amended during M3.3:** the struct is **64 bytes, not 56**. bgfx requires instance data stride
to be a multiple of 16, so 56 would not have uploaded at all. Rather than pad with filler, the
space went to `instanceId` — which GPU ID-buffer picking turns out to need, because the element
index arrives as a vertex attribute while the instance must come from instance data, and a pick
needs both to tell which of 50,000 identical bolts was clicked.

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

---

# Amendment (Aug 2026, second): instancing is not the architecture

The first amendment made instanced batches the organising principle of the renderer. That was
wrong, and two independent findings force the change.

## What forced it

**1. Instancing has never worked.**

`spikes/scale 8 1` places eight boxes on a 2x2x2 grid at 40 mm pitch. The scene builder uploads
eight distinct transforms — dumped from `BgfxResources::uploadInstances` and verified correct,
translations `(0,0,0) (40,0,0) (0,40,0) (40,40,0) (0,0,40) ...`. The framebuffer contains **one
box**, at instance 0's transform. Every instance reads element 0 of the instance stream.

It reproduces on both instance paths — the persistent dynamic vertex buffer and bgfx's transient
`InstanceDataBuffer` — so it predates the M3.4 buffer change. bgfx's Metal backend advertises
`BGFX_CAPS_INSTANCING`, so this is our usage or our shader pipeline, not a platform gap. The root
cause is not yet identified.

Consequence: **every scale number this project has published is void.** "100k parts at 40 draw
calls", the triangle counts, the frame times — all of it measured one part's transform drawn N
times. The claims in the first amendment's tables were never demonstrated.

It survived because **no test in five tiers looks at a pixel**, and the one spike that captures
pixels (`spikes/bgfx_offscreen`) renders a single instance with an identity basis at zero
translation — precisely the configuration under which this bug is invisible. Counting what was
submitted is not evidence that it was drawn.

**2. The benchmark workload is unrepresentative.**

`spikes/scale` builds N placements from 20 unique meshes. That is a 5000:1 duplication ratio, and
it makes instancing look like the whole answer. Real assemblies are not shaped like that: repeats
are fasteners, brackets and fittings — a large minority. The majority of a million-part model is
*distinct* geometry, where instancing does nothing at all. The first amendment's dedupe table
("100,000 parts -> ~1,000 meshes") describes the flattering case as though it were the general
one.

## Decision — clusters and a BVH, with instancing demoted to a special case

The unit of drawing becomes a **cluster**, not a mesh:

```
Part     = mesh ref + transform + colour + element base + world bounds
BVH      = built per rebuild over all parts
Cluster  = a spatially local group of parts; the unit of culling, LOD and drawing
```

Clusters come in two kinds, chosen by mesh multiplicity:

- **Merged** (the default). Part transforms are baked into a shared world-space vertex/index
  buffer. One draw call covers many *distinct* parts, which is the case instancing cannot help.
- **Instanced** (high-multiplicity meshes only). Today's path, kept because merging 50,000
  identical bolts would put 50,000 copies of that mesh in GPU memory. That memory argument — not
  the draw-call argument — is the real and only justification for instancing.

**Culling is BVH traversal**, not the per-batch uniform grid M3.4 introduced. That grid is
per-batch, so it cannot reject across batches; a BVH rejects a subtree of 10,000 parts with one
test, and is the substrate the next two items need.

**LOD is discrete tessellations**, cooked at three deflection tolerances and cached in the DDC
exactly like the single tier is today, selected by projected screen size. Impostors — one quad
per part — are the bottom tier, not the first resort.

**Occlusion culling** against the BVH is what actually carries the overview case. In a dense
assembly most parts are *inside* the model; frustum culling cannot reject them and LOD only makes
drawing them cheaper. This is the technique the incumbent CAD viewers lean on hardest, and it is
the one this renderer has none of.

## What survives

- **Decisions 1-5 of the original ADR.** Unaffected.
- **The three-interface seam.** Unaffected in shape; `Batch` gains a cluster sibling. Occlusion
  needs GPU visibility results feeding a CPU decision, which is a fourth *optional* interface, so
  a backend that cannot do it degrades to frustum-only rather than failing to compile.
- **Content-addressed meshes and the DDC.** Still right, and now also the natural home for LOD
  tiers and for out-of-core streaming later.
- **Element identity.** `CadVertex::element` plus a per-cluster base gives the same absolute slot
  the pick shader already resolves. Topological naming, picking and highlighting are untouched.
  This is the part of the current design that is genuinely load-bearing and correct.

## What this costs

`render/Scene.{h,cpp}` is largely replaced. The per-batch cell grid, the persistent instance
buffers as the *primary* path, and `Instance::elementBase` as the *only* element addressing all
go. Merged clusters mean a moved part dirties its cluster rather than one instance — acceptable,
because assembly geometry is overwhelmingly static, but it is a real regression for the
drag-a-part case and needs measuring rather than assuming.

## The verification rule this ADR now imposes

**A rendering claim is not established by a counter.** Any scale or correctness claim about the
renderer must be backed by a pixel-level assertion on a scene with more than one part, at more
than one transform. `spikes/scale` gets that assertion before any of the work above lands, and
the "draw calls", "instances" and "triangles" counters are demoted to diagnostics.

That rule is the actual lesson here. The renderer was reported as working, at scale, in three
successive commits, on the strength of numbers that were internally consistent and entirely
disconnected from what reached the screen.

## Order

1. Pixel verification in `spikes/scale` — multi-part, multi-transform.
2. Fix instancing (needed for the instanced-cluster case regardless).
3. BVH + merged clusters.
4. LOD tiers from the DDC.
5. Occlusion culling.
6. Qt widget embedding.
