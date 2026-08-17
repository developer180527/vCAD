# Feature audit: how far is vCAD from a functional CAD?

Counted from the tree on 17 Aug 2026, at commit `18fec06`. Every number below was measured today,
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
| Ribbon entries | **67** | of which **44 are disabled stand-ins** → **23 live, ~34%** |
| Document kinds | **1 of 4** | Part. Assembly, Drawing, Presentation declared and inert |
| Import formats | **4** | STEP, IGES, STL, DXF |
| Export formats reachable by a user | **0** | see §2 |
| C plugin ABI | **1.18**, 73 entry points | loader, catalogue and manager UI all landed |
| Tests | **79** C++, **164** Rust (`tests-rs`), **26** Rust (`cad-parse`) | |
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

### A click in the viewport does nothing

`Viewport::mousePressEvent` maps the button to an orbit/pan/zoom gesture and returns. There is no
selection path from the 3D view at all: `Controller::select` is called from exactly one place in the
whole shell, the model tree (`MainWindow.cpp:1021`).

This is the single largest usability gap in the product. Clicking a thing you can see is not a
feature of CAD software, it is the interaction CAD software is made of. And as of this session the
missing piece is no longer missing — `Controller::pickAt` returns an object and an `ElementName`,
with six tests behind it. Nothing calls it.

Downstream consequences, all of which disappear with the same wiring:

- **Fillet and chamfer apply to every edge of the body.** `addEdgeFeature` calls `edgesOf(target)`
  and passes the whole list, because there is no way to pick one. The engine supports per-edge
  selection; the UI cannot express it.
- No hover highlight, although `SceneBuilder::setHighlight` exists and is O(1) by element slot.
- The Body/Face/Edge/Vertex filter in the top-right of the window has nothing to filter.

### There is no way to get geometry out

`core/io` implements STEP, IGES and STL **writers** — `OcctProvider.cpp` has all three, plus
`exportFile` in `Format.h`. The string `exportFile` does not appear anywhere in `shell_qt` or `app`.

A modeller you cannot export from cannot be used for anything, however good the model is. This is one
command and one file dialog away from working, against an implementation that is already tested.

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
| Click geometry to select it | **missing** (pick layer exists, unwired — §2) |
| Export a model | **missing** (writers exist, unwired — §2) |
| Sketch on a face of the model | **core done, shell missing.** `SketchPlane::Kind::Face`, `SketchFrame`, `cad_sketch_create_on_face` (ABI 1.18), face picking and camera alignment all landed. The shell still swaps to a separate 2D canvas on the XY plane |
| Sketch tools beyond line and circle | **missing.** No rectangle, arc, polygon, slot, spline, fillet, trim, extend, offset, mirror, or construction-geometry toggle in the canvas |
| Dimensions that drive geometry | **partial.** Distance and Radius constraints exist and solve; the canvas draws dimensions; there is no dimension *tool* |
| Hole feature | **missing.** Stand-in. The single most-used feature in mechanical CAD |
| Revolve | **missing.** Stand-in — and it is the second primitive every part needs |
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

### The ordering the audit implies

The distance to "someone could use this" is much shorter than the distance to Inventor, and the first
three items are days rather than months:

1. **Wire the pick to selection.** Click a face; the tree follows; fillet takes the edge you picked.
   Everything below it is cheaper afterwards.
2. **Export.** One command, one dialog, three formats already implemented.
3. **Standard views and view modes.** `alignTo` exists; six entries in the View tab.
4. **Finish in-place sketching** (1d steps 3 and 4) — the shell side of a core capability that is
   already tested.
5. **Sketch tools**: rectangle, arc, dimension, trim, offset. This is the laborious one, and it is
   what makes the sketcher usable rather than demonstrable.
6. **Hole, Revolve, and the pattern family.** After this, a person could model a real bracket.
7. Only then assemblies, which is where the architecture gets exercised at a scale that matters.

The characteristic risk this audit measures is the one `COMPETITIVE_REVIEW.md` §5 named: architecture
outrunning features. Three implemented-but-unreachable capabilities is what that looks like from the
inside.
