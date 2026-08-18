# Can vCAD actually get to 100,000 parts?

Architectural review, 13 Aug 2026. Written the day instancing was fixed, which makes it the first
review in this project's history based on a renderer that draws more than one box.

Every number below was measured today on this machine, at commit `b340d88` plus the uncommitted
viewport work. Nothing here is estimated from theory unless it says so.

---

## The short answer

**The scene architecture is sound and the measurements support it.** Draw calls are proportional
to unique meshes and not to part count, culling rejects 80% of a 100k grid in 0.042 ms, and a
camera move costs 0.036 ms because it touches no instance data. Those were the load-bearing claims
of ADR 0007 and they now have honest evidence behind them.

**Three things stand between here and the goal, in descending order of difficulty:**

1. **Unique parts.** Every good number above assumes 20 distinct meshes shared by 100,000
   placements. A real assembly of 100,000 *distinct* parts is a different problem, and the current
   design does not solve it — it degenerates to one draw call per part and a triangle budget we
   cannot hold in memory.
2. **The viewport path.** Offscreen-and-blit has a hard ceiling around 27 fps on an empty scene.
   This is known and was chosen deliberately, but it is now the binding constraint on the shell.
3. **Simulation.** Not started, and not a rendering problem at all.

---

## 1. What the measurements actually say

`spike_scale 100000 20` — 100,000 instances drawn from 20 unique parts:

| Measure | Result | Verdict |
|---|---|---|
| Draw calls | **40** for 100,000 instances | The central claim, holding exactly (2 passes × 20 meshes) |
| Culling | 19,108 of 100,000 instances, 247 of 1,280 cells, **0.042 ms** | Working, and cheap |
| Camera-only change | **0.036 ms** | No instance rebuild on orbit — the property that makes 1M navigable |
| Frame CPU | **0.09 ms** | Submission is free |
| Frame GPU | **7.69 ms** | 5.04M triangles + 2.65M lines |
| Resident GPU memory | **6.2 MB** | For 20 meshes |
| Scene build | 187 ms | One-off, acceptable |
| No-op update | **33 ms** | See §3 — this is a problem in disguise |

The architecture does what ADR 0007 said it would. That is worth stating plainly, because this
project has spent a lot of today discovering that things it believed were false.

### The one alarming result — resolved, and it was two bugs

The pixel check failed at 100k with `101626 lit for 100000 instances, 101626 for one` — identical
counts for the full scene and the one-instance baseline. **The renderer was not at fault.** Two
separate faults produced one symptom:

**1. A real renderer bug: an empty scene never cleared.** bgfx discards a view that receives no
draw calls, and a discarded view never runs its clear — so the framebuffer silently kept the
previous frame. `bgfx::touch` on both the shaded and pick views fixes it. On screen this bug meant
**deleting every feature would leave the old model in the viewport**, and a pick would report hits
on geometry that no longer exists. Worth far more than the test it was found through.

**2. A test that measured something impossible.** The baseline renders one instance at the camera
fitted to the WHOLE assembly. Past roughly 16k parts, one part at that zoom is sub-pixel, so
`minPixels = 2` culls it — correct behaviour — leaving nothing drawn, which (because of bug 1) read
back as a copy of the previous frame. The baseline now disables screen-size culling, and reports
SKIP rather than FAIL when one instance is genuinely sub-pixel.

**What is verified now:** transforms are pixel-verified up to 16,384 instances (×30,573 over the
one-instance baseline). Beyond that the check honestly skips. The submission path does not vary
with instance count, so this is good evidence — but it is not the same as verification at 100k, and
should not be described as though it were. A baseline measured from a ZOOMED camera, where one part
is comfortably visible at any n, would close the gap.

The same review also caught a third instance of the recurring flaw: the culling assertion gated on
total cell count, when cells are spatial bins WITHIN a batch. At 512 instances across 20 meshes
that is one cell per batch, each spanning the whole assembly and correctly unculllable. Gated on
cells per batch now.

That is three assertions in this harness that agreed with themselves rather than with the renderer.
The pattern is worth naming: **every one of them compared a measurement against a threshold chosen
from intuition, instead of against a second measurement taken under known-good conditions.**

---

## 2. The viewport is the immediate ceiling

Measured in the shell today: **41.5 ms/frame, of which submit was 0.0 ms and readback was 41.5 ms.**
Disabling `CAMetalLayer.displaySyncEnabled` took it to ~27 fps. The remaining cost is inherent:

