# planegcs — vendored

FreeCAD's 2D geometric constraint solver. This is the solver a sketch is built on: it takes points,
lines, arcs and constraints between them and finds the geometry that satisfies them all, or reports
which constraints cannot be satisfied together.

**OCCT does not do this.** OCCT is geometry and topology. Constraint solving is a separate problem —
a nonlinear system driven to zero residual, plus rank analysis to tell an under-constrained sketch
from an over-constrained one — and it is the heart of parametric CAD.

## Provenance

| | |
|---|---|
| Upstream | https://github.com/FreeCAD/FreeCAD |
| Path | `src/Mod/Sketcher/App/planegcs/` |
| Commit | `64bc717c82c7188e9f102e5c121a2b5283320d17` ("Sketcher: Remove unused arguments", 2026-03-17) |
| Files | 11, ~455 KB — `GCS`, `Constraints`, `Geo`, `SubSystem`, `qp_eq` |
| Vendored | 13 Aug 2026 |

## Licence — audited before a line was copied

**LGPL-2.1-or-later.** Verified per file rather than assumed from FreeCAD's project licence:

- All 11 files carry `// SPDX-License-Identifier: LGPL-2.1-or-later`. No file lacks one.
- Every header names the **GNU Library** General Public License. None names the plain GPL — a
  single GPL-only file in this directory would have made the whole thing unusable for us.
- Copyright: Konstantinos Poulios (2011), Victor Titov / DeepSOIC (2014).
- The licence text is in `COPYING.LIB`, alongside the code, as the licence requires when
  distributing it.

### What that obliges us to do

LGPL section 6 lets a work that is not itself LGPL link against the library. If it links
**statically**, the distributor must also provide whatever a user needs to relink the application
against a modified version of the library. Dynamic linking satisfies that by construction.

**So `planegcs` is built SHARED** — the one target in this repository that opts out of
`cad_core_library()`'s always-static rule. That rule exists because vcpkg ships OCCT as a static
archive and duplicating it across shared libraries duplicates its global state; planegcs never
touches OCCT (it is Eigen and Boost.Graph), so shared linkage cannot reintroduce that. Same
reasoning ADR 0001 already applied to Qt.

Modifications must stay visible: this file records every change made to the vendored sources, and
`COPYING.LIB` ships with them.

> **Open item, and not a small one:** vCAD itself has **no LICENSE file**. Its own distribution
> terms are undefined, which is a question that has to be answered before any binary is shipped —
> the LGPL obligations above interact with whatever licence is chosen. Flagged rather than decided,
> because it is not an engineering call.

## Changes to the vendored sources

**None.** All 11 files are byte-for-byte upstream.

That is deliberate and it is what the directory layout is for. Upstream includes
`"../../SketcherGlobal.h"` with a relative path, so the tree mirrors FreeCAD's own
(`Mod/Sketcher/App/planegcs/`) and the include resolves without editing anything. Every future sync
is a file copy rather than a merge.

## Shims — ours, not upstream

planegcs includes four FreeCAD headers. Rather than patch the includes out, we supply the headers.

| Shim | Replaces | Why |
|---|---|---|
| `Mod/Sketcher/SketcherGlobal.h` | FreeCAD's export macro header | Defines `SketcherExport` empty. We control visibility on the CMake target, not per declaration. Sits where upstream's `../../` expects it. |
| `shim/FCConfig.h` | FreeCAD's generated build config | Empty. planegcs includes it and uses nothing from it. |
| `shim/boost_graph_adjacency_list.hpp` | FreeCAD's warning-suppressing wrapper | Forwards to `<boost/graph/adjacency_list.hpp>`. Warnings are suppressed on the target instead. |
| `shim/Base/Console.h` | FreeCAD's logging | Solver diagnostics, **off unless `CAD_PLANEGCS_LOG` is set**. The solver logs per iteration and a sketch drag solves on every mouse move, so leaving it on is slow enough to be mistaken for a slow solver. |

The `Console` shim's `log()` takes a format string and variadic arguments, with a `static_assert`
that every argument is scalar. Upstream passes only integers today; if a sync starts passing a
`std::string` into what is ultimately `fprintf`, that is undefined behaviour, and this makes it a
**compile** error instead.

## Dependencies this pulls in

- **Eigen** — already a dependency.
- **Boost.Graph** (`connected_components`, to split a sketch into independent subsystems) and
  **Boost.Math** (constants). New, added to `vcpkg.json`. Header-only, so vcpkg copies headers
  rather than compiling.

Both are `PUBLIC` on the target: `GCS.h` includes Eigen and `Geo.h` includes Boost.Math, so they
leak through planegcs's own headers. Marking Boost `PRIVATE` compiled the library and then failed
the first consumer.

## Verified working

`spikes/planegcs_standalone` drives the real solver, because a library that merely links proves
nothing about a constraint solver:

```
solve: Success (0)   dofs: 0
A = (0.000000, 0.000000)
B = (100.000000, 0.000000)
length = 100.000000 (want 100)
over-constrained: status 2, dofs -1, conflicting 2, redundant 0
OK
```

Point B started at (30, 40) and the solver moved it to exactly (100, 0) from nothing but
"A is at the origin", "the line is horizontal" and "the line is 100 long". `dofs: 0` means it knows
the sketch is fully constrained. The second half adds a contradictory second length and the solver
reports **2 conflicting constraints** rather than silently choosing one.

One API detail worth carrying into the sketcher: **`diagnose()` is what populates the conflict and
redundancy report** — `solve()` alone leaves it stale. Skip it and the UI will show "solved" for a
sketch the solver knows is contradictory.

## Syncing a newer upstream

1. Copy the 11 files from `src/Mod/Sketcher/App/planegcs/` over `Mod/Sketcher/App/planegcs/`.
2. Re-run the licence audit — do not assume it is unchanged:
   ```bash
   grep -h "SPDX-License-Identifier" Mod/Sketcher/App/planegcs/* | sort | uniq -c
   ```
   Anything other than a single `LGPL-2.1-or-later` line stops the sync.
3. Build. New FreeCAD includes appear as missing headers; add a shim rather than editing sources.
4. Run `spike_planegcs`. It must print `OK`.
5. Update the commit hash and date above.
