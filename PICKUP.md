# Pick up here

Written 15 Aug 2026, at the end of a long session. Everything below was verified on that commit,
not remembered.

**The tree is green and every promise currently marked (RESOLVED) is genuinely tested.** Nothing is
half-wired. You can start clean.

---

## What that session actually did

Recorded because the sections below are organised by subject rather than by order, and the arc is
hard to reconstruct from them.

1. **Ported the DXF reader to Rust** behind an unchanged `importDxf`, with a neutral `RawEntity`
   seam so both readers feed one sketch-building pass. Then **differentially fuzzed the two against
   each other**, which found a real bug in the new parser and a worse one in CMake.
2. **Extracted the reusable application shell** into `modules/proshell` — ribbon, theme, icons,
   marking menu, `ShellWindow`, `HomePage` — with `proshell_probe` linking Qt and nothing else to
   prove the boundary. `proshell` contains no occurrence of the string "cad".
3. **Cut two of the three edges** tying the recompute engine to the B-rep kernel, so the engine is
   now reusable by an application with no geometry.
4. **Enforced two plugin-contract rules** that had only ever been prose: re-entrancy and
   determinism (ABI 1.17).
5. **Took CI from three platforms to five** and made it able to fail honestly. It then found ten
   real bugs across MSVC, gcc, and arm64 — see the CI section.
6. **Consolidated the FFI declarations**, which surfaced an 8-byte stack overflow that had been
   present on every platform since ABI 1.15.
7. Wrote two review documents: `docs/design/COMPETITIVE_REVIEW.md` (how far from
   SolidWorks/Inventor, with the numbers counted) and `docs/design/PDF_EDITOR.md` (a concept note,
   committed to nothing).

**Next is features, not architecture.** The plugin loader (step 6), then the boring important
tools. See "Then".

---

## Verified state

| Check | Result |
|---|---|
| C++ tests (`ctest --test-dir build`) | green |
| Rust tests (`cd tests-rs && cargo test --workspace`) | green; `plugin_host.rs` 37, plus `sequences.rs`, `concurrency.rs`, `dxf_fuzz.rs`, `abi_declarations.rs` |
| Parser tests (`cd rust/cad-parse && cargo test`) | green |
| DXF differential (`ctest -R dxf_differential`) | green; 0 value disagreements in 600 mutations |
| DXF fallback (`-DCAD_USE_RUST_DXF=OFF`, rebuild, rerun) | green; differential skips |
| Layering (`cmake --build build`) | Layering OK |
| Qt shell (`cmake --build build-qt --target vcad`) | builds clean |
| ABI golden snapshot | no drift, regenerated for **1.17** |
| Qt shell boundary (`ctest -R proshell_boundary`) | green; `proshell` links Qt and nothing else |
| CI matrix | 5 platforms — Windows x64/arm64, Linux x64/arm64, macOS arm64 |

**158 tests in `tests-rs`, 26 in `rust/cad-parse`, 35 C++ tests, zero failures** at the time of
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

## The reusable shell: `modules/proshell`

A linkable Qt library holding the ribbon, theme, icon machinery, marking menu, `ShellWindow` frame
and `HomePage`. **It contains no occurrence of the string "cad".** vCAD supplies its command
catalogue, its glyph vocabulary, its browser and property table, and an ~80-line `CadHomeModel`.

The barrier was never coupling — `Ribbon`, `Theme` and `MarkingMenu` moved with no edit beyond the
namespace. It was that `shell_qt` was a single `qt_add_executable`, so there was no artifact to
link.

**`proshell_probe` is the part that matters.** It links `proshell` and Qt and nothing else, builds a
whole application in an architecture vocabulary, and runs headless as ctest `proshell_boundary`.
Without it, "this library is reusable" is a claim with one consumer. It bites: adding
`#include "cad/sketch/Sketch.h"` to `Theme.cpp` fails with *file not found*, because proshell's
include path has no `core/`. Compile time, not link time.

Two design decisions worth not relitigating:

- **`ShellWindow` has no document model.** No `IDocumentHost`, no virtual `documentCount()`. The
  subclass gets the tab bar and the page stack and wires them in a dozen lines. An interface written
  now would be shaped entirely by vCAD, and sessions, environments and "Home is not a document" are
  vCAD's ideas *about* documents rather than facts about applications.
