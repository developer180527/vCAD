# A PDF editor on vCAD's primitives

Concept note, 15 Aug 2026. **Nothing here is committed to and nothing has been built.** This
records an idea, what would carry over from vCAD, what would not, and — more usefully — the three
questions whose answers decide whether it is worth starting at all.

Written now because the reuse claim is testable today: `modules/proshell` is probe-tested,
`core/recompute` was cut free of geometry this afternoon, and the "parse hostile bytes in Rust
behind a narrow seam" pattern has been through one full cycle. If those primitives are as general
as they look, a second product should be cheap to *sketch*. If sketching one is expensive, the
claim was wrong and that is worth learning here rather than in a year.

---

## Why PDF, and not Revit or an EDA suite

**The incumbents in the free tier are weakest here.** Free CAD has FreeCAD; free EDA has KiCad, and
KiCad is genuinely good. Free PDF has excellent *viewers* — Okular, Evince, pdf.js — and no good
desktop **editor**. LibreOffice Draw mangles documents. The web tools are per-file uploads. The
CLI tools (qpdf, pdftk) are surgical, not editorial. Acrobat Pro is the standard and is a
subscription.

That is a real gap rather than a crowded field, and it is the reason to prefer this target over
the others. It is also the target with by far the largest domain cost — see the honest accounting
below.

---

## The idea: an edit history that is a graph, not a stack

Acrobat edits **destructively behind an undo stack**. An undo stack is a list: you can walk back
along it and you can throw away its tail, and that is all. Redact something on page 5, then run
OCR, then rotate a section, then flatten a form — and now change the redaction. Acrobat's answer
is to undo everything after it and redo it by hand.

vCAD's recompute engine already models exactly the other thing. An edit is a **feature**: a typed
node with parameters, inputs, and a deterministic compute. The engine tracks dependencies, marks
what a change dirties, recomputes only that, caches results by content, and supports a rollback
marker that suspends everything after a point.

Applied to a document rather than a solid:

- **Every edit is a node.** Redact, OCR, insert pages, rotate, flatten, watermark, compress.
- **Edits are re-parameterizable after the fact.** Change the redaction rectangle applied forty
  edits ago; everything downstream recomputes, and only what depended on it.
- **Edits are reorderable**, subject to their real dependencies rather than to the order they were
  performed in.
- **Rollback is a marker, not a destruction.** Drag it up the list to see the document as it was,
  drag it back down to restore — the sketcher's rollback bar, applied to a PDF.
- **The original file is never modified.** The document *is* the source plus the edit graph, and
  exporting is a recompute.

Nobody ships this for PDF. It is not a UI idea — it is a data model, and it is one we already have
running, tested, and cached.

---

## What carries over from vCAD

Measured against the tree as it stands, not estimated.

| Component | Reuse | Notes |
|---|---|---|
| `modules/proshell` | **Direct** | Acrobat Pro *is* a ribbon, a thumbnail rail, docks, document tabs, a status bar and a home screen. `ShellWindow` + `HomeModel` with a different vocabulary. Already proven domain-neutral by `proshell_probe`. |
| `core/recompute` | **Direct** | The engine described above. Cut free of `cad::sketch` today; it now reaches the kernel only for `Result`. |
| `core/document` | **Nearly** | Nodes, properties, `ObjectState`, history. See the `Output` problem below — this is the one thing PDF forces. |
| `modules/assetlib` (DDC) | **Direct** | Page rasters keyed by (content hash, zoom, device), font subsets, OCR results. Re-rendering a 900-page document *is* the performance problem. Cross-machine sharing is a team feature nobody else has. |
| `abi/` plugin ABI | **Direct** | `struct_size` negotiation, the golden snapshot, additive-only, the determinism contract, the re-entrancy guard. Acrobat's SDK is a moat; a stable C ABI is how you contest it. |
| The Rust-parser pattern | **Direct, and the most valuable** | PDF is the most-attacked document format there is. See below. |
| `core/base`, `core/log`, `core/units` | Direct | Errors, logging, page geometry in points/mm/inches. |
| CI matrix, triplets, testing discipline | Direct | Five platforms, differential fuzzing, determinism checking, "the tests that matter are registered". |

### What does not carry over

- **`core/kernel` (OCCT)** — a PDF has no solids. Zero use.
- **`core/sketch` (planegcs)** — constraint-solved layout for annotations is a stretch. Assume no.
- **`render/` (bgfx)** — bgfx is a GPU abstraction, not a 2D rasterizer. PDF needs nonzero and
  even-odd fills, stroke joins, caps and dashes, clipping, transparency groups, soft masks, blend
  modes and shading types 1–7. The *viewport container* concept transfers; the renderer does not.
- **`core/naming`** — not the code. The *problem* transfers exactly, and is discussed as risk 2.

### The `Output` problem, and why PDF settles an open question

`document::Output` holds a `kernel::Shape` and a `naming::ElementMap` by value, which is why
`core/document` still links the B-rep kernel — the third of the three edges, left uncut this
afternoon with the note that it is *only* wrong for an application with no geometry at all.

**A PDF editor is precisely that application.** So this concept is the answer to that open
question: if this is ever started, edge three gets cut, and `Output` becomes a domain-defined
payload. Roughly 21 sites, and the same seam trick used three times already (`RawEntity`,
`HomeModel`, `RawDocument`).

