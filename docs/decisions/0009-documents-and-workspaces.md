# 0009 — Document kinds, workspaces, and the home page

Status: proposed (Aug 2026)

## The question

*"How will we have the home page in the same app for project management and editor opening?
Home menu for opening editors for assembly, drawing and etc? CAD is incredibly complex, we have
to think about it."*

Correct, and it is the right time to think about it — this decision is very expensive to
retrofit. Everything so far has assumed **one document, of one kind, forever**. Inventor is four
document kinds, N open at once, a home page that is not a document, and a ribbon that changes
shape depending on what you are editing.

## What Inventor actually is, structurally

Worth being precise, because "looks like Inventor" is mostly this:

1. **Four document kinds**, each a genuinely different editor:
   - **Part** (`.ipt`) — features on one solid body. What we have.
   - **Assembly** (`.iam`) — *references* to parts, with placement and joints. Does not own
     geometry; owns transforms and constraints.
   - **Drawing** (`.idw`) — 2D sheets with projected views of parts/assemblies, dimensions, a
     title block. A completely different canvas: paper space, not model space.
   - **Presentation** (`.ipn`) — exploded views and animation.
2. **A Home tab that is not a document.** Recent files, a New panel, the active project. It is
   always present and cannot be closed.
3. **Document tabs along the bottom** (`Home | Part3 ×`), so several documents are open at once
   and switching is instant.
4. **A ribbon whose tabs are a function of the active document kind** — a Drawing has
   `Place Views` / `Annotate` / `Sketch`; a Part has `3D Model` / `Sketch` / `Inspect`. Not a
   mode you select; a consequence of what you are editing.
5. **Environments within a kind.** Entering a sketch swaps in a Sketch ribbon and *changes what
   selection means*. Inventor calls these environments; they are the honest version of
   FreeCAD's workbenches, because the app enters them for you.
6. **A project** (`.ipj`) — search paths that resolve assembly references. Not cosmetic: it is
   what makes "this assembly needs `bracket.ipt`" resolvable on another machine.

## Decisions

### 1. `Session` owns N documents; `Controller` becomes one-per-document

Currently `Controller` *is* the application. It becomes one open document, and a new `Session`
owns the collection plus which one is active. `Session` lives in `app/`, not the shell, because
the iPad needs the same structure — it will show one document at a time, but "one at a time" is
a presentation choice about the same model.

### 2. `DocumentKind` is explicit, and its editor is a separate widget

```
enum class DocumentKind { Part, Assembly, Drawing, Presentation };
```

Not a flag on one god-editor. A Drawing editor draws paper, sheets and dimension leaders; it
shares almost nothing with a 3D viewport but the selection model. Trying to make one widget do
both is how you end up unable to do either well.

**Only `Part` is implemented now.** The others are declared, appear in the New panel, and open a
clearly-labelled "not implemented" editor. That is deliberate: the *shape* of the app should be
visible and honest from the start rather than appearing later and moving everything.

### 3. The Home page is a workspace, not a document

It cannot be closed, has no file, and takes no ribbon tabs of its own. Modelling it as a
document would mean every "for each open document" loop needs a special case forever.

### 4. Ribbon tabs are derived from the active workspace, not registered globally

```
tabsFor(DocumentKind, Environment) -> [RibbonTab]
```

Rebuilt on document switch. This is the anti-workbench decision from [ADR 0008](0008-qt-shell.md)
made concrete: the user never selects a command set, they select a *thing to edit*, and the
commands follow.

### 5. Assemblies reference documents; they do not contain geometry

The most important structural decision here, and the one that would be worst to retrofit.

An assembly's occurrence is `(document reference, transform, visibility)`. Our `render::Placement`
is *already* exactly this shape — which is not luck, it is why the scene layer took placements
rather than walking the document itself. So an assembly editor mostly produces placements, and
the scene layer needs nothing new.

Two consequences to accept now:
- A document reference must survive the referenced file moving. Hence a **project** with search
  paths, not raw absolute paths.
- The recompute DAG spans documents. A part changing must invalidate assemblies that use it. The
  content-addressed cache already handles the *caching* correctly; what is missing is a
  cross-document dependency edge, and it belongs in `Session`.

### 6. A project is a file listing search paths and the shared cache

Minimal and immediately useful: search paths so references resolve, plus the **DDC shared tier
path**. That last part is a real payoff of [ADR 0004](0004-ddc-recompute.md) — "open this project"
can mean "and use the team's cache", so a colleague's tessellation and feature results are
already there.

## What this changes in code

| | now | after |
|---|---|---|
| `app/Controller` | the application | one open document |
| `app/Session` | — | owns documents, active index, project |
| `app/DocumentKind` | — | Part / Assembly / Drawing / Presentation |
| `shell_qt` central widget | the viewport | `QStackedWidget` of workspaces |
| ribbon tabs | fixed at startup | rebuilt per active workspace |
| Home | — | workspace 0, uncloseable |

`render/` and `core/` are untouched. That is a good sign about the seam: adding three document
kinds and multi-document editing does not reach below `app/`.

## Deliberately deferred

Joints and assembly constraints (needs the 3D solver — M5's planegcs work is 2D), drawing view
projection (needs HLR — OCCT has `HLRBRep`), presentation animation, and full `.ipj`
compatibility. Declared here so the structure accommodates them; none are built.
