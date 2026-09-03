# Where vCAD stands

Last audited: 13 Aug 2026, commit `56e16e4`. Measured from the repository, not estimated.

Re-audit rather than trusting this. Every claim was checked against code on the date above, and the
fastest way to make it lie is to read it six months from now. The previous revision went stale in
about a week.

---

## One paragraph

vCAD is a **working parametric modeller**: draw a constrained sketch, extrude it into a solid,
edit a dimension and watch the solid follow, save it, reopen it. The foundations underneath are
genuinely strong — topological naming, deterministic recompute with a content-addressed cache, an
immutable document, a tested C ABI. Two things stop it being usable by anyone else: the **3D
viewport does not render** (instancing is broken, root cause unfound), and the **feature set is
eleven operations** against the hundreds a real modeller needs.

---

## What works

| Subsystem | State |
|---|---|
| Geometry kernel (OCCT 8.0.1) | Working — guarded, `Result<T>`, no raw OCCT above `core/kernel` |
| Topological naming | Working, property-tested. Survives feature edits |
| Document, undo/redo | Working. Immutable with structural sharing, so undo is free |
| Recompute engine | Working. Dirty propagation, partial failure, content-addressed keys |
| DDC cache | Working. Two-tier local + shared |
| **Rollback marker** | Working. Suspends, persists, invalidates no cache entry |
| **Sketcher** | Working. 11 constraint kinds, DOF and conflict reporting, solves on every edit |
| **Sketch editor** | Working. Draw, snap, select, constrain, delete; glyphs and dimensions drawn |
| **Sketch → Extrude** | Working. A dimension edit drives the solid |
| Native format (`.vpart`) | Working. SQLite, atomic saves, schema v1. Save / Save As / Open wired |
| Foreign formats | STEP, IGES, STL. **DXF in and out**, with constraint inference on import |
| C ABI + Python | Working. ABI 1.8, with a real version tripwire |
| **Logging** | Working. Categories, file sink beside the binary, Qt and OCCT adopted |
| Qt desktop shell | Working. Ribbon, browser with state badges, command property panel, Home, marking menu |
| Test infrastructure | 5 tiers, 67 Rust + 21 Catch2 + 9 pytest, CI on macOS/Linux/Windows |

**Size:** 62 commits, ~18,800 lines of our own code, plus vendored planegcs (13.4k) and assetlib.

**Kernel operations — all of them:** `Box` `Cylinder` `Sketch` `Extrude` `Fillet` `Chamfer` `Cut`
`Fuse` `Common` `Translate` `Import`. 25 commands registered in `Controller`.

---

## What does not work

### 1. The renderer — the largest single gap

Instancing has never worked: eight distinct transforms upload, one box draws. It reproduces on both
the persistent and transient instance paths, and **the root cause is unfound**. Consequences:

- Every scale figure this project ever published is void (ADR 0007 amendment).
- The viewport in the shell is a **Qt-painted placeholder**, not the GPU path.
- On-screen presentation has never been attempted.

The rule that came out of it, which generalises: **a rendering claim is not established by a
counter.** Any scale or correctness claim needs a pixel assertion on more than one part at more
than one transform.

### 2. Not enough operations

Missing as FEATURES a user can reach: sweep, loft, shell, draft, rib. This is the
honest distance to a usable modeller, and it is mostly ordinary work now that sketches and extrude
exist.

Pattern and Mirror have both landed, which is what `rotate` and `mirror` in the kernel were built
for. The distinction this section is careful about still holds: an operation the kernel can perform
and a user cannot reach is not a capability, and `rotate` is still one of those — nothing turns a
body about an axis yet, and a circular pattern will be the thing that needs it.

The list below is checked by `tests/acceptance/docs_claims.cpp` against the command catalogue, so
it cannot quietly go stale the way "no revolve, no hole" did once both had shipped. Edit the marker
and the prose together.

<!-- guarded:missing-features sweep loft shell draft rib -->

### 3. No assemblies, drawings, or simulation

Declared, visible, disabled. Assemblies need a 3D constraint solver; drawings need `HLRBRep`.

### 4. Production infrastructure

Logging landed. Still absent: **crash reporting** (designed — ADR 0010), **autosave/recovery**,
**settings persistence** (preferences reset on restart), installers, signing, i18n, telemetry.

### 5. Test coverage has a shaped hole

**Nothing looks at a pixel**, which is exactly how the instancing failure passed a benchmark and got
reported as a success. `shell_qt` has no automated tests at all.

---

## Distance

| Target | Estimate |
|---|---|
| A credible **1.0** — usable by a stranger, without assemblies | ~2 person-years |
| **FreeCAD-class** (assemblies + drawings) | ~3–5 person-years; **≈20% done** |
| **SolidWorks/Inventor-class** | 20–40 years of vendor work; **under 2%** |

"A better FreeCAD" is the achievable framing, and it is winnable because FreeCAD's weaknesses are
architectural — topological naming, recompute correctness, UI coherence — which is where vCAD is
strongest.

---

## Next, in order

1. **Fix instancing.** The one place the project currently misrepresents itself.
2. **Autosave and recovery.** Days of work; loses hours of a user's work without it.
3. **More features** — pattern and mirror first, both unblocked now that `nameCopy` and the kernel's
   `rotate`/`mirror` exist. Ordinary work, large payoff.
4. **Point selection in sketches** — unlocks the five constraints that act on points, including
   Distance, which is what makes a sketch dimensioned rather than merely constrained.
5. **The plugin loader**, with the module ownership table crash attribution needs (ADR 0010).
6. Assemblies, then drawings.

Items 2–4 need no GPU, which matters: the renderer is the one part of the stack that cannot be
verified without a human at a screen.

---

## Decisions on record

ADRs 0001–0010. `0007` carries the amendment voiding the renderer's scale claims; `0010` covers
logging and crash reporting.

Design: `docs/design/DESKTOP_UX.md`, `IPAD_UX.md`, `UI_RESEARCH.md` (what SolidWorks and Inventor
actually do, with sources), `SHELL_INVENTORY.md`.

**Licence: LGPL-2.1-or-later**, so third parties may ship closed-source plugins. See
`COPYRIGHT.md`, which also records what was rejected and why.
