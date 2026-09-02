# Is what we built actually reachable? — and how far is that from industry CAD

21 Aug 2026, measured from the source at `d90250a` plus the working tree. Two questions, because
they have different answers:

1. **Of the capability that exists, how much can a user reach?** This has been the recurring defect
   in this project — three separate capabilities have shipped complete and invisible.
2. **How far is the reachable part from a CAD system someone uses for a living?**

The short answers: **the application layer is now almost fully wired (48 of 56 methods), and the
gaps have moved outward — to the renderer and to the feature catalogue.** And the distance to
industry CAD is no longer mostly about the kernel; it is about the sketcher, assemblies, and
drawings, in that order.

---

# Part 1 — the wiring audit

## 1.1 Feature types → commands → shells

Both shells build their tools from `Controller`'s command catalogue by design, so a feature type
with no command does not exist as far as any user is concerned.

| Feature type | Command | Qt shell | iPad shell |
|---|---|---|---|
| Box, Cylinder | yes | yes | yes |
| Extrude, Sketch | yes | yes | yes (own entry points) |
| Fillet, Chamfer | yes | yes | yes |
| Cut, Fuse, Common | yes | yes | yes |
| **Hole** | yes *(added today)* | yes | **no** |
| **Revolve** | **none** | — | — |
| **Translate** | **none** | — | — |
| **Plane** (datum) | **none** | seeded only | seeded only |
| Import | none, by design | File ▸ Import | **no** |

**14 feature types, 11 reachable.** Revolve and Translate compute, are tested, and have no route at
all. `Plane` is created automatically as the three origin planes (`SketchEnvironment.cpp`) and can
never be created by a user, so there is no offset or angled datum — which is the prerequisite for
sketching anywhere other than on an existing face.

Import is deliberately not a command: it needs a file dialog, so the shell owns it. That is a
defensible exception, but it is also why the iPad shell cannot import anything.

## 1.2 The iPad shell reaches less than the desktop

Six commands exist that the iPad shell never names:

```
feature.hole  feature.sketch  sketch.cancel  sketch.edit  sketch.finish  view.fit
```

Four of those are false alarms — the iPad shell drives sketching and fit through its own bridge
(`beginSketchAt`, `finishSketch`, `cancelSketch`, `fitView`) rather than through command ids, which
is reasonable for a touch shell whose sketch entry is a tap on a plane. **`feature.hole` is a real
gap**, and Import is another.

## 1.3 The application layer is in good shape

Cross-referencing every public method of `Controller` against both shells:

- **56 public methods, 48 referenced by a shell.**
- The 8 that are not: `scriptNextPick` and `scriptPickForTest` (test seams, by design), `saveDigest`
  (internal to `modified()`), `elementSelection` / `selectionLevel` / `hoveredElement` (getters whose
  setters are used), `selectElement` and `alignViewTo` (used inside `app/` itself).

None of these is a stranded capability. **This is the layer that used to be the problem and is not
any more** — worth stating, because the audits before this one found the opposite.

## 1.4 The gaps have moved to the renderer

Three capabilities exist below the seam with nothing driving them:

- **Box / lasso select.** `IPicker::pickRect` is implemented in both backends. `Backend.h` says
  "box/lasso select falls out of it for free". **Zero callers anywhere.** Every CAD user reaches for
  a rubber-band selection within minutes.
- **Display modes.** `SceneFrame::showShaded` and `showEdges` are honoured by the bgfx backend and
  are **never set by anything** — permanently true. The View tab carries Shaded, Shaded + Edges and
  Wireframe as three greyed stand-ins. This is one setter and three menu entries.
- **Section views.** `SectionPlane` exists and is driven only by the sketch "Slice" view. The Inspect
  tab's Section View is a stand-in, though the mechanism underneath it works.

## 1.5 The sketcher: what solves versus what can be applied

| | Exists | Reachable |
|---|---|---|
| Constraint kinds | **12** | **7** — Distance, Radius, Horizontal, Vertical, Parallel, Perpendicular, EqualLength |
| Geometry kinds | **4** — Point, Line, Circle, Arc | **3** tools — Line, Circle, Rectangle |

Unappliable: **Coincident** and **Tangent** (applied automatically when a chain joins, never on
demand), **PointOnLine**, **LockX**, **LockY**. A user therefore cannot fully constrain a sketch they
can draw — the locks are the only way to pin a point in place, and there is no way to invoke them.

**Point geometry has no tool at all**, and an arc can only be produced by a stylus stroke, so a
mouse user cannot draw one.

## 1.6 The ribbon's own self-report

**71 entries, 46 stand-ins → 25 live, 35%.** Four tabs — Inspect, Manage, Annotate, Collaborate —
remain entirely aspirational.

## 1.7 The pattern, and the guard that would end it

Three capabilities have now shipped complete and unreachable: the viewport click, export, and
Hole/Revolve/Translate. Each was found by a human noticing, not by a test.

The check is mechanical and takes a minute to run by hand — every registered feature type either has
a command or an explicit, reasoned exemption. **It should be a test.** `Import` and `Plane` would be
exempt with their reasons recorded, which is the point: the exemption list forces the decision to be
made once and written down, instead of a feature quietly never being wired.

---

# Part 2 — distance to an industry CAD

Measured against what a mechanical engineer actually does in a day, in the order they do it.

## 2.1 Sketching — **the weakest link, and everything stands on it**

