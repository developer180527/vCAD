# ADR 0011 — Plugin ABI compatibility, and how it is enforced

**Status:** Proposed
**Date:** 14 Aug 2026

## The goal, stated precisely

**A plugin compiled today must load and run against a vCAD built in ten years, without
recompilation.** Source compatibility is not enough — the plugin author may be gone, the compiler
may be gone, the company may be gone. The binary has to keep working.

That is an operating-system guarantee, not a library guarantee, and it is expensive. This ADR
records what it costs and what has to change to make it possible at all.

---

## Where we actually are

Two halves of `abi/include/cad/abi/cad_plugin_abi.h` are in completely different states, and the
difference matters more than anything else in this document.

**The Session API is real.** `cad_session_*`, `cad_sketch_*` and the rest are exported, and the
Rust suite drives the core exclusively through them. That is the single best decision this project
made about its boundary: an ABI regression fails 95 tests rather than being discovered by a third
party. It is exercised on every build.

**The plugin API has never been used by anything.** `CadHost`, `CadPluginDesc` and
`cad_plugin_main` appear in the header and nowhere else in the repository — no loader, no host
vtable implementation, no `dlopen`, no test. `register_feature`, `register_command` and
`register_format` are declared with no implementation.

So the honest position: **the plugin ABI is a design, not a contract.** Nothing has ever tested it,
and an interface with no clients is always wrong in ways that only a client reveals. It is much
cheaper to fix that now, before anyone depends on it, than after.

---

## Three things that make a decade impossible today

### 1. Extension points pass an untyped blob — **(RESOLVED)**

```c
CadStatus (*register_feature)(void* ctx, const void* feature_desc);
CadStatus (*register_command)(void* ctx, const void* command_desc);
CadStatus (*register_format)(void* ctx, const void* format_desc);
```

`const void*`. No type, no size, no version. These are the three most important functions in the
plugin API — registering a feature is the entire point of a CAD plugin — and their payload layout
is undescribed. The moment a field is added to a feature descriptor, every existing plugin passes a
struct the host reads past the end of.

**This is the single biggest threat to the goal**, and it is unfixable after the first shipped
plugin. Descriptors must be real, versioned structs whose first member is their own size:

```c
typedef struct {
    uint32_t struct_size;      /* sizeof(CadFeatureDesc) as the PLUGIN saw it */
    uint32_t struct_version;
    /* ... fields, only ever appended ... */
} CadFeatureDesc;
```

The host reads `struct_size`, and uses only the fields the plugin's generation actually has.

**(RESOLVED)** — `CadFeatureDesc`, `CadCommandDesc` and `CadFormatDesc` exist in
`abi/include/cad/abi/cad_plugin_abi.h`, each prefixed with `struct_size`/`struct_version`, and the
three `register_*` entries are typed against them. ABI 1.9.

### 2. `abi_major` equality orphans every plugin on a major bump

`CadPluginDesc.abi_major` "must equal `CAD_ABI_VERSION_MAJOR`". That means the day we bump to 2,
every plugin in existence stops loading. The header's own comment says the vtable design exists so
we can "load two ABI generations side by side" — but nothing implements that, and the version check
as written forbids it.

For the stated goal, the host must be able to hand a v1 plugin a **v1-shaped vtable** while running
v2 internally. That is a shim layer, it is real work, and it is what operating systems actually do.

### 3. `CadHost` has no `size` field — **(RESOLVED)**

It has `abi_major`/`abi_minor`, and a plugin *can* gate on `abi_minor >= N` to know whether a
function pointer exists. That works, but only if fields are appended and never reordered, and
nothing checks that mechanically. A `struct_size` first member is checkable by a machine; a
convention about minor versions is checkable by a careful human, which is the same as not checkable.

**(RESOLVED)** — `CadHost` and `CadPluginDesc` both carry `struct_size`/`struct_version`.

---

## Decision

### The rules

1. **C99 only across the boundary.** Already true. No C++ types, no exceptions, no `std::` anything.
   The C++ ABI is not stable across compilers, standard libraries, or even flags.
2. **Additive only.** Never remove a function, never change a signature, never reorder a struct
   member, never change the meaning of an existing value. A mistake gets a *new* function; the old
   one keeps its old behaviour forever.
3. **Every struct crossing the boundary starts with `uint32_t struct_size`.** Both sides use it to
   negotiate. This is the mechanism that makes rule 2 survivable.
4. **Opaque handles, never pointers.** Already true, and it is why the core can be rewritten
   underneath a plugin without the plugin noticing.
