# Where vCAD stands

Last audited: 13 Aug 2026, at commit `46fcf65`. Measured from the repository,
not estimated.

Re-audit this file rather than trusting it. Every claim below was checked
against code on the date above, and the fastest way to make it lie is to read
it six months from now.

- - -
## One paragraph

The foundations are real: the geometry kernel, topological naming, parametric
recompute with a content-addressed cache, a persistent document format, a
tested C ABI, and a desktop shell that looks like Inventor. The **capability** is
thin: nine kernel operations, no sketcher, and a viewport that is still drawn
by Qt because the renderer's instancing is broken. The project is strong
exactly where hobby CAD projects fail and empty exactly where they normally
start.

- - -
## What works


|Subsystem                   |State  |Notes                                                                     |
|----------------------------|-------|--------------------------------------------------------------------------|
|Geometry kernel (OCCT 8.0.1)|Working|guarded exceptions, `Result\<T>`, no raw OCCT above `core/kernel`         |
|Topological naming          |Working|survives feature edits; property-tested. FreeCAD's oldest bug, solved here|
|Document + undo/redo        |Working|immutable with structural sharing, so undo costs nothing                  |
|Recompute engine            |Working|dirty propagation, partial failure, content-addressed cache keys          |
|DDC cache (assetlib)        |Working|two-tier local + shared; a colleague's results can be reused              |
|Units                       |Working|compile-time dimensions, mm/rad base, text-in parsing                     |
|**Native format (`.vpart`)**|**Working**|SQLite, schema v1, atomic saves. Save / Save As / Open all wired          |
|Foreign import/export       |Working|STEP, IGES, STL                                                           |
|C ABI + Python bindings     |Working|ABI 1.5, hand-written Rust FFI as the reference consumer                  |
|Qt desktop shell            |Working|ribbon, QAT, model browser, properties, document tabs, Home               |
|Test infrastructure         |Working|5 tiers, ~65 Rust tests, Catch2, 9 pytest, CI on macOS/Linux/Windows      |

### Kernel operations, all of them

`Box` · `Cylinder` · `Fillet` · `Chamfer` · `Cut` · `Fuse` · `Common` · `
Translate` · `Import`

Every one is now reachable from the UI. That was not true before commit `93c3232`
: the ribbon rendered Fillet and Chamfer as disabled buttons because no `
Controller` command existed, while the kernel had done both correctly for months.

- - -
## What does not work

### 1\. There is no sketcher, the single largest gap

Every real feature in Inventor's Create panel (Extrude, Revolve, Sweep, Loft,
Rib, Emboss) consumes a **sketch**, and a sketch requires a 2D geometric
constraint solver. **OCCT does not have one**, it does geometry and topology, not
constraint solving.

Status: **zero**. `spikes/planegcs_standalone/` is an empty directory; the
solver was listed in `STACK.md` and never de-risked. There is no vcpkg port for
planegcs, so adopting it means vendoring it in-tree like `assetlib`, after
verifying its licence.

Parametric CAD is sketch + constraints + history. We have the history.

### 2\. The renderer's instancing is broken, and every scale number is void

