# What SolidWorks and Inventor actually do

Researched 13 Aug 2026 against vendor documentation and reference material. Sources at the end.

Written because "copy Inventor and SolidWorks" is our stated UI policy, and a policy of copying is
worthless if we copy from memory. Every claim here is sourced. Where the two disagree, that is
recorded rather than averaged — an average of two coherent designs is usually neither.

**This document corrects a decision in [DESKTOP_UX.md](DESKTOP_UX.md) §3.2.** See "The finding
that changes our design" below.

---

## SolidWorks, region by region

| Region | Where | What it does |
|---|---|---|
| **CommandManager** | Top | "A context-sensitive toolbar that dynamically updates based on the toolbar you want to access." Toolbars are embedded **based on the document type**, and clicking a tab below it swaps the visible set. Explicitly justified as saving graphics-area space. |
| **FeatureManager design tree** | Left | "Displays all the design information about the part, assembly, or drawing file being viewed." Most user operations are stored here. |
| **PropertyManager** | **Left — replaces the tree** | "Most sketch, feature, and drawing tools open a PropertyManager in the left panel. The PropertyManager displays the properties of the entity or feature **so you specify the properties without a dialog box covering the graphics area**." |
| **Heads-up View Toolbar** | **Inside the graphics area**, below the CommandManager | View orientation and appearance — how the model looks, not what it is. |
| **Context toolbars** | At the selection | Appear on selection with feature-relevant commands (Edit Sketch and similar). |
| **Task Pane** | Right | Libraries, appearances, resources. |

Two structural points worth stating plainly:

1. **The CommandManager is not our ribbon.** It is a ribbon *whose tab set is a function of document
   type* — which is exactly the derived-tabs decision in ADR 0009. We arrived at the same design
   independently; good.
2. **The PropertyManager occupies the tree's space.** Running a command does not open a window over
   the model; it *replaces the browser* on the left for the duration. The model stays fully visible
   and fully interactive.

## Inventor, region by region

| Region | What the documentation says |
|---|---|
| **Ribbon** | "Task-oriented arrangement of commands." |
| **Property Panels** | "Provide contextual access to parameters used for creating and editing features." |
| **Mini-toolbars** | Only **7 dialogs** have one, displayed *alongside the dialog*. |
| **Marking menus** | "Context-sensitive **radial** menus", customisable per command. |
| **Navigation bar** | Positionable; can be **linked to the ViewCube** so both move together. |
| **ViewCube** | In all 3D views, placeable in **any of the four corners**. |
| **Browser** | The model tree. |
| **InfoCenter** | Search, upper right. |

## The finding that changes our design

DESKTOP_UX §3.2 committed to an **in-canvas mini toolbar** as the surface for a command in progress,
described as "the concrete version of what ADR 0008 promised". That was my inference, and the
research does not support it as the primary pattern.

**Both applications use a persistent docked panel, not a floating surface:**

- SolidWorks: the **PropertyManager**, in the left panel, explicitly *"without a dialog box covering
  the graphics area"*.
- Inventor: **Property Panels**, *"contextual access to parameters used for creating and editing
  features"*. Its mini-toolbar is a **secondary** device — seven dialogs have one.

So the shared, load-bearing idea is **non-modal input in a fixed location**, and I had latched onto
the floating variant, which is the minority pattern in one of the two products.

### Revised decision

**Command input goes in a docked panel that takes over the model browser's space while a command is
running**, returning the browser when it finishes. Concretely:

- Left dock switches from `Model` to `Properties of <command>` on invoke.
- The viewport is never covered, and stays interactive — selecting geometry feeds the running
  command, which is the entire reason both vendors do it this way.
- Cancel/OK live at the top of the panel, where SolidWorks puts them.

This is *better* for us than the floating toolbar, for a reason specific to our situation: a docked
panel is ordinary widget layout, whereas a floating surface over a **native GPU child window**
cannot be composited by Qt at all — the same problem already flagged for the ViewCube in §3.6. The
research and our own constraints point the same way.

