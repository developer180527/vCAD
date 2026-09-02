/* cad_plugin_abi.h — the entire boundary between the core and everything outside it.
 *
 * C99. No C++ types cross this line. Ever.
 *
 * This one header serves two consumers, which is why it is defined in M1 alongside the core
 * API rather than later:
 *
 *   1. Desktop native plugins (tier 2). C++ across a shared-library boundary means MSVC
 *      debug/release CRT mismatches, libstdc++ vs libc++, and OCCT's handle refcounting
 *      crossing allocators. All of that is fatal and none of it is debuggable in the field.
 *
 *   2. The future iPad shell. Swift imports C cleanly; Swift/C++ interop still has sharp
 *      edges. A C facade costs one design instead of two.
 *
 * Conventions:
 *   - Errors are return codes. Nothing throws across the boundary.
 *   - Strings are (const char*, length), host-owned, valid until the next host call.
 *   - Shapes and document objects are opaque uint64 handles into a host-side registry.
 *     Never pointers: handles survive host-side reallocation and can be validated.
 *   - The host passes a vtable struct, it does not export symbols. That lets us version the
 *     surface, stub functions out for sandboxed (WASM) plugins, and load two ABI
 *     generations side by side.
 */

#ifndef CAD_PLUGIN_ABI_H
#define CAD_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility.
 *
 * ELF and Mach-O export everything by default; PE exports NOTHING unless asked. Without this
 * macro the Windows DLL is a black box and every consumer — the Rust suite, a plugin host,
 * the future Swift bridge — fails to link with unresolved externals. Define CAD_ABI_BUILD
 * when compiling the library itself, and nothing when consuming it.
 */
#if defined(_WIN32)
#  if defined(CAD_ABI_BUILD)
#    define CAD_API __declspec(dllexport)
#  else
#    define CAD_API __declspec(dllimport)
#  endif
#else
#  define CAD_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_ABI_VERSION_MAJOR 1
#define CAD_ABI_VERSION_MINOR 22

/* --- status ------------------------------------------------------------------------- */
typedef int32_t CadStatus;
#define CAD_OK                  0
#define CAD_ERR_INVALID_INPUT   1
#define CAD_ERR_NOT_DONE        2
#define CAD_ERR_INVALID_RESULT  3
#define CAD_ERR_BOOLEAN_FAILED  4
#define CAD_ERR_UNSUPPORTED     5
#define CAD_ERR_NAMING_LOST     6
#define CAD_ERR_KERNEL_EXC      7
#define CAD_ERR_CANCELLED       8
#define CAD_ERR_BAD_HANDLE      9
#define CAD_ERR_NO_PERMISSION  10

/* The call is legal but not legal NOW: it would re-enter the host from inside a host callback.
 *
 * Distinct from CAD_ERR_NO_PERMISSION, which is about what a plugin is allowed to do at all. This
 * one is about WHEN, and a plugin author seeing it has a specific fix -- move the call outside the
 * callback -- rather than a permissions question with no answer. See section 4.6 of
 * docs/design/PLUGIN_CONTRACT.md. */
#define CAD_ERR_REENTRANT      11
#define CAD_ERR_INTERNAL       99

/* --- handles ------------------------------------------------------------------------ */
typedef uint64_t CadShape;      /* 0 == null */
typedef uint64_t CadDocObject;
typedef uint64_t CadDocument;
typedef uint64_t CadTransaction;

/* A topological element identity. `digest` is the fast path; `text` is the round-trippable
 * form (see cad::naming::ElementName::toString). Both are host-owned. */
typedef struct {
    uint64_t    digest;
    const char* text;
    size_t      text_len;
} CadElementId;

typedef struct {
    const char* data;
    size_t      len;
} CadStr;

/* --- capabilities a plugin may request ---------------------------------------------- */
#define CAD_CAP_FILESYSTEM  (1u << 0)
#define CAD_CAP_NETWORK     (1u << 1)
#define CAD_CAP_SUBPROCESS  (1u << 2)
#define CAD_CAP_UI          (1u << 3)

/* --- extension descriptors -----------------------------------------------------------
 *
 * THE SIZE-PREFIX RULE. Every struct that crosses this boundary begins with:
 *
 *     uint32_t struct_size;      set to sizeof(the struct AS THE CALLER COMPILED IT)
 *     uint32_t struct_version;   bumped only when a field's MEANING changes
 *
 * The receiver reads struct_size and uses only the fields that fit inside it. That is the
 * entire mechanism by which a plugin compiled today keeps working against a host built in ten
 * years: the host appends fields, old plugins report a smaller size, and the host serves them
 * with the semantics they were built against.
 *
 * Consequently, and without exception:
 *   - Fields are only ever APPENDED. Never reordered, never removed, never retyped, never
 *     repurposed. A field that turns out to be wrong is left alone and a new one is added.
 *   - A caller MUST set struct_size. A zero or absurd value is rejected, not guessed at.
 *   - A receiver MUST NOT read past struct_size, even for a field it "knows" is there.
 *
 * These replaced `const void*` parameters, which is worth recording because the mistake is an
 * easy one to make again: register_feature/command/format each took an untyped, unsized blob.
 * Nothing was broken, because nothing had ever called them — but the first shipped plugin would
 * have frozen that shape permanently, and adding a single field to a feature descriptor would
 * then have meant the host reading past the end of every existing plugin's struct. See
 * docs/decisions/0011-plugin-abi-compatibility.md.
 */

/* Opaque per-invocation contexts. Handles rather than structs deliberately: what a compute or a
 * command needs to see will grow, and growing an opaque handle's accessors costs nothing while
 * growing a struct costs a version negotiation every time. */
typedef uint64_t CadComputeCtx;
typedef uint64_t CadCommandCtx;

/* --- parameters ----------------------------------------------------------------------
 *
 * A feature's parameters are HOST-OWNED. The plugin declares their shape; the host stores the
 * values in the document as ordinary typed properties, renders them in the property panel, and
 * preserves them even for a feature type it has never heard of (see PLUGIN_CONTRACT.md 4A). A
 * plugin that serialised its own data would take that guarantee away from the user.
 */
