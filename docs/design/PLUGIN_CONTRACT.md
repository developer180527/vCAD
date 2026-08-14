# The plugin contract

What a vCAD plugin is, what the host promises it, and what it must promise back.

Companion to [ADR 0011](../decisions/0011-plugin-abi-compatibility.md), which covers binary
compatibility. This document covers *behaviour*: the rules that keep a third-party plugin from
corrupting a document, poisoning the cache, or making a crash look like the core's fault.

Nothing here is implemented yet. That is deliberate — the contract is being written before the
loader, because the loader is the first client and a client built against an unfinished contract
freezes its mistakes.

---

## 1. What a plugin is

A shared library exporting exactly one symbol, `cad_plugin_main`, returning a `CadPluginDesc`.

### 1.1 vCAD is a platform for mechanical engineers

That sentence has a design consequence: **the extension points must cover the actual work of the
discipline, not just "add a shape".** A plugin author building sheet-metal, piping, mould design,
GD&T or a fastener library must be able to build it *within the contract* — if the contract cannot
express their domain, they will either abandon the platform or reach around it, and reaching around
it is what makes a plugin ecosystem unmaintainable.

So the extension catalogue below is deliberately larger than what the loader will support first.
Everything in it is a commitment about SHAPE, not schedule: each entry is a place we have decided
extension belongs, so that when it is built it is built the same way — a typed, size-prefixed
descriptor registered at `initialize`.

**Phase 1 — the minimum that makes a plugin useful.**

| Extension | What it adds | Descriptor |
|---|---|---|
| **Feature** | A parametric operation in the model tree — Flange, Rib, Lattice | `CadFeatureDesc` |
| **Feature parameters** | The typed inputs the property panel edits, with units and validation | `CadParamDesc` |
| **Command** | A user action: ribbon button, shortcut, marking-menu wedge | `CadCommandDesc` |
| **Format** | An importer and/or exporter | `CadFormatDesc` |

**Phase 2 — what makes it a mechanical platform rather than a modeller.**

| Extension | Why an engineer needs it |
|---|---|
| **Material and physical properties** | Density, then mass, centre of gravity, moments of inertia. A mechanical part without mass properties is a picture |
| **Measurement / inspection** | Custom measurements and derived quantities in the Inspect tab |
| **Document metadata (PMI/BOM)** | Part number, revision, finish, supplier. This is what makes a model a manufacturing artefact |
| **Design rules / validation** | DFM checks, house standards, "no wall thinner than 1.2 mm". Runs over a document and reports findings against named geometry |
| **Standard-part libraries** | Fasteners, extrusions, bearings. Mostly content plus a small generator, and every mechanical platform lives or dies on this |
| **Custom units and quantities** | Torque, pressure, surface finish |

**Phase 3 — gated on core features that do not exist yet.**

| Extension | Blocked on |
|---|---|
| **Assembly mates** | Assemblies, and a 3D constraint solver |
| **Drawing views, annotations, GD&T symbols** | Drawings and `HLRBRep` |
| **Simulation** | Meshing and a solver interface; see the scale review |
| **CAM / toolpaths** | Nothing in core, but a large domain of its own |

**What is deliberately never an extension point** is listed in §6. The catalogue grows by argument,
not by accretion: every entry is a surface we must keep working for a decade.

---

## 2. Lifecycle

```
discover → load → cad_plugin_main() → version check → initialize(host) → register*() → running
                                                                                          ↓
                                                                    shutdown() → unload
```

**Discovery.** Plugins are found in a known directory, each with a **manifest** — id, display name,
semver, `abi_major`, `min_host_minor`, and requested capabilities — as a plain data file beside the
binary.

The manifest exists for one reason, and it is not dependency management: **`dlopen` runs static
initialisers, which is code execution.** Without a manifest the host must execute a plugin to learn
whether it should execute it. With one, the host can show the user what a plugin claims and what it
wants *before* anything of it runs, and can refuse a version mismatch without ever loading it.

A manifest must NOT list library dependencies for conflict-checking. §4.7 makes conflicts
impossible by construction, so a dependency list would have no consumer, could not be verified
against the binary, and would be a permanent surface that lies as soon as someone edits it.

Never loaded from a document — a `.vpart` that could name a library to load would make opening a
file arbitrary code execution.