- **`HomePage` DOES get an interface**, and the inconsistency is deliberate. A home page is a
  finished screen rather than a container, and what varies is four enumerable pieces of data:
  product name, document kinds, recent list, workspace summary. The test is not "is an interface
  good" but "do I know the whole surface".

Icons split along the same line: the library owns the machinery (device pixel ratio, pen width, the
`QIcon`) plus the glyphs every professional application shares; the application registers its own
vocabulary through a provider. A library shipping `extrude` would be a CAD library wearing a
generic name.

**Still vCAD's, correctly**: `Viewport` and `SketchCanvas`. What does not exist in either place is a
generic *viewport container* — the widget hosting a renderer surface with a ViewCube and navigation
bar. That is the next piece of shell worth extracting, and it does not exist yet at all.

---

## CI: five platforms, and the ten bugs they found

The matrix runs Windows x64 and arm64, Linux x64 and arm64, and macOS arm64, plus a lint job and an
ASan/UBSan job. Windows arm64 is there for Surface tablets.

**Read this before adding a platform**, because the pattern repeated: nearly every failure was a
real bug that macOS could not have shown, and two were bugs in things that had never run at all.

| Found by | Bug |
|---|---|
| Lint | `cargo fmt` had been failing for several commits, and `test: needs: lint` meant **no platform had built at all**. That was the actual reason the Rust integration had never run off this machine. |
| Linux gcc | `std::max({...})` without `<algorithm>` — libc++ pulls it in through `<string>`, libstdc++ does not. In the DXF spike *and* in a differential test written the same week. |
| Linux | bgfx wants `-lwayland-egl`; `libxrandr`/`libxinerama`/`libxcursor` missing too. Neither job had ever reached a link. |
| MSVC | An anonymous namespace nested inside `extern "C"` still gives its functions C linkage, so a helper returning `std::pair` was a warning on clang and four errors on MSVC. That warning had been visible on macOS for weeks and read as pedantry. |
| MSVC | `rustc --print native-static-libs` mixes libraries with linker FLAGS; `/defaultlib:msvcrt` is a path as far as CMake is concerned. macOS and Linux print only `-l` tokens. |
| MSVC | `planegcs.dll` had no import library — vendored sources carry no `__declspec(dllexport)`. Fixed with `WINDOWS_EXPORT_ALL_SYMBOLS`; making it STATIC also "works" and quietly changes our LGPL position. |
| Windows | No rpath: `cad_tests.exe` could not find `planegcs.dll`. Fixed in the build (`CMAKE_RUNTIME_OUTPUT_DIRECTORY` on WIN32 only), not in the workflow — it was equally broken for anyone running ctest on Windows. |
| Windows | Python extension could not load its DLLs. **Since Python 3.8 an extension does not resolve dependent DLLs through `PATH`**, so no workflow environment fix could have helped. |
| Linux arm64 | vcpkg downloads its own CMake/Ninja **for x64 only**; needs `VCPKG_FORCE_SYSTEM_BINARIES=1` and a system CMake new enough for its scripts. |
| Linux arm64 | An FFI declaration mismatch that had been passing on garbage in a return register — see the FFI section. |
| Linux arm64 | `glfw3` was in `vcpkg.json`, used by nothing, and pulled the X11/xcb chain from source; one tarball 504'd reliably enough to stop that job ever completing. Removed. |

Two structural notes:

- The vcpkg cache key is per-triplet (`format('cmake/triplets/{0}.cmake', matrix.triplet)`).
  Globbing the directory means adding a platform invalidates every existing platform's hour-long
  cache.
- **Linux arm64 does not build the Qt shell** (`expect_shell: false`). Qt ships no desktop
  linux/arm64 installer binaries and Ubuntu 24.04's own Qt is 6.4.2 against the 6.5 `shell_qt`
  requires. Stated in the matrix rather than discovered, because otherwise the
  "tests that matter are registered" guard fails that job for a non-bug.

---

## The platform tier: two of three edges cut

