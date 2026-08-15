# How far is vCAD from SolidWorks and Inventor?

Competitive review, 15 Aug 2026, at commit `7f77406`. Every number about vCAD below was counted
from the tree today. Statements about the commercial tools are qualitative on purpose — precise
feature counts for them are marketing artefacts and change per release — but the structural claims
are ones anyone who has used both can check.

The short version: **the architecture is ahead of the features by an unusual margin, and the
distance to Inventor is not more of the work done so far — it is a different kind of work.**

---

## 1. What exists today, counted

| Measured | vCAD |
|---|---|
| Feature types that compute | **11** — Box, Cylinder, Fillet, Chamfer, Cut, Fuse, Common, Sketch, Extrude, Translate, Import |
| Sketch constraints | **11** — Coincident, Horizontal, Vertical, Parallel, Perpendicular, Distance, Radius, PointOnLine, EqualLength, LockX, LockY |
| Commands in `Controller`'s registry | **~24** |
| Ribbon entries that are DISABLED stand-ins | **46** of roughly 73 |
| Document kinds implemented | **1 of 4** — Part. Assembly, Drawing and Presentation are declared and inert |
| Product code | ~25,000 lines (`core` 9.6k, `render` 3.4k, `shell_qt` 3.4k, `abi` 3.1k, `proshell` 2.2k, `app` 2.1k, `rust` 1.6k) |
| Test code | ~10,500 lines — 35 C++ tests, 158 Rust tests |

The `planned()` count is the most honest number in the codebase. The ribbon deliberately shows the
application's intended shape with greyed stand-ins for commands that do not exist yet (ADR 0009
decision 2), so **46 of 73 is a self-reported completeness figure of about a third** — and it is
the easy third.

### One stale claim, found while writing this

`SHELL_INVENTORY.md` lists the 3D viewport as **Placeholder — Qt-painted**. That is no longer true:
`Viewport::paintEngine()` returns null and `paintEvent` submits directly when
`controller_.presentsDirectly()` is set, with the Qt readback kept only as the fallback path. The
inventory row should be corrected.

---

## 2. The distance, and why the feature count understates it

Three of the gaps are structural rather than a matter of writing more features.

**Assemblies do not exist, and assemblies are what these products fundamentally are.** Mates and
joints, sub-assemblies, in-context design, lightweight loading, interference detection, exploded
views, bills of materials. This is not forty more feature types bolted onto what we have: it needs
a document model with references *between* documents, and ours is single-document throughout.

**Drawings do not exist.** Projected, section, detail and broken views; dimensioning; GD&T; BOM
tables; sheet formats; revision blocks. The hard primitive is closer than it looks — OCCT's
`TKHLR` is already linked and does hidden-line removal — but the paper-space layer, the view
generation and the annotation model are all absent.

**Then the long tail**, which is laborious rather than interesting: sheet metal, weldments,
surfacing, mold tools, the pattern family, configurations and design tables, equations, splines,
blocks, derived sketches. SolidWorks carries on the order of a hundred feature types. We carry
eleven.

---

## 3. Where vCAD is genuinely ahead

Each of these is either measured in this repository or structurally true by construction.

### Cross-platform, and it is the one a user feels immediately

SolidWorks and Inventor are Windows-only. vCAD builds and tests on **five targets** — Windows x64
and arm64, Linux x64 and arm64, macOS arm64 — with a CI matrix that proves it rather than claiming
it. For an engineer on a Mac today the commercial answer is a virtual machine.

### The content-addressed cache

Neither incumbent has an equivalent. Deterministic compute plus content-addressed results gives
instant reopen and — the part nobody else offers — a **shared team cache**, where a colleague's
rebuild is your cache hit. ADR 0004.

### Determinism as an enforced contract

`CAD_PLUGIN_DETERMINISM_CHECK=1` runs every plugin compute twice and compares by the same hash the
cache keys on. Commercial rebuild is not reproducible in this sense; ours is a stated rule with a
switch that catches violations, and the cache is only sound because of it.

### The plugin ABI