**Version check.** Before `initialize`, and it runs in both directions.

*Backward* (old plugin, new host): the host serves the plugin's `abi_major` generation. A 1.9
plugin on a 1.40 host gets 1.9 semantics, and on a 2.x host gets them through the v1 shim.

*Forward* (new plugin, old host): **refused, with a legible message naming the version required.**
`CadPluginDesc.min_host_minor` states the oldest host the plugin will run on; a host older than
that declines to load it and says so. The alternative — loading it anyway — means the plugin calls
a function pointer the old host never populated, and a null-pointer crash inside third-party code
is the single hardest failure to attribute.

SolidWorks is backward but not forward compatible and does not carry this field, which is why its
add-in authors write runtime version branches. We state it in the descriptor so the *host* can
refuse cleanly instead of the *author* having to defend against it.

**`initialize(host)`** is the only place registration may happen. Registering later is refused: the
ribbon, the feature registry and the format list are built once, and a feature type appearing
mid-session would mean a document could contain features the registry did not have at load.

**`shutdown`** must release everything. The host then unloads the library — but **not** while any
of its features exist in an open document. A plugin whose types are in use stays resident until
those documents close; the alternative is a function pointer into unmapped memory on the next
recompute.

**Plugins are therefore not hot-reloadable, and this is a decision rather than an omission.**
Installing or updating one takes effect at next launch. Reloading a library whose function pointers
are referenced by a live document means unmapping code the recompute is about to call, and the
window in which that is safe is impossible to prove. The cost is a restart; the alternative is a
class of crash nobody can reproduce.

---

## 3. What the host promises

1. **Handles stay valid** for their documented lifetime, and a stale handle returns
   `CAD_ERR_BAD_HANDLE` rather than crashing. Handles are indices into a host registry, never
   pointers, so the host can validate every one. **(RESOLVED for shapes)** — `cad_plugin_host`
   returns a live vtable, and `tests-rs/cad-tests/tests/plugin_host.rs` covers use-after-release,
   double release, release of 0, cross-session handles, and the rule that ids are never reused.
2. **Nothing throws.** Every call returns `CadStatus`. A C++ exception escaping into plugin code
   is a host bug.
3. **Existing behaviour never changes.** Additive-only, per ADR 0011. A call that worked in 1.9
   works identically in 1.40 and in 2.x through the compatibility shim.
4. **The naming system is available. (PARTIAL)** `element_resolve` and `element_name_of` are wired
   and tested for their negatives — an unknown name is refused rather than approximated, and a
   released shape cannot resolve. **But there is no sub-shape enumeration**, so a plugin has no way
   to obtain a face handle to name in the first place. Found by writing the tests, not by reviewing
   the design. Until it lands, `element_*` are usable only for a name a plugin already holds, and
   `shape_faces`/`shape_edges` are the next thing the vtable needs.

   `element_resolve` and `element_name_of` are in the vtable
   because a plugin must be able to hold a reference to a face across a rebuild — the same
   guarantee built-in features get. A plugin restricted to indices would break on every edit, which
   is exactly the FreeCAD failure mode vCAD exists to avoid.