The mini-toolbar is not dropped, just demoted: it belongs later, for a small number of commands with
one or two parameters, exactly as Inventor uses it.

## What neither of us has, and both of them do

- **Marking menus** (Inventor) — right-click **radial** menus, customisable. Radial rather than
  linear because direction is muscle memory: an expert flicks without reading. This is a genuine
  expert-speed feature and we have nothing like it.
- **Context toolbars on selection** (SolidWorks) — commands appear *at the selection*, so the
  common case never travels to the ribbon.
- **Heads-up view toolbar** (SolidWorks) — view controls *inside* the graphics area rather than in
  chrome. We put ViewCube and nav bar there (§3.6) but nothing else.
- **Task Pane** (SolidWorks) — libraries and content, right side. Relevant once standard parts exist.

## Where they disagree, and what we take

| Question | SolidWorks | Inventor | vCAD |
|---|---|---|---|
| Command input | PropertyManager, left, replaces tree | Property panels | **Follow both: docked panel** |
| View controls | Heads-up toolbar in the graphics area | ViewCube + navigation bar, corner, linked | **Inventor's** — already built, and the ViewCube is the stronger idiom |
| Fast command access | Mouse gestures, context toolbars | Marking menus | **Inventor's marking menu**, later. Radial is the better-documented pattern |
| Tab set | Context-sensitive by document type | Ribbon tabs by document kind | **Both agree** — already ADR 0009 |

## What this means for the work queue

1. **Selection on the sketch canvas** stays the top item — context toolbars, marking menus and the
   property panel are all selection-driven, so none of them can be built before it.
2. The **command property panel** replaces the mini-toolbar in §3.2, and should be built with the
   non-modal command state it always needed.
3. **Marking menus** are a later, high-leverage addition — cheap in Qt (a custom `QWidget` popup),
   and disproportionately affects how fast the app feels to someone who uses it daily.

---

## Sources

- [SOLIDWORKS 2026 Help — CommandManager](https://help.solidworks.com/2026/English/SolidWorks/sldworks/c_commandmanager.htm)
- [SOLIDWORKS 2024 Help — Manager Pane](https://help.solidworks.com/2024/English/SolidWorks/sldworks/c_management_panel.htm)
- [SOLIDWORKS 2013 Help — Context Toolbars](https://help.solidworks.com/2013/English/SolidWorks/sldworks/c_context_toolbars.htm)
- [Major User Interface Components — Introduction to SolidWorks](https://openwa.pressbooks.pub/testmhrtc/chapter/major-user-interface-components/)
- [Anatomy of the SOLIDWORKS User Interface — TriMech](https://store.trimech.com/blog/anatomy-of-the-solidworks-ui)
- [User Interface Basics in SOLIDWORKS — Hawk Ridge Systems](https://hawkridgesys.com/blog/user-interface-basics-in-solidworks)
- [Inventor 2026 Help — User Interface](https://help.autodesk.com/view/INVNTOR/2026/ENU/?guid=GUID-8E3158B6-56F5-4D74-9B6A-5D5B6049116A)
- [Inventor 2021 Help — User Interface](https://help.autodesk.com/cloudhelp/2021/ENU/Inventor-Help/files/GUID-8E3158B6-56F5-4D74-9B6A-5D5B6049116A.htm)
- [Inventor 2023 Help — ViewCube Options](https://help.autodesk.com/view/INVNTOR/2023/ENU/?guid=GUID-F91783E5-41BC-46BC-B0CF-9D07FE75C574)
- [Inventor 2024 Help — Navigation Bar](https://help.autodesk.com/view/INVNTOR/2024/ENU/?guid=GUID-FAAB453E-31EB-46B8-9A74-3BF7A4D88EC9)

**Sourcing caveat:** the SOLIDWORKS help pages and two reference articles return HTTP 403 to
automated fetches, so their content here comes from indexed summaries with quoted passages rather
than a direct read. The quotations are verbatim from those summaries. Anything below the level of
detail recorded here should be re-checked against the live pages by hand before it is built on.
