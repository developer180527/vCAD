---
status: unreviewed
---
# Issues — DDC / cook pipeline, triaged (Mon Aug 3)

Two independent external reviews of the content-addressing, memory-budget
scheduling, out-of-process and shared-DDC cook system. Every claim below was
checked against the code before being accepted; several did not survive that,
and saying so is the point of this file. Sections:

- **[FIXED]** — real, and fixed in this pass (with the test that holds it fixed)
- **[REAL, OPEN]** — real, deliberately not done yet, with why
- **[MISDIAGNOSED]** — the symptom described does not exist; several of these
  pointed at a *different* real problem, recorded here
- **[REJECTED]** — not a problem, or the proposed remedy is worse

---

## [FIXED]

### F1. The DDC store grew without bound — nothing ever collected it
`--gc` (`d121a93`) does **not** cover this. `CookService::collectGarbage`
reconciles a project's `.cache/` against its registry; the DDC is a different
store with no referrer to ask, because keys are derived from *inputs*: every
source edit, cooker version bump and settings change mints a new key and orphans
the old blob permanently. Worst on the shared tier, which accumulates every blob
every machine ever produced.

Fixed: `DdcStore::collectGarbage(maxBytes, prune)` — budget + LRU, wired into
`engine_cook --gc` / `--gc-prune` (dry-run by default, matching the existing UX).
Four decisions worth keeping:

- **LRU by mtime, which `fetch` touches on a local hit** — the order reflects
  *use*, not ingest, so an old-but-hot blob is not evicted before a new cold one.
- **Hardlinked blobs (link count > 1) are PINNED, not reclaimable.** Unlinking
  the store's copy frees zero bytes — the project's `.cache` link keeps the inode
  alive — and would force a re-ingest of something demonstrably in use.
  Reporting them as reclaimable would make `freedBytes` a lie.
- **The DDC pass runs AFTER the cache sweep**, because those deletions drop
  hardlinks and un-pin blobs. The other order reclaims nothing on the first run.
- **The shared tier is never collected from a client**, for the same reason
  `evictLocal` never touches it: a client cannot know what another machine still
  needs, and one over-eager GC costs the whole studio a recook. That is an
  administrative decision on the host.

Budget is `ENGINE_DDC_MAX_MB`, default 20 GB; `0` means unbounded (and evicts
nothing — an opt-out, not a cache that deletes itself by default).
Test: `cook_hardening_test`.

### F2. A truncated worker result file read as a successful cook
The severe one. `RESULT ok` is the **first** body line, so a worker killed
part-way through writing — deadline `SIGKILL`, rlimit OOM, signal out of a
corrupt parse — left a file that parsed as a clean success with its `OUTPUT`
lines simply absent. The parent believed it and committed the asset without its
sibling textures: the *silently-untextured build* (cf. `ab77845`), arriving
through the IPC channel instead of the packager.

Fixed: magic + version header and an `END <lines> <digest>` trailer, in
`assetlib/cook_result_file.h` — **one** implementation shared by the writer
(`engine_cook_worker`) and the reader (`cook_dispatch`), which were previously
two hand-rolled halves of an undocumented protocol. The frame is validated
before any field is read; anything incomplete is a failed cook.

FNV-1a, not BLAKE3, deliberately: this detects truncation, and the file sits in
our own temp directory beside the artifact it describes. Anyone who can rewrite
it can rewrite the artifact, and the DDC's content hash is what guards *that*.
Test: `cook_hardening_test` (truncation after the verdict, mid-trailer, digest
mismatch, dropped line, unframed legacy file, empty file).

### F3. `::stat(p.string().c_str())` broke on Windows non-ASCII paths
`path::string()` renders the *native narrow* encoding — the active ANSI code
page on Windows — so a source file with an accented or CJK name did not
round-trip, `::stat` failed, and `statChanged` returned "changed" forever. Fails
safe (it recooks) but recooks on **every** scan, permanently.