5. **Failures are attributed.** A crash inside plugin code is reported as that plugin's, by id, not
   as a vCAD crash (ADR 0010's module ownership table). Note precisely what this is: attribution is
   **post-mortem**, in the crash report. It is not recovery. §5 is explicit that an in-process
   native plugin crash is not survivable, and this promise must not be read as implying it is.
6. **No toolchain is imposed.** C99 with a documented layout is the whole requirement. vCAD will
   never require a specific compiler, C++ standard library, language runtime, allocator, or
   serialisation framework — so no third party's deprecation can break the boundary. Inventor
   inherited a breaking change from Microsoft removing `BinaryFormatter` in .NET 9; there is no
   equivalent lever on us, and that is a deliberate property, not luck.

## 4. What the plugin promises

### 4.1 Compute must be deterministic — the strictest rule here

**Identical inputs must produce identical output, on every machine, in every build, forever.**

This is not a style preference. vCAD's recompute is content-addressed: a result is cached under a
key derived from the inputs and `compute_version`. If a feature's compute is non-deterministic, the
cache returns a result that does not match what recomputing would produce, and the document
silently disagrees with itself — across undo, across save/reload, and across machines sharing a DDC
cache.

Concretely, a compute must not depend on: wall-clock time, random numbers, uninitialised memory,
hash-map iteration order, floating-point mode changes, thread scheduling, the filesystem, or the
network.

**And `compute_version` must be bumped whenever output changes for identical inputs.** Forgetting
it leaves stale cached shapes that survive a rebuild — the single most commonly forgotten field in
any content pipeline.

### 4.2 Emit names for what you create

A feature producing geometry must name its elements through the naming API. Faces and edges that
arrive unnamed cannot be referenced by downstream features, so a fillet on a plugin-made face
breaks on the next edit. A plugin that skips this produces geometry that looks right and cannot be
built on.

### 4.3 Respect thread affinity

Until stated otherwise: **compute may be called on a worker thread; commands are called on the UI
thread; registration happens on the loading thread.** A plugin must not assume they are the same
thread, must not touch UI from compute, and must not block the UI thread in a command.

### 4.4 Do not exceed declared capabilities

`required_caps` is a declaration the user can see and the host can refuse. A plugin that declares no
`CAD_CAP_NETWORK` and opens a socket is in breach, and once sandboxing exists it will simply fail.

**Until sandboxing exists, capabilities are advisory and must be described that way to users.** A
native plugin runs with the application's full privileges regardless of what it declared. Presenting
an unenforced declaration as a permission grant would be worse than showing nothing, because it
invites a trust decision the software cannot honour.

### 4.5 Behave at the boundary

- Set `struct_size` on every descriptor.
- Check for NULL vtable entries — a sandboxed tier offers fewer.
- Treat unknown `CadStatus` values as generic failures; new codes will appear.
- Copy strings immediately; host strings are valid only until the next call on that session.
- Never store a handle past its documented lifetime.

### 4.6 Do not re-enter the host

A plugin must not call back into the host from inside a host callback in ways that mutate state it
is already inside. Specifically: **`compute` must not mutate the document** (§6 already forbids it,
and §4.1 depends on it), and a command must not invoke another command reentrantly.

Added after finding that Inventor can be crashed by sending an API command while it is busy. A
boundary that permits reentrancy has to be reentrant everywhere, forever — cheaper to forbid it.

### 4.7 Bring your own dependencies, privately

A plugin must **statically link its dependencies**, or ship them privately, and must export exactly
one symbol: `cad_plugin_main`. Everything else must be hidden
(`-fvisibility=hidden`, no `__declspec(dllexport)`).

The host loads plugins with `RTLD_LOCAL` on POSIX so a plugin's symbols never enter the global
namespace, and two plugins linking different versions of the same library therefore cannot collide.

This is the failure that has damaged every large plugin ecosystem — two add-ins, one process,
incompatible versions of a shared library. Revit did not gain native dependency isolation until
2025. It is not an API-design problem, which is exactly why it is easy to leave out of an API
design, and it is why the C boundary matters: **vCAD never asks a plugin to link against our C++
dependencies, so there is nothing of ours for a plugin to conflict with.**

---

## 4A. Persistence: a document must outlive the plugin that made it

**A `.vpart` containing a plugin's feature must open on a machine where that plugin is not
installed, and must not lose the plugin's data.**

This is a platform-defining requirement and it is not currently met — the recompute rejects an
unknown feature type outright with "a plugin may be missing".

The rules:

1. **Parameters are stored by the host**, in the document, as ordinary typed properties. A plugin
   does not serialise its own data and does not choose a format. This is what allows the host to
   preserve data for a feature type it has never heard of.
2. **An unknown feature type is PRESERVED, not dropped.** It loads as an opaque node carrying its
   type name, its parameters and its last computed shape. It shows in the tree, greyed, saying
   which plugin id provides it. Save the file and every byte comes back.
3. **It is not silently recomputed.** Its cached shape is displayed and marked stale-but-shown;
   downstream features go Blocked with a reason naming the missing plugin, exactly like any other
   blocked feature.
4. **Reinstalling the plugin restores full behaviour**, because nothing was lost.

The failure this prevents is the one that ends platforms: opening a colleague's file, seeing
"unknown feature, removed", saving, and destroying their work.

---

## 4B. External inputs, and why they are a cache problem — **(RESOLVED for built-ins)**

A feature that reads anything outside the document — a file on disk, a network resource, an
environment variable — **breaks the content-addressed cache**, because the cache key is computed
from the document and the external thing is not in it.

This is not hypothetical and it is not a plugin-only concern. The built-in `Import` feature had
exactly this bug: its key covered the `path` STRING, so editing the referenced STEP file on disk
and recomputing served the shape cached from the old contents. **Fixed** — `FeatureType` now carries
an `externalInputs` hook and `Engine::cacheKeyOf` folds each declared file's content digest into
the key (`tests/acceptance/m2_external_input_cache.cpp`).

The C-side `declare_external_input` below is the same mechanism exposed to plugins, deliberately:
built-ins and plugins are held to one rule, because the cache cannot tell them apart.

**A second half of that bug is still open**, and plugins inherit it: nothing *notices* the file
changed. Recompute skips objects that are Clean with an output, so the corrected key is only
consulted when something else marks the feature dirty. Watching external inputs for change is
unbuilt, for built-ins and plugins alike.

Therefore any feature, built-in or plugin, that depends on external state must **declare that
dependency as a content digest** which the host folds into the cache key:

```c
/* On CadFeatureDesc. Called at CACHE-KEY time, before compute. */
CadStatus (*external_inputs)(void* plugin_ctx, CadFeatureCtx fc,
                             CadPathSink sink, void* sink_ctx);
```

**(RESOLVED — and corrected.)** An earlier draft of this section had the plugin declare its
external inputs from *inside* `compute`, via `declare_external_input`. **That cannot work.** The
cache key is computed before compute runs, so a declaration made during compute arrives too late to
affect the key it exists to change — the plugin would believe it had declared its dependency while
the cache served stale geometry anyway. Caught before anything was built on it.

The correct shape is a callback on the descriptor, called at key time, mirroring
`recompute::FeatureType::externalInputs` on the C++ side.

A plugin that reads an external resource without declaring it is in breach of §4.1, and its
results will be wrong in ways that look like the host's fault.

---

## 5. Failure isolation

**A misbehaving plugin must not take the application down, and must not corrupt a document.**

- A compute returning an error marks that feature Failed with the plugin's message; dependents go
  Blocked with a reason. Exactly the existing path for a failed built-in.
- A compute that crashes is, in-process, not survivable in general. Isolation is therefore a
  *roadmap* item — the out-of-process or WASM tier the ABI header already anticipates — and until
  it exists, native plugins are trusted code and the user must be told so at install time.
- **Document mutation is transactional.** A plugin's changes go through `txn_begin`/`txn_commit`,
  and a plugin that dies mid-transaction leaves the document at its last committed state. This is
  the one isolation guarantee that holds even for in-process native code, and it is why the
  transaction API is in the vtable rather than a convenience.

---

## 6. What is deliberately not offered

| Not offered | Why |
|---|---|
| Direct viewport drawing | Ties plugins to bgfx and to the renderer's internals; both must stay replaceable |
| Access to OCCT types | The C++ ABI is not stable; this is the whole reason for the C boundary |
| Replacing built-in features | Makes every document's meaning depend on load order |
| Reading another plugin's state | Turns two plugins into one distributed system with no contract between them |
| Loading from a document | Opening a file would become code execution |
| Mutating the document from `compute` | Compute is a pure function of its inputs; §4.1 depends on it |

---

## 6A. What the incumbents got wrong

Researched rather than assumed. Each item is a failure mode with a real cost, and each one either
maps to a rule above or exposed a gap we had missed.

### SolidWorks — the compatibility burden is on the plugin author

SOLIDWORKS is backward compatible but explicitly **not forward compatible**: a new API cannot be
used on an older release. So an add-in targeting several versions must call `GetVersion` at runtime
and branch, or use an older method that may be missing critical bug fixes. Whole frameworks
(`xCAD`) exist to manage multi-version targeting with fallback chains, and macro-feature parameters
need hand-written schema migration.

**The lesson: they made compatibility the plugin author's job.** Every author pays it, forever, and
most pay it badly.

*Our answer:* the host serves the plugin's generation (ADR 0011), and size-prefixed descriptors mean
an old plugin's smaller struct is simply understood. Version branching should never appear in
plugin code. **If a plugin author ever has to write `if (host_version >= X)`, we have failed** — the
only legitimate use is opting INTO something new, never keeping something old working.

### FreeCAD — no stable naming, and unmaintainable forks

The topological naming problem: a shape changes its internal name after an operation, so dependent
features break or compute wrongly. The fix lived for years in RealThunder's LinkStage fork, which
"went out of sync with upstream" and "wasn't written in a way that was maintainable by anybody other
than RealThunder"; upstreaming took a task force more than a year across three phases. Alongside
that, the Python 3 and Qt 5 migrations broke addons wholesale.

**Two lessons.** First, unstable naming is not a modelling inconvenience — it is a *plugin API*
problem, because every plugin holding a face reference is broken by it. Second, a platform-critical
fix living in one person's fork is a governance failure that costs years.

*Our answer:* topological naming is in the core, property-tested, and exposed to plugins through
`element_resolve`/`element_name_of` (§3.4). This is the single biggest thing vCAD does differently.

### Inventor — in-process COM, and runtime coupling

Add-ins are in-process COM implementing `ApplicationAddInServer`. Sending an API command while
Inventor is busy can crash it — a reentrancy hazard. And compatibility is coupled to the .NET
runtime: Inventor 2027 keeps binary compatibility for .NET 8 add-ins, but Microsoft's removal of
`BinaryFormatter` in .NET 9 is a breaking change arriving from outside Autodesk entirely.

**The lesson: binding your plugin ABI to a managed runtime imports someone else's breaking
changes.**

*Our answer:* C99 only, no runtime, no reflection, no serialisation framework. Nothing in the
boundary can be deprecated by a third party. The reentrancy hazard was a genuine gap — now §4.6.

### Everyone — dependency conflicts between plugins

Two add-ins needing different versions of the same library in one process. Revit only gained native
dependency isolation in 2025; before that the ecosystem relied on AppDomains, `AssemblyLoadContext`
or IL-merging every dependency.

**This is the classic plugin-ecosystem killer and it has nothing to do with API design.** It was
missing from our plan entirely — now §4.7.

---

## 7. Settled designs for the phase-1 blockers

Written here rather than in the header because the plan is being fixed before any code. These are
decisions, not sketches; the header change is mechanical once agreed.

### 7.1 `CadParamDesc` — typed feature parameters — **(RESOLVED)**

A feature's parameters must be host-owned (§4A: that is what lets a document survive a missing
plugin), typed (the property panel has to render and validate them), and unit-aware (this is a
mechanical CAD tool; a length is not a number).

