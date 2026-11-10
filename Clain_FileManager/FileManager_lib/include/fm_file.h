/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_file.h - Unified File API (Simple + Advanced)
 *
 * Advanced API:  user owns an FM_File object and drives it with
 * File_Open/Read/Write/Seek/...   No dynamic allocation.
 *
 * Simple API:    one-shot calls such as File_WriteText("/x","hi"),
 *                File_ReadAll("/x", buf, size), File_Copy(...).
 */

#ifndef FM_FILE_H
#define FM_FILE_H

#include "fm_types.h"
#include "fm_filesystem.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Open modes (bit flags)                                              */
/* ------------------------------------------------------------------ */
#define FM_OPEN_READ      0x01u
#define FM_OPEN_WRITE     0x02u
#define FM_OPEN_CREATE    0x04u   /* create if missing                */
#define FM_OPEN_APPEND    0x08u   /* start writing at end of file     */
#define FM_OPEN_TRUNCATE  0x10u   /* discard existing content         */
#define FM_OPEN_READ_WRITE (FM_OPEN_READ | FM_OPEN_WRITE)

/* ------------------------------------------------------------------ */
/* Advanced handle (opaque fields; size is fixed at compile time)      */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t  used;                      /* internal                    */
    uint8_t  mode;                      /* open mode flags             */
    void    *mount;                     /* internal FM_MountEntry*     */
    FM_FS   *fs;                        /* internal FM_FS*             */
    uint8_t  fh[FM_FS_HANDLE_SIZE];     /* adapter handle storage      */
    uint64_t size;                      /* cached file size            */
    uint64_t pos;                       /* cached position             */
    char     path[FM_PATH_MAX];         /* original path               */
    char     resolved[FM_PATH_MAX];     /* resolved (normalized) path  */
} FM_File;

/* ------------------------------------------------------------------ */
/* Advanced API                                                        */
/*                                                                     */
/* Initialize a user-owned handle before passing it to File_OpenEx.   */
void File_Init(FM_File *f);

/* دیاگ نو-مموری (-14): چند اسلات از pool هشت‌تایی درگیر است و چه فایل‌هایی
 * دست Handles باز مانده‌اند. وقتی File_Open با FM_ERR_NO_MEMORY شکست
 * می‌خورد یعنی همهٔ اسلات‌ها occupied اند — معمولاً نشت handle (فایلی که
 * Close نشده). File_PoolDump هر اسلات درگیر را با مسیرش لاگ می‌کند؛
 * count فقط عدد برمی‌گرداند (برای تست/نمایش UI). */
uint32_t File_PoolUsed(void);
void     File_PoolDump(void);
/* File_PoolUsed() is the TOTAL (pool + external).  The two are separate
 * resources of FM_MAX_OPEN_FILES entries each: `internal` counts handles
 * from File_Open(), `external` counts caller-owned handles opened with
 * File_OpenEx().  Either pointer may be NULL. */
void     File_PoolUsedSplit(uint32_t *internal, uint32_t *external);

/* File_Open returns a handle from an internal static pool sized by    */
/* FM_MAX_OPEN_FILES (no allocation).  File_OpenEx opens into a        */
/* user-provided FM_File object (used by subsystems such as the        */
/* logger and audio/video readers).                                    */
/* ------------------------------------------------------------------ */
FM_File *File_Open(const char *path, uint32_t mode);
FM_Result File_OpenEx(FM_File *f, const char *path, uint32_t mode);
FM_Result File_Close(FM_File *f);
FM_Result File_Read(FM_File *f, void *buf, uint32_t len, uint32_t *read);
FM_Result File_Write(FM_File *f, const void *buf, uint32_t len,
                     uint32_t *written);
FM_Result File_Append(FM_File *f, const void *buf, uint32_t len,
                      uint32_t *written);
FM_Result File_Seek(FM_File *f, int64_t offset, FM_SeekOrigin origin);
FM_Result File_Tell(FM_File *f, uint64_t *pos);
FM_Result File_Size(FM_File *f, uint64_t *size);
FM_Result File_Truncate(FM_File *f, uint64_t size);
FM_Result File_Flush(FM_File *f);
FM_Bool  File_EOF(FM_File *f);
FM_Bool  File_IsOpen(const FM_File *f);