| | Industry standard | vCAD |
|---|---|---|
| Entities | line, arc, circle, ellipse, spline, polygon, slot, text | line, circle, rectangle (arc by stylus only) |
| Editing | trim, extend, offset, mirror, fillet, chamfer, split | **none** |
| Constraints | ~12 kinds, all applicable, with a manager | 12 solve, 7 applicable |
| Dimensions | driving, named, referenced across features | driving; no names, no expressions |
| Feedback | inference glyphs, DOF readout, conflict highlighting | inference, DOF readout, conflict message |

**Verdict: this is the gap that most limits what can be modelled.** No trim and no offset means any
profile that is not a simple closed loop of lines and circles has to be drawn exactly right the first
time. Rectangle landing recently is the single biggest improvement to date; trim and offset are the
next two.

## 2.2 Part modelling — **the middle of the pack, honestly placed**

| | Industry standard | vCAD |
|---|---|---|
| Sketched features | extrude, revolve, sweep, loft, rib | extrude; revolve exists, unreachable |
| Applied features | fillet, chamfer, shell, draft, hole wizard, thread | fillet, chamfer, hole (plain) |
| Patterns | linear, circular, mirror, sketch-driven, table | **none** |
| Direct edit | move/rotate/delete face, push-pull | **none** |
| Multi-body | split, combine, boolean between bodies | booleans yes; split no |

**Patterns and mirror are the two that change how long a real job takes**, because they are how one
feature becomes twenty. A bolt circle is currently eight holes drilled by hand.

`Hole` is a plain cylindrical cut — no counterbore, countersink, tapped sizes or standards tables,
which is what "hole wizard" means in the industry.

## 2.3 Assemblies — **absent, and architectural**

Industry: mates and joints, sub-assemblies, in-context design, interference detection, exploded
views, bills of materials, lightweight loading of thousands of parts.

vCAD: **one document kind of four.** This is not a feature backlog; it needs references *between*
documents, which the document model does not have. It is the largest single piece of architecture
still to be written, and the audits have said so consistently.

## 2.4 Drawings — **absent, and mostly laborious**

Industry: projected/section/detail/broken views, dimensioning, GD&T, BOM tables, sheet formats,
revision blocks, associativity back to the model.

vCAD: nothing. The hard primitive is closer than it looks — OCCT's `TKHLR` (hidden-line removal)
ships with the kernel, though it is **not** linked today — but paper space, view generation and the
annotation model are all unwritten.

## 2.5 Interop and data — **the closest to parity**

| | Industry standard | vCAD |
|---|---|---|
| Import | STEP, IGES, STL, DXF, native competitors' formats | STEP, IGES, STL, DXF |
| Export | STEP, IGES, STL, DXF, PDF, 3MF | STEP, IGES, STL + DXF for sketches |
| Native format | versioned, forward-compatible | `.vpart`, versioned |
| PDM / versioning | check-in/out, revisions, where-used | none |

Import and export are genuinely comparable for the formats that matter most. **No autosave and no
crash recovery** is the glaring reliability gap — for a tool people keep a day's work in, that is a
bigger risk than any missing feature.

## 2.6 Performance — **fine now, known ceilings later**

- `Document::add` is **quadratic** (n^1.85, profiled).
- The sketch solver is **cubic** (n^2.9), 98% inside a dense QR.

Neither bites at current sizes. Both bite the day assemblies arrive — which is exactly when they
stop being footnotes.

## 2.7 Where vCAD is genuinely ahead

Not a consolation list; these are things the incumbents cannot easily retrofit.

- **Cross-platform.** Five targets in CI plus an iPad shell running the same core. SolidWorks and
  Inventor are Windows-only; the answer for a Mac user is a virtual machine.
- **A content-addressed compute cache**, which makes reopen instant and a shared team cache possible.
  Neither incumbent has an equivalent.
- **Determinism as an enforced contract**, checkable with an environment variable.
- **A plugin ABI that is additive-only**, with a golden header snapshot, a real loader, and a museum
  that loads a frozen binary from every ABI generation on every test run. SolidWorks puts the
  compatibility burden on the add-in author; Inventor inherits .NET's breaking changes.
- **Topological naming designed in rather than retrofitted**, and property-tested.
- **A memory-safe importer**, differentially fuzzed against the parser it replaced.
- **A domain-neutral shell** (`modules/proshell`) with a probe that fails the build if it touches a
  domain type — a second professional application inherits the whole frame.

## 2.8 The honest summary

**As a modelling tool: usable for simple prismatic parts, and nothing beyond.** You can sketch on a
face, extrude, fillet picked edges, drill a hole, and export a STEP file. The moment a part needs a
pattern, a trimmed profile, a swept feature or a second body positioned against the first, it stops.

**As a platform: ahead of its feature list by an unusual margin**, and that was the deliberate bet.

### The order the evidence supports

1. **Wire what exists** — Revolve, Translate, a datum Plane, Hole on iPad, box select, display
   modes. Days, not months, and it widens the vocabulary immediately.
2. **Sketch editing** — trim, offset, mirror, and an arc tool for mouse users. The floor everything
   else stands on.
3. **The five unappliable constraints**, so a sketch can actually be fully constrained.
4. **Patterns and mirror.** The largest multiplier on real work.
5. **Measure**, which is entirely missing and one call from geometry the user can already select.
6. **Autosave and crash recovery.** Not a feature; a reason to trust the tool with a day's work.
7. **Assemblies**, which is the next real piece of architecture.

Nothing on that list is blocked by the kernel, the naming layer, the cache or the ABI. That is the
useful conclusion: **the remaining work is breadth, not depth.**
