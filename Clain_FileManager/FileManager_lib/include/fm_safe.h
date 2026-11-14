/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_safe.h - Temporary-file replacement and flush-backed operations (optional)
 *
 * Atomic write pattern: write to a temp file, flush, then rename over
 * the target. Atomic visibility and crash durability depend on the
 * filesystem and device. The FatFS adapter emulates rename-over with
 * multiple operations; it is not atomic against reset or observers.
 */

#ifndef FM_SAFE_H
#define FM_SAFE_H

#include "fm_types.h"

FM_BEGIN_DECLS

/* Replace via a flushed temporary file; see backend limits above. */
FM_Result File_SafeReplace(const char *path, const void *data, size_t size);
FM_Result File_SafeWriteText(const char *path, const char *text);
FM_Result File_AtomicWrite(const char *path, const void *data, size_t size);

/* Flush-backed append: writes the record through the normal file handle
 * and flushes before returning.  This avoids silently buffered data, but
 * does not claim atomicity for a power loss in the middle of a device
 * program operation. */
FM_Result File_SafeAppend(const char *path, const void *data, size_t size);

/* Remove the deterministic stale temporary file (<path>.fm_tmp). */
FM_Result File_Recover(const char *path);

/* Transactional update of a whole file with a rollback copy.
 * On success the backup is removed; on failure the original is
 * restored from the backup. */
FM_Result File_Transaction(const char *path, const void *data, size_t size);

FM_END_DECLS

#endif /* FM_SAFE_H */
