/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_core.c - Framework init/deinit, version, error strings
 */

#define FM_DEBUG_TAG "core"
#include "fm_debug.h"
#include "file_manager.h"
#if FM_USE_FLAC
#include "fm_flac.h"
#endif
#if FM_USE_VORBIS
#include "fm_vorbis.h"
#endif
#if FM_USE_OPUS
#include "fm_opus.h"
#endif
#include "fm_cache.h"
#include "fm_internal.h"

#define FM_VERSION_MAJOR 1
#define FM_VERSION_MINOR 0
#define FM_VERSION_PATCH 1

static int32_t g_last_error = (int32_t)FM_OK;
static void (*g_idle)(void);
static uint8_t g_idle_busy;
static void (*g_busy_enter)(void);
static void (*g_busy_leave)(void);
static uint8_t g_busy_depth;

#if FM_USE_THREADS
static FM_Mutex g_core_lock;
static uint8_t  g_core_lock_ready = 0;
#endif

#if FM_USE_IMAGE || FM_USE_MJPEG
#if FM_USE_THREADS
static FM_Mutex g_codec_lock;
static uint8_t  g_codec_lock_ready = 0;
#else
static uint8_t  g_codec_busy = 0;
#endif

FM_Result FM_CodecScratch_Enter(void)
{
#if FM_USE_THREADS
    uint8_t ready;

    FM_Lock();
    if (!g_codec_lock_ready && FM_Mutex_Create(&g_codec_lock) == FM_OK)
    {
        g_codec_lock_ready = 1;
    }
    ready = g_codec_lock_ready;
    FM_Unlock();
    if (!ready)
    {
        return FM_ERR_INTERNAL;
    }
    FM_Mutex_Lock(&g_codec_lock);
    return FM_OK;
#else
    if (g_codec_busy != 0u)
    {
        return FM_ERR_BUSY;
    }
    g_codec_busy = 1u;
    return FM_OK;
#endif
}

void FM_CodecScratch_Leave(void)
{
#if FM_USE_THREADS
    FM_Mutex_Unlock(&g_codec_lock);
#else
    g_codec_busy = 0u;
#endif
}
#endif /* FM_USE_IMAGE || FM_USE_MJPEG */

uint32_t FileManager_GetVersion(void)
{
    return (uint32_t)((FM_VERSION_MAJOR << 16) |
                      (FM_VERSION_MINOR << 8) |
                      (FM_VERSION_PATCH));
}

const char *FileManager_GetVersionString(void)
{
    return "1.0.1";
}

void FileManager_Deinit(void)
{
#if FM_USE_THREADS
    if (g_core_lock_ready)
    {
        FM_Mutex_Destroy(&g_core_lock);
        g_core_lock_ready = 0;
    }
#if FM_USE_IMAGE || FM_USE_MJPEG
    if (g_codec_lock_ready)
    {
        FM_Mutex_Destroy(&g_codec_lock);
        g_codec_lock_ready = 0;
    }
#endif
#else
    /* nothing to do */
#endif
}

void FM_SetIdle(void (*fn)(void))
{
    g_idle = fn;
}

void FM_Idle(void)
{
    if (g_idle != NULL && g_idle_busy == 0u)
    {
        g_idle_busy = 1u;
        g_idle();
        g_idle_busy = 0u;
    }
}

void FM_SetBusyHooks(void (*enter)(void), void (*leave)(void))
{
    g_busy_enter = enter;
    g_busy_leave = leave;
}

void FM_BusyEnter(void)
{
    if (g_busy_depth == 0u && g_busy_enter != NULL)
    {
        g_busy_enter();
    }
    if (g_busy_depth != 255u)
    {
        g_busy_depth++;
    }
}

void FM_BusyLeave(void)
{
    if (g_busy_depth == 0u)
    {
        return;
    }
    g_busy_depth--;
    if (g_busy_depth == 0u && g_busy_leave != NULL)
    {
        g_busy_leave();
    }
}

