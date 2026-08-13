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
