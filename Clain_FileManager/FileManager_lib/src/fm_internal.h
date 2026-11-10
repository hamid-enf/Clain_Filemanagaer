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

/* VFS resolution: normalize path, find the mount entry, produce the
 * filesystem-relative path (no leading '/') in rel_buf. */
typedef struct
{
    char mount_path[FM_PATH_MAX];  /* mount point as registered */
    FM_FS *fs;
    FM_Bool used;
} FM_MountEntry;

FM_MountEntry *FM_VFS_GetMounts(void);
uint32_t FM_VFS_GetMountCount(void);

FM_Result FM_VFS_Resolve(const char *path, FM_MountEntry **mount,
                         char *rel_buf, size_t rel_buf_size);

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
/* Exclusive use of the scratch buffer.  Acquire returns NULL while the
 * buffer is already in use on this thread (nested call, e.g. from a
 * progress callback) -- callers report FM_ERR_BUSY.  Every successful
 * Acquire must be paired with exactly one Release. */
uint8_t *FM_InternalBufferAcquire(size_t *size);
void     FM_InternalBufferRelease(void);

/* FM_TRUE when both paths resolve to the same entry of the same mount. */
FM_Bool FM_File_SamePath(const char *a, const char *b);

#if FM_USE_IMAGE || FM_USE_MJPEG
/* Exclusive use of the still-image / MJPEG codec state: the PNG window
 * and row buffers, the BMP row scratch, the JPEG work buffer and the
 * file-static state of picojpeg (shared by JPEG stills and MJPEG video).
 *
 *   FM_USE_THREADS=1  blocks until the other thread is done, so two
 *                     tasks decoding at once are serialised.  Must not
 *                     be re-entered from the same thread (FM_SetIdle
 *                     hook, codec callback): the port mutexes are not
 *                     recursive.
 *   FM_USE_THREADS=0  the only way in twice is a nested call from such
 *                     a hook; it gets FM_ERR_BUSY and the outer codec
 *                     run is left intact.
 *
 * Every FM_OK from Enter must be paired with exactly one Leave. */
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

FM_END_DECLS

#endif /* FM_INTERNAL_H */