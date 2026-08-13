# Desktop UX specification

Status: draft (Aug 2026) · Rev A · Desktop only; iPad in `IPAD_UX.md`

Companion to [ADR 0008](../decisions/0008-qt-shell.md) (Qt shell) and
[ADR 0009](../decisions/0009-documents-and-workspaces.md) (documents and workspaces). Those two
decided the *structure*. This decides the *interaction*, which is the part the next layer of code
would otherwise encode by accident.

The palette and stylesheet are already implemented in `shell_qt/src/Theme.cpp` — this document does
not restate them.

---

## Why this is written before more code

Four of the seven decisions below change signatures in `app/` or `render/`. Two of them
(`IPicker` resolution, in-progress command state) touch code that already exists and already has
tests. Specifying them now is a small edit; specifying them after the sketch environment,
assembly editing and drawing views each grow a private notion of "mode" is a cross-cutting
refactor.

The single most useful thing that fell out of writing this: **`Environment` is not a concept in
`app/` at all, and three separate future features all need it.** See decision 3.

---

## 1. Window anatomy

Top to bottom, no menu bar anywhere — Inventor has none, and on macOS a `QMenuBar` goes to the
global bar where it is invisible in screenshots and useless for discoverability.

| Band | Contents | Height |
|---|---|---|
| Title bar | OS-native. `vCAD — <active document title>` | native |
| QAT strip | Blue **File** tab, then New/Open/Save · Undo/Redo. Right-aligned: selection filter | ~28 px |
| Ribbon | Tab strip over a page of panels; collapse chevron at right | ~118 px, collapsible |
| Body | `Model` dock ‖ viewport ‖ `Properties` dock | fills |
| Document tabs | `Home` (uncloseable) then one per open document, `•` for modified | ~26 px |
| Status bar | message · selection + filter · coordinates · units · mesh stats | ~22 px |

Docks are 236 px (left) and 268 px (right), both resizable, both always present. The model
browser is not a panel you go and find.

---

## 2. Ribbon contents, per document kind

Tabs are **derived** from `(DocumentKind, Environment)` — never registered globally. This is
ADR 0009 decision 4 made concrete.

### Part · model environment

| Tab | Panels → commands |
|---|---|
| **3D Model** | **Sketch**: Start Sketch · **Create**: Box, Cylinder, Extrude, Revolve · **Modify**: Cut, Fillet, Chamfer, Shell, Hole · **Pattern**: Rectangular, Circular, Mirror · **Edit**: Undo, Redo, Delete |
| **Sketch** | **Manage**: Start Sketch, Edit Sketch, Delete Sketch |
| **Inspect** | **Measure**: Measure · **Analysis**: Section View, Mass Properties, Draft Analysis |
| **Annotate** | **3D Annotation**: Dimension, Note |
| **Manage** | **Parameters**: Parameters · **Cache**: Cache Status, Purge Local |
| **View** | **Navigate**: Fit, Ortho · **Appearance**: Shaded, Shaded+Edges, Wireframe · **Visibility**: Origin Planes, All Sketches |

Implemented today: Box, Cylinder, Cut, Fillet, Chamfer, Undo, Redo, Delete, Fit, Ortho.
Everything else is present and **disabled**, for the same reason the unimplemented document kinds
are visible on Home — the shape of the app should be honest from the start.

The **Manage → Cache** panel is worth noting: it surfaces the DDC in the UI. No other CAD app has
this because no other CAD app has a content-addressed recompute cache.

### Part · sketch environment

Collapses to a **single tab**, `Sketch`: **Draw** (Line, Circle, Rectangle) · **Constrain**
(Dimension, Coincident, Parallel, Tangent, Equal) · **Pattern** · **Exit** (Finish Sketch).

### Assembly · model environment

`Assemble` · `Design` · `Inspect` · `Manage` · `View`. Assemble carries **Component** (Place,
Create) and **Position** (Joint, Free Move). All disabled — no assembly editor yet.

### Drawing

`Place Views` · `Annotate` · `Sketch` · `Manage` · `View`. All disabled; needs `HLRBRep`.

---

## 3. Decisions

### 3.1 `Environment` becomes a first-class concept in `app/`

**The gap this document found.** Entering a sketch is not a command that returns — it is a *mode
the app enters for you*, which changes the ribbon, changes what a click selects, and changes what
the viewport draws. Three future features need exactly this: the sketch environment, assembly
component-edit-in-place, and drawing sheet editing.

Right now `Controller` has no representation of it, so each of those would invent its own. Add:

```cpp
enum class Environment : std::uint8_t { Model, Sketch, EditInPlace, Sheet };
```

owned per open document, with `enter(Environment, target)` / `exit()` on `Controller`. The shell
observes changes and rebuilds the ribbon; it does not decide the mode.

Crucially the app enters environments *for* the user. That is the honest version of FreeCAD's
workbenches, and it is why contextual tabs solve discoverability where a workbench switcher
destroys it.

### 3.2 Commands are non-modal, and their surface is a docked property panel

> **Superseded, Aug 2026.** This section originally specified an in-canvas mini toolbar floating
> over the viewport. Research into both reference applications
> ([UI_RESEARCH.md](UI_RESEARCH.md)) shows that is the minority pattern: SolidWorks uses the
> **PropertyManager** in the left panel, explicitly "without a dialog box covering the graphics
> area", and Inventor uses **property panels**, with mini-toolbars on only seven dialogs. The
> non-modal principle below was right; the surface was my inference and was wrong.

A command in progress takes over the **left dock**, replacing the model browser until it finishes.
The viewport is never covered and stays interactive, so selecting geometry feeds the running
command — which is precisely why both vendors do it this way.

