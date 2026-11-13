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

/* Creates the directory and any missing parents.  A directory that is
 * already there is success; a FILE of that name (at any level of the
 * path) is FM_ERR_NOT_DIRECTORY. */
FM_Result Directory_Create(const char *path);
FM_Result Directory_Delete(const char *path);            /* empty only  */
FM_Result Directory_DeleteRecursive(const char *path);
FM_Result Directory_Exists(const char *path, FM_Bool *exists);
/* Rename / move a directory within one volume (the tree below it moves
 * with it).  Not a rename:
 *   FM_ERR_NOT_DIRECTORY     old_path is a file (use File_Rename)
 *   FM_ERR_INVALID_ARGUMENT  old_path is a mount point, or new_path
 *                            lies inside old_path
 *   FM_ERR_UNSUPPORTED       the two are on different volumes (use
 *                            Directory_Move)
 *   FM_ERR_BUSY              a file on or below old_path is open
 *   FM_ERR_TOO_LARGE         the new name has FM_NAME_MAX characters or
 *                            more
 * Onto its own name is FM_OK and changes nothing.  Directory_Move on one
 * volume is this call. */
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
 * Return FM_ERR_CANCELLED from the callback to stop the scan.
 *
 * `filter` is matched against the entry name: '*' stands for any run of
 * characters (also none), everything else for itself.  NULL or "" lists
 * everything.
 *
 * Rules for every walk of a tree (List, ScanRecursive, Size, Copy, Move,
 * DeleteRecursive):
 *  - A tree may be FM_DIR_MAX_DEPTH (16) directories deep below `path`;
 *    a deeper one is FM_ERR_TOO_LARGE.  The walk has one directory open
 *    at a time and its stack use does not grow with the depth.
 *  - The callback must not add or remove entries in the tree it is being
 *    shown.  Coming back from a sub-directory the walk reads the parent
 *    again and skips the entries it has done, which is right only if the
 *    directory lists them in the same order as before; entries would be
 *    skipped or visited twice otherwise.
 *  - An entry whose name does not fit FM_NAME_MAX stops the walk with
 *    FM_ERR_TOO_LARGE (a name is never shortened).  Cards written on a
 *    PC can hold such names: see FM_NAME_MAX in fm_config.h. */
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