typedef enum {
    CAD_PARAM_LENGTH = 0,   /* stored in millimetres, shown in the user's units */
    CAD_PARAM_ANGLE = 1,    /* stored in degrees */
    CAD_PARAM_REAL = 2,     /* dimensionless */
    CAD_PARAM_INTEGER = 3,
    CAD_PARAM_BOOL = 4,
    CAD_PARAM_TEXT = 5,
    CAD_PARAM_OBJECT = 6,   /* a reference to another feature */
    CAD_PARAM_ELEMENT = 7,  /* a named face/edge/vertex; survives rebuilds */

    /* Lists. A fillet takes MANY edges, a loft many profiles, a pattern many seeds:
     * single-valued parameters cannot express most real mechanical features. The core already
     * stores vector<ObjectId> properties and already hashes them into the cache key, so this
     * exposes something that exists rather than inventing it.
     *
     * Omitting these was the most expensive mistake this design nearly made. Adding a kind later
     * is legal, but every plugin written meanwhile would have packed lists into TEXT, and those
     * workarounds would be permanent. */
    CAD_PARAM_OBJECT_LIST = 8,
    CAD_PARAM_ELEMENT_LIST = 9
} CadParamKind;

/* The numeric values above are STORED IN DOCUMENTS and are therefore permanent. They are written
 * explicitly rather than left implicit so that reordering the enum cannot silently change what a
 * saved file means. */

#define CAD_PARAM_REQUIRED   (1u << 0)
#define CAD_PARAM_COSMETIC   (1u << 1)  /* excluded from the cache key, like colour */
#define CAD_PARAM_READ_ONLY  (1u << 2)  /* computed output: shown, not editable */

typedef struct {
    uint32_t     struct_size;
    uint32_t     struct_version;

    /* Stable key, written into every document that uses this feature. PERMANENT: renaming it
     * orphans saved data. `label` exists so the user-visible string can change freely. */
    const char*  name;
    const char*  label;
    const char*  tooltip;

    uint32_t     kind;          /* CadParamKind */
    uint32_t     flags;         /* CAD_PARAM_* */

    /* Ignored for kinds where they make no sense. A plugin wanting no bound sets min > max,
     * which is unambiguous and needs no extra flag. */
    double       default_value;
    double       min_value;
    double       max_value;
} CadParamDesc;

/* Migration context, for evolving a feature's stored parameters between schema versions. */
typedef uint64_t CadMigrationCtx;

/* A feature's PARAMETERS, without its computed inputs.
 *
 * Separate from CadComputeCtx because it is available EARLIER: the cache key is built before any
 * input has been computed, and external_inputs below runs at key time. A compute context can hand
 * back one of these (compute_feature_ctx), so the parameter accessors are written once and serve
 * both. */
typedef uint64_t CadFeatureCtx;

/* Receives one external path. The host supplies it; the plugin calls it once per file it will
 * read. A callback rather than a returned array so neither side has to own or free a list. */
typedef void (*CadPathSink)(void* sink_ctx, const char* path, size_t path_len);

/* A feature type: the thing that makes a plugin a CAD plugin rather than a script. */
typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    /* Reverse-DNS and globally unique, e.g. "com.acme.sheetmetal.Flange". Stored in saved
     * documents, so it is permanent: renaming it orphans every file that used it. */
    const char* type_name;

    /* Bumped by the PLUGIN when its compute produces different output for identical inputs.
     * This is what invalidates cached results from an older build of the plugin. It is the
     * single most commonly forgotten field in a content pipeline, and getting it wrong shows
     * up as a stale shape that survives a rebuild — see recompute::FeatureType::version. */
    uint32_t    compute_version;
    uint32_t    reserved0;              /* explicit padding; keeps the layout the same on 32- and 64-bit */

    /* The shape of this feature's STORED PARAMETERS. Distinct from compute_version, and
     * conflating the two is a correctness bug in both directions: this one says the stored shape
     * changed and old documents need migrating; compute_version says the OUTPUT changed for
     * identical inputs and caches must be dropped. */
    uint32_t    param_schema_version;
    uint32_t    reserved1;

    void*       plugin_ctx;             /* passed back to compute; the plugin's own state */
    CadStatus (*compute)(void* plugin_ctx, CadComputeCtx ctx);

    /* Files this feature will READ that are not part of the document. May be NULL, and NULL is
     * the correct answer for every purely parametric feature.
     *
     * Called at CACHE-KEY time, before compute, and that timing is the entire point. An earlier
     * draft of this API had the plugin declare its external inputs from INSIDE compute -- which
     * cannot work, because the key it is meant to change has already been computed by then. The
     * result would have been a plugin that believed it had declared its dependency and a cache
     * that served stale geometry anyway.
     *
     * The host hashes each path's CONTENT into the key, so editing a referenced file invalidates
     * exactly the features that read it. Mirrors recompute::FeatureType::externalInputs, which is
     * how the built-in Import feature does the same thing -- built-ins and plugins are held to one
     * rule because the cache cannot tell them apart. */
    CadStatus (*external_inputs)(void* plugin_ctx, CadFeatureCtx fc,
                                 CadPathSink sink, void* sink_ctx);

    /* Called when a document holds this feature saved at an OLDER param_schema_version, before
     * compute, once per object; the result is written back to the document. May be NULL, and a
     * NULL migration is not data loss: the old parameters are preserved untouched and the
     * feature is marked as needing attention. Migration is forward-only — there is no downgrade,
     * and a document touched by a newer plugin may not open under an older one. */
    CadStatus (*migrate_params)(void* plugin_ctx, CadMigrationCtx mc,
                                uint32_t from_version, uint32_t to_version);
} CadFeatureDesc;

/* A user-invocable command: a ribbon button, a shortcut, a marking-menu wedge. */
typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    const char* id;                     /* "com.acme.sheetmetal.unfold", stable; shortcuts bind to it */
    const char* label;
    const char* tooltip;
    const char* icon_name;              /* resolved by the shell's theme; may be NULL */

    void*       plugin_ctx;
    /* Non-zero when the command should be enabled. Called often — on every selection change —
     * so it must be cheap and must not mutate the document. */
    int32_t   (*enabled)(void* plugin_ctx, CadCommandCtx ctx);
    CadStatus (*invoke)(void* plugin_ctx, CadCommandCtx ctx);

    /* WHERE the button goes. Fusion's vocabulary, because it is the one users have: a TAB is
     * "SOLID" or "SURFACE"; a SECTION is "CREATE" or "MODIFY" within a tab; a command is
     * "Extrude" within a section.
     *
     * Both name an id that must ALREADY be registered — a built-in one, or one this plugin
     * registered itself. A command naming an unknown tab or section is REFUSED rather than having
     * them created for it: implicit creation means a typo silently produces an empty tab called
     * "Creat", and the author sees a missing button with no error.
     *
     * NULL means the host's default for this plugin, which is a section of its own inside the
     * tab a plugin-provided command would otherwise have no home in. Never nothing: a command
     * that registers successfully and appears nowhere is worse than one that is refused. */
    const char* tab;
    const char* section;
    uint32_t    placement;              /* CAD_UI_* below */
    uint32_t    order;                  /* lower first; ties broken by registration order */
} CadCommandDesc;