The reusable thing in this codebase is not the shell, it is **a parametric document with a cached
dependency graph, stable element identity, and a plugin ABI**. That was true in principle and false
in the build graph: three edges tied the engine to the B-rep kernel.

- ~~`units → kernel`~~ **cut.** It was `Result.h` and nothing else — a module about millimetres
  linked OCCT to say "that unit is not recognised". `Result`, `Error` and `ErrorCode` now live in
  `core/base`, which depends on nothing. `cad/kernel/Result.h` remains as an alias header so the
  ~200 call sites that say `kernel::Result` did not have to change; they are all correct.
- ~~`recompute → sketch`~~ **cut.** It was `Features.cpp` — the built-in feature catalogue living
  inside the engine module, so the dependency graph, dirty propagation, caching and rollback
  dragged in planegcs. Now `core/features`, and `FeatureRegistry::builtins()` became the free
  function `cad::features::builtins()` so the engine's own header names no feature type at all.
  **`core/recompute` now reaches the kernel only for `Result`/`Error`. Zero geometry.**
- `document → kernel` **not cut, and possibly should not be.** `document::Output` holds a
  `kernel::Shape` and a `naming::ElementMap` by value, so the header needs both definitions.
  Removing it means type-erasing `Output`, which touches ~21 sites and is the core data structure.

  Before paying that: **who is it for?** Any application with 3D geometry — BIM, EDA's board and
  enclosure work, anything doing clash detection — wants exactly "geometry plus stable names" as a
  feature output. `Output` is only wrong for an application with no geometry at all, such as a pure
  schematic or netlist tool. That is a real target but a narrower one than it first appears, and
  the honest question is whether it justifies restructuring the node every feature writes to.

  **`docs/design/PDF_EDITOR.md` is the case where the answer is yes** — a PDF editor is exactly the
  application with no geometry, and that note records what else would carry over if it is ever
  started. Read it before cutting this edge for any other reason.

The two edges that were cut cost one include line and one file move between them. The third is a
different kind of change and should be decided rather than drifted into.

## Platforms

The shipping target is wider than the matrix was: **Windows (x64 + arm64), Linux (x64 + arm64),
FreeBSD, macOS (arm64 only)**. Windows arm64 matters specifically for Surface tablets, where
stylus-driven sketching is the point.

CI now covers five of those. What is worth knowing before extending it further:

- **Our own code is already architecture-clean.** No SIMD, no intrinsics, no arch `#ifdef`s. Of
  thirteen platform guards, every `__APPLE__` one is an *additive* Metal path rather than an
  `else` branch, so a new POSIX platform falls through correctly instead of into a Linux-assuming
  branch. Porting risk is in dependencies and toolchain, not in our sources.
- **Linux arm64 does not build the Qt shell in CI.** Qt ships no desktop linux/arm64 binaries
  through the installer, and Ubuntu 24.04's own Qt is 6.4.2 against the 6.5 `shell_qt` requires.
  The job is marked `expect_shell: false` and says so rather than letting the registration guard
  fail for a non-bug.
- **FreeBSD's blocker is vcpkg, not the code.** FreeBSD triplets are community-supported and OCCT
  and Qt come from ports, so FreeBSD really means a second dependency-acquisition path — CMake
  finding system packages instead of vcpkg. That work also serves Linux distro packagers, which
  is the argument for doing it properly rather than as a FreeBSD special case.
- **FreeBSD arm64 has no prebuilt Rust std** (tier 3). With the DXF reader in Rust and CI policy
  `CAD_REQUIRE_RUST=ON`, that target needs `-Z build-std` on nightly or an explicit decision that
  it runs the dime fallback. Undecided.
- **No stylus support exists at all** — no `QTabletEvent`, no pressure, tilt, proximity or palm
  rejection, on any platform. That is a feature rather than a port, and it belongs in `proshell`:
  pressure and tilt are a professional-application capability, not a CAD one.

## FFI declarations: one place, enforced

`cad-sys` is the only crate allowed to declare `extern "C" fn cad_*`. `abi_declarations.rs` fails
the build if a second one appears, and `cargo fmt`-clean duplication is exactly how the two bugs
below survived.

