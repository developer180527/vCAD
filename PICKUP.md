# Pick up here

Written 15 Aug 2026, at the end of the session that ported the DXF reader to Rust. Everything below
was verified on that commit, not remembered.

**The tree is green and every promise currently marked (RESOLVED) is genuinely tested.** Nothing is
half-wired. You can start clean.

---

## Verified state

| Check | Result |
|---|---|
| C++ tests (`ctest --test-dir build`) | green |
| Rust tests (`cd tests-rs && cargo test --workspace`) | green; `plugin_host.rs` 29, plus `sequences.rs`, `concurrency.rs`, `dxf_fuzz.rs` |
| Parser tests (`cd rust/cad-parse && cargo test`) | green |
| DXF differential (`ctest -R dxf_differential`) | green; 0 value disagreements in 600 mutations |
| DXF fallback (`-DCAD_USE_RUST_DXF=OFF`, rebuild, rerun) | green; differential skips |
| Layering (`cmake --build build`) | Layering OK |
| Qt shell (`cmake --build build-qt --target vcad`) | builds clean |
| ABI golden snapshot | no drift, regenerated for 1.16 |

**153 tests in `tests-rs`, 26 in `rust/cad-parse`, 35 C++ tests, zero failures** at the time of
writing.

Two build directories, on purpose: `build` (core, tests, spikes) and `build-qt` (renderer + Qt
shell). Rust links the library from `build/abi`, so **run `cmake --build build` before
`cargo test`** or the suite tests a stale library. Note that `--target cad_sketch` is NOT enough —
`tests-rs` links `build/abi`, so a partial build silently tests the old DXF reader while the log
still shows the old wording. Build everything.

**Run the Rust suite in PARALLEL at least once before believing it.** A harness bug that only
appears under concurrency passed cleanly under `--test-threads=1` and aborted the process the
moment the full suite ran — see the note at the bottom.

---

## Done: the DXF parser in Rust

**All three steps are finished and the Rust reader is the default.** Importers parse untrusted
bytes from strangers — the one place in vCAD where an attacker controls the input, and the largest
CVE source in this industry. That argument covers the bytes and nothing past them, which is why
only the bytes moved.

1. **Fuzzing the dime importer** (`tests-rs/cad-tests/tests/dxf_fuzz.rs`) found three real bugs in
   what shipped: a SIGSEGV from 23 bytes, a 28.7-second hang from 82 bytes, and a one-past-the-end
   write. All three are guarded in `Dxf.cpp` and the guards still run in front of BOTH readers.
2. **Build integration** proved on a stub: `rust/cad-parse` as a `staticlib`, `cmake/CadRust.cmake`,
   `--offline --locked`, `panic = "abort"`.
3. **The parser** is `rust/cad-parse/src/dxf.rs` with its C surface in `src/dxf_c.rs` and the
   hand-written header in `rust/include/cad_parse.h`.

### The shape that makes it verifiable

`Dxf.cpp` was restructured so **both readers stop at a neutral `RawEntity` list** and a single
`buildSketch` does all projection, scaling, construction-layer matching, degenerate rejection and
counting. The domain half exists once. Without that, comparing the readers would compare two whole
importers, which proves much less.

`-DCAD_USE_RUST_DXF=OFF` selects dime. **Run the DXF tests both ways after touching this code** —
that is the only thing separating "the new reader agrees with the old one" from "the new reader
agrees with tests written alongside it". They currently produce byte-identical output on
`tests/data/sketch_profile.dxf`, down to the constraint-inference numbers downstream.

dime is **compiled in both configurations** even though it is called in only one. A fallback behind
an `#if` nobody builds stops compiling within a release or two, and discovers that on the machine
with no cargo — exactly the machine that needs it.

### Two behaviour changes, both deliberate

- **Partial imports.** dime refused any file it could not read completely; the Rust reader imports
  what parses and counts the rest in `DxfImportReport::malformed`, with a WARNING and a clause in
  `summary()`. The old policy's argument was sound — half a profile looks like a whole profile —
  and what changed is that there is now a number saying how much was lost. Revisit if a user ever
  reports acting on a partial import; refusing on `malformed > 0` is a two-line change.
