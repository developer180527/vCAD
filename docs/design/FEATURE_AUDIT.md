# Feature audit: how far is vCAD from a functional CAD?

Counted from the tree on 17 Aug 2026, at commit `18fec06`; **re-measured 18 Aug 2026 at
`e5f86a9`** — changed numbers and closed gaps are marked. The re-count exists because two of the
three §2 gaps were meant to have been closed since, and one of them was. Every number below was measured today,
not carried over from `COMPETITIVE_REVIEW.md` (15 Aug), which asked a different question — how vCAD
compares *architecturally* to SolidWorks and Inventor. This one asks something narrower and more
useful right now: **could somebody do a job with it?**

The short answer: **the gap is not mostly feature count. It is that the features which exist cannot
be reached.** Three of the findings below are capabilities already implemented and tested in the core
with no way for a user to invoke them.

---

## 1. What is there, counted

| Measured | Count | Notes |
|---|---|---|
| Feature types that compute | **11** | Box, Cylinder, Extrude, Sketch, Fillet, Chamfer, Cut, Fuse, Common, Translate, Import |
| Commands in `Controller`'s registry | **19** | 8 feature, 5 edit, 3 sketch, 2 view, 1 delete |
| Sketch constraint kinds | **11** | Coincident, Horizontal, Vertical, Parallel, Perpendicular, Distance, Radius, PointOnLine, EqualLength, LockX, LockY |
| Sketch drawing tools in the UI | **2** | Line, Circle. (`Sketch` itself also has Point and Arc — no tool reaches them) |
| Ribbon entries | **66** | of which **43 are disabled stand-ins** → **23 live, ~35%** |
| Document kinds | **1 of 4** | Part. Assembly, Drawing, Presentation declared and inert |
| Import formats | **4** | STEP, IGES, STL, DXF |
| Export formats reachable by a user | **0** | see §2 |
| C plugin ABI | **1.21** (was 1.18) | loader, catalogue, manager UI, ribbon and settings extension. The shell now *loads* plugins, which until 18 Aug it never did |
| Tests | **110** C++ (was 79), **164** Rust (`tests-rs`), **26** Rust (`cad-parse`) | |
| Product code | **~27,000 lines** | core 9.9k, abi 3.9k, shell_qt 3.6k, render 3.6k, proshell 2.2k, app 2.2k, rust 1.6k |
| Test code | **~12,300 lines** | a 0.45 ratio to product code |

### Ribbon completeness by tab

The `planned()` stand-in count is the most honest number in the codebase — the ribbon deliberately
shows the application's intended shape with greyed entries (ADR 0009 decision 2), so this is a
self-reported completeness figure.

| Tab | Entries | Stand-ins | Live |
|---|---|---|---|
| 3D Model | 35 | 22 | 13 |
| View | 7 | 5 | 2 |
| Sketch (in document) | 3 | 1 | 2 |
| Sketch (Home) | 6 | 0 | 6 |
| Tools (Home) | 5 | 5 | 0 |
| Inspect | 4 | 4 | **0** |
| Manage | 3 | 3 | **0** |
| Annotate | 2 | 2 | **0** |
| Collaborate (Home) | 2 | 2 | **0** |

Four tabs are entirely aspirational.

---

## 2. The three gaps that are not feature work

These matter more than the counts, because in each case the hard part is done and the reachable part
is not. They are also the cheapest wins in the repository.

### A click in the viewport does nothing — **CLOSED 18 Aug**

`Viewport::mouseReleaseEvent` now calls `Controller::clickAt` when the mouse did not travel far
enough to count as a drag, and the fillet path takes the picked edges. The four bugs this uncovered
(a click and an orbit being indistinguishable until release, highlight batches dropped by culling,
every computed body being placed so the fillet z-fought its own input, and a pick message nobody
emitted) are fixed.

It is wired, not finished. It has been described in use as "VERY buggy", and the threshold-based
click/drag split is the likeliest cause: 4 px at device resolution is tight on a trackpad. Worth
re-measuring against a real session before adding more on top of it.

### There is no way to get geometry out — **CLOSED 18 Aug**

