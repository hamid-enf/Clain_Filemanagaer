/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_internal.h - Internal cross-module helpers (not public API)
 */

#ifndef FM_INTERNAL_H
#define FM_INTERNAL_H

#include "file_manager.h"

FM_BEGIN_DECLS

void FM_Core_SetLastError(FM_Result err);
void FM_Storage_SetFreeSpace(const char *name, uint64_t free);
FM_Storage *FM_Storage_FirstMounted(void);
/* Storage state flags, read under the storage registry lock (they are
 * changed by Storage_Mount / _Unmount / _Insert / _Remove, possibly from
 * a card-detect task). */
FM_Bool  FM_Storage_IsPresent(const FM_Storage *s);
FM_Bool  FM_Storage_IsMounted(const FM_Storage *s);
uint64_t FM_Storage_FreeSpaceHint(const FM_Storage *s);

/* VFS resolution: normalize path, find the mount entry, produce the
 * filesystem-relative path (no leading '/') in rel_buf. */
typedef struct
{
    char mount_path[FM_PATH_MAX];  /* mount point as registered */
    FM_FS *fs;
    FM_Bool used;                  /* mounted: visible to path lookup  */
    FM_Bool reserved;              /* FS_Mount / FS_Unmount in progress */
    uint16_t pins;                 /* path operations in flight         */
#if FM_USE_THREADS
    FM_Mutex lock;                 /* serialises the backend            */
    FM_Bool lock_ready;
#endif
} FM_MountEntry;

FM_MountEntry *FM_VFS_GetMounts(void);
uint32_t FM_VFS_GetMountCount(void);

FM_Result FM_VFS_Resolve(const char *path, FM_MountEntry **mount,
                         char *rel_buf, size_t rel_buf_size);

/* Resolve AND pin: until the matching FM_VFS_Unpin() the mount cannot be
 * unmounted (FS_Unmount returns FM_ERR_BUSY), so mount->fs stays valid.
 * Every path operation that calls into a backend uses this pair; an open
 * file / directory handle keeps its mount alive the same way. */
FM_Result FM_VFS_Pin(const char *path, FM_MountEntry **mount,
                     char *rel_buf, size_t rel_buf_size);
void      FM_VFS_Unpin(FM_MountEntry *mount);

/* Per-mount backend lock.  FatFs, LittleFS and FMFS keep per-volume
 * state (FAT window, lookahead buffer, block caches, the FMFS index)
 * that is not safe to enter from two tasks, so EVERY fs->ops.* call is
 * made with this lock held.  It is a leaf with respect to the read
 * cache and the scratch claims, and is never held across a user
 * callback.  `mount` is an FM_MountEntry* (void* because that is how
 * FM_File / FM_Dir store it).  No-ops when FM_USE_THREADS == 0. */
#if FM_USE_THREADS
void FM_Mount_Lock(void *mount);
void FM_Mount_Unlock(void *mount);
#else
#define FM_Mount_Lock(mount)   ((void)(mount))
#define FM_Mount_Unlock(mount) ((void)(mount))
#endif

/* r = mount->fs->ops.<call>, with the mount's backend lock held. */
#define FM_FS_CALL(mount, r, call)                 \
    do                                             \
    {                                              \
        FM_Mount_Lock(mount);                      \
        (r) = (mount)->fs->ops.call;               \
        FM_Mount_Unlock(mount);                    \
    } while (0)

/* For backend entry points that take the FM_FS itself instead of a path
 * (FMFS_Repack): pin the mount `fs` is mounted on and take its backend
 * lock.  Returns NULL, with nothing held, when `fs` is not mounted;
 * FM_VFS_LeaveFS(NULL) is a no-op. */
FM_MountEntry *FM_VFS_EnterFS(const FM_FS *fs);
/* A handle is open on `rel` of this mount (tree: or below it).  Call
 * with the mount's backend lock held (see src/fm_file.c). */
FM_Bool        FM_File_OpenOnPath(const FM_MountEntry *mount,
                                  const char *rel, FM_Bool tree);
void           FM_VFS_LeaveFS(FM_MountEntry *mount);

/* Create / destroy the locks owned by each module.  Called from
 * FileManager_Init / FileManager_Deinit. */
void FM_VFS_SyncInit(void);
void FM_VFS_SyncDeinit(void);
void FM_Storage_SyncInit(void);
void FM_Storage_SyncDeinit(void);
void FM_File_SyncInit(void);
void FM_File_SyncDeinit(void);

/* File pool introspection (used to refuse unmount with open files). */
FM_Bool FM_File_AnyOpenOnMount(const FM_MountEntry *mount);
FM_Bool FM_Dir_AnyOpenOnMount(const FM_MountEntry *mount);

/* Streaming copy helper shared by File_Copy and Directory_Copy. */
FM_Result FM_File_CopyStream(FM_File *src, FM_File *dst,
                             FM_ProgressCallback progress, void *user_data);

/* Global lock for the VFS tables */
void FM_Lock(void);
void FM_Unlock(void);

/* Internal scratch buffer */
uint8_t *FM_InternalBuffer(void);
size_t   FM_InternalBufferSize(void);
/* Exclusive use of the scratch buffer (FM_SCRATCH_IOBUF claim).  Another
 * task that holds it makes Acquire wait; a nested call on the owning
 * task (e.g. from a progress callback) gets NULL -- callers report
 * FM_ERR_BUSY.  Every successful Acquire must be paired with exactly
 * one Release. */