- **Precision.** The Rust reader is f64; dime is f32. See the note on `importDxf` before tightening
  any round-trip tolerance — tightening it to what Rust alone delivers breaks the dime build for a
  reason unrelated to whatever is being tested.

### What is NOT ported, and should not be

`DxfExport.cpp` writes files we control. It has no untrusted input and none of the security
argument applies. It also stays the independent implementation that makes an export/import round
trip a real check rather than the same code run twice.

### What differential testing found, and why it is worth keeping

Running the same mutated corpus through both readers and comparing is a different kind of check
from either parser's own tests. A test asserts what its author believed; where the author misread
the format, the test agrees with the mistake. The other parser did not make the same mistake.

Two bugs on the first run, neither findable from inside the Rust suite:

1. **The Rust reader rejected any file with one corrupt group code anywhere** as "that file is not
   an ASCII DXF" — telling a user to convert a file that already is one, and discarding everything
   read before the damage. dime read 93 of 600 such files without complaint. The check belonged
   only at the FIRST record; after that a bad code means a DXF that goes bad part-way through.
2. **`cmake/CadRust.cmake` was not watching the parser's own sources.** `DEPENDS` named
   `src/lib.rs`, correct when that was the only file and silently wrong once the crate grew. Edits
   to the parser did not rebuild the library, so the C++ side linked a stale archive — and the
   differential test dutifully compared the new dime path against an old Rust one. Now globbed
   with `CONFIGURE_DEPENDS`.

The second is the more alarming one: it made a *measurement* lie, and the only reason it surfaced
is that the numbers did not move when they obviously should have.

The assertions are shaped around what a disagreement means. Two readers reading the same entities
and disagreeing about the NUMBERS must never happen and is checked at zero. Reading a *different
set* of entities is a documented policy difference — dime's `atof` salvages a numeric prefix so
`2NaN` becomes a radius of 2, the Rust reader refuses the token — and is bounded and counted. The
bounds are recorded against a measured run at the bottom of the file rather than guessed.

Run it both ways after touching either reader: `-DCAD_USE_RUST_DXF=OFF` makes the whole suite
exercise dime, and the differential skips rather than failing on a build with no Rust toolchain.

### Still open here

- **Windows and Linux CI still have not run any of this**, but the workflow now asks them to and
  the reason they were not is understood. The lint job runs `cargo fmt --all --check` on tests-rs
  and the matrix declares `needs: lint`, so a formatting drift that had been there for several
  commits was silently preventing every platform from building. Formatted, and the workflow now
  also sets `-DCAD_REQUIRE_RUST=ON`, installs Qt so the shell and `proshell_boundary` are built,
  and asserts that `rust_boundary`, `dxf_differential` and `proshell_boundary` are actually
  registered before running the suite.

  **CI has now run.** macOS went green with everything working — Qt installed, `proshell_boundary`
  ran offscreen, the Rust parser was required and used, `dxf_differential` executed. The other
  three jobs failed, on two causes, both now fixed and both worth knowing about:

  * **Linux and ASan: `glfw3` needs `libxrandr`/`libxinerama`/`libxcursor` dev headers** and fails
    at *configure* with "RandR headers not found". Nothing to do with any recent work — those jobs
    had simply never reached a vcpkg install, because the lint gate in front of them was failing.
  * **Windows: `rustc --print native-static-libs` mixes libraries with linker FLAGS.** On MSVC it
    ends `/defaultlib:msvcrt`, and CMake reads a leading `/` as an absolute path, so ninja hunted
    for a rule to build a file by that name and the link died naming no target of ours. macOS and
    Linux print only `-l` tokens, which pass through fine — so this could not have surfaced
    anywhere but Windows. Flags now go to `INTERFACE_LINK_OPTIONS`.

  Windows *configured* successfully, which means the MSVC `.lib` naming, the cargo invocation and
  `--offline --locked` all work there. Still unproven on Windows and Linux: whether `vcad` and
  `proshell` compile at all, since neither job reached a full build.