File → Export writes the visible bodies, with the format chosen by extension and the filter list
built from the io registry rather than hard-coded — so a format compiled in conditionally appears
exactly when it is available, instead of being offered in a dialog and refused on write.

Two decisions were the actual work, and neither was in the writers:

- **What to export.** The VISIBLE bodies, by the same tip-body rule that decides what to draw, so
  the file matches the screen. A Box consumed by a Fillet is still in the document; writing both
  would put the un-filleted block in the file beside the real part, and the user would find out in
  whatever opened it. The test fillets a box and asserts the imported volume is the FILLETED one —
  the two differ, which is what lets the test tell which was written.
- **Refusing before writing.** An empty document and an unhandled extension both refuse without
  creating a file. A zero-byte STEP that opens as nothing is worse than being told there is nothing
  to export.

The tests read every export back through the IMPORTER rather than trusting the writer's return
value. `core/io` had tested writers for weeks and export was still the cheapest disqualifying gap,
because nothing called them — so "the writer succeeded" was never the question worth asking.

### A sketch on a side face extruded to nothing — **found and FIXED 18 Aug**

Worth recording in full, including the part of it I got wrong, because the wrong half is instructive.

**The claim that failed.** The first pass of this audit reported that the resolved face frame was
never consumed — `resolvedFrame()` has no callers in `core/`, `app/`, `shell_qt/` or `tests/`. That
grep was for the ACCESSOR. `Sketch::to3d` and `Sketch::toWire` read the member `resolved_` directly,
so the profile was always placed on the face correctly. A search for the public getter said nothing
about the private field, and reading it as "nothing uses this" was an unfounded leap.

**The bug that was real, and worse than described.** `computeExtrude` took its direction from a
stored plane index (0=XY, 1=XZ, 2=YZ), which only ever described the three global planes. A sketch
on a face got whichever global axis the index happened to hold — and `Controller::addExtrude`
defaulted it to XY for every extrude, face-placed or not.

For a sketch on a SIDE face that direction lies in the profile's own plane. Sweeping a face along a
direction it contains does not produce a thin solid or a wrong-facing one; it produces **nothing**.
Measured on the +X face of a 40x30x20 box: `volume() == 0.0`, centroid still at x = 40. The feature
reported success.

**Why nothing caught it.** `sketch_plane.cpp` asserts `placement().kind`, the face string, and
`needsResolution()` — all claims about the DECLARATION, none about geometry. The self-agreeing
assertion pattern again, and the third time it has produced a green suite over a broken feature.

**The fix, and the compatibility decision inside it.** No index means measure the profile's own
normal, which is right for any plane and is what the function's doc comment always claimed. An index
that IS present still wins, because the XZ frame's own normal is -Y (`u x v` with u=x, v=z) while
XZ extrudes have always grown towards +Y: measuring it would silently reverse every XZ extrude in
every existing document. The index is the record of what the file meant when it was written.
`Controller::addExtrude` now carries the index across only when the sketch actually has one, instead
of defaulting it to XY.

`tests/acceptance/extrude_direction.cpp` covers both, and asserts coordinates: a centroid at
x = 42.5 and a volume of 180 for the face case, and the positive-axis direction for all three global
planes — the latter added because changing the code path for global sketches could have flipped
their sign and nothing else in the suite pinned it down. It flipped XZ on the first run, which is
how the compatibility question surfaced at all.

### Measurement is implemented and unreachable

`kernel::Shape` has `measure()` (mass and centre of mass, area/centroid for a face, length/midpoint
for an edge) and `volume()`. The entire Inspect tab — Measure, Section View, Mass Properties, Draft
Analysis — is four disabled stand-ins.

---

## 3. What a functional CAD needs, and where we stand

Grouped by whether the absence stops work, hinders it, or merely limits scope.

### Tier 1 — a user cannot do a job without these

