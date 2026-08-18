# How the incumbents structure modelling, and what vCAD copies

Researched 18 Aug 2026 against current vendor documentation, because this is exactly the area where
working from memory produces something that *looks* like CAD and behaves like nothing. Sources are
listed at the end; where a claim is inference rather than something a source states, it says so.

The immediate question that prompted this: **why are the origin planes sitting in vCAD's model tree
between the Box and the Sketch?** The answer turns out to be structural, not cosmetic, and it is the
first section.

---

## 1. The browser and the history are two different things

This is the distinction vCAD is currently missing, and everything else in this document depends on
it.

A parametric modeller has two orthogonal facts to show:

- **What exists**, organised by kind — bodies, sketches, datums, joints, materials.
- **What happened**, in order — the feature history, which is what makes the model parametric and
  what you scrub, roll back and reorder.

The three incumbents resolve this differently:

| | What exists | What happened |
|---|---|---|
| **Fusion** | Browser (left), folders by kind | **Timeline** (bottom), a separate horizontal strip |
| **SolidWorks** | FeatureManager tree | *the same tree*, ordered, with a rollback bar |
| **Inventor** | Model browser | *the same tree*, ordered, with an End of Part marker |

Fusion splits them into two surfaces. SolidWorks and Inventor combine them into one ordered tree —
but they do **not** therefore show a flat list of everything. They use **folders** for the things
that are not history: the Origin folder, the Bodies folder, Sensors, Annotations, Material.

**vCAD today has one flat list and no folders**, so seeding three datum planes put them in the
feature history, which is where you look for "what did I do", between a Box and a Sketch you made.
They read as three modelling steps the user does not remember performing. That is the bug in the
screenshot, and it is not fixed by hiding them — it is fixed by the tree having somewhere else to
put them.

### What Fusion's browser holds by default

A new Fusion document's browser has **three folders before you do anything**: Document Settings,
Named Views, and Origin. Bodies and Sketches folders appear only once you create a body or a sketch.
The Origin folder holds the three planes, the three axes and the origin point, and **it is hidden by
default** — the screenshot shows its eye icon slashed while Bodies and Sketches are open eyes.

Note what that means: the datums are *always present*, *always in the tree*, *never in the history*,
and *not drawn* until you ask. vCAD arrived at "present, in the tree, not drawn" already; the piece
it is missing is "not in the history".

### SolidWorks

The FeatureManager holds the origin and the three default planes at the top, above the features, and
which tree items appear at all is configurable under Options → FeatureManager, where several are set
to hide by default. Same shape as Inventor: datums at the top, features below, folders separating
them.

---

## 2. The sketch environment

### Entering

**The order is: pick the surface, then start the sketch.** All three let you select a face or a
plane first and then invoke Sketch; all three also let you invoke Sketch first and then ask you to
pick. What none of them do is silently choose a plane for you — which is what vCAD did until this
week, and is why sketching on the model was unreachable even though every piece underneath worked.

**What changes on entry:**

- **Fusion** swaps the toolbar to a contextual **SKETCH tab** and opens the **Sketch Palette** on
  the right. The 3D environment's tabs stay visible but the Sketch tab is active, and a large green
  **FINISH SKETCH** button sits at the right end of the toolbar.
- **SolidWorks and Inventor** likewise swap to a sketch-specific ribbon tab with an exit command.

vCAD already does the contextual-tab half of this correctly.

**The camera.** Fusion rotates to look at the sketch plane, controlled by the **Auto Look At Sketch**
preference (Preferences → Design), and the Sketch Palette carries a manual **Look At** button for
the same thing. SolidWorks has the equivalent option, *Auto-rotate view normal to sketch plane on
sketch creation* — **off** by default through 2020 and **on** by default from 2021, which is a
useful signal: the vendor changed the default towards auto-rotating once they had the telemetry.

So auto-rotating into the plane on entry is right, and vCAD does it.

**On exit**, the sources are clear that the Sketch tab disappears and you return to the modelling
environment. **None of them state that the camera is restored to its pre-sketch orientation**, and
my reading is that Fusion leaves you looking at the plane. vCAD restores the previous view, which
was an explicit request — recording it here as a *deliberate divergence* rather than a copy, so the
next person does not "fix" it back.

### 2b. How a line is actually drawn, click by click

The section I should have written first. §2 covers the ENVIRONMENT — which tab, which camera, which
palette — and none of that is what a person does with their hands. Everything below is behaviour a
user performs hundreds of times an hour, and getting any of it wrong makes the sketcher unusable
while every model-level test still passes.

**A line chains.** Click sets the start. Click again ends the first segment *and starts the next
from that endpoint*. Click, click, click draws a connected run. The command stays active throughout.
This is the single most important fact about the tool, and vCAD had it wrong: two clicks per
segment, endpoint discarded. That makes a closed rectangle require four clicks landing on exactly
coincident pixels — which is why sketches came out open, and why Extrude reported ERR.