- a readback must pump at least two bgfx frames before the pixels are valid;
- then ~4 MB is copied GPU→CPU per frame at this window size, more at higher resolution;
- then Qt blits it again.

None of that is our rendering. The scene submits in 0.09 ms at *100,000 parts*; the shell spends
37 ms on an empty document. **The offscreen path costs roughly 400× what drawing the scene costs.**

Offscreen-and-blit was chosen knowingly, to sidestep the DESKTOP_UX §3.6 compositing decision and
get pixels into the window on all three platforms at once. It did that. It cannot be the shipping
path, and the decision it deferred is now the thing blocking progress.

**Native surface is required**, and with it §3.6 comes due: on macOS a native `CAMetalLayer` child
view will not reliably let Qt paint widgets on top, so the ViewCube, navigation bar, context
toolbar and marking menu must become renderer-drawn. That is real work, and it is the price of an
interactive viewport.

Half-resolution-while-dragging would buy roughly 4× on the readback and is cheap to build, but it
is a palliative for a path we are going to delete. Worth doing only if the native surface is far
off.

---

## 3. The real problem: unique parts

Every good number in §1 rests on **20 unique meshes**. Change that assumption and the picture
changes completely.

### Draw calls

The claim is "draw calls ∝ unique meshes". With 100,000 *distinct* parts that is 200,000 draw
calls (two passes each). At a generous 10 µs per call that is 2 seconds a frame. The architecture
does not degrade here — it inverts.

Real assemblies sit between the two extremes: fasteners repeat heavily and dominate part *count*,
while machined parts are unique and dominate *geometry*. A realistic 100k-part machine might hold
10–20k distinct meshes. That is still 20–40k draw calls, and still far too many.

**What is needed:** merging small static parts into shared buffers, and GPU-driven indirect
drawing (`bgfx` supports indirect draws; bgfx's own `37-gpudrivenrendering` example is the
reference). Neither exists today. This is the largest single piece of unbuilt renderer work.

### Triangles and memory

6.2 MB resident for 20 meshes. Unique parts scale that linearly: 100,000 distinct meshes at a
modest 50 KB each is **5 GB of GPU buffers**, before LOD, before edges. Not viable on any laptop.

**What is needed**, none of which exists:

- **Level of detail.** A part covering 4 pixels does not need its 5,000-triangle tessellation. This
  is the single biggest lever and it is missing entirely.
- **Quantised vertex formats.** Position as 16-bit normalised within the mesh's bounds, normals
  octahedral-encoded. Routinely 2–3× on memory.
- **Streaming / out-of-core.** Tessellation on demand for what is visible, evicting what is not.
  The DDC cache already content-addresses tessellation, so the storage half of this exists —
  the eviction and residency half does not.
- **Occlusion culling.** Inside a machine assembly most parts are hidden by the housing. Frustum
  culling cannot see that; hierarchical depth-buffer occlusion can, and it is the difference
  between drawing 19,108 parts and drawing 2,000.

### The no-op update is a warning

**33 ms** to conclude that nothing changed, at 100k placements. It is honest work — an O(placements)
content digest — but it means an idle redraw at 1M parts would cost ~330 ms. The digest needs to
become hierarchical, or placements need explicit dirty tracking. Cheap to fix, easy to forget until
it dominates.

---

## 4. Simulation

Asked about directly, so answered directly: **simulation is not close, and it is not a rendering
problem.**

"Simulating a 100k-part assembly" is at least three distinct disciplines, none of which vCAD has
started:

- **Multibody dynamics** (does the mechanism move, does it collide) — needs a constraint solver
  over rigid bodies, which is also what assemblies need. Shares nothing with the render path.
- **FEA** (does the part survive load) — needs a volumetric mesher, which is a specialism of its
  own and harder than the CAD kernel work done so far, plus a solver.
- **CFD / thermal** — a different specialism again.

Each is a multi-year subsystem, and every one of them is a domain where mature open-source options
exist (CalculiX, Project Chrono, OpenFOAM) that a plugin could drive rather than reimplement.

**This is the clearest case yet for the plugin architecture already planned.** Simulation should be
a consumer of the kernel and the document through a stable interface, developed on its own
timeline, and it should not enter the core. Building it into core would couple the release of a
modeller to the readiness of a solver — which is precisely the coupling that makes large CAD
projects stall.

