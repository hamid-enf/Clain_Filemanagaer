/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_directory.h - Directory management & scanning
 */

#ifndef FM_DIRECTORY_H
#define FM_DIRECTORY_H

#include "fm_types.h"
#include "fm_file.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Advanced directory handle                                           */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t used;                       /* internal                    */
    void   *mount;                      /* internal FM_MountEntry*     */
    FM_FS  *fs;                         /* internal FM_FS*             */
    uint8_t fh[FM_FS_DIRHANDLE_SIZE];   /* adapter dir handle storage  */
    char    path[FM_PATH_MAX];          /* directory being listed      */
    char    resolved[FM_PATH_MAX];
} FM_Dir;

/* ------------------------------------------------------------------ */
/* Basic operations                                                    */
/* ------------------------------------------------------------------ */
/* Initialize a user-owned directory handle before Directory_OpenEx. */
void Directory_Init(FM_Dir *d);

FM_Result Directory_Create(const char *path);
FM_Result Directory_Delete(const char *path);            /* empty only  */
FM_Result Directory_DeleteRecursive(const char *path);
FM_Result Directory_Exists(const char *path, FM_Bool *exists);
FM_Result Directory_Rename(const char *old_path, const char *new_path);
FM_Result Directory_GetInfo(const char *path, FM_FileInfo *info);

/* ------------------------------------------------------------------ */
/* Listing                                                             */
/* ------------------------------------------------------------------ */
/* Directory_Open returns a handle from an internal static pool sized
 * by FM_MAX_OPEN_DIRS (no allocation). */
FM_Dir *Directory_Open(const char *path);
/* Open into a user-provided FM_Dir object (used by subsystems). */
FM_Result Directory_OpenEx(FM_Dir *d, const char *path);
FM_Result Directory_Close(FM_Dir *d);
/* Next entry; returns FM_OK and fills *entry, or FM_ERR_EOF at end. */
FM_Result Directory_Read(FM_Dir *d, FM_DirEntry *entry);
FM_Result Directory_Rewind(FM_Dir *d);

/* Count entries (type filter: FM_ITEM_FILE, FM_ITEM_DIR or 0 = all). */
FM_Result Directory_Count(const char *path, uint8_t type_filter,
                          uint32_t *count);

/* Recursive listing.  For every item (files + dirs, depth first) the
 * callback is invoked with a full FM_FileInfo.  progress may be NULL.
 * Return FM_ERR_CANCELLED from the callback to stop the scan. */
FM_Result Directory_List(const char *path, const char *filter,
                         FM_ItemCallback callback, void *user_data);

/* Recursive scan with progress and cancellation.  cancel may point to
 * a volatile flag; when it becomes non-zero the scan stops and
 * returns FM_ERR_CANCELLED. */
FM_Result Directory_ScanRecursive(const char *path, FM_ItemCallback callback,
                                  FM_ProgressCallback progress,
                                  void *user_data,
                                  volatile FM_Bool *cancel);

/* Copy / move a directory tree (progress optional).
 *
 * `dst` equal to `src`, or inside it, is FM_ERR_INVALID_ARGUMENT for a
 * copy (a tree copied into itself never ends); a move onto itself is a
 * no-op, a move into itself FM_ERR_INVALID_ARGUMENT.
 *
 * Directory_Copy: if `dst` did not exist and the copy fails, everything
 * it created is removed again.  When copying INTO an existing folder
 * nothing is removed after a failure (the files that were there before
 * cannot be told apart from the new ones).
 *
 * Directory_Move across two volumes is copy + delete.  If deleting the
 * source fails after the copy completed, the error is returned and the
 * destination is kept: the source may be partly gone by then, so the
 * destination is the only complete copy. */
FM_Result Directory_Copy(const char *src, const char *dst,
                         FM_ProgressCallback progress, void *user_data);
FM_Result Directory_Move(const char *src, const char *dst,
                         FM_ProgressCallback progress, void *user_data);

/* Total size of all files inside a directory (recursive). */
FM_Result Directory_Size(const char *path, uint64_t *size);

/* ------------------------------------------------------------------ */
/* Sorting helpers (used by Directory_List when needed)                */
/* ------------------------------------------------------------------ */
void Directory_SortByName(FM_DirEntry *entries, uint32_t count);
void Directory_SortBySize(FM_DirEntry *entries, uint32_t count);

FM_END_DECLS

#endif /* FM_DIRECTORY_H */