---

## What would have to be borrowed, and the licence traps

The domain cost is the real cost. None of the following is worth writing ourselves at the start.

| Need | Library | Licence | Verdict |
|---|---|---|---|
| Rendering, object model | **pdfium** | BSD-3 | The pragmatic base. |
| Structural transforms, encryption, linearization | **QPDF** | Apache-2 | Excellent for the non-rendering half. |
| Text shaping | HarfBuzz | MIT | |
| Font rasterization | FreeType | FTL or GPL-2 | Use under FTL. |
| OCR | Tesseract | Apache-2 | |
| Colour management | Little-CMS | MIT | |
| JPEG 2000 | OpenJPEG | BSD-2 | |
| — | **MuPDF** | **AGPL or commercial** | **TRAP.** Small, excellent, and would decide the product's licence. |
| — | **Ghostscript** | **AGPL or commercial** | **TRAP**, same reason. |

The permissive stack is complete. The two tempting libraries are the two that are not permissive,
which is exactly the shape of the libdxfrw decision recorded in COPYRIGHT.md.

### The philosophical compromise, stated rather than glossed

vCAD's rule is that **untrusted bytes are parsed in a memory-safe layer**. pdfium is C++. Using it
is a compromise, not an expression of that rule, and it should be described that way.

It is the right compromise, and the path off it is one already walked: **dime → Rust DXF is the
small rehearsal for pdfium → Rust PDF.** Same narrow seam, same neutral type between reader and
domain, same differential harness proving the new parser agrees with the old one on bytes neither
author anticipated. That cycle took a day for DXF and found two real bugs. PDF is very much larger,
but the technique is not in question.

---

## Potential features

The differentiators, in rough order of how much they depend on the architecture rather than on
grinding out domain code:

1. **Non-destructive edit graph** — reorder, re-parameterize, rollback marker. The product.
2. **Provable redaction** — content genuinely removed rather than covered, verifiable because the
   export is a recompute from the source plus the graph, not an in-place edit of bytes.
3. **Deterministic export** — byte-identical output for identical input. The contract already
   requires this of features. It buys document diffing, reproducible signing, and a real answer to
   "is this the same file".
4. **Content-addressed page cache** — instant reopen, shared across a team.
5. **Compare documents** — much stronger when both sides are deterministic.
6. **A stable plugin ABI** for batch and bespoke workflows.
7. Then the table stakes, none of which are interesting but all of which are required: OCR, forms
   (AcroForm), digital signatures with LTV, page operations, annotations and review, text editing,
   PDF/A conversion and preflight, accessibility tagging.

Text editing deserves a warning of its own: it needs **font synthesis** when an embedded subset
lacks a glyph you just typed. This is where every free PDF editor fails and why Acrobat's text
editing works.

---

## The three risks that decide it

**1. Determinism.** The whole model rests on same inputs → same output; that is what makes the
cache safe and rollback meaningful. PDF operations leak entropy casually: metadata timestamps,
font subsetting whose glyph order follows hash iteration, incremental-update object numbering,
compression levels. A non-deterministic feature does not fail loudly — it silently serves a stale
cache entry, the worst failure shape available. **`CAD_PLUGIN_DETERMINISM_CHECK` already exists and
should be pointed at a pdfium operation on day one.** If PDF operations cannot be made
deterministic, the architecture does not transfer, and that is a week-one finding.

**2. Stable identity across edits — the naming problem in a different hat.** "Redact the paragraph
at (x, y) on page 5", then insert a page above it or reflow the text. *Which paragraph is that
now?* This is topological naming for documents. It took the CAD industry thirty years and FreeCAD
still struggles.

The encouraging part: PDF may be genuinely easier. A text run carries font, size, position, and in
a tagged PDF a place in a logical structure tree — far more to anchor an identity to than a B-rep
face has. Possibly tractable where our version was brutal. Worth its own spike.

**3. Fidelity.** If a document renders differently from Acrobat, the tool is dead for professional
use regardless of its architecture. This is the argument for pdfium rather than anything of our
own, for a long time.

---

## What to build first

**A spike that answers risks 1 and 2 and contains no PDF parsing of our own.**

Open a document through pdfium. Apply a stack of transformations as recompute features. Cache page
rasters in the DDC. Drive it from `proshell`. Then:

- turn on the determinism check and see whether pdfium operations survive it;
- perform an edit, insert a page above it, and see what it takes to keep the edit anchored.

Weeks, not years, and almost entirely made of parts that are already built and green. It answers
the only two questions that matter, plus the one that actually decides the product: **is
time-travel editing better to use, or only better on paper?**

If the answer is yes, the parser, the renderer and the domain grind become worth their years. If
it is no, the cost was weeks.

---

## Open questions

1. Does a pdfium-based operation pass the determinism check, and if not, can it be made to?
2. What is the stable identity of a text run, an image, an annotation, across edits?
3. Is `Output` as a domain-defined payload (edge three) the right shape, or does a document want
   something else entirely?
4. Where does the plugin ABI's geometry half go for a product with no geometry — unimplemented and
   returning `CAD_ERR_UNSUPPORTED`, or a genuine split of the vtable?
5. Is this a separate product on shared primitives, or a second front that starves vCAD? That is a
   decision about attention, not architecture, and it should be made deliberately.