Fixed: `std::filesystem::last_write_time` + `file_size` with `error_code`, no
path-to-string conversion. Note the stored mtime is now `file_time_type`'s raw
tick count (its epoch is unspecified) — fine, because the value is only ever
compared against another produced by the same function. **One-time cost:** the
first scan after this lands sees every record as stat-changed and rehashes once;
the hash then matches, so nothing recooks.

### F4. Migration errors were unconditionally discarded, and `open()` lied
See M1 for what the reviewers got wrong here. The real defect: *every* SQLite
error was swallowed, so a genuine failure (corrupt DB, read-only file, full
disk) was indistinguishable from the benign "column already exists", `migrate()`
returned `void`, and `open()` returned `true` regardless. An unusable registry
reported success and the cook then silently recorded nothing.

Fixed: match the benign case exactly (`duplicate column name`), treat anything
else as a failure, and `open()` now fails when the schema cannot be reached.
Added `PRAGMA user_version` (`kSchemaVersion`), which also lets us **refuse a
database written by a newer build** rather than write to it and drop the columns
this build cannot see — a real hazard once a cache is shared across machines on
different builds. Test: `cook_hardening_test`.

### F5. `findBySourcePath` per scanned file — O(N) statement *compilations*
Worse than the "O(N) SQL queries" reported: it builds the SQL by string
concatenation and calls `sqlite3_prepare_v2` on every call, and compilation is
the expensive half. Fixed by extending the snapshot that `hashIndex()` already
builds in the same function — one `all()` now serves both move detection and the
existence check. Safe as a snapshot because the directory walk visits each
`rel` at most once.

**Not** claimed as a speedup: unmeasured at scale. Warm no-op scan is ~37 ms on
`fps_shooter` (2026-08-03), which is the number to beat if anyone optimises here
on purpose.

### F6. POSIX memory cap did not cover the child's static initializers
`setrlimit` was applied inside the worker's `main()` from `argv`, so everything
before `main` ran uncapped. Fixed with `fork` + `setrlimit` + `execv`: rlimits
are inherited across `exec`, so the cap predates the worker's first instruction
— the property the Windows path already had (it assigns the job object while the
process is still suspended). Only async-signal-safe calls sit between fork and
exec; this parent is multithreaded, so a `malloc` there could deadlock on a lock
another thread held at fork time. A distinct exit code (66) now reports "cannot
exec the worker" instead of it looking like a crashed cook.

### F7. `MemGovernor` clamped an unbalanced release without asserting
Confirmed — loud `fprintf`, no assert. Added one: the clamp keeps a release
build scheduling, it does not make the imbalance correct.

### F9. Every record in the registry was marked Missing
Found while verifying the scanner decomposition, not by either review. One
registry holds records from more than one asset root — `CookService` scans the
project's assets AND the engine's own defaults against the same `projectRoot`,
and the runtime scans only the project's. The "mark absent files Missing" sweep
iterated EVERY record regardless of which root had just been walked, so each scan
declared the other root's assets deleted: the project scan marked every engine
shader Missing, the engine scan marked every project asset Missing, and a plain
editor boot marked all the engine defaults Missing.

Measured on `fps_shooter` before the fix: **653 of 653 records Missing** — the
entire registry. Nearly harmless in practice only because `cookIsStale` ignores
`state` except to keep a Failed record failed, so nothing recooked and nothing
broke. But `findByState(Missing)` was pure fiction, and any editor UI or tooling
that trusted it would show a project whose every asset had vanished.

Fixed by scoping the sweep to the root actually scanned: a scan of root X can only
make claims about assets under X. Records already mis-marked heal on the next scan
that sees the file present — verified: 653 → 0, with `0 cooked`, since state is
not what decides staleness.

Test: `cook_hardening_test` (both roots survive each other's scans, a genuine
deletion under the scanned root IS still caught so the scoping cannot be a
disabled sweep, and a Missing record heals). Mutation-proved: restoring the
unscoped sweep fails 3 assertions.

### F8. Dependency invalidation: wired, and not the way this file first said
Was O1. Both mechanisms were unwired — `DdcKeyInputs::depHashes` populated by
nobody, `dependents()`/`transitiveDependents()` never called from the cook path.
The advice here was "wire one or delete both". Checking the code settled *which*,
and ruled the other out entirely:

