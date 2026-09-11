# 0003 — Native document format is SQLite

Status: accepted (Aug 2026) — shipped 13 Aug 2026, `79881c7`
Consumers: `core/io` (DocumentStore), `app` (Controller save/open), `abi` (`cad_document_save`/`cad_document_open`).

## Decision
A single-file SQLite database, WAL mode, versioned schema.

## Rationale
- We already have the single-writer/multi-reader WAL discipline from assetlib; same model,
  same review instincts.
- Partial reads (open a 5000-part assembly without deserializing all of it), which zip+XML
  (FreeCAD's choice) cannot do.
- Crash safety for free.
- `UIDocument`/`NSFileCoordinator` on iPad want a single file.
- Debuggable with `sqlite3` on a customer's machine.

## Must survive schema evolution
Unknown feature types from missing plugins, and unknown PMI/annotation payloads, are stored
opaquely and round-tripped rather than dropped. A document referencing a missing plugin opens
read-only with the unresolved features visible — it does not fail to open. See docs/FORMATS.md.