uint8_t *FM_InternalBufferAcquire(size_t *size);
void     FM_InternalBufferRelease(void);

/* FM_TRUE when both paths resolve to the same entry of the same mount. */
FM_Bool FM_File_SamePath(const char *a, const char *b);
FM_Bool FM_Mount_NoCase(const FM_MountEntry *mount);
/* a name of FM_NAME_MAX characters or more (the last component of
 * `rel`, or with `all` any of them): cannot be listed, so not created */
FM_Bool FM_Path_NameTooLong(const char *rel, FM_Bool all);
/* FM_TRUE while a mounted file system uses this storage. */
FM_Bool FM_VFS_StorageInUse(const FM_Storage *storage);
/* The one rename every caller goes through (File_Rename, File_Move,
 * Directory_Rename, Directory_Move): onto itself is a no-op, a path with
 * an open handle on or below it is FM_ERR_BUSY.  `m` must be pinned. */
FM_Result FM_File_RenameResolved(FM_MountEntry *m, const char *from,
                                 const char *to);

/* Exclusive claims on the library's shared work areas (fm_core.c).
 *
 *   FM_USE_THREADS=1, port with thread identity (all ready-made ports):
 *       another task holding the claim  -> Enter waits for it;
 *       nested call on the owning task  -> FM_ERR_BUSY.
 *   FM_USE_THREADS=1, port without FM_Thread_Self, and FM_USE_THREADS=0:
 *       claim already held, by anyone   -> FM_ERR_BUSY.
 *
 * Every FM_OK from Enter must be paired with exactly one Leave, on the
 * same thread.  Hierarchy when more than one is held: XFER, then CODEC,
 * then IOBUF. */
typedef enum
{
    FM_SCRATCH_XFER = 0,   /* fm_transfer.c chunk buffer                  */
    FM_SCRATCH_CODEC,      /* PNG / JPEG / BMP / MJPEG decoder state      */
    FM_SCRATCH_IOBUF,      /* FM_InternalBuffer (File_Copy, File_Insert)  */
    FM_SCRATCH_H264,       /* H.264 decode: diagnostics + h264bsd latches */
    FM_SCRATCH_COUNT
} FM_ScratchId;

FM_Result FM_Scratch_Enter(FM_ScratchId id);
void      FM_Scratch_Leave(FM_ScratchId id);

#if FM_USE_IMAGE || FM_USE_MJPEG
/* The FM_SCRATCH_CODEC claim: the PNG window and row buffers, the BMP
 * row scratch, the JPEG work buffer and the file-static state of
 * picojpeg (shared by JPEG stills and MJPEG video).  Two tasks decoding
 * at once are serialised; a nested decode from the FM_SetIdle hook or a
 * codec callback gets FM_ERR_BUSY and the outer run is left intact. */
FM_Result FM_CodecScratch_Enter(void);
void      FM_CodecScratch_Leave(void);
#endif

#if FM_USE_IMAGE
/* Shrink (sw,sh) so dest RGB/gray fits `capacity` and optional max_w/max_h
 * (0 = no clamp).  Aspect ratio is preserved. */
FM_Result FM_Image_FitSize(uint16_t src_w, uint16_t src_h, size_t capacity,
                           size_t bpp, uint16_t max_w, uint16_t max_h,
                           uint16_t *dst_w, uint16_t *dst_h);
FM_Result FM_Image_ReadExact(FM_File *f, void *data, uint32_t size);
/* width*height*bpp in 64 bits; FM_ERR_TOO_LARGE when it exceeds
 * FM_IMAGE_MAX_BYTES / size_t, FM_ERR_INVALID_ARGUMENT for a zero. */
FM_Result FM_Image_Bytes(uint32_t width, uint32_t height, size_t bpp,
                         size_t *bytes);
/* Pixel bytes an encoder / transform will read from `img`, checked
 * against BOTH data_capacity and (when given) data_size. */
FM_Result FM_Image_CheckSource(const FM_Image *img, size_t *need);

#if FM_USE_JPEG
FM_Result FM_JPEG_LoadFile(FM_File *f, FM_Image *img, void *pixel_buffer,
                           size_t capacity, uint16_t max_w, uint16_t max_h);
FM_Result FM_JPEG_ProbeFile(FM_File *f, FM_ImageInfo *info);
FM_Result FM_JPEG_EncodeFile(FM_File *f, const FM_Image *img);
#endif
#if FM_USE_PNG
FM_Result FM_PNG_LoadFile(FM_File *f, FM_Image *img, void *pixel_buffer,
                          size_t capacity, uint16_t max_w, uint16_t max_h);
FM_Result FM_PNG_ProbeFile(FM_File *f, FM_ImageInfo *info);
FM_Result FM_PNG_EncodeFile(FM_File *f, const FM_Image *img);
#endif
#endif /* FM_USE_IMAGE */

/* JPEG encode that hands the file out in pieces (the video writer puts
 * them into an AVI chunk).  FM_ERR_UNSUPPORTED when FM_USE_JPEG is 0. */
typedef FM_Result (*FM_JpegSinkFn)(void *ctx, const uint8_t *data,
                                   uint32_t len);
FM_Result FM_JPEG_EncodeSink(const FM_Image *img,
                             const FM_JpegEncodeOptions *opt,
                             FM_JpegSinkFn sink, void *ctx);

FM_END_DECLS

#endif /* FM_INTERNAL_H */