/* ------------------------------------------------------------------ */
/* Simple API - whole-file operations                                  */
/* ------------------------------------------------------------------ */
FM_Result File_Create(const char *path);                  /* empty file */
FM_Result File_Delete(const char *path);
/* Rename inside one mount (cross-mount -> FM_ERR_UNSUPPORTED, use
 * File_Move).  Renaming an entry onto itself -- also when the two paths
 * are merely spelled differently ("/a/b" vs "/a/./b") -- is a no-op that
 * returns FM_OK (FM_ERR_NOT_FOUND if the entry does not exist). */
FM_Result File_Rename(const char *old_path, const char *new_path);
FM_Result File_Exists(const char *path, FM_Bool *exists);
FM_Result File_GetInfo(const char *path, FM_FileInfo *info);
FM_Result File_SizeOf(const char *path, uint64_t *size);

/* Read whole file into user buffer (fails with FM_ERR_TOO_LARGE if the
 * file does not fit).  *read returns actual bytes. */
#if FM_USE_CACHE
/* Read-cache control (see fm_config.h FM_CACHE_LINE_SIZE / FM_CACHE_LINES).
 * The cache is enabled by default and speeds up small sequential or
 * repeated reads by fetching whole blocks. */
void    File_SetCacheEnabled(FM_Bool enabled);
FM_Bool File_GetCacheEnabled(void);
void    File_CacheGetStats(uint32_t *hits, uint32_t *misses);
void    File_CacheResetStats(void);
#endif

FM_Result File_ReadAll(const char *path, void *buffer, size_t buffer_size,
                       size_t *read);
FM_Result File_WriteAll(const char *path, const void *data, size_t size);
FM_Result File_AppendAll(const char *path, const void *data, size_t size);

/* Text helpers: buffer must be at least 1 byte; result is NUL
 * terminated.  *read excludes the terminator. */
FM_Result File_ReadText(const char *path, char *buffer, size_t buffer_size,
                        size_t *read);
FM_Result File_WriteText(const char *path, const char *text);
FM_Result File_AppendText(const char *path, const char *text);

/* Copy / move with optional progress reporting (cb may be NULL).
 *
 * File_Copy is all-or-nothing for the destination: the data is written to
 * "<dst>.fm_tmp" and renamed over dst only after it has been written,
 * flushed and closed.  On any failure the temp file is removed, dst does
 * not appear half-written, and a dst that already existed keeps its old
 * content.  Consequence: replacing an existing file needs free space for
 * the old and the new copy at the same time.  (Adapters without rename,
 * or a dst too long for the suffix, write dst directly and delete it on
 * failure.)  src and dst naming the same file -> FM_ERR_INVALID_ARGUMENT.
 *
 * Progress: cur == total is reported exactly once, after the data has
 * been flushed.  The callback must not start another File_Copy /
 * File_Move / File_Insert / File_DeleteRange / File_ReplaceRange: they
 * share one scratch buffer and the nested call returns FM_ERR_BUSY.
 *
 * File_Move is a rename inside one mount and copy + delete across mounts.
 * If the source cannot be deleted after the copy, the error is returned
 * and the copy is removed again -- unless dst already existed before the
 * call, in which case it is left in place (now holding the new content)
 * rather than deleted. */
FM_Result File_Copy(const char *src, const char *dst,
                    FM_ProgressCallback progress, void *user_data);
FM_Result File_Move(const char *src, const char *dst,
                    FM_ProgressCallback progress, void *user_data);

/* ------------------------------------------------------------------ */
/* File editing (streaming, RAM-friendly)                              */
/* ------------------------------------------------------------------ */
FM_Result File_Insert(const char *path, uint64_t offset, const void *data,
                      size_t size);
FM_Result File_DeleteRange(const char *path, uint64_t offset, uint64_t len);
FM_Result File_ReplaceRange(const char *path, uint64_t offset, uint64_t len,
                            const void *data, size_t size);

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */
FM_Result File_GetLastError(void);
const char *File_GetErrorString(FM_Result err);
const char *FM_ResultString(FM_Result err);   /* alias */

FM_END_DECLS

#endif /* FM_FILE_H */
