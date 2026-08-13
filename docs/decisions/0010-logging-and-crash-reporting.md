# 0010 — Logging, and crash reporting adapted from crashkit

Status: proposed (Aug 2026)

## Context

An audit of production-readiness probed for logging, crash reporting, autosave, i18n,
installers and telemetry. **All six returned zero files.** vCAD has no way to find out what
happened on someone else's machine.

A design note for a game-engine crash reporter (`engine/docs/plans/future-plans/crash-reporting.md`)
already works most of this through. This ADR records what transfers unchanged, what CAD changes,
and — the part that matters most — **what we should build first, which is not crash reporting.**

## Logging comes first, and it is not close

The note's own argument applies to itself: *"the value is not the capture"*. A crash reporter is
months and needs an out-of-process handler, Crashpad, symbol archiving and a delivery pipeline.
**Logging is days**, and at our stage — zero external users, every crash happening on a machine we
own — a log file plus a local dump is most of the value.

There is also a debt logging pays off immediately. Third-party libraries already write to stderr
with no route into anything: dime prints `DXF loading failed at line: 0` on a bad file, OCCT prints
its own diagnostics, and planegcs is silenced entirely behind `CAD_PLANEGCS_LOG` because it would
otherwise flood a drag. Every one of those is a diagnostic we currently discard or suppress rather
than record.

### `core/log`

- In `core/`, so the kernel and the solver can use it. **No Qt, no dependency** — it has to compile
  for the iPad target and be callable from `abi/`.
- Levels and **categories** (`kernel`, `sketch`, `recompute`, `io`, `render`, `shell`), because the
  useful filter in this application is subsystem, not severity: a fillet failure and a file-parse
  failure are both warnings and are never interesting at the same time.
- Sinks: rotating file + stderr. The file is **attachment number one** for any future crash report.
- Route Qt's `qInstallMessageHandler` and OCCT's printers into it, so third-party noise lands in
  the same place as ours instead of on a terminal nobody is watching.
- Cheap when off. A disabled category must cost a predicted branch, because `recompute` will log
  per feature and `sketch` per solve.

## What transfers from crashkit unchanged

- **The architectural law.** A crashing process cannot report its own crash: heap corrupt, stack
  exhausted, the faulting thread holding the lock the reporter needs. Out-of-process handler,
  started at launch, reading the dying process from outside.
- **Borrow the capture, own the abstraction.** `ICaptureBackend` with Crashpad first, `null` for
  tests. The per-platform table in the note (Mach exception ports, `sigaltstack`, `__fastfail`) is
  months of work on OS versions we do not own, and it is not our differentiator.
- **Annotations and breadcrumbs published continuously**, never at crash time. Fixed-capacity
  shared memory, no allocation, no locks.
- **Disk queue first, always.** Upload is a separate concern from capture.
- **Classify server-side from recorded inputs**, never a tier written at crash time. A confident
  wrong tier is worse than none.
- **The phasing**, which is the note's best idea: annotations + breadcrumbs + disk queue with a
  `null` backend needs no OS work at all and is immediately useful for asserts.

### Symbols reuse assetlib, exactly as the note predicted

Symbol archives keyed by build id are content-addressed immutable blobs with a budget and a GC —
**the DDC shape we already have in `modules/assetlib`**. The note argues this for the engine repo;
it is equally true here, and it means the expensive half of symbol storage is already written and
tested.

## What CAD changes

### 1. A `Plugin` tier, and it matters more here than in a game

vCAD is LGPL specifically so third parties can ship plugins, including paid closed-source ones
(`COPYRIGHT.md`). That guarantees crashes in code we did not write and cannot debug.

**Attribution is therefore not a nicety, it is a support-cost and reputation question.** Without it
every plugin crash is a vCAD crash in the user's mind. The ownership table the note describes —
module range → tier, versioned, registered at startup — is the mechanism, and the plugin loader
must register each plugin's range as it loads it. That is a requirement on the loader, which does
not exist yet, and is worth knowing before it is designed.

Tiers: `Kernel` (OCCT), `Solver` (planegcs), `Core` (ours), `Shell` (Qt), **`Plugin`**, `Platform`,
`Gpu`, `Assert`, `Resource`.

### 2. The payload is the user's intellectual property

A game's minidump leaks usernames and save data. **A CAD minidump can contain the geometry of an
unreleased product.** That is a trade secret, not a privacy inconvenience, and it changes the
default rather than adding a consent checkbox:

- **Never attach the document.** Not by default, not behind a checkbox nobody reads.
- Minimize hard: stack, registers, module list, annotations. Full-heap dumps are opt-in per report,
  with the reason stated in the prompt.
- What we attach instead is the **feature tree shape** — operation types and counts, no dimensions,
  no names, no geometry. `Box → Sketch → Extrude → Fillet(4 edges)` reproduces most kernel crashes
  and discloses nothing.

### 3. Determinism gives us something a game cannot have

Recompute is deterministic and content-addressed. A crash report can carry the **document digest
and the failing feature's cache key**, which means a crash is often reproducible *exactly* — and if
the shared DDC tier has the inputs, reproducible on our machine without the user sending a file at
all. That is a materially stronger position than the note's target has, and it comes free from
work already done.

### 4. `Hang` needs rethinking

The note's watchdog assumes frames. A CAD recompute legitimately runs for minutes, so a
frame-based watchdog would fire constantly on correct behaviour. Ours must distinguish *working*
from *stuck*: `Engine` publishes a progress counter into the annotation block, and the handler
treats "no progress for N seconds" as the hang signal, not "no frame".

### 5. Delivery is premature

Consent flows, `HttpSink`, kill switches and rate limiting exist because a game has players. We
have none. **Stop at the disk queue**, with a local report the user can attach to an issue by hand.
Building ingestion before there is anyone to ingest from is the scope creep the note warns about,
arriving early.

## Plan

1. **`core/log`** — levels, categories, file + stderr sinks; adopt Qt and OCCT output. Days.
2. **Breadcrumbs and annotations** in the same module, `null` capture, disk queue. No OS work.
3. **Crashpad behind `ICaptureBackend`**, macOS first (the machine CI and development share).
4. **Symbol archive on assetlib**, plus a symbolize tool.
5. **Ownership table and tiering**, timed to the plugin loader so it lands with the thing that
   makes it necessary.
6. Everything else — other platforms, HTTP, consent — only when there are users.

Steps 1 and 2 are worth doing next. Step 3 onward should wait for the renderer, because a crash
reporter for an application whose viewport does not work yet is instrumentation pointed at the
wrong problem.

## The test that matters

The note's matrix is right, and its last four cases are where homegrown reporters fail: a crash on
a non-main thread, a crash *during shutdown*, a crash *inside the handler*, and a crash *before
init*. Ours adds one the note does not need: **a crash inside a plugin**, asserting the report is
attributed to `Plugin` and names it. If that one does not work, the tiering has no value.