Note what Fusion does NOT do: there is no polyline object. Each segment is a separate line; the
chain is an interaction, not a data structure. vCAD's `Sketch` already stores separate lines, so
nothing in the model has to change — only the tool.

**A chain ends** on Escape, on a double-click, by clicking the on-screen checkmark, or by starting
another command. Closing the loop back onto the start point also ends it naturally.

**The pointer snaps** — to existing points, to the origin, and to the grid. This is what makes
"auto endpoint joining" happen: without it a click lands *near* the previous endpoint and the two
segments do not meet, so the profile never closes no matter how carefully the user aims. Snapping
is not a convenience here; it is the mechanism by which a closed profile is possible at all.

**Constraints are inferred as you draw.** A line drawn roughly horizontal receives a horizontal
constraint automatically. This is why a Fusion sketch ends up nearly fully constrained without a
separate dimensioning pass, and why vCAD's sketches — which infer nothing — stay under-constrained
however carefully they are drawn.

**Two dimension fields, not one**: length and angle, both live, both editable. **Tab locks a field**
— a lock icon appears and the value stops tracking the mouse, so the user can aim the direction
without disturbing a length they have already decided. vCAD shows one field and the angle read-only.

**Units.** The fields are in the document's display units, not raw millimetres.

#### What this means for vCAD, concretely

| Behaviour | vCAD before this pass |
|---|---|
| Chaining | **missing** — two clicks per segment, endpoint dropped |
| Snap to existing endpoints | **missing** — so profiles could not reliably close |
| Escape / double-click to end | **missing** — no way to end a chain that did not exist |
| Inferred horizontal/vertical | **missing** |
| Length AND angle, both editable | length only; angle read-only |
| Units in the field | raw millimetres, unlabelled |
| Line weight | 2 px logical — one physical pixel on a Retina display |

### The Sketch Palette — the piece vCAD has nothing like

Fusion's palette is a docked panel of sketch-scoped options, not commands:

| Group | Entries |
|---|---|
| Linetype | Construction, Centerline |
| View | **Look At**, Sketch Grid, **Slice** |
| Show/hide | Profile (blue shading on closed regions), Points, Dimensions, Constraints, Projected Geometries, Construction Geometries |
| Behaviour | Snap, 3D Sketch |
| Contextual | options for the active tool — spline degree, curvature combs, and the Line tool's own mode |

Three of these are load-bearing and vCAD has none of them:

- **Show Profile** — closed regions shade blue. This is how a user knows *before* extruding whether
  their curves form a profile. vCAD's equivalent feedback is an ERR on the Extrude afterwards, which
  is the same information delivered later and angrier.
- **Slice** — temporarily cuts away material in front of the sketch plane. Sketching on a face
  buried inside a part is otherwise done blind.
- **Look At** — a manual re-aim, for after you have orbited away.

### Drawing

Fusion's Line tool shows **live dimension fields while you draw** — the `28.069 mm` and `119.6 deg`
boxes in the reference screenshot — and they are editable: type a number and the segment is created
with a driving dimension already applied. Combined with automatic constraint inference, this is how
a sketch ends up constrained without a separate dimensioning pass.

vCAD draws a dashed rubber band with no numbers. The band is right; the numbers are the next thing
that makes the sketcher usable rather than demonstrable.

---

## 3. What vCAD should copy, in order

Ordered by how much each unblocks, not by size.

### 3.1 Give the tree folders — this is the reported bug

Adopt the **Inventor/SolidWorks** shape, because vCAD's tree is already a single ordered list and
splitting into a Fusion-style Browser + Timeline is a much larger change with no benefit yet:

```
Part1
├─ Origin            ← folder, collapsed, hidden by default
│   ├─ XY Plane
│   ├─ XZ Plane
│   └─ YZ Plane
├─ Box               ← history, in order
├─ Sketch
└─ Extrude  ⚠
```

The rule to encode: **an object is either reference geometry or history, and the tree shows the two
separately.** Datums are reference. That rule also answers where axes, the origin point and future
work planes go without another debate.

### 3.2 Show the profile

Shade closed regions of the active sketch. It is the difference between finding out you have an open
profile now versus after pressing Extrude, and vCAD already computes exactly this — `toFace()`
succeeding *is* the test, and it now runs without failing the feature.

### 3.3 Dimensions while drawing

Live length and angle on the rubber band, editable, creating a driving constraint. The solver, the
constraint kinds and the overlay all exist; this is a UI over them.

### 3.4 Look At and Slice

Both are small once the camera and sketch plane are known, and both are on the palette because users
reach for them constantly.

### 3.5 A sketch palette to put them in

Only after there are three or four things to hold. Building the panel first would be building
furniture for an empty room.

---

## 3b. What is actually built, checked against the tree on 18 Aug 2026

Verified by reading the code, not by remembering writing it. Everything in §3 except the palette
itself has landed since this document was written.