- **The fuzz corpus is thin** — two seeds, both ours. See the fixture notes below; a fuzzer
  starting from thin material stays thin.
- ~~No differential fuzzing.~~ **Done** — `tests/acceptance/dxf_differential.cpp`, ctest
  `dxf_differential`. It found two real bugs on its first run; see below.
- **INSERT / BLOCKS are not expanded** by either reader — counted as unsupported so the user is
  told, rather than silently handed an empty sketch. Real drawings use blocks heavily, so this is
  the most likely next complaint from an actual user.

### Test fixtures: what to fetch and what to watch for

**These are DATA, used to harden a parser — not code, and not shipped.** That framing settles most
of the licensing question: sample CAD corpora exist to be tested against, and using them that way
is what they are for. Two practical points remain, and they are practical rather than legal:

- The repository is **public**, so committing a file redistributes it whether or not it ships.
  Prefer sources that plainly allow that — OCCT's own test data, NIST's PMI/CAD corpus (US
  government, public domain), ODA/Autodesk published DXF samples. Record where each came from in a
  `README` beside them; six months on, "where did this file come from" is unanswerable otherwise.
- **Size.** Real assemblies run to tens or hundreds of megabytes and do not belong in git. Commit
  small fixtures directly; fetch large ones with a script that checksums what it downloads, so CI
  fails loudly if a remote file changes underneath it.

What the corpus needs to cover, because a fuzzer starting from thin material stays thin:

- files from DIFFERENT producers (SolidWorks, CATIA, Inventor, FreeCAD) — each writes valid-but
  -different STEP, and disagreement between producers is where importers break;
- an assembly, not only single parts;
- geometry with the tolerance soup real data carries: near-degenerate faces, seams, tiny edges;
- at least one file known to be MALFORMED, so the error path has a real specimen and not only a
  mutated one.

## After that: step 5, error containment (§5)

Step 4 is done (ABI 1.15). Writing its test first was worth it: the DATA was never at risk —
parameters are ordinary typed properties, so a document already round-tripped through a session
without its plugin unchanged. What was wrong was the reporting, and §4A now has
`ObjectState::NeedsPlugin`, its own count in the recompute report, and a grey `PLUGIN` badge
instead of a red `ERR`.

§5 is the other half of "a plugin must not take the user down with it", and it is the one with a
hard limit: an in-process native plugin that segfaults is NOT survivable, and §5 already says so.
What is achievable is containment of everything short of that — a compute that throws, that
returns nonsense, that never returns — plus honest attribution when it is not.

Read §5 before designing; the boundary between "contained" and "not survivable" is the whole
decision, and promising more than in-process C can deliver is worse than promising nothing.

## Plugin contract: two documented rules are now enforced

ABI 1.17. Both were rules the contract stated and nothing checked, which is the gap worth closing
before the loader exists — a rule a plugin author can violate without noticing is a rule that will
be violated.

- **§4.6 re-entrancy.** `register_feature` from inside `compute` returns `CAD_ERR_REENTRANT`.
  Verified red before green: without the guard the registration simply succeeds.
- **§4.1 determinism.** `CAD_PLUGIN_DETERMINISM_CHECK=1` runs every plugin compute twice and
  compares by `naming::contentHash`. Tested by a pair — caught when on, NOT caught when off — so
  the passing test is evidence about the check rather than about the fake plugin.

The determinism test runs in a **subprocess**, because the host reads the variable once into a
static and this suite runs tests in parallel: `getenv` racing `setenv` is UB, so the value cannot
be set from inside a test. Same approach as `m1_determinism_subprocess.cpp`.

Still prose, and unenforceable until the calls exist: the rest of §4.6 (`txn_begin` and
`register_command` are declared and NULL), §4.4 capabilities (advisory until sandboxing, and the
contract says so), §4.7 dependency isolation (needs the loader).

## Known test gaps, in priority order

Everything here is a gap someone identified and nobody has closed. Ordered by what a professional
losing work would care about.

1. **The four accessors added in 1.16 have NO tests** — `compute_param_element`,
   `compute_param_count`, `compute_param_element_at`, `compute_param_shape_at`. They are the
   machinery a fillet plugin needs, and they were wired without tests. Smallest item here.
