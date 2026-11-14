/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_path.h - Path manipulation module
 *
 * Paths always use '/' as separator.  Absolute paths start with '/'
 * and the first component is either a mount point ("/sd/...") or the
 * root mount ("/...").  All functions are buffer-safe and never
 * allocate.
 */

#ifndef FM_PATH_H
#define FM_PATH_H

#include "fm_types.h"

FM_BEGIN_DECLS

/* Join two path components: Path_Join("/data", "logs") -> "/data/logs".
 * Handles missing/duplicated separators and empty parts.  `out` may be
 * `a` or `b` (joining in place); FM_ERR_TOO_LARGE leaves it unchanged.
 * When preserving an overlapping `a` requires a temporary copy, its length
 * must be less than FM_PATH_MAX; non-overlapping joins have no such limit. */
FM_Result Path_Join(const char *a, const char *b, char *out, size_t out_size);

/* Normalize a path in place: collapses '.', '..', duplicate separators.
 * "/storage/data/../image/test.jpg" -> "/storage/image/test.jpg" */
FM_Result Path_Normalize(char *path, size_t path_size);

/* Copy the directory portion (without trailing separator) into out. */
FM_Result Path_GetDirectory(const char *path, char *out, size_t out_size);

/* Copy the last component (file or dir name) into out. */
FM_Result Path_GetFilename(const char *path, char *out, size_t out_size);

/* Copy the extension (without the dot, lowercased) into out.
 * "a/b.TXT" -> "txt"; "a/b" -> "" */
FM_Result Path_GetExtension(const char *path, char *out, size_t out_size);

/* Remove the extension in place: "a/b.txt" -> "a/b" */
FM_Result Path_RemoveExtension(char *path, size_t path_size);

/* Parent directory: "/a/b/c" -> "/a/b" ; "/a" -> "/" ; "/" -> "/" */
FM_Result Path_GetParent(const char *path, char *out, size_t out_size);

/* Root: "/a/b" -> "/" ; "a/b" -> "a" */
FM_Result Path_GetRoot(const char *path, char *out, size_t out_size);

FM_Bool Path_IsAbsolute(const char *path);
FM_Bool Path_IsRelative(const char *path);

/* Does the path (or its parent chain) contain ".." after normalization? */
FM_Bool Path_IsNormalized(const char *path);

FM_END_DECLS

#endif /* FM_PATH_H */
