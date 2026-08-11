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

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_ABI_VERSION_MAJOR 1
#define CAD_ABI_VERSION_MINOR 2

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

/* --- host interface ------------------------------------------------------------------
 * Function-pointer table handed to the plugin at init. A NULL entry means the host does
 * not offer that call in this configuration (e.g. a sandboxed tier); plugins MUST check.
 */
typedef struct CadHost CadHost;

struct CadHost {
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

    /* extension-point registration */
    CadStatus (*register_feature)(void* ctx, const void* feature_desc);
    CadStatus (*register_command)(void* ctx, const void* command_desc);
    CadStatus (*register_format)(void* ctx, const void* format_desc);
};

/* --- plugin interface ---------------------------------------------------------------- */
typedef struct {
    uint32_t    abi_major;          /* must equal CAD_ABI_VERSION_MAJOR */
    uint32_t    abi_minor;          /* host may be newer */
    const char* id;                 /* reverse-DNS, e.g. "com.acme.sheetmetal" */
    const char* display_name;
    const char* semver;
    uint32_t    required_caps;      /* CAD_CAP_* bitmask; host may refuse */

    CadStatus (*initialize)(const CadHost* host);
    void      (*shutdown)(void);
} CadPluginDesc;

/* The one symbol a plugin shared library must export. */
const CadPluginDesc* cad_plugin_main(void);

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

/* --- lifecycle --- */
CadSession cad_session_create(void);

/* Session with the on-disk DDC tier enabled at `cache_dir`. Pass NULL or "" for assetlib's
 * default location. The disk tier is what lets a result computed on one machine — or by CI —
 * be served to another without recomputing. */
CadSession cad_session_create_cached(const char* cache_dir);
void       cad_session_release(CadSession);

/* Last error on this session, as a NUL-terminated string. Never NULL. */
const char* cad_session_last_error(CadSession);

/* --- document editing --- */
CadStatus cad_object_add(CadSession, const char* type, CadObject* out);
CadStatus cad_object_remove(CadSession, CadObject);
CadStatus cad_object_set_length(CadSession, CadObject, const char* prop, double millimetres);
CadStatus cad_object_set_real(CadSession, CadObject, const char* prop, double value);
CadStatus cad_object_set_int(CadSession, CadObject, const char* prop, int64_t value);
CadStatus cad_object_set_bool(CadSession, CadObject, const char* prop, int32_t value);
CadStatus cad_object_set_text(CadSession, CadObject, const char* prop, const char* value);
CadStatus cad_object_set_object(CadSession, CadObject, const char* prop, CadObject target);
CadStatus cad_object_set_element(CadSession, CadObject, const char* prop,
                                 const char* element_name);
/* Marks a property cosmetic: excluded from the recompute cache key. */
CadStatus cad_object_set_cosmetic(CadSession, CadObject, const char* prop, int32_t cosmetic);

/* --- queries --- */
CadStatus   cad_object_state(CadSession, CadObject, int32_t* out_state);
const char* cad_object_error(CadSession, CadObject);
CadStatus   cad_object_face_count(CadSession, CadObject, uint64_t* out);
CadStatus   cad_object_edge_count(CadSession, CadObject, uint64_t* out);
CadStatus   cad_object_cache_key(CadSession, CadObject, uint64_t* out);
CadStatus   cad_object_is_valid_shape(CadSession, CadObject, int32_t* out);
CadStatus   cad_object_volume(CadSession, CadObject, double* out);

/* Content hash of an object's output, as a 64-hex-char string. Empty if not computed. */
const char* cad_object_content_hash(CadSession, CadObject);

/* The element name of the edge bounded by the two named faces of a Box object, in the
 * text form ElementName::toString produces. Empty string if there is no such edge. This is
 * how a caller with no C++ access takes a stable geometric reference. */
const char* cad_box_edge_between(CadSession, CadObject box, int32_t face_a, int32_t face_b);
const char* cad_box_face_name(CadSession, CadObject box, int32_t face);

/* --- recompute --- */
typedef struct {
    uint64_t computed;
    uint64_t cached;
    uint64_t skipped;
    uint64_t failed;
    uint64_t blocked;
} CadRecomputeReport;

CadStatus cad_recompute(CadSession, CadRecomputeReport* out);
CadStatus cad_cache_stats(CadSession, uint64_t* out_hits, uint64_t* out_misses);
CadStatus cad_cache_reset_stats(CadSession);

/* --- undo/redo --- */
CadStatus cad_undo(CadSession, int32_t* out_did_undo);
CadStatus cad_redo(CadSession, int32_t* out_did_redo);
CadStatus cad_document_digest(CadSession, uint64_t* out);
CadStatus cad_object_count(CadSession, uint64_t* out);

/* --- file interchange --- */

/* Writes an object's geometry. Format is chosen by the path's extension. */
CadStatus cad_object_export(CadSession, CadObject, const char* path);

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
CadStatus   cad_import_probe(CadSession, const char* path, int32_t assumed_units,
                             CadImportInfo* out);
const char* cad_import_summary(CadSession);

/* Extensions we can read / write, as a single comma-separated string. */
const char* cad_readable_extensions(CadSession);
const char* cad_writable_extensions(CadSession);

/* --- units, exposed so bindings do not reimplement parsing --- */
CadStatus cad_parse_length(const char* text, int32_t assumed_system, double* out_mm);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CAD_PLUGIN_ABI_H */