Honest framing: **assemblies and drawings come first.** A modeller that cannot express an assembly
cannot simulate one, and vCAD cannot express an assembly today.

---

## 5. Verdict

The foundations are the right ones and are now, finally, measured rather than asserted. The scene
layer, the naming, the recompute engine and the document are genuinely strong, and the draw-call
architecture holds under a 100,000-instance load.

**The gap is not architectural error — it is unbuilt work, and the unbuilt work is well understood.**
Nothing in this review requires undoing a decision. LOD, occlusion culling, indirect draws, mesh
merging and a native surface are all *additions* that the existing seams accommodate. That is the
best thing to be able to say after a scale review.

The honest scale claim today: **100,000 instances of 20 parts render at interactive rates in the
spike, and that is not yet verified at the pixel level. The shell renders at 27 fps regardless of
content, because of the viewport path, not the renderer.**

### Ordered plan

1. ~~Resolve the 100k pixel-check failure.~~ **Done** — an empty scene never cleared (`bgfx::touch`
   missing) plus a baseline that measured a legitimately culled instance. See above. Remaining
   nicety: a zoomed baseline, so the pixel check covers 100k rather than skipping.
2. **Native surface for the viewport**, and with it the §3.6 decision on renderer-drawn overlays.
   This unblocks the entire shell.
3. **Level of detail.** The largest lever for both memory and triangle count.
4. **Occlusion culling.** The largest lever for real, enclosed assemblies.
5. **Mesh merging and indirect draws.** What makes unique-part assemblies possible at all.
6. **Hierarchical placement digest**, before 1M makes it obvious.
7. **Assemblies** (a 3D constraint solver) — the prerequisite for the entire simulation question.
8. **Simulation as plugins**, driving mature solvers, on its own timeline.

Items 3–5 are the ones that turn "100,000 bolts" into "100,000 parts", and that distinction is the
difference between the demo and the goal.


---

# Amendment, 19 Aug 2026: measured again, and the ceiling is not where this document said

Everything above reasons about 100,000 parts. Re-running `spike_scale` with UNIQUE parts found a
hard wall three orders of magnitude earlier, and it fails **silently**.

## What was measured

| Unique parts | Draw calls | Triangles | Resident | Frame | Scene build |
|---|---|---|---|---|---|
| 2,000 | 3,976 | 108,720 | 11.6 MB | 2.81 ms (356 fps) | 4.3 s |
| 3,000 | **0** | **0** | 12.5 MB | — | — |
| 4,000 | **0** | **0** | 13.1 MB | — | — |
| 8,000 | **0** | **0** | 13.2 MB | — | bgfx assert at shutdown |

**Above roughly 2,000 unique parts, vCAD draws nothing at all.** Not slowly — nothing.

### Why

`BGFX_CONFIG_MAX_VERTEX_BUFFERS` and `BGFX_CONFIG_MAX_INDEX_BUFFERS` are both **4096**. vCAD
allocates, per unique mesh, one vertex buffer for surfaces, one for edges, and one index buffer. Two
vertex buffers per mesh against a pool of 4096 gives a ceiling of about **2,048 unique meshes**. Past
it every upload returns an invalid handle, `submitBatches` skips the batch because the lookup fails,
and the frame comes out empty.

Nothing reports this. The uploads return `BufferId::None`, the draw loop treats that as "nothing to
draw", and the viewport renders an empty scene at a very good frame rate.

### The spike passed

    PASS  draw calls 0 <= 16000 (2 passes x 8000 unique meshes)

An upper bound is satisfied by zero. The spike's *other* check ("nothing rasterised") did fail, so
the run was not entirely green — but the headline draw-call claim, the one this whole document is
built on, is vacuous exactly when the renderer has stopped working. Another self-agreeing assertion,
in the file written to guard against them.

## What this changes

The original §3 treats shared buffers and indirect drawing as scaling work for 100k parts. They are
not. **Shared buffers are what makes 2,049 parts possible**, and that is a number a real assembly
passes on the first day.

## Decisions

Ordered by what unblocks the next thing, not by size.

### P0 — the renderer is wrong today

1. **Fail loudly on an upload that fails.** An invalid buffer handle must surface as an error the
   user sees, not a silently skipped batch. This is one afternoon and it converts every future
   version of this bug from invisible to obvious.
2. **Fix the vacuous assertion.** `draw calls <= N` becomes `draw calls == expected` and
   `triangles > 0`. A bound that zero satisfies is not a test.