FM_Result FileManager_Init(void)
{
    /*
     * فلگ FM_CACHE را اینجا اعمال کن. اینجا امن است چون تا وقتی
     * برنامه به این تابع برسد، MPU_Config() قطعاً اجرا شده.
     */
    FM_Cache_AutoInit();
    FM_EVENT_LOG("FileManager v%s init (port=%s, debug=%s)",
                 FileManager_GetVersionString(), FM_PORT_NAME,
                 FM_Debug_LevelName());

    /* Reset the global last-error slot. */
    g_last_error = (int32_t)FM_OK;

#if FM_USE_FLAC
    (void)FM_Flac_Init();
#endif
#if FM_USE_VORBIS
    (void)FM_Vorbis_Init();
#endif
#if FM_USE_OPUS
    (void)FM_Opus_Init();
#endif

#if FM_USE_THREADS
    if (!g_core_lock_ready)
    {
        FM_Result r = FM_Mutex_Create(&g_core_lock);
        if (r != FM_OK)
        {
            FM_ERR_LOG("mutex create failed (%d)", (int)r);
            return r;
        }
        g_core_lock_ready = 1;
    }
#else
    /* single threaded: nothing to initialize */
#endif

    return FM_OK;
}

/* Internal: record the last error (used by all modules). */
void FM_Core_SetLastError(FM_Result err)
{
#if FM_USE_THREADS
    if (g_core_lock_ready)
    {
        FM_Mutex_Lock(&g_core_lock);
    }
    g_last_error = (int32_t)err;
    if (g_core_lock_ready)
    {
        FM_Mutex_Unlock(&g_core_lock);
    }
#else
    g_last_error = (int32_t)err;
#endif
}

FM_Result File_GetLastError(void)
{
#if FM_USE_THREADS
    FM_Result err;
    if (g_core_lock_ready)
    {
        FM_Mutex_Lock(&g_core_lock);
    }
    err = (FM_Result)g_last_error;
    if (g_core_lock_ready)
    {
        FM_Mutex_Unlock(&g_core_lock);
    }
    return err;
#else
    return (FM_Result)g_last_error;
#endif
}

const char *FM_ResultString(FM_Result err)
{
    switch (err)
    {
    case FM_OK:                    return "OK";
    case FM_ERR_INVALID_ARGUMENT:  return "invalid argument";
    case FM_ERR_INVALID_PATH:      return "invalid path";
    case FM_ERR_NOT_FOUND:         return "not found";
    case FM_ERR_ACCESS:            return "access denied";
    case FM_ERR_NO_SPACE:          return "no space left";
    case FM_ERR_IO:                return "I/O error";
    case FM_ERR_TIMEOUT:           return "timeout";
    case FM_ERR_BUSY:              return "busy";
    case FM_ERR_EXISTS:            return "already exists";
    case FM_ERR_NOT_EMPTY:         return "directory not empty";
    case FM_ERR_UNSUPPORTED:       return "unsupported";
    case FM_ERR_CORRUPTED:         return "corrupted data";
    case FM_ERR_NOT_MOUNTED:       return "not mounted";
    case FM_ERR_NO_MEMORY:         return "out of memory";
    case FM_ERR_EOF:               return "end of file";
    case FM_ERR_TOO_LARGE:         return "too large for buffer";
    case FM_ERR_INVALID_FORMAT:    return "invalid format";
    case FM_ERR_CRC:               return "CRC mismatch";
    case FM_ERR_ABORTED:           return "operation aborted";
    case FM_ERR_READ_ONLY:         return "read-only";
    case FM_ERR_NOT_OPEN:          return "not open";
    case FM_ERR_IS_DIRECTORY:      return "is a directory";
    case FM_ERR_NOT_DIRECTORY:     return "not a directory";
    case FM_ERR_CANCELLED:         return "cancelled";
    case FM_ERR_NOT_READY:         return "not ready";
    case FM_ERR_INTERNAL:          return "internal error";
    default:                       return "unknown error";
    }
}

const char *File_GetErrorString(FM_Result err)
{
    return FM_ResultString(err);
}