It also suits our constraints better than the floating version: a docked panel is ordinary widget
layout, whereas a floating surface over a native GPU child window cannot be composited by Qt at all
— the same problem §3.6 raises for the ViewCube.

ADR 0008 promised non-modal and never said what it means. It means `Controller` needs real
**in-progress command state**, not just `execute`:

- a started command with partially-filled parameters
- a **preview**: an uncommitted feature that `Engine` evaluates but `History` does not keep
- commit or abandon

That last point is the expensive bit — it touches `Document`, `Engine` and `History` together.
Today commands are instantaneous, which is why Box simply appears at default dimensions.

**Blocks:** every command more interesting than a default-sized primitive.

### 3.3 Selection is filtered by entity type, globally and visibly

A segmented control at the right of the QAT: **Body · Face · Edge · Vertex**. Non-negotiable in
CAD — picking an edge inside a dense assembly is otherwise impossible.

`IPicker` currently resolves a pixel to one `ElementName`. With a filter it must resolve to the
**nearest entity of the requested type**: clicking a face interior with the Edge filter active
should find that face's bounding edges, not fail. The ID buffer already carries per-element
identity, so this is a resolution rule, not new GPU work.

**Open question — see §5.**

### 3.4 Feature state lives in the browser; errors are inline and non-blocking

Per-feature badges in the tree:

| Badge | Meaning | Behaviour |
|---|---|---|
| `ERR` | Feature failed to compute | Marked in place. Every feature that *did* compute still shows. No dialog, no rollback. Reason appears in Properties → Diagnostics. |
| `WARN` | Topological reference resolved but shifted | Model is valid; the reference moved. Reviewable. |
| strikethrough | Suppressed | Skipped by recompute. |

This exploits something already built: `Engine` supports partial failure, so one broken feature
does not invalidate the rest. Most CAD UIs throw a modal and leave you guessing which feature
broke.

`Controller::tree()` must carry this state. `Engine` already computes it and currently throws it
away.

### 3.5 Properties separate driving parameters from measured results

Two groups, visually distinct:

- **Parameters** — editable, unit-bearing, what you typed.
- **Measured** — italic, dimmed, never editable. Volume, mass, face count, degrees of freedom.

Volume is not a property you set. Conflating the two is how CAD UIs end up with greyed-out fields
whose greyness means four different things.

Fields take **text**, not numbers — `2 in` is valid input in an mm document, because unit parsing
lives in `Controller::setProperty`. Already true; this documents it as intentional.

### 3.6 ViewCube and navigation bar are viewport overlays

Top-right of the viewport: ViewCube, with a vertical nav bar (Home, Orbit, Pan, Zoom, Look At,
Fit) beneath it. Spatial controls belong next to the space they control, not in a toolbar.

**This has a real implementation consequence.** They sit *above* the bgfx surface, and a native
child window will not accept Qt widgets composited on top — that is not how native surfaces work.
So either:

1. **The renderer draws them** as part of the frame, or
2. the viewport becomes a composited surface, with a measurable cost at the 100k-part target.

**Recommendation: option 1.** More work, keeps the fast path intact. This decision lands the
moment bgfx is wired into a `QWidget`, so it should be settled *before* that work, not during.

### 3.7 Home is a project workspace and it surfaces the shared cache

Settled by ADR 0009; the UX addition is one strip at the top of Home:

```
Project: bracket-rev-c │ Search paths: 2 configured │ ● Shared cache online · 12 418 entries
```

That last field is the visible payoff of [ADR 0004](../decisions/0004-ddc-recompute.md) — "open
this project" also means "use the team's cache", so a colleague's tessellations and feature
results are already computed. Worth surfacing precisely because it is a genuine advantage over
every alternative.

New tiles: Part (enabled) · Assembly · Drawing · Presentation (disabled, labelled `Not yet`).
Recent list shows name, path, and relative time.

---

## 4. Status bar

Left: last action message. Right, in order: selection + active filter · cursor coordinates
(tabular figures) · units (`mm · deg`) · mesh stats (`3 mesh · 3 inst · 4 812 tri`).

The mesh stats are developer-facing today and should stay only until the renderer is trustworthy.
Coordinates require the picker to report a 3D point under the cursor, which the ID buffer does not
currently give — depth readback or a ray/BRep intersection. Small, but not free.

---

## 5. Open questions

Three, and they need your answer rather than my guess:

1. **Hover-cycling for ambiguous picks.** Inventor lets you hover an ambiguous spot and step
   through candidates under the cursor. Worth building, or is the type filter enough for v1?
2. **Shifted topological references: warn or block?** `WARN` in §3.4 risks a silently wrong model;
   blocking risks nagging on every legitimate edit. I lean **warn plus a review affordance**, but
   it is a judgment call about your users, and ADR 0005's naming system exists precisely to make
   this detectable.
3. **Viewport overlays: renderer-drawn or Qt-composited?** §3.6. I recommend renderer-drawn. This
   is the one with performance consequences at 100k parts, and the one I would most like settled
   before touching the viewport.

---

## 6. Build order

Dependency-ordered, cheapest-and-most-blocking first:

1. **`Environment` in `app/`** — §3.1. The missing shared concept; three features need it.
2. **In-progress command state in `Controller`** — §3.2. Unblocks every non-trivial command.
3. **Filtered picking in `IPicker`** — §3.3. A resolution rule over an ID buffer we already produce.
4. **Feature state in `Controller::tree()`** — §3.4. Surface what `Engine` already knows.
5. **bgfx into a `QWidget`**, carrying the GLFW → SDL3 switch — with §3.6 settled first, so the
   ViewCube is not retrofitted.

Steps 1–4 are all in `app/` and `render/` interfaces, all testable from Rust without a GPU, and
none require the viewport to work. That is deliberate: it keeps progress independent of the one
part of the stack I cannot verify in this environment.
