# iPad UX — divergence analysis

Status: **partial** (Aug 2026). Full design pass deferred; desktop was specified first, in
[DESKTOP_UX.md](DESKTOP_UX.md).

This document exists early on purpose. The iPad shell is SwiftUI over the same `app/` layer, so
every desktop interaction decision is implicitly an iPad decision too. Deferring the *design* is
fine; deferring the question **"is this shared or not?"** is not, because getting it wrong means
rewriting the layer everything sits on.

Scope reminder: iPad is a **stripped-down 3D-printing client**. No native simulation, no plugins,
no JIT. Desktop is the full product.

---

## What is shared and what is not

| Concern | Desktop | iPad | In `app/`? |
|---|---|---|---|
| **Command parameters** | In-canvas mini toolbar, typed fields | Direct manipulation — drag a face, Pencil-draw a profile; numeric entry as fallback | **Shared.** Both need the same in-progress command state and live preview (§3.2). Only the surface differs. |
| **Environment** | Ribbon re-tabs | Full-screen mode switch with its own tool strip | **Shared** — and currently missing. See §3.1. |
| **Selection filter** | Persistent segmented control | Inferred from gesture and zoom level; no permanent control, screen is too precious | **Shared.** Filtered resolution belongs in `IPicker`; *who sets the filter* differs. |
| **Feature tree** | Always-present dock | Pull-out panel — but present, because editing history is the entire reason to use a parametric CAD | **Shared.** `Controller::tree()` already returns data, not widgets. |
| **Properties** | Docked grid | Inspector sheet on selection | **Shared.** Same parameter/measured split; text-in unit parsing matters *more* on a touch keyboard. |
| **Ribbon** | Core to the identity | Does not exist. Wrong for touch at any size. | **Not shared.** The command *registry* is shared; presentation is not. |
| **Simulation, plugins** | Full | Absent by design | **Not shared.** Capability gate, decided per shell. |
| **Document kinds** | Part, Assembly, Drawing, Presentation | Part only, plus read-only assembly viewing | **Shared** enum, per-shell support matrix. |

**Five of eight rows are shared.** That ratio is the argument for `app/` existing, and the reason
these questions had to be asked before more of it was written.

---

## The one thing this analysis changed

`Environment` (DESKTOP_UX §3.1) was going to be a `shell_qt` detail — a ribbon-rebuilding trick.
Checking it against iPad shows it is a **model concept**: on iPad, entering a sketch is a
full-screen mode change with different gestures, and there is no ribbon to rebuild. If the rule
"a sketch environment restricts selection to the sketch plane" lives in `MainWindow`, it does not
exist on iPad, and the same model behaves differently on two clients.

That is exactly the failure mode ADR 0008 created `app/` to prevent.

---

## Deferred to the full pass

Pencil interaction is the substance of the iPad design and none of it is decided yet:

- Pencil vs. finger: what each means, and whether Pencil is ever required
- Hover (Pencil Pro / M2 iPad) for pre-selection highlight — the closest thing touch has to a mouse
  hover, and a real advantage worth designing around
- Double-tap and squeeze gesture assignment
- Two-finger orbit/pan/zoom vs. a ViewCube equivalent
- Sketch input: freehand stroke → inferred constraints, which is the Shapr3D signature and the
  hardest single problem here
- 3D-print-oriented output: wall thickness check, overhang preview, slicer handoff
- Whether the shared `app/` layer ships as a static library into the app bundle, and what that does
  to OCCT binary size on iOS

None of these change `app/`'s shape, which is why they can wait. The table above could not.
