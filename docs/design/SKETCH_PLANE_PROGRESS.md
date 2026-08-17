# Step 1 in progress: the sketch plane becomes a reference

Live working notes. **Delete this file when step 1 is finished** — it exists so a context flush
mid-task does not lose the design decisions, not as a permanent document.

Design and traps: `PICKUP.md`, "Start here: step 1". Read that first.

---

## Status

- [x] **1a. `SketchPlane` type + serialisation, both forms, round-trip tested.** DONE.
      `tests/acceptance/sketch_plane.cpp`, 5 tests. 56 C++ tests green, Rust green.
- [x] **1b. Resolve `Kind::Face` at recompute time.** DONE.
      - `isPlaced()`; `toWire` refuses a face sketch with no frame rather than falling back.
      - `SketchFrame` on the sketch; `to3d` uses it, circles/arcs sweep about its normal.
      - `kernel::planeOf(face)` measures a planar face, refuses a curved one, stable across calls.
      - `computeSketch` resolves the face through the input's element map and sets the frame.
      - The face's owning feature is a real INPUT (an ObjectId property), so `cacheKeyOf` folds its
        cache key in — moving the face invalidates the sketch instead of leaving it cached at the
        old position.
      - 64 C++ tests, Rust green.
- [x] **1c.** DONE (ABI 1.18). `cad_sketch_create_on_face` ABI entry point + Rust wrapper + a test that sketches on a
      box face and extrudes from it.
- [ ] **1d.** ← NEXT. Shell: pick a face, camera aligns, sketch draws in place.

## Decisions made while building 1a

**The face is stored as its NAME TEXT, not as a `naming::ElementName`.** `core/sketch` does not
depend on `core/naming` and this keeps it that way. The text form is what round-trips through the
saved file anyway, and resolving it to geometry is `core/recompute`'s job — that layer already has
naming and already has the referenced feature's output. A sketch that resolved its own face would
need the document, which it must not have.

**`plane_` becomes `placement_`, and `plane()` still returns the global plane.** Every existing
caller (`to3d`, `toWire`'s normal, the ABI, the shell) keeps working unchanged, because for
`Kind::Global` the answer is identical. `Kind::Face` deliberately does NOT work yet — it
serialises and round-trips, and 1b makes it compute.

**Serialisation is additive.** The existing `plane <NAME>` line is unchanged and still written
always. Two new lines appear only when they carry information:

```
plane XY              # unchanged, always written; the global plane or the fallback
plane_kind 1          # omitted when Global
plane_face <text>     # omitted unless Kind::Face
```

An old file has neither new line and loads as `Global`, which is what it meant. A new file
carrying `plane_kind` opens in an old build as a plain global sketch rather than failing — the
unknown tag is ignored by the existing reader loop. That is a **deliberate forward-compatibility
property**, and it is the reason the global plane is still written even when a face is set.

## Traps confirmed while working, not just predicted

- `Sketch::to3d` and `Sketch::toWire` both switch on the plane, and both now read
  `placement_.global`. **They are `Kind::Global`-only and currently say so nowhere.** A face sketch
  reaching either of them today gets the global answer silently, which puts geometry somewhere the
  user did not draw it. `needsResolution()` exists for callers to check; 1b must make these two
  refuse rather than guess. This is the single most dangerous loose end in 1a.

- The real method names are `serialize`/`deserialize`, not `toText`/`fromText`.

## What 1c has to do

The core can now place a sketch on a face; nothing above the core can ASK for one.

- `cad_sketch_create_on_face(session, const CadElementId*, CadSketch* out)` beside the existing
  `cad_sketch_create`, which cannot change signature (ADR 0011, and the golden snapshot enforces
  it). Regenerate the snapshot deliberately, bump the ABI minor, sync `cad-sys`.
- The Rust wrapper, then a test that sketches on a box face and EXTRUDES from it — the extrude is
  the half that proves the frame is usable downstream, not just stored.
- `Controller` needs to set the `body` ObjectId property when a face is picked. That property name
  is currently a convention in `computeSketch` and the tests; it should become a named constant
  before a third caller invents a fourth spelling.

Two things that were got right in 1b and must stay right:Two things to get right:

1. **A face that cannot be resolved is `NamingLost` and the feature goes Blocked with a reason
   naming it** — never a silent fall back to the global plane. Falling back moves a user's geometry
   without telling them, and the naming layer exists precisely so this case is detectable.
2. **The resolved plane must be part of the cache key**, or editing the referenced face leaves the
   sketch cached against the old position. `Engine::cacheKeyOf` already mixes an ObjectId
   property's target cache key, so the sketch feature must *reference* the face's feature as an
   input rather than only naming it in text — otherwise it is the Import bug again, third
   variation.


## 1c as built, and the one thing it does not prove

`cad_sketch_create_on_face(session, const CadElementId*, out)` — a separate entry point rather than
an argument on `cad_sketch_create`, whose signature is frozen (ADR 0011, enforced by the golden
snapshot). It takes the face's TEXT and ignores the digest: a digest is only meaningful inside the
process that produced it, and a placement that stopped working after reopening the file would be
worse than one that never worked.

Rust wrapper `Session::new_sketch_on_face(&str)`, and three tests in
`tests-rs/cad-tests/tests/sketch_on_face.rs`: a sketch is created on a named face; a face sketch
with no body refuses to compute and says why; a face sketch WITH its body computes and extrudes to
the right volume.

**The gap in that last test, stated because it is the shape of mistake this project keeps making:**
volume is translation-invariant, so a profile built on the wrong plane and extruded the right
distance would pass it. The pad's POSITION is not asserted from Rust, because the wrapper has no
centroid or bounds accessor and adding ABI surface for a test is the wrong trade. Position IS
asserted for the sketch itself in `tests/acceptance/sketch_plane.cpp`, which can reach `measure()`.

Closing it properly means either a position accessor on the ABI — justified on its own merits, as
a shell will want one — or a C++ acceptance test that extrudes a face-placed sketch and checks the
solid's centroid. The second is free and should be done first.