**The registry cascade cannot work.** `cookIsStale` decides staleness from the DDC
key alone and never reads `rec.state` (bar keeping a Failed record failed), so
marking dependents `Stale` is a no-op — they still match their key and are still
skipped. A cascade is also wrong for a SHARED DDC even in principle: it is local to
one machine's registry, while another machine fetches by content key and would be
served the stale blob regardless. `transitiveDependents()` is therefore now
documented as a QUERY (for "what does this affect" tooling), not an invalidation
path.

**The missing piece was in the interface, not the plumbing.** `ctx.addDependency`
takes a UUID and cookers have no registry lookup, so a cooker whose extra input was
a plain FILE could not declare it at all. Both cookers that have one hand-rolled
`blake3File()` into `settingsFingerprint` — correct, and unenforceable: a cooker
that forgot looked exactly like one with no extra inputs. Added
`ICooker::declaredInputs()`; the pipeline hashes what it returns into the key.
`ShaderCooker` (its `.sc` stage sources, transitively through `#include`) and
`MaterialCooker` (the `.shader` manifest) migrated onto it.

`depHashes` is also fed from the recorded UUID dependency set now, as the
dependency's SOURCE hash, snapshotted in one query per batch. That covers what only
a cook can discover, from the second cook onward.

**Enforcement is the real deliverable:** `cook_deps_test` perturbs every declared
input of every registered cooker and requires the key to move. Mutation-proved — an
undeclared input fails 3 assertions. A cooker added later with a second input it
does not declare now fails a test instead of serving stale output for the life of
the project.

Deliberately NOT transitive (see R1): source hashes, not the dependency's cooked
key. A material depends on the shader's declared interface, not its shading code,
and the test pins both directions — the manifest re-keys it, the `.sc` does not.

---

## [REAL, OPEN]

### O2. Bit-for-bit determinism is load-bearing for the shared tier
A shared DDC keyed on inputs silently asserts that identical inputs produce
identical outputs on every machine. If macOS-arm64 and Linux-x86-64 differ by one
padding byte, the store serves one machine's output to the other under a key
claiming equivalence — undetectably. No test covers this. Cheap first step: hash
every cooked output on both platforms over the same corpus and diff. Connects
directly to the cross-ISA determinism lane in
`docs/plans/automated-testing-soak-fuzz-plan.md`.

### O2b. `settingsFingerprint` cannot signal failure distinctly
`MaterialCooker` does the right thing by encoding failures into the fingerprint
(`"unparsed"`, `"shader-missing:<ref>"`, `"…@unreadable"`), so each failure mode
gets its own key and self-heals. But the interface returns a bare `std::string`,
so a cooker that returns `""` on a transient read error would hash identically to
"no settings" and cache a wrongly-cooked artifact under a key that looks correct.
Worth tightening to `std::optional<std::string>` now that F8's enforcement test
exists to catch the fallout.

### O4. Unreadable sources are skipped silently
`computeCookKey` returns `{}` for an unreadable source and `cookIsStale` then
returns `false` ("cooking would fail identically"). The behaviour is defensible
and self-heals when the file becomes readable — but nothing *reports* that an
asset was skipped, so it looks like an asset that was never authored. Reporting,
not re-designing, is the fix.