3. **Shared mega-buffers.** Suballocate many meshes into a few large vertex/index buffers, keyed by
   vertex layout. Removes the handle ceiling entirely, and is the precondition for everything in P1:
   you cannot batch draws across meshes that live in different buffers.

### P0 as built, 19 Aug

Shared buffers landed. `BufferId` names a suballocation; meshes are packed into a few large dynamic
buffers and bound as windows into them.

| Unique parts | Before | After |
|---|---|---|
| 2,000 | 3,976 calls, 108,720 tris | unchanged |
| 8,000 | **0 calls, nothing drawn** | 8,112 calls, 221,856 tris, 71.8 MB, 194 fps |
| 20,000 | — | 8,044 calls (after culling), 219,912 tris, 253.5 MB, 156 fps |

Three things worth carrying forward:

- **bgfx dynamic buffers created FROM MEMORY take that block's size as their size** and truncate
  every later update, reporting it and carrying on. `ALLOW_RESIZE` did not grow them either. Growth
  is therefore an explicit chain of fixed chunks.
- **Freeing a suballocation reclaims nothing.** Compaction is not built, so arenas grow and never
  shrink within a session — bounded by the document, not by session length.
- **Indirect drawing is bigger than this document assumed.** bgfx exposes `createIndirectBuffer` and
  `destroy` and *no update*: an indirect buffer can only be written by a COMPUTE SHADER. There is no
  CPU-filled path, so collapsing submits is not a small addition to the arena — it is the
  GPU-driven pipeline, with per-instance data and bounds in GPU buffers and culling in compute.
  Caps on this machine report `indirect=1 compute=1`, so it is available.

The 20,000-part run also confirms §P2 item 9 with a number: the no-op placement digest costs
**19.4 ms** at 20k, against 1.8 ms at 2k. Linear, and it is paid on every idle frame.

### P1 item 6 as built, 19 Aug: tessellation in parallel

`MeshCache::warm` tessellates every missing mesh across all cores before the scene walks placements
serially. Measured on this machine:

| Unique parts | Scene build before | After |
|---|---|---|
| 2,000 | 4,255 ms | **1,935 ms** |
| 8,000 | 17,221 ms | **7,980 ms** |

Draw calls and triangle counts are byte-identical either way, which is the point: `tessellate` is a
pure function of a shape and its settings, and that claim is what already justified caching it.

**2.2x, not 8x**, and the gap is the honest part of this result. Tessellation is now off the critical
path but the rest of the scene build is not: content-hashing every shape for its cache key, encoding
meshes into the DDC, and the GPU uploads all remain serial on the calling thread. Those are the next
thing to measure, and the measurement should come before any more threading.

The cache is never touched concurrently — missing keys are found on the calling thread, meshes are
built into a local vector by the workers, and results are published after the join. Concurrency is
confined to `tessellate` itself.

### P1 — the 100k path

4. **Indirect drawing.** With meshes sharing buffers, a bind serves many draws — but each visible
   range is still its own `submit`. Real reduction needs `bgfx` indirect draws with the draw
   arguments built on the GPU after culling (bgfx's `37-gpudrivenrendering` is the reference).
5. **Level of detail, edges first.** The 2,000-part scene drew **297,635 lines against 108,720
   triangles** — edges are the larger half of the cost and the first thing that should vanish with
   distance. A part covering four pixels needs neither its edges nor its full tessellation.
6. **Tessellation off the main thread, and on demand.** Scene build measured 4.3 s at 2,000 meshes
   and 17.2 s at 8,000 — roughly 2 ms per mesh, so 100k is **three and a half minutes** of blocking
   work. The DDC already content-addresses tessellation, so the storage half exists; what is missing
   is doing it in parallel and only for what is visible.

### P2 — after the above changes the numbers

7. **Occlusion culling.** Frustum culling drew 572 of 2,000; inside a housing most of the rest is
   hidden and frustum culling cannot tell.
8. **Quantised vertex formats.** 16-bit positions within mesh bounds, octahedral normals. 2-3x on
   memory, and memory is what LOD is fighting.
9. **Incremental placement digest.** 1.8 ms at 2,000 placements is O(n) per idle frame; at 100k that
   is ~90 ms to conclude nothing changed.

### Not now

Streaming/out-of-core, and anything that assumes an assembly larger than memory. The first useful
version of vCAD does not need it, and the design will be better informed once 5 through 7 exist.
