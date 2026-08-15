# Licensing

vCAD is licensed under the **GNU Lesser General Public License, version 2.1 or later**
(`LGPL-2.1-or-later`). The full text is in [LICENSE](LICENSE).

## Why LGPL, and what it means for you

LGPL is *weak copyleft*. In practice:

- **You may write a plugin for vCAD, keep it closed source, and sell it.** Plugins load the core as
  a shared library through the C ABI in `abi/` — they link dynamically and incorporate none of our
  source, which is what LGPL permits.
- **If you modify vCAD's own code and distribute it, publish those changes.** The obligation is
  scoped to this library, not to whatever links it.

This is the same licence FreeCAD uses, for the same reason: an open core that a commercial ecosystem
can be built on.

Two structural rules follow from the licence rather than from taste, and they must stay true:

1. Plugins load the core as a **shared library**. Never statically embedded.
2. The plugin ABI stays **C, with no inline implementation in the header** — a C header with no
   logic in it carries nothing with it. A C++ template API would make the boundary arguable.

## Third-party components

Copyleft flows one way: the most restrictive licence anything links sets the floor for the whole
product. Everything here is at or below LGPL, which is what keeps the plugin story above possible.

| Component | Licence | How we use it |
|---|---|---|
| Open CASCADE Technology | LGPL-2.1 with exception | Geometry kernel. Dynamically linked. |
| **planegcs** (FreeCAD) | **LGPL-2.1-or-later** | 2D constraint solver, **vendored** — see `modules/planegcs/VENDORED.md`. Built SHARED to satisfy the relink obligation. |
| Qt | LGPL-3 / commercial | Desktop shell only. Dynamically linked; never reaches `core/`. |
| dime (Coin3D) | BSD-3-Clause | DXF read, FALLBACK path only. Permissive. |
| Eigen | MPL-2.0 | Linear algebra. |
| Boost (Graph, Math) | BSL-1.0 | Required by planegcs. |
| SQLite | Public domain | Native document format. |
| bgfx / bx | BSD-2-Clause | Renderer. |
| assetlib | see `modules/assetlib/` | Content-addressed cache, vendored. |
| Catch2 / proptest | BSL-1.0 / MIT-Apache | Tests only; not distributed. |

### Deliberately rejected

- **libdxfrw** (GPL-2.0) and **dxflib** (GPL-2.0) — DXF libraries. Linking either would raise the
  floor to GPL and make proprietary plugins impossible. `dime` does the same job under BSD.
- **LibreDWG** (GPL-3.0-or-later) — DWG. Same problem, and its write support is unreliable past
  R2004. vCAD supports DXF; use ODA's free File Converter for DWG.

*Not legal advice. If plugin sales become significant, have the boundary reviewed properly.*