### O5. Windows reserved device names in DDC manifest members
`ddcFetchRecord` already rejects `/`, `\`, `:` and `.`/`..` (so drive letters and
ADS are covered), but not `CON`/`AUX`/`NUL`/`COM1`… or >255-char names. Only
reachable from a hostile *shared* store, which is a real threat model for a
studio mount, but Windows-only and low. Do it with the Windows port.

### O6. Result-file protocol is not fuzzed
`cook_hardening_test` covers the truncation shapes that motivated F2, but the
parser is untrusted-input surface and belongs in `tests/fuzz`. Cheap to add
against the existing harness.

### O7. `estimatePeakBytes` has no feedback loop
Fair: a grossly wrong estimate can still OOM the scheduler. Measuring actual
peak RSS per cooker and adjusting is a genuine improvement — but it needs the
measurement infrastructure first, and the enforcement rlimit (F6) already bounds
the damage to one asset. Not urgent.

### O8. `std::printf` throughout instead of a levelled logger
Confirmed and accepted for now: `assetlib` is deliberately engine-dependency-free
(it cannot reach `core/logger.h`), so this needs an injectable sink on the
module boundary rather than a find-and-replace.

---

## [MISDIAGNOSED]

### M1. "Migrations are unconditional — a failure skips the rest silently"
The skip-cascade **does not happen**. `migrate()` is a `for` loop of independent
`sqlite3_exec` calls; a failure at N cannot prevent N+1. The loop was already
idempotent, just noisily so (7 failing ALTERs per open). The real defect
underneath it is F4, and the proposed remedy (conditional ALTER via
`table_info`) was not the fix — checking *which* error occurred was.

### M2. "`settingsFingerprint` fails → the key becomes empty → staleness is wrong"
Not what happens. `computeDdcKey` hashes an empty settings string happily; the
key stays valid. `computeCookKey` *does* already guard the source-hash case
explicitly, and `cookIsStale` handles an empty key with a documented rationale.
The real, narrower version of this concern is O3.

### M3. "Manual `depHashes` arrays risk poisoning if nested deps mutate"
Assumed `depHashes` was in use. It was populated by nobody — the real shape of the
problem, and its fix, is F8.

---

## [REJECTED]

### R1. Merkle-DAG cache keys (recursively folding dependency content hashes)
The wrong shape here, not just unnecessary. `MaterialCooker` deliberately hashes
**only the `.shader` manifest** — the declared interface — and not the `.sc`
stage sources, because shading-code edits change no byte of a cooked material. A
naive recursive fold of content hashes reintroduces exactly the over-invalidation
that comment exists to prevent: every material in the project recooking on every
shader edit. The existing per-cooker `settingsFingerprint` is *more precise* than
the proposed replacement. What was genuinely missing was enforcement — see F8,
which adds it without making the fold transitive.

### R2. Hardlink locking: `ReplaceFileW` / `renameat2(RENAME_EXCHANGE)` for commits
Premise does not fit a CAS. DDC blobs are content-addressed and `chmod 0444`: a
blob's content never changes for a given key, so there is nothing to update
in place — you write a *new* key. Ingest is already temp-file + atomic rename.
`ETXTBSY` is also wrong: it applies to executables being executed, not data
files, and writing to a file another process holds open for read is fine on
Linux. The Windows sharing-violation concern is real but belongs to the `.cache`
materialization path, not here.

### R3. cgroups v2 / systemd scopes, and `posix_spawnattr_setflags`, for the rlimit
Direction was right (POSIX lagged Windows) but both remedies are wrong.
`posix_spawn` has **no** rlimit attribute in POSIX — the mechanism does not
exist. And cgroups is heavy infrastructure, Linux-only, for something
`fork`+`setrlimit`+`exec` solves in three lines and portably, because rlimits
are inherited across `exec`. See F6.

### R4. Replace the sidecar result file with pipes or a shared-memory ring (FlatBuffers)
Large build, justified by throughput numbers nobody has. Cold cook is already
1.8 s for `fps_shooter`; the sidecar is one small file write per asset, and it is
deliberately human-readable for debugging a failing cook. F2 fixed the actual
defect (undetectable truncation) at a fraction of the cost. Revisit if profiling
ever shows result I/O mattering.

### R5. Persistent worker pool / batching several assets per process
Same objection, plus it trades away the property the child process exists for:
one asset per process means one corrupt file kills one cook. Batching widens the
blast radius to every asset sharing the process. Would need a throughput problem
to justify, and there isn't one.

### R6. SQLite access timestamps in the DDC for LRU
Would put a SQLite database on a shared network mount, where its locking is
famously unsafe (NFS/SMB). Filesystem mtime needs no database, no locking, and
works per-tier. See F1.

### R7. Move `backfillDdc` off the startup path onto a thread pool
Measured before accepting: warm no-op is ~37 ms end to end on `fps_shooter`
(2026-08-03). There is no startup stall to move. Revisit if a project appears
where there is one.
