# Selection: what a click or a tap resolves to

Researched 19 Aug 2026 against current vendor documentation. Sources at the end.

The question that prompted this: **tap-to-select on iPad, in a scene with thousands of parts.** A
finger is not a mouse, and the difference is not solved by making the hit test more accurate — it is
solved by making it *ambiguous on purpose* and then ranking the candidates.

Everything here is a rule about what a pointing gesture MEANS, so all of it belongs in `app/`, shared
by both shells. `docs/design/IPAD_UX.md` already recorded selection as a shared concern; this is the
detail behind that row.

---

## 1. Nobody picks a single pixel

The universal technique — old enough to be in patents from the 1990s, and still what everything does
— is a **picking aperture**: a square of pixels around the pointer, not the one pixel under it. The
prior art literally says the aperture "may be the coordinate of a single pixel or coordinates that
represent a plurality of pixels, such as a square of 16 by 16 pixels."

This matters more than it sounds, and it is not only a touch concern:

- **An edge is one pixel wide.** Requiring the user to land on that pixel makes edge selection a
  test of motor control. Every desktop CAD application has a tolerance of several pixels for exactly
  this reason, which is why clicking *near* an edge in SolidWorks selects it.
- **A vertex is worse.** It is a point.
- **A finger is enormous.** Apple's own guidance puts the minimum touch target at **44×44 points** —
  88×88 device pixels on a Retina iPad — because that is roughly a fingertip. MIT's Touch Lab
  measured the average fingertip at 16–20 mm. A one-pixel hit test on a tablet is not a strict
  version of selection; it is a broken one.

**So the aperture is a parameter, and the shells differ only in what they pass.** A mouse passes a
few pixels; a finger passes tens. That single number is the entire desktop/tablet difference in the
hit test, which is what makes the rest of the logic shareable.

vCAD's renderer can already do this. `IPicker::pickRect` reads a rectangle out of the id buffer
today — it simply throws the positions away and returns a deduplicated set. The pick pass renders
the scene and reads back regardless of aperture size, so **a large aperture costs nothing extra**.

## 2. Nearest is not "nearest depth"

Once the aperture returns several candidates, something must choose. Depth alone is the obvious
answer and the wrong one: the frontmost thing under a fingertip is almost always a *face*, because
faces cover area and edges do not. Rank by depth and edges become unselectable by touch.

The rule that matches how the incumbents behave:

1. **Kind first — vertex, then edge, then face.** Small targets win over large ones. This is what
   makes "tap near an edge" select the edge rather than the face it borders.
2. **Then screen distance** from the tap centre, so between two edges the nearer one wins.
3. **Then depth**, which only decides between candidates that are equally close and the same kind.

When a `SelectionLevel` other than Body is active, it *restricts* rather than reorders: the level
decides what a pick resolves to, which is already this codebase's rule.

## 3. Ambiguity is resolved by asking, not by guessing

Every serious CAD application has an escape hatch for "you picked the wrong one, and I cannot know
that":

- **SolidWorks — Select Other.** Right-click and get a list of the entities behind the cursor;
  hovering a row highlights it in the model.
- **Shapr3D — overlap pop-up.** "When multiple items overlap in the same area, a pop-up list appears
  so you can select the exact sketch, face, or edge."
- **Onshape — Precision Selector.** Touch and hold and a magnified crosshair appears, *offset from
  the finger*, so the thing being selected is not under the hand selecting it.

The common shape: the same query returns a **ranked list**, and the interaction decides whether to
take the first entry or show them all. So the shared API returns candidates, and taking the top one
is the caller's shortcut — not a different code path.

## 4. Touch selection is a toggle

Onshape: "tap to select, tap again to deselect"; double-tap clears everything. This is *not* the
desktop convention, where a plain click replaces the selection and Ctrl adds to it — and the reason
is that a tablet has no modifier key. Additive selection has to be the default when there is nothing
to hold down.

vCAD follows the platform rather than forcing one convention onto both: `additive` is already a
parameter of `Controller::clickAt`, and the shells pass what their input can express.

## 5. Box selection has a direction

Recorded for when it lands, because it is a detail people get wrong from memory: dragging
left-to-right selects only what is **entirely inside** the box; right-to-left selects everything the
box **touches**. Onshape draws the first with a solid outline and the second dotted. AutoCAD,
SolidWorks and Inventor all behave this way.

---

## What vCAD builds

| Piece | Where | Status |
|---|---|---|
| Aperture read of the id buffer | `render/`, `IPicker::pickAperture` | new — `pickRect` already reads the region and discards positions |
| Candidate ranking (kind, distance, depth) | `app/`, `SelectionRanking` | new, pure, no GPU — unit-testable |
| `candidatesAt(x, y, radius)` | `app/Controller` | new; the list behind Select Other |
| `tapAt(x, y, radius, additive)` | `app/Controller` | new; takes the top candidate, toggles on re-tap |
| Aperture radius per input | shells | mouse ≈ 4 px, finger ≈ 44 pt |
| Select Other / precision selector UI | shells | deferred; the query it needs exists from day one |
| Box select, direction-dependent | `app/` | deferred, specified above |

The deliberate consequence: **the iPad and the desktop cannot disagree about what a tap means**,
because both call the same function and differ only in the radius they pass.

---

## Sources

- [Selecting geometry — Shapr3D Help Center](https://support.shapr3d.com/hc/en-us/articles/7770768736924-Selecting-geometry)
- [Selection — Onshape Help](https://cad.onshape.com/help/Content/Home/selection.htm)
- [Tech Tip: Mastering Gestures Mobile — Onshape](https://www.onshape.com/en/resource-center/tech-tips/tech-tip-mastering-gestures-mobile)
- [Select Other — SOLIDWORKS Help](https://help.solidworks.com/2021/english/SolidWorks/sldworks/HIDD_SelectOther_dlg.htm)
- [Selecting Faces with SOLIDWORKS Select Other tool — Javelin](https://www.javelin-tech.com/blog/2016/06/solidworks-select-other-tool/)
- [Method and apparatus for improved graphics picking using auxiliary buffer information — US 6,072,506](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6072506) (picking aperture)
- [Touch Target Sizes — LukeW](https://www.lukew.com/ff/entry.asp?1085=) and Apple's 44×44 pt minimum
