# Industry-standard file format support

Native format is ours (`docs/decisions/0003-native-format.md`). Everything below is
interchange. Verified Aug 2026.

## Tiers

### Tier 1 — must work at v1, free via OCCT

| Format | Direction | OCCT toolkit | Notes |
|---|---|---|---|
| **STEP** AP203 / AP214 / **AP242** | R/W | `TKDESTEP` | The contractual neutral format. Aerospace (Airbus, Safran), automotive (Stellantis, Renault, Valeo), defense and energy mandate it. 8.0 reads it up to 75% faster than 7.7. |
| **IGES** up to 5.3 | R/W | `TKDEIGES` | Legacy, still required for older supply chains. Superseded by AP242 for anything needing semantic PMI. |
| **STL** | R/W | `TKDESTL` | 3D printing baseline. |
| **glTF 2.0** | R/W | `TKDEGLTF` | Visualization/web handoff. |
| **OBJ** | R/W | `TKDEOBJ` | Mesh interchange. |
| **VRML** | W | `TKDEVRML` | Legacy viz. |
| **PLY** | R/W | `TKDEPLY` | Scan data. |
| **3MF** | R/W | *external: lib3mf* | **Required for the iPad/3D-printing product.** Not in OCCT — use `lib3mf` (BSD-2, 3MF Consortium). Core spec + Production, Beam Lattice, Materials extensions. Strictly better than STL: units, colors, materials, assemblies, print tickets. |

### Tier 2 — commercial OCCT add-ons, buy when a customer pays for it

OCCT ships these only as paid **Advanced Data Exchange Components**, built on the same
architecture as the STEP/IGES interfaces:

- **Parasolid** (X_T/X_B) — SolidWorks/NX/Solid Edge native
- **ACIS SAT** — Inventor/AutoCAD/Fusion lineage
- **JT** — Siemens DMU / lightweight visualization; the automotive viz standard
- **DXF** — 2D drawing interchange
- **IFC** — BIM

Do not design around their absence. The importer extension point (below) must make adding
them a drop-in, and we should be able to enable them with a license key and a CMake flag.

**Free alternatives worth evaluating rather than buying immediately:**
- **DXF/DWG**: GNU **LibreDWG** — but it is **GPLv3**, so it cannot link into a proprietary
  binary. If we ship proprietary, LibreDWG must be an out-of-process converter, exactly like
  CalculiX. (This is what FreeCAD does as an alternative to the proprietary ODA converter.)
  The commercial option is **ODA Drawings SDK** — membership required, not publicly available.
- **IFC**: IfcOpenShell (LGPL) sits on OCCT already.
- **JT**: no credible free implementation. Buy it or skip it.

### Tier 3 — MBD / metrology, post-v1

- **QIF 3.0** — the XML standard for CAD↔metrology interoperability. Along with
  **STEP AP242 ed3**, one of only two formats that preserve *semantic* (machine-readable)
  PMI/GD&T. Relevant only once we do PMI at all.

## PMI is a real architectural constraint

If we ever want semantic PMI/GD&T — and any serious manufacturing customer will ask — it is
not an import filter bolted on later. It needs:

- persistent element IDs that survive round-trip (our element map already gives us this,
  and it is why the naming layer is M1 and not M5),
- an annotation model in the document that is *attached to topology*, not to geometry,
- AP242 saved views and assembly-level PMI in the document tree.

AP242 coverage varies by vendor and by edition (Ed.1–Ed.4) and by capability — semantic
representation, tessellated presentation, saved views, assembly PMI, persistent IDs. We do
not have to support all of it at v1, but the **document model must not make it impossible.**
Concretely: `DocObject` needs an annotation attachment slot referencing `ElementId`, and the
native format must round-trip unknown annotation payloads rather than dropping them.

## Architecture

One extension point, `cad::io::IFormatProvider`, registered in a `FormatRegistry`.
Deliberately mirrors OCCT's own `DE_Wrapper` plugin system (7.8+) so that OCCT-native
formats are a thin delegation and third-party/plugin formats look identical to the app:

```
FormatRegistry
  ├── OcctProvider     (STEP, IGES, STL, glTF, OBJ, VRML, PLY)  -> DE_Wrapper
  ├── Lib3mfProvider   (3MF)
  ├── SubprocessProvider (DWG via LibreDWG, license-isolated)
  └── <plugin-supplied providers, same interface>
```

Every provider reports capabilities (read/write, solids/mesh/assembly/PMI/colors/units) so
the UI can explain *before* a conversion what will be lost.

## Non-negotiable rules

1. **Import never silently discards.** Unsupported entities produce a structured import
   report the user can inspect. Round-trip loss is a UI-visible fact, not a surprise.
2. **Units are explicit at the boundary.** Every provider declares source units; the core is
   unit-typed. STL and DXF carry no reliable units — prompt, never guess.
3. **Import is a Feature.** An imported STEP is a document node with a source hash and a DDC
   key like any other, so re-import is cached and downstream features survive it.
4. **Shape healing runs on ingest** (`ShapeFix`/`ShapeUpgrade`), with the result recorded in
   the import report. Foreign B-rep is routinely invalid.
5. **Element IDs are assigned at import** and persisted, so a re-imported revision of the same
   supplier part keeps downstream references alive where geometry matches.
