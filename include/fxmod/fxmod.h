/* Public, ABI-stable C API for fxmod.
 *
 * Opaque handles + explicit error objects, so this is usable from C,
 * Fortran (via iso_c_binding), or any other language with a C FFI -- not
 * just C++. C++ callers who want richer structural access than this
 * surface offers should use <fxmod/module.hpp> instead; this header is
 * implemented in terms of that one.
 *
 * Ownership: fxmod_open() yields a fxmod_module* the caller must free
 * with fxmod_module_free(). Any *_error** out-parameter is populated only
 * on failure (left untouched/NULL on success) and must be freed with
 * fxmod_error_free() if set. Any `char*`/`char**` this API hands back is
 * owned by the caller and must be released with fxmod_free_string() /
 * fxmod_free_string_array() -- except fxmod_module_version() and
 * fxmod_module_name(), which return a pointer owned by the module and
 * valid only until that module is freed.
 */
#ifndef FXMOD_H
#define FXMOD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxmod_module fxmod_module;
typedef struct fxmod_error fxmod_error;

typedef enum fxmod_status {
  FXMOD_OK = 0,
  /* Input could not be understood at all: bad file, bad gzip, checksum
   * mismatch, malformed S-expression, unrecognized format, ... */
  FXMOD_ERROR = 1,
  /* Input was understood but falls outside what this library translates:
   * an unsupported gfortran module version, an untranslatable symbol
   * under strict emission, an ifx file (recognized, not implemented). */
  FXMOD_UNSUPPORTED = 2,
} fxmod_status;

typedef enum fxmod_family {
  FXMOD_FAMILY_UNKNOWN = 0,
  FXMOD_FAMILY_GFORTRAN = 1,
  FXMOD_FAMILY_FLANG = 2,
  FXMOD_FAMILY_IFX = 3,
} fxmod_family;

/* --- errors --------------------------------------------------------- */

fxmod_status fxmod_error_code(const fxmod_error *err);
/* Never NULL. Owned by the error object; valid until fxmod_error_free(). */
const char *fxmod_error_message(const fxmod_error *err);
void fxmod_error_free(fxmod_error *err);

/* --- freeing values this API hands back ------------------------------ */

void fxmod_free_string(char *s);
void fxmod_free_string_array(char **arr, size_t count);

/* --- opening a module -------------------------------------------------
 *
 * Detects the family from the file's own magic bytes and parses it.
 * Returns FXMOD_OK with *out_module set on success. On failure, *out_module
 * is left NULL, and *out_error is set (if out_error is non-NULL) -- the
 * caller must free it with fxmod_error_free().
 */
fxmod_status fxmod_open(const char *path, fxmod_module **out_module,
                           fxmod_error **out_error);

void fxmod_module_free(fxmod_module *module);

fxmod_family fxmod_module_family(const fxmod_module *module);

/* The format's own version marker ("15"/"16" for gfortran, "v1" for
 * Flang). Owned by `module`; do not free. */
const char *fxmod_module_version(const fxmod_module *module);

/* Best-effort module name (the file's stem). Owned by `module`. */
const char *fxmod_module_name(const fxmod_module *module);

/* --- regenerating Fortran source --------------------------------------
 *
 * The library's flagship operation: turn a parsed module back into plain,
 * compilable Fortran module source, regardless of which compiler wrote
 * the original file.
 *
 * strict != 0: fails with FXMOD_UNSUPPORTED if any public symbol could
 * not be translated; *out_text is left NULL.
 * strict == 0: always succeeds (for a module that opened at all) and
 * returns the best-effort source plus the list of what was skipped.
 *
 * out_text is malloc'd, NUL-terminated UTF-8; free with fxmod_free_string().
 * out_problems/out_problem_count are optional (pass NULL/NULL to ignore);
 * when requested, free the array with fxmod_free_string_array().
 */
fxmod_status fxmod_module_emit_fortran_source(
    const fxmod_module *module, int strict, char **out_text,
    char ***out_problems, size_t *out_problem_count, fxmod_error **out_error);

/* --- Flang header convenience ------------------------------------------
 *
 * Wraps `body` (arbitrary Fortran source, typically the output of
 * fxmod_module_emit_fortran_source) in the header a Flang-compatible
 * reader expects (BOM + magic + FNV-1a checksum). A convenience only --
 * most callers should prefer compiling the emitted source with the target
 * compiler directly rather than hand-constructing its module file.
 *
 * out_bytes is malloc'd; free with fxmod_free_string() (it is byte data,
 * not necessarily NUL-terminated text-safe, but the same allocator is used).
 */
fxmod_status fxmod_wrap_flang_header(const char *body, size_t body_len,
                                        char **out_bytes, size_t *out_len,
                                        fxmod_error **out_error);

#ifdef __cplusplus
}
#endif

#endif /* FXMOD_H */