FM_Result FileManager_MountMemory(FM_Storage *storage, FM_FS *fs,
                                  const char *name, void *address,
                                  uint64_t capacity, uint32_t block_size,
                                  FM_Bool read_only)
{
    FM_Result r;

    if (storage == NULL || fs == NULL || name == NULL || address == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = MemoryStorage_Init(storage, name, address, capacity, block_size,
                           read_only);
    if (r != FM_OK)
    {
        return r;
    }
    r = Storage_Register(storage);
    if (r != FM_OK)
    {
        return r;
    }
    r = Storage_Mount(name);
    if (r != FM_OK)
    {
        (void)Storage_Unregister(name);
        return r;
    }
#if FM_USE_FMFS
    r = FMFS_Init(fs);
    if (r != FM_OK)
    {
        (void)Storage_Unmount(name);
        (void)Storage_Unregister(name);
        return r;
    }
    r = FS_MountDefault(fs);
#else
    (void)Storage_Unmount(name);
    (void)Storage_Unregister(name);
    return FM_ERR_UNSUPPORTED;
#endif
    if (r != FM_OK)
    {
        (void)Storage_Unmount(name);
        (void)Storage_Unregister(name);
    }
    return r;
}

FM_Result FileManager_MountStorage(FM_Storage *storage, FM_FS *fs)
{
    if (storage == NULL || fs == NULL || storage->name[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
#if !FM_USE_FMFS
    return FM_ERR_UNSUPPORTED;
#else
    {
    FM_Storage *registered = NULL;
    FM_Bool registered_here = FM_FALSE;
    FM_Bool mounted_here = FM_FALSE;
    FM_Result r;

    r = Storage_Find(storage->name, &registered);
    if (r == FM_ERR_NOT_FOUND)
    {
        r = Storage_Register(storage);
        if (r != FM_OK)
        {
            return r;
        }
        registered_here = FM_TRUE;
    }
    else if (r != FM_OK || registered != storage)
    {
        return (r == FM_OK) ? FM_ERR_EXISTS : r;
    }

    if (!storage->mounted)
    {
        r = Storage_Mount(storage->name);
        if (r != FM_OK)
        {
            if (registered_here) (void)Storage_Unregister(storage->name);
            return r;
        }
        mounted_here = FM_TRUE;
    }
    r = FMFS_Init(fs);
    if (r == FM_OK)
    {
        r = FS_MountDefault(fs);
    }
    if (r != FM_OK)
    {
        if (mounted_here) (void)Storage_Unmount(storage->name);
        if (registered_here) (void)Storage_Unregister(storage->name);
    }
    return r;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Internal scratch buffer                                             */
/*                                                                     */
/* The buffer is handed out by pointer (FM_InternalBuffer) and used    */
/* across several calls, so it cannot be protected by a mutex around   */
/* the accessor.  In threaded builds each thread gets its own copy     */
/* (thread-local storage), which makes concurrent File_Copy /          */
/* File_Insert / Json_LoadString / audio recording safe.  GCC-style    */
/* TLS is available on STM32 (arm-none-eabi-gcc) and ESP32 (ESP-IDF).  */
/* ------------------------------------------------------------------ */
#if FM_USE_INTERNAL_BUFFER
#if FM_USE_THREADS && defined(__GNUC__)
static __thread uint8_t FM_DMA_ALIGNED g_io_buf[FM_INTERNAL_BUFFER_SIZE];
#else
static uint8_t FM_DMA_ALIGNED g_io_buf[FM_INTERNAL_BUFFER_SIZE];
#endif
#endif

uint8_t *FM_InternalBuffer(void)
{
#if FM_USE_INTERNAL_BUFFER
    return g_io_buf;
#else
    return NULL;
#endif
}

size_t FM_InternalBufferSize(void)
{
#if FM_USE_INTERNAL_BUFFER
    return sizeof(g_io_buf);
#else
    return 0;
#endif
}

/* In-use flag next to the buffer.  It has the same storage class as the
 * buffer: thread-local where the buffer is thread-local (each thread
 * owns its copy, so only recursion on that thread can collide), shared
 * otherwise (then the test-and-set runs under the core lock). */
#if FM_USE_INTERNAL_BUFFER
#if FM_USE_THREADS && defined(__GNUC__)
static __thread uint8_t g_io_buf_busy;
#define IO_BUF_LOCK()   ((void)0)
#define IO_BUF_UNLOCK() ((void)0)
#else
static uint8_t g_io_buf_busy;
#define IO_BUF_LOCK()   FM_Lock()
#define IO_BUF_UNLOCK() FM_Unlock()
#endif
#endif

uint8_t *FM_InternalBufferAcquire(size_t *size)
{
#if FM_USE_INTERNAL_BUFFER
    uint8_t *p = NULL;

    IO_BUF_LOCK();
    if (g_io_buf_busy == 0u)
    {
        g_io_buf_busy = 1u;
        p = g_io_buf;
    }
    IO_BUF_UNLOCK();
    if (p != NULL && size != NULL)
    {
        *size = sizeof(g_io_buf);
    }
    return p;
#else
    (void)size;
    return NULL;
#endif
}

void FM_InternalBufferRelease(void)
{
#if FM_USE_INTERNAL_BUFFER
    IO_BUF_LOCK();
    g_io_buf_busy = 0u;
    IO_BUF_UNLOCK();
#endif
}