| §3 item | Status | Where |
|---|---|---|
| 3.1 Tree folders | **done** | `TreeGroup::{History,Origin}` in `Controller`, folder rendered in `MainWindow::refreshTree`, collapsed, no object id |
| 3.2 Show Profile | **done** | `SceneBuilder::setSketchProfile`, fed by `Controller::pushSketchProfile`. **Opaque, not translucent** — the shaded shader emits alpha 1 and per-instance alpha needs a uniform, since `i_data3.w` carries the element-id base |
| 3.3 Dimensions while drawing | **partly** | Length is typeable and creates a driving `Distance`/`Radius`. The **angle is read-only** — Fusion's Tab-between-fields needs field-switching state |
| 3.4 Look At | **done** | Sketch tab → View; re-aims via `alignCameraToSketch` |
| 3.4 Slice | **done** | `Controller::setSliceEnabled`. Needed real work: `SectionPlane` existed and the backend **ignored it entirely**, so this added the clip uniform, a world-position varying and a per-fragment discard in both the shaded and edge shaders |
| 3.5 Sketch palette | **not started** | Four things now exist to put in it: Look At, Slice, a Show Profile toggle, construction linetype |

### Correcting FEATURE_AUDIT.md

That audit claimed vCAD has "no constraint UI whatsoever". **Wrong.** The Sketch tab has a Constrain
panel wired through `Controller::applySketchConstraint`, exposing five of the eleven constraint
kinds: Horizontal, Vertical, Parallel, Perpendicular, EqualLength. The grep behind that claim looked
for `ConstraintKind::` and the shell spells it `CK::` through a local alias — a search that found
nothing and was read as "nothing exists", the same unfounded leap as the `resolvedFrame()` one.

The six kinds with no button are Coincident, Distance, Radius, PointOnLine, LockX and LockY.
Distance and Radius are now *reachable* by typing a dimension while drawing, but there is still no
way to dimension geometry that already exists.

### Still open from §2, unchanged

- **No sketch grid, and no snapping to one.** `snapTolerance` is a preference that nothing in the
  sketcher reads for grid snap.
- **No automatic constraint inference.** Fusion infers horizontal, vertical, coincident and tangent
  as you draw; vCAD applies only what the user asks for, so a sketch drawn by hand stays
  under-constrained.
- **No construction or centreline linetype in the UI.** `Geometry::construction` exists in the model
  and `toWire` already honours it; nothing sets it.
- **Show Points / Dimensions / Constraints / Projected Geometry toggles**: none, and there is nothing
  to toggle yet — dimensions and constraints are not drawn in the viewport at all.
- ~~Export is still zero call sites~~ — **done 18 Aug.** File → Export, formats read from the io
  registry so a conditionally-compiled one appears exactly when available, mesh-only formats
  labelled as such in the dialog. `measure()` still has no caller in the shell.

---

## 4. Deliberate divergences, recorded so they are not "fixed"

| vCAD | Incumbents | Why |
|---|---|---|
| Camera restored on Finish Sketch | Fusion leaves you on the plane | Explicit request; a sketch should not cost the view you arranged |
| Orbit toggle on the quick-access strip | ViewCube / navigation bar | vCAD has no ViewCube yet, and orbit was otherwise unreachable on a trackpad |
| Open curves compute, Extrude refuses | Same | Convergent, not divergent — worth noting because vCAD arrived here by fixing a bug rather than by copying |

---

## 5. What this document does not cover

Assemblies, joints and mates; drawings; the timeline's reorder and rollback interactions beyond what
vCAD already has; configurations and parameters. Each deserves its own pass before being built, for
the same reason this one exists: the failure mode is not building the wrong thing badly, it is
building something plausible that no user recognises.

---

## Sources

- [Exploring Browser and Timeline Organization in Fusion 360 — Noble Desktop](https://blog.nobledesktop.com/learn/cad/exploring-browser-and-timeline-organization-in-fusion-360)
- [Master the Timeline, Browser, & Preferences — Autodesk Fusion Blog](https://www.autodesk.com/products/fusion-360/blog/master-the-timeline-browser-preferences/)
- [Sketch Palette reference — Autodesk Fusion Help](https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/GUID-4183A4B7-E002-4396-AD5A-7FF3C8B2F33A.htm)
- [Why Doesn't Fusion 360 Look Directly at the Sketch? — Product Design Online](https://productdesignonline.com/tips-and-tricks/why-doesnt-fusion-360-look-directly-at-the-sketch/)
- [Auto-Rotate View on Sketch Edit — SOLIDWORKS 2018 What's New](https://help.solidworks.com/2018/english/WhatsNew/c_auto_rotate_view_sketch_edit.htm)
- [SOLIDWORKS Macro to toggle "Auto-rotate view normal to sketch plane" — Javelin](https://www.javelin-tech.com/blog/2020/11/solidworks-macro-to-toggle-auto-rotate-view-normal-to-sketch-plane-on-sketch-creation-and-sketch-edit/)
- [Get The Most Out Of SOLIDWORKS FeatureManager Design Tree — Hawk Ridge Systems](https://hawkridgesys.com/blog/get-the-most-out-of-solidworks-featuremanager-design-tree)