/* Where a command may appear. A plugin asks; the host decides the geometry. */
#define CAD_UI_RIBBON        (1u << 0)   /* the section named above */
#define CAD_UI_CONTEXT_MENU  (1u << 1)   /* right-click, when the selection matches */
#define CAD_UI_MARKING_MENU  (1u << 2)   /* eligible for a wedge, if the user assigns one */

/* The BUILT-IN tab and section ids.
 *
 * Part of the ABI, and therefore permanent. A plugin that puts its button in CAD_SECTION_CREATE
 * must still land there in ten years, so these are defined here rather than invented in the host:
 * a string literal in the implementation is not a promise, and this is.
 *
 * The LABELS are not fixed — "3D Model" may be renamed or translated. Only the ids are. */
#define CAD_TAB_MODEL      "cad.tab.model"
#define CAD_TAB_SKETCH     "cad.tab.sketch"
#define CAD_TAB_INSPECT    "cad.tab.inspect"
#define CAD_TAB_ANNOTATE   "cad.tab.annotate"
#define CAD_TAB_MANAGE     "cad.tab.manage"
#define CAD_TAB_VIEW       "cad.tab.view"

#define CAD_SECTION_SKETCH     "cad.section.sketch"
#define CAD_SECTION_CREATE     "cad.section.create"
#define CAD_SECTION_PRIMITIVES "cad.section.primitives"
#define CAD_SECTION_MODIFY     "cad.section.modify"
#define CAD_SECTION_PATTERN    "cad.section.pattern"
#define CAD_SECTION_EDIT       "cad.section.edit"
#define CAD_SECTION_HISTORY    "cad.section.history"

/* --- settings ------------------------------------------------------------------------
 *
 * A plugin declares settings; it does not draw them. The shell renders whatever kinds it knows,
 * which is what lets one declaration become a desktop dialog, a tablet list and a line in a support
 * bundle — and what stops fifteen plugins each inventing their own idea of what a preferences page
 * looks like.
 *
 * The HOST stores the values, under one preferences file, keyed by the setting's id. A plugin that
 * persisted its own would have to choose a format and cope with being uninstalled — and worse, the
 * settings window could then not show a page for a plugin that is not loaded, so a user could not
 * re-enable a plugin's ribbon contribution without the plugin already being on. Same reasoning as
 * §4A: the host stores a plugin's data so the data outlives the plugin. */
typedef enum {
    CAD_SETTING_BOOL = 0,
    CAD_SETTING_INT = 1,
    CAD_SETTING_DOUBLE = 2,
    CAD_SETTING_TEXT = 3,
    CAD_SETTING_CHOICE = 4,   /* stored as the INDEX, so a label can be translated */
} CadSettingKind;

/* The numeric values above are STORED IN PREFERENCES and are therefore permanent, written
 * explicitly for the same reason CadParamKind's are. */

typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    /* Dotted, stable, and PERSISTED — "com.acme.sheetmetal.gauge". Never renamed: a rename silently
     * discards whatever the user had chosen, which is worse than imperfect wording. Prefix it with
     * the plugin id, because the id space is shared with the host and every other plugin. */
    const char* id;
    const char* label;

    /* One sentence, shown under the field. A setting whose effect is only discoverable by trying it
     * is one most users leave alone. */
    const char* description;

    uint32_t    kind;           /* CadSettingKind */
    uint32_t    reserved0;

    /* Bool uses 0 or 1; Choice uses the index; Text uses `default_text` and ignores this. */
    double      default_value;
    double      minimum;
    double      maximum;
    const char* default_text;

    const char* const* choices;
    uint32_t    choice_count;
    uint32_t    reserved1;
} CadSettingDesc;

typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    /* Naming an EXISTING page merges into it, unlike a ribbon tab, which is refused. Two plugins
     * adding a group to a shared page is the normal case rather than a conflict; two plugins
     * claiming one ribbon tab is not. */
    const char* id;
    const char* label;
    const char* icon_name;

    /* The heading this plugin's settings sit under within the page. Groups are visual only, so they
     * have no id and nothing is stored against them. */
    const char* group_label;
} CadSettingsPageDesc;

/* A ribbon TAB — "SOLID", "SHEET METAL". The largest thing a plugin may add, and the one to add
 * least often.
 *
 * A full domain suite (sheet metal, mould design) legitimately owns a tab. Fifteen plugins each
 * claiming one is how a mature CAD ribbon becomes unreadable — Revit had to retrofit a one-tab
 * limit per add-in after exactly that. vCAD's answer is not to forbid it but to make it the USER's
 * ribbon: every plugin-provided tab, section and command can be hidden in settings without
 * uninstalling the plugin. A user who cannot turn a tab off eventually turns the plugin off. */
typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    const char* id;                     /* "com.acme.sheetmetal.tab", stable; layouts persist it */
    const char* label;                  /* "SHEET METAL", shown to the user; may change freely */
    uint32_t    order;                  /* lower is further left; built-in tabs occupy 0..999 */
    uint32_t    reserved0;
} CadTabDesc;

/* A SECTION within a tab — "CREATE", "MODIFY". Either a new one, or nothing: to add buttons to an
 * EXISTING section a plugin names it on the command and does not register anything here. */
typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    const char* id;                     /* "com.acme.sheetmetal.flanges" */
    const char* label;                  /* "FLANGES" */
    const char* tab;                    /* an existing tab id, or one this plugin registered */
    uint32_t    order;
    uint32_t    reserved0;
} CadSectionDesc;

/* An import/export format. */
#define CAD_FORMAT_IMPORT  (1u << 0)
#define CAD_FORMAT_EXPORT  (1u << 1)

typedef struct {
    uint32_t    struct_size;
    uint32_t    struct_version;

    const char* id;                     /* "com.acme.formats.step-ap242" */
    const char* display_name;           /* shown in the file dialog */

    /* Lowercase, without the dot: {"stp", "step"}. Host-side matching is case-insensitive. */
    const char* const* extensions;
    uint32_t    extension_count;
    uint32_t    directions;             /* CAD_FORMAT_IMPORT | CAD_FORMAT_EXPORT */

    void*       plugin_ctx;
    /* NULL when the corresponding direction is not offered. The host checks `directions` AND
     * the pointer, because a descriptor that claims a direction it cannot perform is exactly
     * the sort of thing a third-party plugin does. */
    CadStatus (*import_file)(void* plugin_ctx, const char* path, CadDocument into);
    CadStatus (*export_file)(void* plugin_ctx, const char* path, CadDocument from);
} CadFormatDesc;