```c
typedef enum {
    CAD_PARAM_LENGTH = 0,   /* stored in millimetres, shown in the user's units */
    CAD_PARAM_ANGLE,        /* stored in degrees */
    CAD_PARAM_REAL,         /* dimensionless */
    CAD_PARAM_INTEGER,
    CAD_PARAM_BOOL,
    CAD_PARAM_TEXT,
    CAD_PARAM_OBJECT,       /* a reference to another feature */
    CAD_PARAM_ELEMENT,      /* a named face/edge/vertex — survives rebuilds, see 4.2 */

    /* Lists. A fillet takes MANY edges, a loft takes many profiles, a pattern takes many seeds:
     * single-valued parameters cannot express the majority of real mechanical features. The core
     * already stores std::vector<ObjectId> properties and Engine::cacheKeyOf already hashes them,
     * so this is exposing something that exists rather than inventing one.
     *
     * Omitting these was the most expensive mistake this design nearly made: adding a kind later
     * is additive and legal, but every plugin written in the meantime would have worked around
     * their absence by packing lists into TEXT, and those workarounds would then be permanent. */
    CAD_PARAM_OBJECT_LIST,
    CAD_PARAM_ELEMENT_LIST
} CadParamKind;

typedef struct {
    uint32_t     struct_size;
    uint32_t     struct_version;

    const char*  name;          /* stable key, stored in the document; permanent once shipped */
    const char*  label;         /* shown to the user; may be changed freely */
    const char*  tooltip;
    CadParamKind kind;

    /* Defaults and bounds. Ignored for kinds where they make no sense. A plugin that wants no
     * bound sets min > max, which is unambiguous and needs no extra flag. */
    double       default_value;
    double       min_value;
    double       max_value;

    uint32_t     flags;         /* CAD_PARAM_* below */
} CadParamDesc;

#define CAD_PARAM_REQUIRED    (1u << 0)
#define CAD_PARAM_COSMETIC    (1u << 1)  /* excluded from the cache key, like colour — see 4.1 */
#define CAD_PARAM_READ_ONLY   (1u << 2)  /* computed output, shown but not editable */
```