5. **Error codes are permanent.** A code's meaning never changes; new conditions get new codes.
   Plugins must treat unknown codes as generic failures — documented, and tested by *returning* an
   unknown code to an old plugin in CI.
6. **Major versions are served, not replaced.** When ABI 2 arrives, the host keeps a v1 shim. A
   major bump is a promise to maintain two surfaces, not permission to break one.

### The enforcement, which is the actual decision

Rules 1–6 are what every project intends. They fail everywhere anyway, because **discipline does
not survive a decade of contributors.** What survives is a test that fails.

1. **A golden header snapshot. (RESOLVED)** `cad_plugin_abi.h` is hashed per declaration; a test
   fails if any existing declaration changes. Adding is allowed, editing is not. This turns rule 2
   from an intention into a build failure.

   Implemented as `tests-rs/cad-tests/tests/abi_golden.rs` against `abi/abi_golden.txt`, covering
   **82 functions, 25 types and 42 macros**. Verified red on a changed signature and on a removal,
   green on an addition. Regenerated deliberately with
   `CAD_ABI_GOLDEN_UPDATE=1 cargo test -p cad-tests --test abi_golden`, so a reviewer sees the
   golden file change and knows exactly what the boundary gained.
2. **A compatibility museum. (RESOLVED)** Compiled plugin binaries, one per ABI generation, checked into the
   repository and loaded by CI **forever**. Not source — binaries, because recompiling from source
   tests source compatibility, which is not the promise. When ABI 1.8's plugin no longer loads, CI
   goes red and the guarantee is visibly broken rather than quietly.
3. **A hostile plugin in the suite.** One that returns errors, returns unknown codes, holds handles
   past their validity, and unloads mid-operation. A boundary that only survives well-behaved
   callers is not a boundary.
4. **The Session API's existing role continues.** It is already the most-tested surface in the
   project; the plugin vtable should be implemented *in terms of it* wherever possible, so the two
   cannot drift.

**(RESOLVED)** — `tests/museum/abi-1.<minor>/<triple>/` holds a frozen, compiled plugin binary with
its manifest and a `PROVENANCE` file recording the ABI, the commit it was built from, the compiler
and a sha256. `tests/acceptance/plugin_museum.cpp` loads every exhibit for the current platform
through the REAL loader and the real host vtable on every run.

Verified against both failure modes it exists to catch: a host that stops serving ABI generation 1
fails it, and an EMPTY museum fails it too — a silently-empty loop is how a guarantee stops being
checked without anyone noticing.

The exhibits are never rebuilt. `tests/plugins` builds from source every run and checks something
different and also useful; this checks the thing nothing else can. When it fails, the host is
wrong — refreshing an exhibit to make CI green converts the one test that can detect a broken
promise into a record of having broken it.

Point 2 is the one that actually delivers the goal. Everything else is hygiene.

---

## Negotiation, precisely — **(RESOLVED)**

Implemented as `cad_abi_accepts(abi_major, min_host_minor, out_reason)` in `abi/src/Session.cpp`,
which is the decision the loader will call rather than a rule the loader will re-derive. Six tests
in `tests-rs/cad-tests/tests/abi_versioning.rs` cover both directions, including that every minor
this host has passed through is still accepted, and that a refusal explains itself.

Layout invariants are enforced at COMPILE time in `tests/acceptance/m2_abi_layout.cpp`: every
descriptor begins with `struct_size` at offset 0, and the `CadParamKind` values — which are stored
in documents — are pinned so reordering the enum fails the build instead of silently turning a
40 mm extrude into a 40 degree one. Verified that moving a field above `struct_size` does fail the
build.

Three fields exist and it must be unambiguous which one answers which question.

| Question | Answered by |
|---|---|
| "Does this function pointer exist?" | **`CadHost.struct_size`** — the only authority |
| "Which generation's semantics am I getting?" | `abi_major` |
| "Which feature set does the host advertise?" | `abi_minor` — informational, for messages and logs |

`struct_size` is authoritative because it is mechanical. A rule like "`txn_savepoint` exists from
1.12" is a fact a human must remember and a machine cannot check; `offsetof(CadHost, txn_savepoint)
+ sizeof(ptr) <= host->struct_size` is checkable by the compiler and true by construction. A NULL
entry within `struct_size` separately means "present in this generation but not offered in this
configuration" — a sandboxed tier — so plugins check both, and §4.5 already requires it.

### The only legitimate version branch

§6A commits us to: if an author must branch to keep something *working*, we have failed.

Branching to opt **into** something new is different and is expected:

```c
/* Legitimate: use a better call when present, keep working when absent. */
if (offsetof(CadHost, fillet_variable) + sizeof(void*) <= host->struct_size
    && host->fillet_variable != NULL) { ... }