2. **Importer fuzzing.** Zero fuzz targets exist. See the section above.
3. **A hostile plugin.** ADR 0011 enforcement point 3 requires one and it does not exist: a plugin
   returning unknown status codes, setting output twice, releasing handles it does not own,
   registering during compute, returning CAD_OK having done nothing. "A boundary that only survives
   well-behaved callers is not a boundary" is our own sentence.
4. **`migrate_params` is declared and never exercised.** The whole parameter-evolution story is
   untested behaviour.
5. **`compute_version` cache invalidation.** We assert identical features SHARE a cache entry;
   nothing asserts that bumping the version stops them sharing. That is the "stale geometry
   survives a rebuild" bug.
6. **The NaN-revert path is not proven to run.** `sketch_sequences.rs` asserts no coordinate is ever
   non-finite, and that invariant holds over the campaign — but nothing confirms the revert branch
   was ever ENTERED. The guard holding and the guard being tested are different claims.
7. **Shared DDC tier under concurrency.** `concurrency.rs` pins per-session locking; two sessions
   sharing the on-disk cache is a real data-race surface and is uncovered.
8. **Autosave and crash recovery do not exist.** No test can cover this because the feature is
   absent. For a tool someone keeps a day's work in, this is a larger reliability risk than
   anything above — and native plugins being able to take the process down makes it worse.

## Then

6. **The loader** — discovery, manifest, `dlopen` with `RTLD_LOCAL`, lifecycle. Plus the
   compatibility museum and the hostile-plugin test. Built last, deliberately: the loader is the
   first client and a client freezes the design it is built against.
7. **WASM sandbox ADR** — decided in principle (`PLUGIN_CONTRACT.md` §9). Needs: memory ownership
   across the linear-memory boundary, what a handle means when the guest cannot hold a host
   pointer, and the per-recompute performance cost.

Also open, from the performance work and not on the plugin thread at all:

- **`Document::add` is quadratic** (n^1.85 measured). Profiled with samply: the cost is
  `set_length` → `Engine::invalidate` → `Document::replace`, and every `replace` deep-copies the
  whole `std::map` of objects. `Document::dependents()` is an O(n) scan called once per invalidate.
  Both are in `tests-rs/cad-bench/tests/scaling.rs`, `#[ignore]`d with the measurements.
- **The sketch solver is cubic** (n^2.9). 98% of it is `GCS::System::diagnose` running a DENSE
  full-pivot QR. `GCS.h` already offers `EigenSparseQR` and we never set it; and `diagnose()` runs
  on every solve when the constraint set rarely changes between drags.

---

## Things a fresh session will otherwise rediscover the hard way

- **`extern "C" {` wraps the whole ABI header.** Any parser that tracks brace depth never returns to
  zero. This silently gave the golden snapshot 42 macros and *zero functions*, and it passed a
  `len() > 20` guard. The extractor now counts by kind.
- **Add to `CadHost` at the END, never in the middle.** A plugin compiled against an older minor
  computes every earlier member's offset from the layout it saw, so an inserted member shifts all
  of them and silently calls the wrong function pointer. The golden snapshot catches it — it
  reported "3 new declarations — legal" beside "CHANGED: struct CadHost" — but only if you read
  which of the two it said.
- **`withError` forces `Failed`.** `withState(Blocked).withError(e)` therefore produces a *Failed*
  object; only the reverse order gives a blocked one with a message. `withBlocked(e)` exists now so
  nobody has to know that.
- **Every comparison with NaN is false.** A guard written as `if (v <= 0.0) reject;` therefore
  ACCEPTS NaN, and a convergence test written as a threshold declares victory on it. Both bugs
  existed here, in five kernel sites and in the sketch solver. Use `isPositiveFinite` / `isFinite`
  from `kernel/Guard.h`.
- **Kernel API names**, all of which cost a compile cycle to find: `BoxResult` has `.op` (use
  `.op.shape()`), booleans are `booleanCut`/`booleanFuse`, validity is `shape.validate()` returning
  `Result<void>`, `ElementMap` has `size()` and no `empty()`, log level is `Warning` not `Warn`.