Two decisions inside that are worth stating out loud:

- **`name` is permanent.** It is written into every saved document that uses the feature. Renaming
  it orphans data, so it is treated exactly like `type_name`. `label` exists precisely so the
  user-visible string can change without touching the stored key.
- **`CAD_PARAM_COSMETIC` is part of the ABI**, not a host heuristic. The cache excludes cosmetic
  properties from the key (`Engine::cacheKeyOf` already does this for colour and labels), and only
  the plugin knows which of its parameters do not affect geometry. Getting this wrong is a
  correctness bug in both directions: mark a geometric parameter cosmetic and edits are ignored;
  mark a cosmetic one geometric and every colour change rebuilds the part.

### 7.1a Evolving a feature's parameters — **(RESOLVED — declared; migration not yet run by anything)**

`CadFeatureDesc` carries a `param_schema_version`, and a plugin that changes its parameter set
provides a migration:

```c
/* Called by the host when a document contains this feature saved at an OLDER schema version.
 * Runs before compute, once per object, and its result is written back to the document. */
CadStatus (*migrate_params)(void* plugin_ctx, CadMigrationCtx mc,
                            uint32_t from_version, uint32_t to_version);
```

Without this, a plugin can never rename a parameter, split one into two, or change a unit, because
every document ever saved holds the old shape. SolidWorks made add-in authors hand-roll exactly
this for macro-feature parameters; making it part of the contract means one implementation the host
can test rather than one per plugin.

