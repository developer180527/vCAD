# Desktop shell inventory

The checklist for building the **whole window as non-functional UI first**, then attaching
behaviour. Derived from [UI_RESEARCH.md](UI_RESEARCH.md) (what SolidWorks and Inventor actually
have) and [DESKTOP_UX.md](DESKTOP_UX.md) (what we decided).

## Why this list exists

"Build the UI completely" needs a definition or it never finishes — SolidWorks has hundreds of
surfaces and we are not building all of them. This is the bound: **every region either appears
below or is deliberately out of scope.** When every row is Present, the shell is done and the work
becomes attaching behaviour.

Two rules keep the exercise honest:

1. **Build containers, not decoration.** A region earns its place by being somewhere functionality
   will plug in. A pretty panel with nowhere for logic to live is worse than no panel.
2. **Disabled, not absent.** A command that does not work yet is greyed, never hidden — the
   convention already used for the unimplemented document kinds and most of the ribbon. It keeps
   the app's real shape visible and stops the layout moving when behaviour lands.

---

## Regions

| Region | Status | Notes |
|---|---|---|
| Title bar with document name and dirty marker | **Present** | |
| QAT: File tab, New/Open/Save, Undo/Redo | **Present** | |
| QAT right: selection filter (Body/Face/Edge/Vertex) | **Present** | Shell-owned until `IPicker` filters |
| Ribbon: tab strip, panels, large/small buttons, captions | **Present** | |
| Ribbon: derived tabs per document kind and environment | **Present** | ADR 0009 |
| Ribbon: collapse chevron | **Present** | |
| Ribbon: split buttons with drop-downs | **Present** | Combine has one |
| Model browser (feature tree) | **Present** | |
| Browser: per-feature icons | **Present** | |
| Properties dock | **Present** | |
| Document tabs along the bottom | **Present** | |
| Status bar: message, units, stats | **Present** | |
| Home workspace: rail, project strip, recent | **Present** | |
| 3D viewport | **Placeholder** | Qt-painted; bgfx is a separate job |
| Sketch canvas | **Present** | Draw, snap, select, constrain, delete |
| Logging | **Present** | Categories, file sink beside the binary, Qt and OCCT adopted |
| — | | |
| Command property panel (left dock takeover) | **Present** | Box, Cylinder, Extrude. Retired the QInputDialog stopgap |
| **ViewCube** | **MISSING** | In the placeholder's paint only, not a real widget |
| **Navigation bar** | **MISSING** | Same |
| **Heads-up view toolbar** | **MISSING** | SolidWorks pattern: view controls inside the graphics area |
| Context toolbar on selection | **Present** | Sketch canvas; positioned above the selection |
| Marking menu (radial right-click) | **Present** | Eight fixed wedges, dead zone, disabled not absent |
| Options / Settings dialog | **Present** | Units, navigation, inference tolerances. **Not persisted across restarts** |
| **Browser: rollback marker as a draggable row** | **MISSING** | Works from the context menu; not draggable |
| Browser: feature state badges | **Present** | ERR / BLOCKED / dirty, and strikethrough for rolled-back |
| Browser: context menu | **Present** | Edit Sketch, Rename, Roll Back to Here, Roll Forward, Delete |
| Sketch: dimension display | **Present** | Amber, with extension lines, offset off the geometry |
| Sketch: constraint glyphs | **Present** | Green; coincidence as a dot rather than a letter |
| Sketch: DOF readout in the status bar | **Present** | Curves, constraints, and the DOF or conflict count |
| **Task pane** (right, libraries/appearances) | Out of scope | Needs content that does not exist |
| **InfoCenter / search** | Out of scope | |

---

## Build order

**Status, Aug 2026: items 1, 2, 3, 5 and 6 are done.** What remains is item 4, which is gated on a
decision, and item 7, which is small. Ordered by *how much functionality each unblocks*, not by
visual impact.

1. **Command property panel.** The left dock swaps from `Model` to the running command and back.
   Everything with a parameter needs it — extrude distance, fillet radius, sketch dimensions — and
   it replaces the `QInputDialog` stopgap already noted in the Radius command.
2. **Browser badges and context menu.** State is already computed and thrown away at the UI edge.
3. **Sketch dimensions and constraint glyphs.** Without them the sketcher is write-only: you can
   apply a constraint and then cannot see that you did.
4. **ViewCube and navigation bar as real widgets**, positioned as Inventor allows (any corner,
   optionally linked). They must be **drawn by the renderer** rather than composited by Qt over a
   native surface — DESKTOP_UX 3.6, still the open question that blocks the bgfx viewport.
5. **Options dialog.** Units and navigation preset first; both already exist in `app/`.
6. **Context toolbar**, then **marking menu**. Both selection-driven, both now possible.
7. **Rollback marker in the tree**, draggable.

Items 1–3 and 5 are ordinary widget work with no dependency on the renderer, which is the point of
doing the shell first: **none of it is blocked by the instancing bug.**

## What this deliberately does not include

The 3D viewport stays a placeholder throughout. Wiring bgfx is its own job with its own
prerequisite (item 4's compositing decision), and mixing it into shell work is how both end up
half-done.