/* --- host interface ------------------------------------------------------------------
 * Function-pointer table handed to the plugin at init. A NULL entry means the host does
 * not offer that call in this configuration (e.g. a sandboxed tier); plugins MUST check.
 */
typedef struct CadHost CadHost;

struct CadHost {
    /* sizeof(CadHost) as the HOST compiled it. A plugin uses this to tell whether a trailing
     * function pointer exists at all, which abi_minor alone can only imply by convention — and
     * a convention is checkable by a careful human, which is the same as not checkable. */
    uint32_t struct_size;
    uint32_t struct_version;

    uint32_t abi_major;
    uint32_t abi_minor;
    void*    host_ctx;

    /* diagnostics */
    void (*log)(void* ctx, int32_t level, const char* msg);
    CadStr (*last_error)(void* ctx);

    /* geometry */
    CadStatus (*make_box)(void* ctx, double dx, double dy, double dz, CadShape* out);
    CadStatus (*boolean_fuse)(void* ctx, CadShape a, CadShape b, CadShape* out);
    CadStatus (*boolean_cut)(void* ctx, CadShape a, CadShape b, CadShape* out);
    CadStatus (*fillet_edges)(void* ctx, CadShape base, const CadElementId* edges,
                              size_t edge_count, double radius, CadShape* out);
    CadStatus (*shape_validate)(void* ctx, CadShape s);
    void      (*shape_release)(void* ctx, CadShape s);

    /* naming — the reason plugins can hold references across rebuilds at all */
    CadStatus (*element_resolve)(void* ctx, CadShape s, const CadElementId* id,
                                 CadShape* out_sub);
    CadStatus (*element_name_of)(void* ctx, CadShape s, CadShape sub, CadElementId* out);


    /* document */
    CadStatus (*txn_begin)(void* ctx, CadDocument doc, const char* label, CadTransaction* out);
    CadStatus (*txn_commit)(void* ctx, CadTransaction t);
    CadStatus (*txn_abort)(void* ctx, CadTransaction t);

    /* extension-point registration. Typed and size-negotiated; see the size-prefix rule. */
    /* Parameters arrive WITH the feature, so a feature and its parameters cannot disagree. */
    CadStatus (*register_feature)(void* ctx, const CadFeatureDesc* desc,
                                  const CadParamDesc* params, uint32_t param_count);

    /* Ribbon structure. Order matters and the host enforces it: a section must name a tab that
     * exists, and a command must name a section that exists. Registering in the wrong order is
     * refused with a message rather than silently creating what was missing.
     *
     * All three are legal for a plugin, which is a deliberate change from the first draft of
     * PLUGIN_CONTRACT 7.3: it forbade new tabs outright, and the reason to allow them is that a
     * real domain suite genuinely owns one. The protection moved from "you may not" to "the user
     * may hide it". */
    /* Settings arrive WITH their page, so a page and its fields cannot disagree. */
    CadStatus (*register_settings_page)(void* ctx, const CadSettingsPageDesc* page,
                                       const CadSettingDesc* settings, uint32_t setting_count);

    CadStatus (*register_tab)(void* ctx, const CadTabDesc* desc);
    CadStatus (*register_section)(void* ctx, const CadSectionDesc* desc);
    CadStatus (*register_command)(void* ctx, const CadCommandDesc* desc);
    CadStatus (*register_format)(void* ctx, const CadFormatDesc* desc);

    /* --- appended in 1.13 ---------------------------------------------------------------
     *
     * AT THE END, and everything after this must be too. A plugin compiled against 1.12 computes
     * every earlier member's offset from the layout it saw; inserting a member anywhere above
     * shifts all of them and silently calls the wrong function pointer. Growth goes on the end,
     * and `struct_size` is how a plugin tells which of these it may read. */

    /* Sub-shape enumeration: how a plugin obtains a face to name in the first place.
     *
     * Count-then-index, matching every other list in this ABI, so neither side allocates or frees
     * an array. `kind` is one of CAD_SUB_*.
     *
     * The ORDER is stable for a given shape but carries no meaning — it is the handle you name
     * through element_name_of that is durable, never the index. A plugin storing "face 3" across
     * a rebuild has reintroduced the problem this whole naming system exists to remove.
     *
     * Each returned handle is a new shape the caller must release. */
    CadStatus (*shape_sub_count)(void* ctx, CadShape s, uint32_t kind, uint32_t* out);
    CadStatus (*shape_sub_at)(void* ctx, CadShape s, uint32_t kind, uint32_t index,
                              CadShape* out);

    /* --- appended in 1.14: compute context accessors -------------------------------------
     *
     * How a plugin's compute reads its inputs and returns its result. On CadHost rather than on
     * the descriptor so the set can grow without touching any struct layout.
     *
     * READ-ONLY BY CONSTRUCTION. There is no compute_set_param here, no document handle, and no
     * transaction reachable from a CadComputeCtx. §4.1's determinism rule is enforced by the
     * SHAPE of this API rather than by asking politely: a compute cannot mutate the document
     * because nothing here lets it. Do not add a convenience that breaks this. */

    /* Inputs: the already-computed outputs of the features this one depends on, in declaration
     * order. Each shape is borrowed for the duration of the compute and must not be released. */
    CadStatus (*compute_input_count)(void* ctx, CadComputeCtx cc, uint32_t* out);
    CadStatus (*compute_input_shape)(void* ctx, CadComputeCtx cc, uint32_t index, CadShape* out);

    /* Parameters, by the `name` declared in CadParamDesc. A missing or wrongly-typed parameter
     * is CAD_ERR_INVALID_INPUT, never a silent default: a plugin reading a parameter that is not
     * there has a bug, and returning 0.0 would hide it inside geometry. */
    CadStatus (*compute_param_real)(void* ctx, CadComputeCtx cc, const char* name, double* out);
    CadStatus (*compute_param_int)(void* ctx, CadComputeCtx cc, const char* name, int64_t* out);
    CadStatus (*compute_param_text)(void* ctx, CadComputeCtx cc, const char* name, CadStr* out);

    /* The result. Exactly one call per successful compute; a second is CAD_ERR_INVALID_INPUT. */
    CadStatus (*compute_set_output)(void* ctx, CadComputeCtx cc, CadShape shape);