| Capability | Status |
|---|---|
| Click geometry to select it | **done 18 Aug**, and reported buggy in use — §2 |
| Export a model | **missing** (writers exist, unwired — §2) |
| Sketch on a face of the model | **core done 18 Aug**, shell still swaps to a separate 2D canvas. The profile is placed on the face and extrudes along its normal; what is missing is drawing it in place — §2 |
| Sketch tools beyond line and circle | **missing.** No rectangle, arc, polygon, slot, spline, fillet, trim, extend, offset, mirror, or construction-geometry toggle in the canvas |
| Dimensions that drive geometry | **partial.** Distance and Radius constraints exist and solve; the canvas draws dimensions; there is no dimension *tool* |
| Hole feature | **missing.** Stand-in. The single most-used feature in mechanical CAD |
| Revolve | **missing.** Stand-in — and it is the second primitive every part needs |
| Extrude with an operation (Join/Cut/Intersect/New solid) | **missing.** Extrude only ever makes a new solid; combining is a separate `Cut`/`Fuse`/`Common` feature the user must add by hand. Inventor and SolidWorks both put the operation ON the extrude, and vCAD is meant to copy that shape |
| Extrude beyond blind | **missing.** No symmetric, two-sided, through-all, to-face, or taper — one distance, one direction |
| Tangent constraint | **missing.** Arcs exist and 11 constraints exist, but none of them is tangent, so no profile with a filleted corner can be fully constrained. Also missing: concentric, symmetric, equal-radius, angle, midpoint |
| Patterns (linear, circular, mirror) | **missing.** Three stand-ins |
| Standard views (front/top/right/iso) | **missing.** Now trivial: `CameraController::alignTo` does exactly this, and the View tab has no entries for them |
| Save and open | **done.** Native `.vpart`, `QFileDialog`-wired, dirty tracking by content digest |
| Undo/redo | **done**, plus roll back / roll forward through history |
| Units | **done.** Display units as a preference, text parsing with units |

### Tier 2 — the work is possible but unpleasant

Shell/thicken/offset, draft, sweep, loft, coil, rib, emboss, decal, split, direct edit, delete face,
thread, configurations, equations, appearance and materials, section views, view modes (only shaded
exists — wireframe and shaded+edges are stand-ins), origin plane visibility, sketch visibility.

### Tier 3 — scope, not function

Assemblies and drawings, both of which are architectural rather than laborious (assemblies need
inter-document references the document model does not have; drawings need a paper-space layer, though
OCCT's `TKHLR` for hidden-line removal is already linked). Then sheet metal, weldments, surfacing,
mold tools, simulation, CAM, PDM.

### Known ceilings, measured

- `Document::add` is **quadratic** (n^1.85, profiled).
- The sketch solver is **cubic** (n^2.9), 98% inside a dense QR in `GCS::System::diagnose`.

Neither bites at current model sizes. Both bite the day assemblies arrive, which is precisely when
they stop being footnotes.

---

## 4. So how far is it?

**As a product: not usable for a real job.** Not because of the 11 features, but because of §2 — you
cannot click a face, and you cannot get your model out. Either one alone is disqualifying.

**As a platform: further along than the feature count suggests.** Topological naming, a
content-addressed cache, deterministic compute, a versioned C plugin ABI with a real loader and a
compatibility museum, a reusable domain-neutral shell, five-platform CI, a memory-safe importer with a
differential oracle. These are the expensive-to-retrofit things, and they are done.

### The ordering the audit implies (revised 18 Aug)

1. ~~Make sketch-on-a-face actually geometric.~~ **Done 18 Aug** — see §2.
2. **Export.** Still zero call sites. One command, one dialog, three writers already implemented and
   tested. Unchanged from the 17 Aug audit and still the cheapest disqualifying gap.
3. **Standard views and view modes.** `alignTo` still has no caller in the shell.
4. **Sketch tools**: rectangle, arc, dimension, trim, offset — plus the **tangent** constraint,
   without which a filleted profile cannot be constrained at all. The canvas still exposes exactly
   two drawing tools, Line and Circle, and no constraint UI whatsoever.
5. **Revolve, Hole, and the pattern family**, and give Extrude its Join/Cut/Intersect operation so
   combining stops being a manual second feature.
6. **Measurement**, which is implemented and needs a dialog.
7. Only then assemblies.

Item 1 is new to this pass and displaces everything: the 17 Aug audit ranked picking first, and
picking is now done.

The characteristic risk this audit measures is the one `COMPETITIVE_REVIEW.md` §5 named: architecture
outrunning features. Three implemented-but-unreachable capabilities is what that looks like from the
inside.
