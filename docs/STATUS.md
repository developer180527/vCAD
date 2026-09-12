# Where vCAD stands

Last audited: 12 Sep 2026, commit `af45ece`. Measured from the repository, not estimated.

Re-audit rather than trusting this. Every claim was checked against code on the date above, and the
fastest way to make it lie is to read it six months from now. The previous revision went stale in
about a week.

---

## One paragraph

vCAD is a **working parametric modeller**: draw a constrained sketch, extrude it into a solid,
edit a dimension and watch the solid follow, see it shaded on the GPU, save it, reopen it. The
foundations underneath are genuinely strong — topological naming, deterministic recompute with a
content-addressed cache, an immutable document, a tested C ABI. What stops it being usable by
anyone else is no longer the renderer, which works: it is the **feature set — sixteen
operations** against the hundreds a real modeller needs, and the absence of the production
infrastructure in §4 below, autosave first among it.

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
| C ABI + Python | Working. ABI 1.23, with a version tripwire that fired — it sat three minors behind |
| **Logging** | Working. Categories, file sink beside the binary, Qt and OCCT adopted |
| Qt desktop shell | Working. Ribbon, browser with state badges, command property panel, Home, marking menu |
| Renderer | Working. bgfx; Metal verified on macOS by `vcad_probe`, presenting directly |
| Test infrastructure | 5 tiers, all under one `ctest` run — the Rust suite among them since 12 Sep — CI on macOS/Linux/Windows |

**Size:** 202 commits, ~62,600 lines of our own code (excluding vendored planegcs and assetlib).
Counted with `git rev-list --count HEAD` and a `wc -l` over first-party sources — recount rather
than trusting either number, which is why the command is written here instead of the method. For the
test count, ask `ctest -N`: a number written here went stale three times in one afternoon.

**Kernel operations — all of them:** `Box` `Chamfer` `Common` `Cut` `Cylinder` `Extrude` `Fillet`
`Fuse` `Hole` `Import` `Mirror` `Pattern` `Plane` `Revolve` `Sketch` `Translate`.

This list is checked against `features::builtins()` in both directions by
`tests/acceptance/docs_claims.cpp` — adding an operation fails the suite until the marker below is
updated, and so does deleting a name to quieten it. It had rotted to eleven entries while the
registry held sixteen.

<!-- guarded:kernel-operations Box Chamfer Common Cut Cylinder Extrude Fillet Fuse Hole Import Mirror Pattern Plane Revolve Sketch Translate -->

---

## What does not work

### 1. The renderer works; what is missing is a pixel

This section used to say instancing had never worked — eight transforms uploading and one box
drawing, root cause unfound — and that the shell's viewport was a Qt-painted placeholder. Both
were true when written. Neither is now, and **nothing failed when they stopped being true**, which
is why the claim is now an assertion: `docs_claims.cpp` builds two bodies and requires two DRAWN
draw ranges in the frame, and `vcad_probe` reports the live path (`renderer Metal, presenting
directly`).

What is genuinely still absent:

- **No test looks at a pixel.** Everything above counts draw calls, instances and ranges, which is
  a proxy. The rule that came out of the original failure still stands unmet: **a rendering claim
  is not established by a counter.**
- **Content addressing is currently lost to naming, and with it the scale claim.** One mesh shared
  by many instances is implemented and unit tested, but nothing in the application shares a mesh,
  and it is not merely that assemblies do not exist yet. `Engine::cacheKeyOf` mixes in the object's
  id — correctly, because the id is the naming serial and is stamped into every element name — so
  two identical features are not identical to the cache, and N identical parts tessellate N times.
  Three tests in the Rust suite assert the lost property and are `#[ignore]`d against the ADR 0004
  amendment, which records the fix: cache a RELATIVE element map keyed by content without the
  serial, and rebase the names on retrieval. Until then every claim about 50,000-part scenes is
  false rather than merely untested.
- Scale figures published before ADR 0007's amendment remain void; nothing has re-measured them.

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
reported as a success.

`shell_qt` is no longer untested — `vcad_probe` drives the real widgets with synthetic events and
asks the Controller what happened — but it is registered with `QT_QPA_PLATFORM=offscreen`, so every
check in it that needs a native window (the window-button band, and anything about the GPU path)
reports SKIP under `ctest` and only ever runs when someone runs the binary by hand.

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

1. **Autosave and recovery.** Days of work; loses hours of a user's work without it, and it is the
   largest single gap between this and something a stranger can be handed.
2. **Rebasable cached names**, which buys back content addressing and the scale claim — see the
   ADR 0004 amendment. Three ignored Rust tests come back on when it lands.
3. **A pixel assertion.** One test that renders a known scene and reads the buffer back would close
   the hole every other renderer claim is measured through a proxy to avoid.
4. **More features** — sweep, loft, shell, draft, rib. Ordinary work now that sketches, extrude,
   pattern and mirror exist.
5. **Point selection in sketches** — unlocks the five constraints that act on points, including
   Distance, which is what makes a sketch dimensioned rather than merely constrained.
6. **The plugin loader**, with the module ownership table crash attribution needs (ADR 0010).
7. Assemblies, then drawings — and assemblies are what finally exercises mesh dedupe.

Items 1, 2, 4 and 5 need no GPU, which matters: the renderer is the one part of the stack whose
remaining gap cannot be closed without either a screen or that pixel test.

---

## Decisions on record

ADRs 0001–0010. `0007` carries the amendment voiding the renderer's scale claims; `0010` covers
logging and crash reporting.

Design: `docs/design/DESKTOP_UX.md`, `IPAD_UX.md`, `UI_RESEARCH.md` (what SolidWorks and Inventor
actually do, with sources), `SHELL_INVENTORY.md`.

**Licence: LGPL-2.1-or-later**, so third parties may ship closed-source plugins. See
`COPYRIGHT.md`, which also records what was rejected and why.