    /* The failure MESSAGE. A CadStatus is a code, and a code cannot say "the flange radius is
     * larger than the material allows". Without this every plugin failure would surface as a
     * generic string, which fails the bar the core already meets: recompute names the feature
     * responsible and says what went wrong, and blocked dependents quote it.
     *
     * `detail` is for developers and goes to the log, never to the user — the same split as
     * kernel::Error's message/detail. */
    CadStatus (*compute_fail)(void* ctx, CadComputeCtx cc, const char* message,
                              const char* detail);

    /* An element-valued parameter: a face or edge the user picked, by NAME rather than by index,
     * so it survives the rebuild that reorders the geometry. */
    CadStatus (*compute_param_element)(void* ctx, CadComputeCtx cc, const char* name,
                                       CadElementId* out);

    /* List-valued parameters. Count first, then index: handing back a pointer and a length would
     * require the host to own an array whose lifetime the plugin cannot see.
     *
     * Without these a plugin cannot implement a fillet, which takes MANY edges -- the example the
     * list kinds were added for. The kinds shipped in 1.10 and the means to read them did not,
     * which is the kind of gap that only shows up when something tries to use the API. */
    CadStatus (*compute_param_count)(void* ctx, CadComputeCtx cc, const char* name,
                                     uint32_t* out);
    CadStatus (*compute_param_element_at)(void* ctx, CadComputeCtx cc, const char* name,
                                          uint32_t index, CadElementId* out);
    CadStatus (*compute_param_shape_at)(void* ctx, CadComputeCtx cc, const char* name,
                                        uint32_t index, CadShape* out);

    /* The parameters of the feature being computed, as a context that external_inputs also
     * accepts. CadFeatureCtx and CadComputeCtx share a handle space deliberately: the parameter
     * accessors above accept either, so they are written once rather than duplicated for the two
     * moments a plugin needs to read its own parameters. */
    CadStatus (*compute_feature_ctx)(void* ctx, CadComputeCtx cc, CadFeatureCtx* out);
};

/* Sub-shape kinds for shape_sub_count / shape_sub_at.
 *
 * EXPLICIT numeric values, like every other constant that can reach a document. These end up in
 * plugin source that outlives this header, so a reordering must never silently change meaning. */
#define CAD_SUB_FACE   1u
#define CAD_SUB_EDGE   2u
#define CAD_SUB_VERTEX 3u

/* --- plugin interface ---------------------------------------------------------------- */
typedef struct {
    uint32_t    struct_size;        /* sizeof(CadPluginDesc) as the plugin compiled it */
    uint32_t    struct_version;

    /* The host serves this generation; it does NOT require it to be the current one. A host
     * running ABI 2 is expected to hand an abi_major==1 plugin a v1-shaped CadHost rather than
     * refuse it — that is what "a plugin written today still works in a decade" means, and it
     * is why the host passes a vtable instead of the plugin linking against symbols. */
    uint32_t    abi_major;
    uint32_t    abi_minor;          /* host may be newer */
    const char* id;                 /* reverse-DNS, e.g. "com.acme.sheetmetal" */
    const char* display_name;
    const char* semver;
    uint32_t    required_caps;      /* CAD_CAP_* bitmask; host may refuse */

    /* The OLDEST host this plugin will run on, within abi_major. A host with a smaller
     * CAD_ABI_VERSION_MINOR declines to load it and says which version is required.
     *
     * This is the forward direction, and it is refusal rather than compatibility. Loading a
     * plugin that needs functions the host never populated means calling a null pointer inside
     * third-party code, which is the hardest possible failure to attribute. SolidWorks carries
     * no such field, which is why its add-in authors write runtime version branches instead. */
    uint32_t    min_host_minor;

    CadStatus (*initialize)(const CadHost* host);
    void      (*shutdown)(void);
} CadPluginDesc;

/* The one symbol a plugin shared library must export. */
CAD_API const CadPluginDesc* cad_plugin_main(void);

/* =====================================================================================
 * Session API
 * =====================================================================================
 *
 * The same C surface, exported directly by the core library rather than reached through a
 * host vtable. This is what out-of-process consumers use: the Rust test suite, the future
 * SwiftUI iPad shell, and any language binding.
 *
 * It exists deliberately: driving the core from Rust means the acceptance tests exercise
 * exactly the boundary that plugins and the iPad app will use, so an ABI regression fails
 * the test suite instead of being discovered by a third party.
 *
 * Ownership rules, uniformly:
 *   - Handles are opaque, non-zero on success, zero on failure.
 *   - Every cad_*_release is idempotent and accepts 0.
 *   - Returned strings are owned by the session and valid until the NEXT call on that
 *     session. Copy immediately.
 *   - Nothing throws. Every fallible call returns CadStatus.
 */

typedef uint64_t CadSession;   /* owns a document, a feature registry, and a cache */
typedef uint64_t CadObject;    /* an object id within a session's document */

/* The version this library was BUILT with. Exists so a binding can verify its own constants
 * against the real thing instead of restating them and drifting silently. */
CAD_API void cad_abi_version(uint32_t* out_major, uint32_t* out_minor);

/* Would THIS build of the host load a plugin declaring these versions? Non-zero for yes.
 *
 * The loader's decision, exposed as a function rather than left as prose, so the rule is tested
 * rather than described. Backward: any abi_major this host still serves is accepted, whatever its
 * abi_minor. Forward: a plugin whose min_host_minor exceeds this host's minor is refused.
 *
 * `out_reason` receives a user-facing explanation when the answer is no; pass NULL to ignore it.
 * The string is static and needs no freeing. */
/* The host vtable this session would hand a plugin.
 *
 * Exposed as a session call so the boundary is drivable -- and therefore testable -- before any
 * loader exists. It is also what an in-process plugin tier would use. The returned pointer is
 * owned by the session and valid until the session is released.
 *
 * Entries the current configuration does not offer are NULL; plugins must check, per
 * PLUGIN_CONTRACT.md 4.5. */
CAD_API const CadHost* cad_plugin_host(CadSession);

CAD_API int32_t cad_abi_accepts(uint32_t plugin_abi_major, uint32_t plugin_min_host_minor,
                                const char** out_reason);

/* --- lifecycle --- */
CAD_API CadSession cad_session_create(void);

/* Session with the on-disk DDC tier enabled at `cache_dir`. Pass NULL or "" for assetlib's
 * default location. The disk tier is what lets a result computed on one machine — or by CI —
 * be served to another without recomputing. */
CAD_API CadSession cad_session_create_cached(const char* cache_dir);
CAD_API void       cad_session_release(CadSession);

