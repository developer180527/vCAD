# 0003 — Native document format is SQLite

Status: proposed — decide before M2 code

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
