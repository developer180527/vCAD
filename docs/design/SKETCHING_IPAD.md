# Sketching with a stylus: what Shapr3D actually does

Researched 19 Aug 2026 against Shapr3D's current documentation and its user community. Sources at
the end. Where something is inference rather than a documented statement, it says so.

This is the companion to [SELECTION.md](SELECTION.md) and answers the question that shapes the code
rather than the polish: **how does a pen stroke become geometry?**

---

## 1. A stroke, not a series of taps

The headline finding, and it settles the design question that was open:

> "Use your pen to start drawing a line, just like you would on paper, and to finish the line, lift
> your pen."

Pen down, drag, lift — **one segment per stroke**. Not tap-a-start-point-then-tap-an-end-point, which
is how the desktop sketcher works and how vCAD's `SketchDrawing` works today. The next stroke
continues from the endpoint, so a run of strokes chains the same way a run of clicks does.

**Automatic Line/Arc** is the flagship: one tool, and "Shapr3D automatically switches your sketch to
a line or arc depending on your pen gesture". The mode is explicitly described as a *pen* feature.
The documentation does not state the decision rule; measuring how far the stroke departs from the
straight chord between its endpoints is the obvious implementation and is what vCAD will do, but it
is inference, not a documented fact.

Note what this is **not**: it is not freehand shape recognition. You are not scribbling a rough
rectangle and having four lines inferred. Each stroke is one primitive, and the only inference is
*which* primitive.

## 2. Constraints are applied while you draw, and connection constraints are not optional

Two settings, and the distinction between them is the important part:

- **Auto-constraining ON** — Horizontal/Vertical, Perpendicular, Tangent and Coincident are applied
  as you sketch. Tangent specifically "when an arc is created at an endpoint".
- **Auto-constraining OFF** — "the only constraints that are automatically created are between
  connected endpoints or midpoints."

So coincidence at a join is *never* optional. That is the right instinct: a chain whose segments
merely touch is not a profile, and the user cannot see the difference until Extrude refuses. vCAD
already learned this the expensive way — see MODELLING_UX.md §2b, where the absence of endpoint
snapping made closed profiles nearly impossible to draw.

## 3. Snapping is a list of named things, and it is worth copying literally

Shapr3D's Snapping Options enumerate:

| Snap | What it catches |
|---|---|
| **Grid** | points on the active plane's grid |
| **Sketch guidelines** | purple extensions from existing elements, showing where coincident/tangent is available |
| **Sketch guidepoints** | endpoints, midpoints, arc centres, profile centres |
| **3D guidepoints** | vertices, edges, edge midpoints, face and hole centres of solid bodies |
| **Distant edges** | geometry off the sketch plane, projected in, when in an orthogonal view |

The guideline idea is the one vCAD is missing and should take: a purple ray extended from an
existing element, shown *while* drawing, that tells you a constraint is available before you commit
to it. It makes the inference visible instead of surprising.

## 4. Dimensions: typed after, not during — and this is their weak spot

Editing is: tap the element, tap the dimension label, type on a numpad, tap the check. The field
takes **expressions** — `12+34`, `50/2` — which is a genuinely good detail worth copying. Dimension
kinds are length, diameter, radius and angle.

But live dimensions *while drawing* are a long-standing user complaint: dimensions "show when you tap
a line, until you select something else", and the community asks repeatedly for persistent display.

**vCAD already does better here** and should keep it: live length and angle appear while a segment is
being drawn, in the document's units, with Tab to lock a value. That was built for the desktop and
is not a Shapr3D copy. Recording it here so nobody "fixes" it toward the reference later.

## 5. What NOT to copy: timing-dependent modes

The clearest lesson from the user community, and it is a criticism:

> "you need to be just a little quicker moving the pencil after locating the start point. If you get
> the selection tool, then there was too long of a pause before moving."

Tool behaviour that depends on how long a finger dwells is unlearnable — the user cannot see the
timer, so the same gesture produces different results and it reads as unreliability. Another user
resolved their sketching difficulties by turning off every Pencil Pro feature (squeeze, double-tap,
hover, haptics), which is a strong signal about layering more gesture meanings onto the pen.

**vCAD's rule: mode comes from WHAT is touching the screen, never from how long.** `UITouch.type`
already distinguishes a stylus from a finger, and that distinction is free, instantaneous and
visible to the user because they chose which instrument to pick up.

---

## What vCAD builds, and in what order

The shared layer already has most of this. `SketchDrawing` works entirely in sketch 2D coordinates,
chains segments, snaps to endpoints within a pixel tolerance, infers horizontal/vertical, and
supports typed lengths with Tab-lock. `core/sketch` already stores arcs (`GeoKind::Arc`,
`addArc`), and planegcs solves the constraints. None of that needs rewriting for a tablet.

| Step | Where | Note |
|---|---|---|
| 1. Pick a plane or face, enter the sketch | `app/` (exists) | `pickSketchFace` + `beginSketchOn` already do this; the iPad needs the gesture wired |
| 2. Stroke input: down, move, up | `app/SketchDrawing` | **new** — a stroke API beside the click API, not instead of it. The desktop keeps clicking. |
| 3. Line vs arc from the stroke | `app/` | **new**, pure, testable: deviation from the chord decides |
| 4. Tangent constraint when an arc starts at an endpoint | `app/` | **new**; core supports it |
| 5. Stylus draws, finger navigates | `shell_ios` | from `UITouch.type`, no timers |
| 6. Sketch guidelines (the purple rays) | `app/` + shells | **new**, and the piece that makes inference visible |
| 7. Expression input in dimension fields | `app/` | `units::parse` already exists; expressions do not |

Steps 2–4 are the substance and are all shared C++, which is the point: the desktop gets stroke
input for free the day someone drags with a mouse held down, and an Android shell inherits the whole
of it.

---

## Sources

- [Automatic Line/Arc — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7770681244316-Automatic-Line-Arc)
- [Line — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7874250627484-Line) and [Arc](https://support.shapr3d.com/hc/en-us/articles/7874294891804-Arc)
- [Constraint Settings — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7394385784476-Constraint-Settings)
- [Snapping Options — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7873946289564-Snapping-Options)
- [Editing sketch dimensions — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7874245890844-Editing-sketch-dimensions)
- [Sketching in Shapr3D — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/18816009328284-Sketching-in-Shapr3D)
- [Shapr3D manual: drawing lines and arcs with a pen — Novedge](https://novedge.com/blogs/design-news/shapr3d-manual-drawing-lines-and-arcs-with-a-pen-sketching)
- [Apple Pencil instructions — Shapr3D Community](https://discourse.shapr3d.com/t/apple-pencil-instructions/35588) (the timing criticism)
- [Dimensions while sketching — Shapr3D Community](https://discourse.shapr3d.com/t/dimensions-while-sketching/21342) (the dimension complaint)