/* Last error on this session, as a NUL-terminated string. Never NULL. */
CAD_API const char* cad_session_last_error(CadSession);

/* --- document editing --- */
CAD_API CadStatus cad_object_add(CadSession, const char* type, CadObject* out);
CAD_API CadStatus cad_object_remove(CadSession, CadObject);
CAD_API CadStatus cad_object_set_length(CadSession, CadObject, const char* prop, double millimetres);
CAD_API CadStatus cad_object_set_real(CadSession, CadObject, const char* prop, double value);
CAD_API CadStatus cad_object_set_int(CadSession, CadObject, const char* prop, int64_t value);
CAD_API CadStatus cad_object_set_bool(CadSession, CadObject, const char* prop, int32_t value);
CAD_API CadStatus cad_object_set_text(CadSession, CadObject, const char* prop, const char* value);
CAD_API CadStatus cad_object_set_object(CadSession, CadObject, const char* prop, CadObject target);
CAD_API CadStatus cad_object_set_element(CadSession, CadObject, const char* prop,
                                 const char* element_name);
/* Marks a property cosmetic: excluded from the recompute cache key. */
CAD_API CadStatus cad_object_set_cosmetic(CadSession, CadObject, const char* prop, int32_t cosmetic);

/* --- queries --- */
CAD_API CadStatus   cad_object_state(CadSession, CadObject, int32_t* out_state);
CAD_API const char* cad_object_error(CadSession, CadObject);
CAD_API CadStatus   cad_object_face_count(CadSession, CadObject, uint64_t* out);
CAD_API CadStatus   cad_object_edge_count(CadSession, CadObject, uint64_t* out);
CAD_API CadStatus   cad_object_cache_key(CadSession, CadObject, uint64_t* out);
CAD_API CadStatus   cad_object_is_valid_shape(CadSession, CadObject, int32_t* out);
/* Enclosed volume in mm^3. CAD_ERR_NOT_DONE when the shape could not be measured -- *out is
 * untouched in that case, and is never a non-finite number. */
CAD_API CadStatus   cad_object_volume(CadSession, CadObject, double* out);

/* Content hash of an object's output, as a 64-hex-char string. Empty if not computed. */
CAD_API const char* cad_object_content_hash(CadSession, CadObject);

/* The element name of the edge bounded by the two named faces of a Box object, in the
 * text form ElementName::toString produces. Empty string if there is no such edge. This is
 * how a caller with no C++ access takes a stable geometric reference. */
CAD_API const char* cad_box_edge_between(CadSession, CadObject box, int32_t face_a, int32_t face_b);
CAD_API const char* cad_box_face_name(CadSession, CadObject box, int32_t face);

/* --- recompute --- */
typedef struct {
    uint64_t computed;
    uint64_t cached;
    uint64_t skipped;
    uint64_t failed;
    uint64_t blocked;

    /* Appended in 1.15. Features whose type is not registered, counted apart from `failed`:
     * a shell reporting "3 features could not be built" over a document that is complete and
     * correct tells the user their file is damaged when it is not. See PLUGIN_CONTRACT.md 4A. */
    uint64_t needs_plugin;
} CadRecomputeReport;

CAD_API CadStatus cad_recompute(CadSession, CadRecomputeReport* out);
CAD_API CadStatus cad_cache_stats(CadSession, uint64_t* out_hits, uint64_t* out_misses);
CAD_API CadStatus cad_cache_reset_stats(CadSession);

/* --- undo/redo --- */
CAD_API CadStatus cad_undo(CadSession, int32_t* out_did_undo);
CAD_API CadStatus cad_redo(CadSession, int32_t* out_did_redo);
CAD_API CadStatus cad_document_digest(CadSession, uint64_t* out);
CAD_API CadStatus cad_object_count(CadSession, uint64_t* out);

/* --- sketches -------------------------------------------------------------------------
 *
 * A sketch is 2D geometry plus CONSTRAINTS between it, solved by planegcs. Handles are
 * session-scoped like everything else here.
 *
 * Sketches are not document objects yet, so these live on the session directly. When a Sketch
 * feature exists, that becomes the owner and these stay as the editing surface. */

typedef uint64_t CadSketch;

/* Plane the sketch is drawn on. */
#define CAD_SKETCH_PLANE_XY 0
#define CAD_SKETCH_PLANE_XZ 1
#define CAD_SKETCH_PLANE_YZ 2

/* Geometry kinds, matching cad::sketch::GeoKind. */
#define CAD_GEO_POINT  0
#define CAD_GEO_LINE   1
#define CAD_GEO_CIRCLE 2
#define CAD_GEO_ARC    3

/* Which characteristic point of a geometry a constraint refers to. */
#define CAD_POINT_START  0
#define CAD_POINT_END    1
#define CAD_POINT_CENTER 2

/* Constraint kinds, matching cad::sketch::ConstraintKind. */
#define CAD_CON_COINCIDENT    0
#define CAD_CON_HORIZONTAL    1
#define CAD_CON_VERTICAL      2
#define CAD_CON_PARALLEL      3
#define CAD_CON_PERPENDICULAR 4
#define CAD_CON_DISTANCE      5
#define CAD_CON_RADIUS        6
#define CAD_CON_POINT_ON_LINE 7
#define CAD_CON_EQUAL_LENGTH  8
#define CAD_CON_LOCK_X        9
#define CAD_CON_LOCK_Y       10
/* Two curves meet smoothly where their named points coincide. ADDED IN 1.22 — appended, because
   these values are positional and a plugin compiled against 1.21 must keep meaning what it meant.
   Pass both point refs; a plugin that wants a rounded corner adds coincidence as well, since
   tangency alone aligns directions without bringing the curves together. */
#define CAD_CON_TANGENT      11

typedef struct {
    int32_t  solved;
    int32_t  dofs;            /* 0 == fully constrained; negative == over-constrained */
    uint64_t conflicting;     /* constraints that cannot all hold */
    uint64_t redundant;
} CadSolveReport;

typedef struct {
    int32_t kind;             /* CAD_GEO_* */
    int32_t construction;
    double  p[5];             /* meaning depends on kind -- see cad::sketch::Geometry */
} CadSketchGeo;

typedef struct {
    uint64_t coincident;
    uint64_t horizontal;
    uint64_t vertical;
    uint64_t parallel;
    uint64_t perpendicular;
    int32_t  dofs_before;
    int32_t  dofs_after;
    uint64_t conflicting;
} CadInferReport;

CAD_API CadStatus cad_sketch_create(CadSession, int32_t plane, CadSketch* out);