Three rules make it safe:

- **Migration is forward-only.** There is no downgrade. A document touched by a newer plugin is not
  guaranteed to open under an older one, and the host says so rather than corrupting it.
- **A missing migration is not data loss.** If a plugin declares a newer schema and provides no
  `migrate_params`, the old parameters are *preserved untouched* and the feature is marked as
  needing attention — §4A's rule applies to a plugin's own old data, not only to a missing plugin.
- **`param_schema_version` is not `compute_version`.** The first says the stored *shape* changed;
  the second says the *output* changed for identical inputs. Conflating them either re-migrates
  documents that are already correct or serves stale cached geometry.

Parameters are registered as an array alongside the feature, so a feature and its parameters arrive
together and cannot disagree:

```c
CadStatus (*register_feature)(void* ctx, const CadFeatureDesc* desc,
                              const CadParamDesc* params, uint32_t param_count);
```

### 7.2 Compute context accessors

`CadComputeCtx` is opaque; these are how a plugin reads its inputs and returns its result. They go
on `CadHost`, not on the descriptor, so they can be extended without touching any struct layout.

```c
/* inputs: the already-computed outputs of the features this one depends on */
CadStatus (*compute_input_count)(void* ctx, CadComputeCtx cc, uint32_t* out);
CadStatus (*compute_input_shape)(void* ctx, CadComputeCtx cc, uint32_t index, CadShape* out);

/* parameters, by the name declared in CadParamDesc */
CadStatus (*compute_param_real)(void* ctx, CadComputeCtx cc, const char* name, double* out);
CadStatus (*compute_param_int)(void* ctx, CadComputeCtx cc, const char* name, int64_t* out);
CadStatus (*compute_param_text)(void* ctx, CadComputeCtx cc, const char* name, CadStr* out);
CadStatus (*compute_param_element)(void* ctx, CadComputeCtx cc, const char* name,
                                   CadElementId* out);

/* List-valued parameters. Count first, then index — rather than handing back a pointer and a
 * length, which would require the host to own an array whose lifetime the plugin cannot see. */
CadStatus (*compute_param_count)(void* ctx, CadComputeCtx cc, const char* name, uint32_t* out);
CadStatus (*compute_param_element_at)(void* ctx, CadComputeCtx cc, const char* name,
                                      uint32_t index, CadElementId* out);
CadStatus (*compute_param_shape_at)(void* ctx, CadComputeCtx cc, const char* name,
                                    uint32_t index, CadShape* out);

/* the result. Exactly one call per successful compute; a second is CAD_ERR_INVALID_INPUT */
CadStatus (*compute_set_output)(void* ctx, CadComputeCtx cc, CadShape shape);

/* The failure MESSAGE. A CadStatus is a code, and a code cannot say "the flange radius is larger
 * than the material allows". Without this every plugin failure would surface as a generic string,
 * which fails the bar the core already meets: the recompute names the feature responsible and says
 * what went wrong, and blocked dependents quote it. A plugin's failures must be as legible as a
 * built-in's or the tree becomes a wall of "operation failed".
 *
 * `detail` is for developers and goes to the log, never to the user — the same split as
 * kernel::Error's message/detail. */
CadStatus (*compute_fail)(void* ctx, CadComputeCtx cc, const char* message, const char* detail);

/* The feature's parameters, for accessors shared with external_inputs (§4B). External
 * dependencies are NOT declared from here -- they are declared on the descriptor, at key
 * time, because by the time compute runs the key already exists. */
CadStatus (*compute_feature_ctx)(void* ctx, CadComputeCtx cc, CadFeatureCtx* out);
```