Drift between two declarations of the same C function is **not a compile error in any language
involved** — Rust believes the `extern` block, C exports what it exports, and the linker matches on
the symbol name alone. It is undefined behaviour at the call, on some platforms, sometimes. So the
rule cannot be "be careful"; it has to be "there is only one place", enforced by something that
fails.

Two real bugs, both found this way:

- `cad_mesh_element_name` declared as returning `CadStr` where C returns `const char*`. A pointer
  comes back in one register, a two-word struct in two — so `.len` read whatever was in the second.
  Non-zero on four platforms, zero on Linux arm64/gcc. **Underneath it the test never tessellated**,
  so the string was empty everywhere, always: a wrong FFI signature was holding a broken assertion
  upright.
- `concurrency.rs` declared `RecomputeReport` with **five** `u64` where C has had **six** since ABI
  1.15 (`needs_plugin`). `cad_recompute` wrote 48 bytes into a 40-byte stack local on every call, on
  every platform, for as long as that field has existed. Nothing crashed because the eight bytes
  landed on adjacent stack, and nothing could have caught it — the sanitizer job builds the C++
  suite, not the Rust one.

**That second point is the standing gap**: there is no ASan/UBSan build of the Rust test suite, and
it is the half that does the FFI. Worth closing.

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

## Then — and the priority has changed

**The next work is features, not architecture.** That is a deliberate decision taken at the end of
the session, and `docs/design/COMPETITIVE_REVIEW.md` is the argument for it: 46 of ~73 ribbon
entries are disabled stand-ins, there are 11 feature types against SolidWorks' hundred or so, and
one of four document kinds is implemented. The expensive-to-retrofit work is done. The
characteristic risk of that ordering is architecture that never meets features, in a codebase whose
culture makes the remaining work the least attractive kind.

So: the loader first, because it is the one architectural item that unblocks a whole category, and
then the boring important tools.

6. **The loader** — discovery, manifest, `dlopen` with `RTLD_LOCAL`, lifecycle. Plus the
   compatibility museum and the hostile-plugin test. Built last, deliberately: the loader is the
   first client and a client freezes the design it is built against. **It is also the thing that
   turns the plugin ABI from a well-designed contract with zero clients into something real** — no
   third-party plugin has ever been loaded, and every plugin test to date runs an in-process fake.
7. **WASM sandbox ADR** — decided in principle (`PLUGIN_CONTRACT.md` §9). Needs: memory ownership
   across the linear-memory boundary, what a handle means when the guest cannot hold a host
   pointer, and the per-recompute performance cost.

**And after the loader, the features.** `COMPETITIVE_REVIEW.md` names **assemblies** as the single
objective that would move vCAD from impressive architecture to a usable tool: it is the last gap
that is architectural rather than merely long (it needs references BETWEEN documents, which the
document model does not have), it is what these products fundamentally are, and it exercises the
document model, the naming layer and the cache together at a scale a single part never reaches —
which is exactly where the two performance ceilings below stop being footnotes. Drawings are second
and are more laborious than architectural; hidden-line removal is already linked through `TKHLR`.

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

## Documents worth reading before starting

- `docs/design/COMPETITIVE_REVIEW.md` — how far from SolidWorks and Inventor, with every vCAD
  number counted from the tree rather than remembered. Read it before deciding what to build next;
  it is the argument for the priority change above.
- `docs/design/PLUGIN_CONTRACT.md` — the contract, the incumbents' mistakes it was designed
  against, and §8's implementation order. Steps 1–4 are RESOLVED; 5 and 6 are not.
- `docs/design/PDF_EDITOR.md` — a concept note for a second product on these primitives.
  **Committed to nothing**, and deliberately parked: the decision at the end of the session was to
  finish this one first. It also happens to answer the open question about the third platform edge.
- `docs/design/SHELL_INVENTORY.md` — corrected 15 Aug; the 3D viewport row had said "Placeholder"
  long after bgfx started presenting directly. Other rows may have drifted the same way.

`docs/STATUS.md` was last audited 13 Aug and is now well behind, missing the renderer and plugin
threads entirely. Its own header warns that reading it without re-auditing is how it starts lying.
Re-audit it rather than trusting it.