- **`nameprimitive` binds the FACES, not the solid.** `nameOf(solid)` correctly returns nothing.
- **`Session` already has a `scratch` string** for strings returned across the boundary. Reuse it.
- **Recompute skips objects that are Clean with an output** (`Engine.cpp`). This is why the Import
  cache fix is only half a fix: the key is now correct, but nothing *notices* a file changed.
- **Add to `CadHost` at the END.** Checked this session by looking: `compute_fail` was last, and
  the five 1.16 accessors follow it. The golden snapshot reports a middle insertion as
  "CHANGED: struct CadHost" rather than as an addition — but only if you read WHICH of the two it
  said.
- **`external_inputs` runs at CACHE-KEY time, not during compute.** That is why it lives on the
  descriptor and takes a `CadFeatureCtx` rather than a `CadComputeCtx`: when the key is built, no
  input has been computed. An earlier design had the plugin declare it from inside compute, which
  cannot work — the key it changes has already been computed by then. It was declared-but-unwired
  for a while, which silently reintroduced the Import bug through the plugin path.
- **`CadFeatureCtx` and `CadComputeCtx` share one handle space deliberately**, so the parameter
  accessors are written once and serve both moments a plugin reads its own parameters.
- **An invariant written `if let Ok(v) = ...` is an invariant that turns itself off.** The Clean
  -object volume check did exactly this: a Clean object whose volume could not be computed passed
  without comment. Assert the call succeeded, then assert the value.
- **A test harness must not keep host state in a `static mut`.** The fake plugin did, passed under
  `--test-threads=1`, and SIGABRT'd under the parallel suite. `plugin_ctx` is what a plugin carries
  its state in; a harness that cheats around it is not testing the boundary it claims to.

---

## Open, and unconfirmed by a human

**Does the native Metal surface actually render on screen?** The code landed, the status bar should
read `readback 0.0`, and nobody has looked. If it does not, the offscreen fallback is automatic, so
the shell is not broken either way.

**`--shot` hangs, but only with a document open.** Measured on `f2e5447`: `--shot --home` exits 0
and writes the PNG; `--shot` with a document times out. So the cause is in the viewport render
path, not in the screenshot code — an earlier note here said it had "never worked", which is too
strong and made the bug look unfindable. Evidence: the offscreen path repaints continuously (paints
1–4 render, 5+ arrive with nothing dirty), and Qt drains newly-posted events inside the same
`sendPostedEvents` pass, so the pending zero-timer that calls `grab()` never runs. `sample` puts the
main thread in an ordinary Qt repaint, not in the grab.

## Decisions made, so they are not relitigated

- **Sandbox tier is WASM.** Makes `CAD_CAP_*` enforceable rather than advisory and a plugin crash
  survivable, and it fits a C99 ABI over integers with no exceptions. Native plugins remain for
  what WASM cannot do, but they stay trusted code and the installer must say so. Needs its own ADR:
  memory ownership across the linear-memory boundary, what a handle means when the guest cannot
  hold a host pointer, and the per-recompute cost.
- **Importers move to Rust; the exporter does not.** Untrusted input is the whole argument, and
  `DxfExport.cpp` writes files we control.
- **Plugin UI is declarative, never drawn.** Two shells (Qt, SwiftUI) mean a plugin that draws
  works on one. A plugin cannot create a top-level ribbon tab — that is a user decision in
  settings, not a plugin decision at registration. Revit had to retrofit that limit; FreeCAD's
  equivalent is workbench proliferation.
- **The API is forever; the geometry is reproducible only within a kernel generation.** A 2026
  plugin loads in 2036. The shape it produces may differ if the kernel improved, and the document
  says so. This is the one real boundary on the decade promise and it is deliberate.

---

`docs/STATUS.md` was last audited 13 Aug and is now well behind, missing the renderer and plugin
threads entirely. Its own header warns that reading it without re-auditing is how it starts lying.
Re-audit it rather than trusting it.
