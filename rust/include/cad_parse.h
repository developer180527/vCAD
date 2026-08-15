/* The C surface of the Rust parsing library.
 *
 * HAND-WRITTEN, not generated. cbindgen would be one more tool in the build, one more thing to
 * install on three platforms, and one more generated file to keep in sync — for a surface this
 * small the header IS the specification, and writing it by hand means the C++ side and the Rust
 * side are two independent statements of the same contract rather than one derived from the other.
 * If this grows past a dozen functions, revisit.
 *
 * Same boundary rules as the plugin ABI (docs/design/PLUGIN_CONTRACT.md): C only, caller-allocated
 * buffers, no allocator shared across the seam, nothing that can throw or unwind.
 */
#ifndef CAD_PARSE_H
#define CAD_PARSE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI generation the C++ side expects. Compared against cad_parse_abi_version() at startup:
 * the two halves are compiled separately, and a stale build directory can pair a new header
 * with an old archive. A mismatch that is caught is a message; one that is not is a wrong
 * answer from a parser. */
#define CAD_PARSE_ABI_EXPECTED 1

/* The generation this library was actually built as. */
int cad_parse_abi_version(void);

/* Writes a NUL-terminated build identity into `buffer` and returns the bytes written, excluding
 * the terminator. Returns 0 — writing nothing — when `buffer` is NULL or `capacity` is too small
 * to hold the identity and its terminator. Never truncates: a half-written identity is worse
 * than none. */
size_t cad_parse_build_id(char* buffer, size_t capacity);

/* ---------------------------------------------------------------------------------------------
 * DXF
 *
 * Turns bytes we did not write into a checked list of entities, and nothing more. Projection onto
 * a sketch plane, unit scaling, construction-layer matching and the degenerate-geometry rules stay
 * on the C++ side, where the sketch types are — the argument for parsing this in Rust covers the
 * bytes and stops there.
 *
 * Count-then-index, like the plugin ABI's shape accessors. Nothing here is a struct crossing the
 * boundary, so there is no layout to agree on and a stale header cannot make C++ read the wrong
 * field; it can only fail to link, which is loud.
 *
 * EVERY accessor is total. A NULL handle or an out-of-range index returns the documented zero
 * value rather than reading memory. These are called from loops bounded by a count from a previous
 * call, and the bug that shape produces is an off-by-one at the end — this makes that bug a wrong
 * number instead of an exploitable one.
 * ------------------------------------------------------------------------------------------- */

/* Entity kinds. Values are explicit and must match src/dxf_c.rs: the two are independent
 * statements of one contract, and a reordering on either side would silently turn every circle
 * into an arc. */
#define CAD_DXF_LINE     0
#define CAD_DXF_CIRCLE   1
#define CAD_DXF_ARC      2
#define CAD_DXF_POINT    3
#define CAD_DXF_POLYLINE 4

/* Parse outcomes. Zero is success. */
#define CAD_DXF_OK                    0
#define CAD_DXF_ERR_NOT_DXF           1  /* binary DXF, DWG renamed, or not DXF at all */
#define CAD_DXF_ERR_RECORD_TOO_LONG   2
#define CAD_DXF_ERR_TOO_LARGE         3
#define CAD_DXF_ERR_TRUNCATED         4  /* ends part-way through a record */
#define CAD_DXF_ERR_UNREADABLE        5  /* could not be opened or read */

typedef struct CadDxfDocument CadDxfDocument;

/* Reads a DXF file. Returns NULL on failure with `*out_error` set to a CAD_DXF_ERR_* code;
 * `out_error` may be NULL. The result must be released with cad_dxf_free. */
CadDxfDocument* cad_dxf_parse_file(const char* path, int* out_error);
void cad_dxf_free(CadDxfDocument* doc);

/* The message for a CAD_DXF_ERR_* code. Same buffer contract as cad_parse_build_id, except that
 * the FULL length is returned even when nothing was written, so a caller can retry with a larger
 * buffer. */
size_t cad_dxf_error_message(int code, char* buffer, size_t capacity);

size_t cad_dxf_entity_count(const CadDxfDocument* doc);

/* One of the CAD_DXF_* kinds, or -1 for an out-of-range index. */
int cad_dxf_entity_kind(const CadDxfDocument* doc, size_t index);

/* Number of x,y PAIRS: 2 for a line, 1 for a circle/arc/point centre, n for a polyline. */
size_t cad_dxf_entity_point_count(const CadDxfDocument* doc, size_t index);

/* Writes the `point`-th x,y pair. Returns non-zero on success; on failure the outputs are left
 * UNTOUCHED, so a caller that ignores the result reads what it initialised them to. Both
 * coordinates in one call because a pair that cannot be split cannot be half-updated. */
int cad_dxf_entity_point(const CadDxfDocument* doc, size_t index, size_t point,
                         double* out_x, double* out_y);

double cad_dxf_entity_radius(const CadDxfDocument* doc, size_t index);

/* Arc angles, in DEGREES as the file stores them. The caller converts to radians — that
 * conversion and the test that pins it already live on the C++ side, and doing it in both places
 * would scale every arc twice. */
double cad_dxf_entity_start_angle(const CadDxfDocument* doc, size_t index);
double cad_dxf_entity_end_angle(const CadDxfDocument* doc, size_t index);

/* DXF flags. Bit 0 set on a polyline means closed: the last vertex joins the first and that
 * segment is not stored. */
long long cad_dxf_entity_flags(const CadDxfDocument* doc, size_t index);

/* Bulges make a polyline segment an arc. Zero for a polyline with no curved segments at all —
 * such a polyline reports a count of 0 rather than a run of zeros, so a straight profile never
 * appears to have lost curvature. */
size_t cad_dxf_entity_bulge_count(const CadDxfDocument* doc, size_t index);
double cad_dxf_entity_bulge(const CadDxfDocument* doc, size_t index, size_t segment);

/* Layer name. Same buffer contract as cad_dxf_error_message. */
size_t cad_dxf_entity_layer(const CadDxfDocument* doc, size_t index,
                            char* buffer, size_t capacity);

/* Entities the file described but that could not be read — a required field missing, or a
 * coordinate that was not finite. Distinct from geometry we chose to drop. */
size_t cad_dxf_malformed_count(const CadDxfDocument* doc);

/* Entity types present that this parser does not produce, with counts, so the import can say
 * "SPLINE x4" and let the user judge whether the missing geometry mattered. */
size_t cad_dxf_unsupported_count(const CadDxfDocument* doc);
size_t cad_dxf_unsupported_name(const CadDxfDocument* doc, size_t index,
                                char* buffer, size_t capacity);
size_t cad_dxf_unsupported_occurrences(const CadDxfDocument* doc, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* CAD_PARSE_H */