`spikes/scale 8 1` uploads eight distinct transforms and the framebuffer
contains **one box** at instance 0's transform. It reproduces on both the
persistent buffer and bgfx's transient one, and Metal advertises `
BGFX_CAPS_INSTANCING`. **Root cause not found.** See ADR 0007's amendment.

Consequences:

- Every published scale figure, 100k parts at 40 draw calls, the triangle
  counts, the frame times, measured one transform drawn N times. All void.
- The viewport in the shell is a **Qt-drawn placeholder**, not the GPU path.
- Presentation into a real window has never been attempted.

The rule that came out of it, which generalises past rendering: **a rendering
claim is not established by a counter.** Any scale or correctness claim needs a
pixel assertion on a scene with more than one part at more than one transform,
before the work lands.

### 3\. Everything past a single part

No sketches, no assemblies, no drawings, no simulation, no plugin loader.
Assembly, Drawing and Presentation documents are declared, listed in the New
panel, and disabled, deliberately, so the shape of the application is visible
and honest.

### 4\. Test coverage has a shaped hole

Five tiers and roughly 65 tests, and **not one looks at a pixel**, which is
exactly how the instancing failure passed a benchmark and got reported as a
success. Also untested: the entire `shell_qt` layer, and `app/`'s commands. The
same class of bug (looks right, counts right, does nothing) can hide there on
the same terms.

- - -
## Interop, verified against this OCCT build

Present: `TKDESTEP` `TKDEIGES` `TKDESTL` `TKDEOBJ` `TKDEPLY` `TKDEVRML` `
TKDECascade` `TKRWMesh` `TKXCAF` (79 headers, including PMI: `XCAFDoc_Dimension`
, `XCAFDoc_GeomTolerance`)


|Format                                |Read|Write|Notes                                                                                                           |
|--------------------------------------|----|-----|----------------------------------------------------------------------------------------------------------------|
|STEP AP203/214/242                    |yes |yes  |full B-rep; the format that matters                                                                             |
|IGES                                  |yes |yes  |                                                                                                                |
|STL / OBJ / PLY / VRML                |yes |yes  |mesh only. OBJ/PLY need provider registration                                                                   |
|glTF                                  |—   |—    |in OCCT, **not compiled here**: needs the port's `rapidjson` feature (an OCCT rebuild)                          |
|DXF / DWG                             |no  |no   |**not in OCCT at all.** DXF needs a third-party library or our own R12 writer; DWG is realistically commercial (ODA)|
|Parasolid / ACIS / JT / `.ipt` / `.sldprt`|no  |no   |proprietary; needs licensed translators. Out of reach. STEP is the answer                                       |

Known gap: import **flattens to one shape**. A real STEP file carries an assembly
tree, per-part names, colours and PMI, and all of it is discarded. Importing
via XCAF instead is the highest-value interop change, and it doubles as the
assembly foundation, because an XCAF occurrence is already the shape of `
render::Placement`.

- - -
## Distance to "professional"

**Target A, a credible parametric modeller, roughly FreeCAD-class.** Sketch →
features → assemblies → drawings, saveable, usable for real parts.


|Work                                                                                       |Rough solo effort|
|-------------------------------------------------------------------------------------------|-----------------|
|2D sketcher + constraint solver, with UI                                                   |6–12 months      |
|Sketch-based features (extrude, revolve, sweep, loft, hole, shell, draft, patterns, mirror)|6–12 months      |
|Renderer correctness, on-screen, picking, culling, LOD                                     |4–6 months       |
|Assemblies + 3D mate solver                                                                |6–12 months      |
|Drawings (HLR, dimensions, GD\&T, title blocks, BOM)                                       |6–12 months      |
|Robustness: autosave/recovery, installers, crash reporting, docs, localisation             |3–6 months       |

**≈ 3–5 person-years. Roughly 15–20% done**, and the completed fraction is the
part most projects get wrong.

**Target B, competitive with Inventor or SolidWorks.** 20–40 years and thousands
of person-years of CAM, sheet metal, weldments, surfacing, FEA/CFD, PDM,
standards conformance, DWG interop. **Under 2%.** The achievable framing is "a
better FreeCAD", which is legitimate precisely because FreeCAD's weaknesses are
architectural.

- - -
## Architecture, and what it costs

Layering, enforced by `tools/check_layering.py` on every build:

```
core   -> OCCT, planegcs, assetlib, Eigen. Nothing else.
render -> core + bgfx
app    -> core + render          (no toolkit: the iPad shell reuses this)
abi    -> core + render
shell_* -> app + its own toolkit
```
This is why three document kinds and multi-document editing were added without
touching `core/` or `render/`, and why `Controller` can be shared by Qt and
SwiftUI. The price is that anything needing a file dialog (Import, Open, Save)
splits into a path-taking method in `app/` plus a dialog in the shell,
deliberate, and the pattern is now consistent across all three.

Plugins: the frozen C ABI is the hard half and it exists (`abi/`, version 1.5,
consumed by the Rust suite as proof). Missing is the host side, discovery,
version negotiation, failure isolation, and a way for a plugin to contribute
ribbon commands and document kinds. A plugin that cannot add a button is not a
plugin. Worth an ADR before it is built.

- - -
## Decisions on record

`0001` Qt linking · `0002` bgfx · `0003` native format (SQLite) · `0004` DDC
recompute · `0005` topological naming · `0006` testing tiers · `0007` renderer
seam **\+ amendment voiding the scale claims** · `0008` Qt shell · `0009`
documents and workspaces

Design: `docs/design/DESKTOP_UX.md` (window anatomy, per-kind ribbon contents,
seven decisions), `docs/design/IPAD_UX.md` (shared-vs-divergent analysis only;
the Pencil design is deferred).

UI policy: **copy Inventor and SolidWorks.** Not a style preference,
professionals have muscle memory for them, and originality in a CAD interface
is a cost. Deliberate divergences so far, both requested: Home's rail is
resizable, and the document tab bar sits inside the content column so the rail
spans full window height.

- - -
## Next

1.  **Sketcher + 2D solver.** The long pole; everything in Create/Modify unlocks
    behind it.
2.  Selection and picking wired to the viewport, the sketcher needs it anyway.
3.  Renderer: find the instancing bug, with a pixel test as the gate.
4.  Import via XCAF, keeping assembly structure and colours.
5.  Assemblies, then drawings.

Steps 1, 2 and 4 are in `core`/`app` and need no GPU, which matters: the
renderer is the one part of the stack that cannot be verified without a human
looking at a screen.

