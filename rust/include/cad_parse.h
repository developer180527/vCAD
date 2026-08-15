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

#ifdef __cplusplus
}
#endif

#endif /* CAD_PARSE_H */