/* Creates a sketch placed on a model FACE rather than on a global plane.
 *
 * `face` is an element name, which is what makes the placement survive a rebuild: the face keeps
 * its identity when an edit changes face ORDER, which an index would not. That is the whole reason
 * a CAD tool needs topological naming, and the failure it prevents is the one FreeCAD is known for.
 *
 * The sketch is not LOCATED by this call. Locating it needs the body's element map, which only the
 * recompute has — so the sketch feature must also reference the body as an object property, and
 * until it does the sketch refuses to build a profile rather than falling back to a global plane.
 *
 * A separate entry point rather than an extra argument to cad_sketch_create, because that one's
 * signature is frozen: ADR 0011 is additive-only and the golden snapshot enforces it. */
CAD_API CadStatus cad_sketch_create_on_face(CadSession, const CadElementId* face, CadSketch* out);
CAD_API void      cad_sketch_release(CadSession, CadSketch);

CAD_API CadStatus cad_sketch_add_line(CadSession, CadSketch, double x1, double y1, double x2,
                                      double y2, int32_t construction, uint32_t* out_geo);
CAD_API CadStatus cad_sketch_add_circle(CadSession, CadSketch, double cx, double cy, double radius,
                                        int32_t construction, uint32_t* out_geo);
CAD_API CadStatus cad_sketch_add_arc(CadSession, CadSketch, double cx, double cy, double radius,
                                     double start_angle, double end_angle, int32_t construction,
                                     uint32_t* out_geo);

/* One entry point for every constraint kind. Unused parameters are ignored per kind, which keeps
 * the ABI from growing a function per constraint -- there will eventually be dozens. */
CAD_API CadStatus cad_sketch_constrain(CadSession, CadSketch, int32_t kind, uint32_t a,
                                       int32_t a_point, uint32_t b, int32_t b_point, double value,
                                       uint64_t* out_index);

CAD_API CadStatus cad_sketch_solve(CadSession, CadSketch, CadSolveReport* out);
CAD_API CadStatus cad_sketch_geometry_count(CadSession, CadSketch, uint64_t* out);
CAD_API CadStatus cad_sketch_geometry(CadSession, CadSketch, uint64_t index, CadSketchGeo* out);
CAD_API CadStatus cad_sketch_constraint_count(CadSession, CadSketch, uint64_t* out);

/* Serialised form -- the text a document stores. Host-owned, valid until the next call. */
CAD_API CadStr   cad_sketch_serialize(CadSession, CadSketch);
CAD_API CadStatus cad_sketch_deserialize(CadSession, const char* text, CadSketch* out);

/* Deliberately absent for now: a call returning the profile as a CadShape. This ABI has no shape
 * handle registry, and inventing one to serve a single call would be the wrong shape for the
 * problem -- Extrude needs shapes to become document FEATURES, not loose handles. Added with
 * Extrude, designed once. */

/* DXF import. The result is UNCONSTRAINED -- a DXF carries no constraints. `scale` multiplies
 * every coordinate; DXF's own $INSUNITS is unreliable in the wild, so the caller decides. */
CAD_API CadStatus cad_sketch_import_dxf(CadSession, const char* path, int32_t plane, double scale,
                                        CadSketch* out);

/* Writes the sketch as DXF R12. `scale` DIVIDES coordinates, inverting import's multiply, so an
 * import at 25.4 followed by an export at 25.4 returns the original numbers. Constraints are not
 * written -- DXF cannot carry them. */
CAD_API CadStatus cad_sketch_export_dxf(CadSession, CadSketch, const char* path, double scale);

/* Infers constraints from near-relationships, then solves. Pass 0 for a tolerance to take the
 * default. `parallel_perpendicular` is off unless non-zero: it guesses wrong too often. */
CAD_API CadStatus cad_sketch_infer(CadSession, CadSketch, double point_tolerance,
                                   double angle_tolerance_deg, int32_t parallel_perpendicular,
                                   CadInferReport* out);

/* --- native documents (ADR 0003) ---
 *
 * The FEATURE TREE, not geometry: saving records how the part was built, so reopening restores an
 * editable model rather than a dead solid. Use cad_object_export for geometry interchange.
 *
 * cad_document_open REPLACES the session's document and clears its undo history — opening a file
 * is not an edit you can undo past. */
/* Rollback marker: features AFTER `id` are suspended -- not computed, no output, not drawn.
 * Pass 0 to roll the whole tree forward. Moving it is navigation, not an edit, so it does not
 * create an undo step. */
CAD_API CadStatus cad_rollback_set(CadSession, CadObject id_or_zero);
CAD_API CadStatus cad_rollback_get(CadSession, CadObject* out_id_or_zero);

CAD_API CadStatus cad_document_save(CadSession, const char* path);
CAD_API CadStatus cad_document_open(CadSession, const char* path);

/* --- file interchange --- */

/* Writes an object's geometry. Format is chosen by the path's extension. */
CAD_API CadStatus cad_object_export(CadSession, CadObject, const char* path);

/* What an import found. Everything a caller needs to warn the user BEFORE committing to a
 * conversion — docs/FORMATS.md rule 1: import never silently discards. */
typedef struct {
    uint64_t solids;
    uint64_t faces;
    int32_t  units_were_assumed;   /* the format carried no unit declaration */
    int32_t  unsupported_count;    /* entity kinds we could not represent */
    int32_t  warning_count;
} CadImportInfo;

/* Reads a file and reports on it WITHOUT adding anything to the document. For a file dialog
 * preview. To actually use the geometry, add an "Import" feature and set its "path". */
CAD_API CadStatus   cad_import_probe(CadSession, const char* path, int32_t assumed_units,
                             CadImportInfo* out);
CAD_API const char* cad_import_summary(CadSession);

/* Extensions we can read / write, as a single comma-separated string. */
CAD_API const char* cad_readable_extensions(CadSession);
CAD_API const char* cad_writable_extensions(CadSession);

/* --- tessellation (M3.1) --- */

/* What tessellating one object produced. Counts rather than data: the Rust suite verifies
 * structure and cache behaviour, and shipping a million vertices across the ABI to assert a
 * number would be absurd. The bgfx backend consumes RenderMesh in-process. */
typedef struct {
    uint64_t triangles;
    uint64_t vertices;
    uint64_t edgePolylines;
    uint64_t edgePoints;
    uint64_t elements;        /* named face+edge slots; every one must resolve */
    float    boundsMin[3];
    float    boundsMax[3];
} CadMeshInfo;