**Read-only by construction.** There is no `compute_set_param`, no document handle, and no
transaction call reachable from a `CadComputeCtx`. §4.1's determinism requirement is thereby
enforced by the shape of the API rather than by asking politely — a compute *cannot* mutate the
document, because nothing here lets it.

---

## 7.3 How plugins reach the user interface

### The constraint that decides everything

vCAD has **two shells**: Qt on the desktop, SwiftUI on iPad. A plugin that draws its own widgets
works on exactly one of them.

So: **plugins never draw. They declare, and each shell renders natively.** This is not a
restriction invented for plugins — it is the rule `app/Controller` already follows, which is why
`Controller.h` has no Qt in it and why `PropertyRow` and `CommandParameter` are text-in, text-out.
Plugins are held to the architecture the core already has.

It also, for free, avoids the worst toolkit trap: **a plugin that linked Qt would inherit every Qt
ABI break, every version mismatch, and the dependency conflicts of §4.7.** Inventor add-ins are
coupled to the .NET runtime and inherited Microsoft's `BinaryFormatter` removal. A plugin that
links our toolkit is the same mistake with a different vendor.

### The primary UI is the parameter list, not a dialog

For a feature, `CadParamDesc` (§7.1) *is* the user interface. The plugin declares typed, unit-aware
parameters with labels, defaults and bounds; the desktop shell renders them into the command
property panel, and the iPad shell renders the same declaration as a touch form. Neither the plugin
nor its author is involved in layout.

This is deliberately unglamorous, and it is how a plugin gets a native-feeling UI on two platforms
without writing either.

### Commands declare intent, not position

A `CadCommandDesc` says what it is, not where it goes:

```c
/* Appended to CadCommandDesc; see the size-prefix rule. */
const char* category;      /* "Create", "Modify", "Inspect" — an EXISTING ribbon panel */
uint32_t    placement;     /* CAD_UI_* hints below */
```

```c
#define CAD_UI_RIBBON        (1u << 0)   /* the panel named by `category` */
#define CAD_UI_CONTEXT_MENU  (1u << 1)   /* right-click on a matching selection */
#define CAD_UI_MARKING_MENU  (1u << 2)   /* eligible for a wedge, if the user assigns one */
```

The host owns layout. A plugin asks to appear in *Modify*; it does not ask for x=340.

### The traps, named

**Tab explosion.** The failure that disfigures every mature CAD application: each add-in claims a
top-level ribbon tab, and a user with fifteen add-ins has a ribbon they cannot read. Revit had to
restrict add-ins to a single tab after the fact; FreeCAD's equivalent is workbench proliferation,
where every addon becomes its own mode with its own duplicated commands.

**Our rule: a plugin cannot create a top-level tab.** It contributes commands to *existing*
categories. If a plugin genuinely needs its own tab — a full sheet-metal suite might — that is a
user decision made in settings, not a plugin decision made at registration. Defaulting to "no" and
letting the user promote is recoverable; defaulting to "yes" and asking fifteen vendors to be
considerate is not.

**Modal dialogs.** Not offered. A plugin cannot block the application on its own UI, cannot open a
window, and cannot own an event loop. Anything needing more than a parameter list is a *task panel*
declaration, and that is phase 2 with its own descriptor — not an escape hatch into raw widgets.

