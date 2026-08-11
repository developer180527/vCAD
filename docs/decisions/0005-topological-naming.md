# 0005 — Topological naming: derivation chains, fail loud

Status: accepted (Aug 2026)

## Problem
OCCT gives no stable identity for a face or edge across a rebuild. The user picks a face,
fillets it, changes an upstream dimension; the shape rebuilds and every index moves.
Index-based references pass by accident for small changes and break silently for large ones.
Silent rebinding to the *wrong* face is the worst failure mode a CAD system has — it corrupts
a model without telling anyone.

## Decision
An `ElementName` is the chain of operations that produced an element, rooted at a primitive,
derived from `BRepBuilderAPI_MakeShape::Generated/Modified/IsDeleted`, plus geometric fallback
matching where OCCT's reporting is incomplete (notably booleans and offsets — the fallback is
not optional). The element map travels with the shape and participates in its content hash.

- **Split**: children share the parent chain, disambiguated by `NameStep::discriminator`,
  ordered by child midpoint projected onto the parent's parameter range. This is the main
  source of naming failure.
- **Merge**: lexicographically smallest child chain is canonical; others become aliases so
  references to either survive.
- **Delete**: the reference resolves to nothing, the dependent feature fails legibly and the
  recompute continues past it. **It must never rebind to a neighbour.**
- Any unnamed element in an operation result raises `ErrorCode::NamingLost` at the moment it
  happens, not later.

## Why M1 and not later
FreeCAD spent a decade retrofitting this and shipped a *mitigation* in 1.0 entangled with
`Part::TopoShape`. It is also the prerequisite for semantic PMI round-tripping (AP242
persistent IDs), for DDC keys, and for plugin references that survive rebuilds. Everything
depends on it; nothing can be built cleanly on top of a missing version of it.