/* Tessellates an object's current geometry, through the mesh cache. Deflection and angular
 * deflection are part of the cache key, so changing either is a miss by construction. */
CAD_API CadStatus cad_object_tessellate(CadSession, CadObject, double deflection,
                                        double angular_deflection, CadMeshInfo* out);

/* Element name for a mesh element slot, in the round-trippable text form. Empty if the slot
 * is out of range. This is what proves a GPU pick could resolve to a stable reference. */
CAD_API const char* cad_mesh_element_name(CadSession, CadObject, uint32_t slot);

/* Mesh cache counters. The one that matters at assembly scale is hits: N identical parts must
 * tessellate ONCE. */
CAD_API CadStatus cad_mesh_cache_stats(CadSession, uint64_t* out_hits, uint64_t* out_misses);
CAD_API CadStatus cad_mesh_cache_reset_stats(CadSession);

/* --- scene assembly (M3.2) ---
 *
 * The session owns a SceneBuilder over a NullBackend. That is what makes the scene layer
 * testable with no GPU: the interesting bugs here are wrong instance counts, uploads that
 * should have deduped, and element slots that map to the wrong name — none of which need a
 * rasteriser to catch.
 */

/* Adds a placement of an object. `transform` is a column-major 4x3 affine, or NULL for
 * identity. Many placements of one object is the normal case in an assembly. */
CAD_API CadStatus cad_scene_add_placement(CadSession, CadObject, const float* transform12);
CAD_API CadStatus cad_scene_clear_placements(CadSession);

/* Rebuilds batches if anything changed. Idempotent and cheap when nothing has. */
CAD_API CadStatus cad_scene_update(CadSession, double deflection, double angular_deflection);

/* Submits one frame to the backend. */
CAD_API CadStatus cad_scene_submit(CadSession);

typedef struct {
    uint64_t rebuilds;        /* times update() did real work */
    uint64_t uploads;         /* buffer uploads issued by the scene layer */
    uint64_t gpu_uploads;     /* uploads the backend actually transferred */
    uint64_t gpu_deduped;     /* uploads collapsed onto an existing content hash */
    uint64_t unique_meshes;
    uint64_t instances;
    uint64_t element_slots;
    uint64_t draw_calls;      /* from the last submitted frame */
    uint64_t frame_instances;
    uint64_t frame_triangles;
    uint64_t frames;
    uint64_t highlighted;
    int32_t  orthographic;
} CadSceneStats;

CAD_API CadStatus cad_scene_stats(CadSession, CadSceneStats* out);
CAD_API CadStatus cad_scene_reset_stats(CadSession);

/* Camera. Mirrors CameraController so a shell never reimplements navigation. */
CAD_API CadStatus cad_camera_orbit(CadSession, float dx, float dy);
CAD_API CadStatus cad_camera_pan(CadSession, float dx, float dy);
CAD_API CadStatus cad_camera_zoom(CadSession, float ticks);
CAD_API CadStatus cad_camera_fit(CadSession);
CAD_API CadStatus cad_camera_set_orthographic(CadSession, int32_t ortho);
CAD_API CadStatus cad_camera_set_viewport(CadSession, uint32_t width, uint32_t height);
CAD_API CadStatus cad_camera_distance(CadSession, float* out);

/* Gesture mapping under the active navigation preset. 0=none 1=orbit 2=pan 3=zoom. */
CAD_API CadStatus cad_camera_set_preset(CadSession, int32_t preset);
CAD_API CadStatus cad_camera_drag_for(CadSession, int32_t button, int32_t shift, int32_t ctrl,
                                      int32_t* out_action);

/* Highlighting, by element name. Cheap — one byte, because hover fires on every mouse move. */
CAD_API CadStatus cad_scene_set_highlight(CadSession, const char* element_name, int32_t highlight);
CAD_API CadStatus cad_scene_clear_highlights(CadSession);

/* Picking. Scripts the null picker's next answer, then resolves it the way a real pick would:
 * (instance, slot) -> ElementName. What is under test is OUR mapping, not the GPU's. */
CAD_API CadStatus cad_scene_set_next_hit(CadSession, uint32_t instance, uint32_t element,
                                         int32_t valid);
CAD_API const char* cad_scene_pick(CadSession, uint32_t x, uint32_t y);
CAD_API CadStatus cad_scene_pick_owner(CadSession, uint32_t x, uint32_t y, CadObject* out);

/* How much ribbon exists, including the built-ins. Any output pointer may be null.
 *
 * Deliberately only counts. A shell needs to READ tabs, sections and commands to draw them, and
 * that surface should be designed when the shell is written rather than guessed at now — every
 * function added here is one we support for a decade. This much makes registration observable,
 * which is what a test needs. */
CAD_API CadStatus cad_ribbon_counts(CadSession, uint32_t* out_tabs, uint32_t* out_sections,
                                    uint32_t* out_commands);

/* Loads every plugin in a directory into this session.
 *
 * The shell calls this rather than reaching into abi/src/Loader.h, so the loader stays private and
 * the ONE way a plugin enters a process is a function with a contract.
 *
 * Failures are counted, not thrown and not fatal: one bad plugin must not stop the others, and the
 * user needs to be told which one and why. `cad_session_last_error` carries the last failure's message,
 * which is the honest amount of detail for a count -- a shell that wants per-plugin reasons should
 * use the manager window, which reads manifests without loading anything.
 *
 * Idempotent per directory: loading the same directory twice does not load a plugin twice, because
 * a second registration of the same feature type or ribbon id is refused anyway. */
CAD_API CadStatus cad_plugins_load(CadSession, const char* directory, uint32_t* out_loaded,
                                   uint32_t* out_failed);

/* Reading back what plugins declared, so a shell can render it.
 *
 * Strings point into the session and are valid until the NEXT call on it — the same rule as every
 * other string this ABI returns. Copy before calling again.
 *
 * `out` structs are filled only as far as the struct_size the CALLER set, so a shell built against
 * an older header is not written past its own end. */
CAD_API CadStatus cad_settings_page_count(CadSession, uint32_t* out);
CAD_API CadStatus cad_settings_page_at(CadSession, uint32_t index, CadSettingsPageDesc* out,
                                       uint32_t* out_setting_count);
CAD_API CadStatus cad_settings_at(CadSession, uint32_t page, uint32_t setting,
                                  CadSettingDesc* out);

/* --- units, exposed so bindings do not reimplement parsing --- */
CAD_API CadStatus cad_parse_length(const char* text, int32_t assumed_system, double* out_mm);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CAD_PLUGIN_ABI_H */