```

The distinction is the whole policy. Downward compatibility is the host's job; upward opt-in is the
plugin's choice. A plugin that never opts in must never need to know the host's version at all.

## Deprecation: nothing is ever removed

A function we regret keeps working forever. The mistake gets a replacement beside it, the old one
is marked deprecated **in documentation only**, and the symbol, signature and behaviour stay.

Deprecation is therefore advice to new authors, never a schedule. There is no removal date, no
"supported for three releases", no compile-time warning that becomes an error. The moment removal
is possible, every promise in this ADR is a suggestion.

## Behavioural compatibility — the harder half

Binary compatibility is the easy part and it is what most of this document is about. The failure
that actually destroys trust is subtler: **a call keeps its signature and changes what it does.**

`fillet_edges` gains a better algorithm. OCCT is upgraded and a boolean produces a slightly
different face count. A tolerance is tightened. Nothing recompiles, nothing errors, and every
document built on that operation now has different geometry.

This is not hypothetical for vCAD specifically: we sit on OCCT, and OCCT upgrades change geometry.

**The decision:**

1. **A geometry-changing change to any host operation is a `compute_version` event for everything
   downstream.** It must invalidate cached results, exactly like a plugin bumping its own version.
   The mechanism exists — the cache key already mixes `FeatureType::version` — and the discipline
   is to actually bump it when a kernel change alters output.
2. **A document records the kernel generation it was last computed under.** Opening a document
   built under an older generation is legal, and the user is *told* that reopening will recompute
   geometry that may differ. Silence here is what makes a colleague's part quietly change shape.
3. **Old geometry is not reproduced.** We will not keep every historical kernel path alive to
   rebuild a 2029 part exactly as 2029 built it — that is a cost no open-source project can carry.
   The promise is that a document **opens, recomputes, and tells you it recomputed**, not that the
   bytes are identical forever.

Point 3 is a deliberate limit on the decade promise, and it should be stated plainly rather than
discovered: **the API is forever; the geometry is reproducible only within a kernel generation.**
A plugin from 2026 still loads and runs in 2036. The shape it produces may differ if the kernel
underneath it improved, and the document will say so.

## The support horizon

**Every ABI major is served forever, and we expect to ship very few.** A major is only justified by
a change that cannot be expressed additively, and the size-prefix rule makes almost everything
additive — so the honest expectation is that ABI 1 lasts indefinitely and ABI 2 may never exist.

That is the point. A major bump is a promise to maintain two vtable shims from then on, and the
cost is permanent. Treating it as expensive is what keeps it rare.

## The museum, concretely

For each ABI generation, checked into the repository and loaded by CI on every run:

- a **compiled** plugin binary per supported platform triple (macOS arm64, Windows x64, Linux x64),
  built once against that generation's header and never rebuilt;
- the source it was built from, for reference only — CI must not rebuild it, because rebuilding
  tests source compatibility, which is not the promise;
- a manifest recording the header hash, compiler and date it was produced.

**A new architecture is a new column, not a reset.** When a platform arrives that cannot run the
old binaries at all, that generation's row is marked "not verifiable on this platform" and stays —
an untestable promise must be visible as untestable rather than quietly dropped.

---

## Consequences

**This blocks nothing today and everything later.** No plugin exists, so the descriptor structs and
version negotiation can be fixed at zero cost right now. After the first third-party plugin ships,
`const void*` is permanent.

**It is a real ongoing tax.** The museum grows by one binary per generation and must be built for
three platforms. Old shims accumulate and can never be deleted — that is what the promise means, and
it is why Windows still carries code for applications whose vendors no longer exist.

**It constrains the core less than it looks.** Handles and a C surface mean the kernel, the
document and the renderer can all be rewritten underneath without touching the boundary. The
compatibility burden lands almost entirely on `abi/`, which is where it should be.

**It should be paid before the loader is built, not after.** The loader is the first client, and a
client is what reveals whether the interface is right. Building the loader against the descriptors
as they stand would bake in `const void*` on day one.

---

## What this does not decide

- Sandboxing and permissions (`CAD_CAP_*` exists in the header, unenforced). A WASM tier is
  mentioned in the header's comments and needs its own ADR.
- Which extension points exist beyond feature/command/format.
- Whether plugins may be sold, and under what licence terms — settled separately in `COPYRIGHT.md`
  (LGPL-2.1-or-later exists precisely so they can be closed-source).
- Module ownership for crash attribution — ADR 0010.