**Unremovable UI.** Every plugin-contributed command is listable, hideable and rebindable by the
user, in the same settings surface as built-in commands, without uninstalling the plugin. A user
who cannot turn a button off eventually turns the plugin off.

**Threading.** Commands run on the UI thread (§4.3) and must not block it. Long work belongs in a
compute, which the host may run on a worker. Inventor's documented crash-when-busy is what happens
when this boundary is soft.

**Icons.** Declared by name and resolved by the shell's theme, so a plugin's button matches the
application it is in — including a dark theme it has never seen. A plugin shipping raster icons
would look wrong on every future theme and every display scale.

### What this costs

A plugin author cannot build an arbitrary interface. That is the trade, and it is the right one: it
buys a plugin that works on desktop and iPad, survives a toolkit migration, cannot break the
ribbon, and cannot be styled inconsistently with the host. The alternative — raw widget access —
buys one clever plugin and a decade of UI that cannot be changed because plugins depend on its
internals.

---

## 8. Implementation order

Fixed, so it does not get relitigated per step. Each lands independently and is testable on its own.

| # | Step | Why here |
|---|---|---|
| 1 | ~~**Golden header snapshot test**~~ **(RESOLVED)** | `tests-rs/cad-tests/tests/abi_golden.rs`; 149 declarations pinned, verified red and green |
| 2 | ~~`CadParamDesc` + `min_host_minor` in the header~~ **(RESOLVED)** | ABI 1.10. Layout pinned by static_assert, acceptance rule tested via `cad_abi_accepts` |
| 3a | ~~Shape handles and a live host vtable~~ **(RESOLVED)** | `cad_plugin_host`; 9 tests on the §3 promises |
| 3b | Compute context accessors | Needs registration; the other phase-1 blocker |
| 4 | **Unknown-feature preservation** (§4A) | Failure must not nuke the user's *data*. Independent of the loader and valuable without it |
| 5 | Error containment (§5) | Failure must not nuke the *app* |
| 6 | **The loader**, plus the compatibility museum and hostile-plugin test | Built last, against a finished contract |

Compatibility is verified by **two** CI tests, not one, because they check opposite directions and
only one of them is about compatibility at all:

- **Backward — the museum.** A *compiled* plugin binary per ABI generation, loaded forever. Not
  rebuilt from source: recompiling tests source compatibility, which is not the promise.
- **Forward — clean refusal.** A plugin declaring a `min_host_minor` newer than the host must be
  declined with a legible message naming the required version. This is not compatibility, it is
  *failing correctly*, and it needs its own test because the failure mode it prevents — loading
  anyway and calling a null function pointer inside third-party code — is the hardest possible
  crash to attribute.

Steps 1–3 are the contract. Steps 4–5 are the reliability guarantees. Step 6 is the first client,
and it is deliberately last: the loader is what freezes the design, so it must not be written while
the design is still moving.

---

## 8A. Considered and rejected

Kept because a rejected idea returns every year unless the reasoning is written down.

**`host->is_busy()`.** Proposed to let a plugin avoid the reentrancy crash Inventor exhibits.
Rejected: it is a time-of-check-to-time-of-use race by construction. Between a plugin reading
`is_busy()` and acting on the answer, the host's state can change, so the API *reads* as a safety
mechanism while providing none — and it would encourage exactly the pattern §4.6 forbids. The
prohibition is the protection; for `compute` it is structural, since §7.2's context exposes no way
to mutate anything.

A deferred `queue_operation()` is a more defensible idea, but it adds an asynchronous execution
model to a boundary we have committed to supporting for a decade, before a single plugin exists to
tell us what it should look like. It can be added later — additively, per ADR 0011 — if a real
plugin needs it. Adding it now would be guessing permanently.

---

## 9. Still open

1. **Sandboxing tier.** WASM is mentioned in the ABI header. Needs its own ADR, and it determines
   whether §5's crash isolation is ever real.
2. **Discovery and installation.** Directory layout, signing, per-user vs system.
3. **UI surface for commands.** Where a plugin's button appears in the ribbon, and who decides.

None of these block steps 1–5. Item 1 blocks *trusting* a third-party plugin, and until it exists
native plugins are trusted code — which the user must be told at install time, not discovered
after.