Additive-only, `struct_size` negotiation, a golden snapshot that fails the build on drift, no
managed runtime anywhere in the boundary. SolidWorks places the compatibility burden on the add-in
author — hence runtime `GetVersion` branching and whole frameworks like xCAD to manage it. Inventor
couples to .NET and therefore inherits Microsoft's breaking changes from outside Autodesk. Ours is
structurally better on both counts. See section 5 for the catch.

### Topological naming built in rather than retrofitted

Property-tested in `core/naming`, and exposed to plugins through `element_resolve` /
`element_name_of`. The claim here is *not* that it beats Parasolid — it has had far less geometry
thrown at it. The claim is that it was designed in from the start and is testable, which is what
FreeCAD spent years and a fork unwinding.

### Memory safety on the one surface where input is hostile

The DXF reader is Rust behind a narrow C seam, differentially fuzzed against the C parser it
replaced, which found two real bugs on its first run. Importers are the CVE surface in this
industry.

### Testing discipline

Property tests, permutation tests, differential fuzzing, determinism checking, a boundary probe
that fails if the reusable shell touches a domain type, and a lint forbidding duplicate FFI
declarations. Not a claim about being better than the incumbents' internal QA — it is a claim about
*kind*, and this kind is unusual.

---

## 4. Where vCAD is behind architecturally, not just featurally

### The kernel — the one gap that cannot be engineered around

OCCT versus **Parasolid** (SolidWorks) and **ShapeManager/ACIS** (Inventor). Parasolid is more
robust on hard booleans, blends against thin walls, and self-intersecting sweeps; OCCT fails where
it succeeds. That is a thirty-year investment gap and cleverness does not close it. Healing passes
and validation gates mitigate the symptoms and do not equalise the capability.

This should shape ambition honestly: vCAD can be excellent at everything *around* the kernel, and
will lose specific hard-geometry cases to both incumbents for the foreseeable future.

### Measured performance ceilings

- `Document::add` is **quadratic** (n^1.85 measured, profiled with samply).
- The sketch solver is **cubic** (n^2.9), with 98% of it inside a dense QR in `GCS::System::diagnose`.

Both are known, both are fixable, and until they are, "handles a 10,000-part assembly" is not a
claim vCAD can make.

### The plugin ABI has no loader

No third-party plugin has ever been loaded. Every plugin test to date runs an in-process fake. It
is a well-designed contract with zero clients, and until step 6 of PLUGIN_CONTRACT lands, "our
plugin ABI works" means "our plugin ABI compiles".

### No crash or dependency isolation

In-process native plugins mean a segfault takes the application down — the same position Inventor
is in, and §5 says so. Dependency isolation between plugins, which the study in §6A identified as
the classic ecosystem killer, is currently a paragraph rather than a mechanism.

### Everything outside the modelling core

No PDM, no CAM, no simulation, no rendering pipeline for presentation, no configurations. No
add-ins, no training material, no installed base. Architecture does not beat network effects.

---

## 5. The honest summary, and the risk in it

**The expensive-to-retrofit things were done first.** Topological naming, a versioned plugin ABI, a
content-addressed cache, deterministic compute, a reusable shell, five-platform CI. FreeCAD did it
the other way and spent years unwinding naming; that ordering decision looks right.

**The characteristic risk of that ordering is architecture that never meets features.** The
distance to Inventor is not one or two more years of the work done so far. It is long,
domain-heavy, unglamorous work that is largely not architecturally interesting — and a codebase
whose culture is architectural rigour is exactly the codebase that finds that work least
attractive.

### The one target worth naming

If a single objective would move vCAD from "impressive architecture" to "a tool someone could
use", it is **assemblies**.

- It is the only remaining gap that is *architectural* rather than laborious — it needs
  inter-document references, which the document model does not have.
- It is what these products fundamentally are. A part modeller with no assembly is a component of
  a CAD system, not a CAD system.
- It would exercise the document model, the naming layer and the cache together, at a scale a
  single part never reaches — which is precisely where the quadratic `Document::add` and the cubic
  solve stop being footnotes.

Drawings are the second, and they are more laborious than architectural — worth doing after, and
cheaper than they look because hidden-line removal is already linked.
