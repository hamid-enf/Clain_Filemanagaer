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

/* ------------------------------------------------------------------ */
/* Scratch claims                                                      */
/*                                                                     */
/* A handful of large work areas exist once for the whole library: the */
/* File_Copy / File_Insert buffer, the still-image / MJPEG codec state */
/* and the transfer buffer.  A claim gives one user at a time          */
/* exclusive use of one of them.                                       */
/*                                                                     */
/* Two different things can ask for a claim that is already taken:     */
/*                                                                     */
/*   another task   -- it simply has to wait its turn;                 */
/*   the same task  -- a nested call from a progress callback, the     */
/*                     FM_SetIdle hook or a codec callback.  Waiting   */
/*                     would never end, so it gets FM_ERR_BUSY and the */
/*                     outer operation is left intact.                 */
/*                                                                     */
/* Telling them apart needs the identity of the calling thread, which  */
/* the sync port supplies (FM_SYNC_HAS_THREAD_SELF).  Thread-local     */
/* storage is deliberately not used: on bare-metal toolchains __thread */
/* is one block shared by every RTOS task.  A port without thread      */
/* identity, and every FM_USE_THREADS == 0 build, falls back to "never */
/* wait": whoever finds the claim taken gets FM_ERR_BUSY.              */
/* ------------------------------------------------------------------ */
#define FM_SCRATCH_BLOCKING (FM_USE_THREADS && FM_SYNC_HAS_THREAD_SELF)

typedef struct
{
#if FM_SCRATCH_BLOCKING
    FM_Mutex    mtx;
    FM_ThreadId owner;      /* valid while held != 0 */
    uint8_t     ready;
#endif
    uint8_t     held;
} FM_Scratch;

static FM_Scratch g_scratch[FM_SCRATCH_COUNT];

FM_Result FM_Scratch_Enter(FM_ScratchId id)
{
    FM_Scratch *c = &g_scratch[id];
    FM_Result r = FM_OK;

#if FM_SCRATCH_BLOCKING
    FM_ThreadId self = FM_Thread_Self();

    FM_Lock();
    if (!c->ready && FM_Mutex_Create(&c->mtx) == FM_OK)
    {
        c->ready = 1;
    }
    if (!c->ready)
    {
        r = FM_ERR_INTERNAL;
    }
    else if (c->held != 0u && FM_Thread_Equal(c->owner, self))
    {
        r = FM_ERR_BUSY;            /* nested on the owning thread */
    }
    FM_Unlock();
    if (r != FM_OK)
    {
        return r;
    }
    FM_Mutex_Lock(&c->mtx);         /* waits for another thread */
    FM_Lock();
    c->owner = self;
    c->held = 1u;
    FM_Unlock();
#else
    FM_Lock();
    if (c->held != 0u)
    {
        r = FM_ERR_BUSY;
    }
    else
    {
        c->held = 1u;
    }
    FM_Unlock();
#endif
    return r;
}

void FM_Scratch_Leave(FM_ScratchId id)
{
    FM_Scratch *c = &g_scratch[id];

    FM_Lock();
    c->held = 0u;
    FM_Unlock();
#if FM_SCRATCH_BLOCKING
    FM_Mutex_Unlock(&c->mtx);
#endif
}

#if FM_USE_IMAGE || FM_USE_MJPEG
FM_Result FM_CodecScratch_Enter(void)
{
    return FM_Scratch_Enter(FM_SCRATCH_CODEC);
}

void FM_CodecScratch_Leave(void)
{
    FM_Scratch_Leave(FM_SCRATCH_CODEC);
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
    /* Call once every task has stopped using the library. */
#if FM_USE_THREADS
#if FM_SCRATCH_BLOCKING
    {
        uint32_t i;

        for (i = 0; i < (uint32_t)FM_SCRATCH_COUNT; i++)
        {
            if (g_scratch[i].ready && g_scratch[i].held == 0u)
            {
                FM_Mutex_Destroy(&g_scratch[i].mtx);
                g_scratch[i].ready = 0;
            }
        }
    }
#endif
    FM_File_SyncDeinit();
    FM_Storage_SyncDeinit();
    FM_VFS_SyncDeinit();
    if (g_core_lock_ready)
    {
        FM_Mutex_Destroy(&g_core_lock);
        g_core_lock_ready = 0;
    }
#else
    /* nothing to do */
#endif
}

void FM_SetIdle(void (*fn)(void))
{
    FM_Lock();
    g_idle = fn;
    FM_Unlock();
}

void FM_Idle(void)
{
    void (*fn)(void);

    /* One run of the hook at a time: a nested call (the hook reaching a
     * decoder that polls FM_Idle again) and a call from a second task
     * while the hook is already running are both ignored.  The flag is
     * tested and set under the VFS lock so two tasks cannot both see it
     * clear; the hook itself runs with no lock held. */
    FM_Lock();
    fn = g_idle;
    if (fn != NULL && g_idle_busy == 0u)
    {
        g_idle_busy = 1u;
    }
    else
    {
        fn = NULL;
    }
    FM_Unlock();
    if (fn != NULL)
    {
        fn();
        FM_Lock();
        g_idle_busy = 0u;
        FM_Unlock();
    }
}

void FM_SetBusyHooks(void (*enter)(void), void (*leave)(void))
{
    FM_Lock();
    g_busy_enter = enter;
    g_busy_leave = leave;
    FM_Unlock();
}

/* The depth is counted under the VFS lock so that enter/leave pairs of
 * several tasks add up; the hooks run with no lock held. */
void FM_BusyEnter(void)
{
    void (*fn)(void) = NULL;

    FM_Lock();
    if (g_busy_depth == 0u)
    {
        fn = g_busy_enter;
    }
    if (g_busy_depth != 255u)
    {
        g_busy_depth++;
    }
    FM_Unlock();
    if (fn != NULL)
    {
        fn();
    }
}

void FM_BusyLeave(void)
{
    void (*fn)(void) = NULL;

    FM_Lock();
    if (g_busy_depth != 0u)
    {
        g_busy_depth--;
        if (g_busy_depth == 0u)
        {
            fn = g_busy_leave;
        }
    }
    FM_Unlock();
    if (fn != NULL)
    {
        fn();
    }
}

int FileManager_GetDebugLevel(void)
{
    return FM_DEBUG_LEVEL;
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

#if FM_USE_THREADS
    /* Create every lock now, while the application is still single
     * threaded.  (They are also created on first use, for applications
     * that never call this function -- but two tasks making their very
     * first FileManager call at the same instant could then each create
     * the mutex.  Call FileManager_Init() before starting tasks.) */
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
    FM_VFS_SyncInit();
    FM_Storage_SyncInit();
    FM_File_SyncInit();
#if FM_SCRATCH_BLOCKING
    {
        uint32_t i;

        FM_Lock();
        for (i = 0; i < (uint32_t)FM_SCRATCH_COUNT; i++)
        {
            if (!g_scratch[i].ready &&
                FM_Mutex_Create(&g_scratch[i].mtx) == FM_OK)
            {
                g_scratch[i].ready = 1;
            }
        }
        FM_Unlock();
    }
#endif
#else
    /* single threaded: nothing to initialize */
#endif

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

    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Last error                                                          */
/*                                                                     */
/* File_Open() / Directory_Open() return NULL and leave the reason     */
/* here, errno style.  One slot for the whole process is useless once  */
/* two tasks use the library: task A's failed open would read back     */
/* task B's FM_OK.  With a sync port that can name the calling thread, */
/* every thread therefore gets its own slot (the FM_LAST_ERROR_THREADS */
/* most recently active ones; an evicted or never-seen thread reads    */
/* the process-wide value, which is also what a port without thread    */
/* identity and every single-threaded build use).  No TLS involved.    */
/* ------------------------------------------------------------------ */
#if FM_USE_THREADS && FM_SYNC_HAS_THREAD_SELF && (FM_LAST_ERROR_THREADS > 0)
#define FM_LAST_ERROR_PER_THREAD 1
typedef struct
{
    FM_ThreadId owner;
    int32_t     err;
    uint32_t    stamp;      /* 0 = free; larger = used more recently */
} FM_LastErrorSlot;

static FM_LastErrorSlot g_last_error_tab[FM_LAST_ERROR_THREADS];
static uint32_t g_last_error_clock;
#else
#define FM_LAST_ERROR_PER_THREAD 0
#endif

/* Internal: record the last error (used by all modules). */
void FM_Core_SetLastError(FM_Result err)
{
#if FM_USE_THREADS
    if (g_core_lock_ready)
    {
        FM_Mutex_Lock(&g_core_lock);
    }
    g_last_error = (int32_t)err;
#if FM_LAST_ERROR_PER_THREAD
    {
        FM_ThreadId self = FM_Thread_Self();
        FM_LastErrorSlot *slot = NULL;
        uint32_t i;

        for (i = 0; i < (uint32_t)FM_LAST_ERROR_THREADS; i++)
        {
            FM_LastErrorSlot *t = &g_last_error_tab[i];

            if (t->stamp != 0u && FM_Thread_Equal(t->owner, self))
            {
                slot = t;
                break;
            }
            if (slot == NULL || t->stamp < slot->stamp)
            {
                slot = t;           /* free, else least recently used */
            }
        }
        g_last_error_clock++;
        if (g_last_error_clock == 0u)
        {
            /* wrapped: age everything to "old", keep 0 meaning free */
            for (i = 0; i < (uint32_t)FM_LAST_ERROR_THREADS; i++)
            {
                if (g_last_error_tab[i].stamp != 0u)
                {
                    g_last_error_tab[i].stamp = 1u;
                }
            }
            g_last_error_clock = 2u;
        }
        slot->owner = self;
        slot->err = (int32_t)err;
        slot->stamp = g_last_error_clock;
    }
#endif
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
#if FM_LAST_ERROR_PER_THREAD
    {
        FM_ThreadId self = FM_Thread_Self();
        uint32_t i;

        for (i = 0; i < (uint32_t)FM_LAST_ERROR_THREADS; i++)
        {
            if (g_last_error_tab[i].stamp != 0u &&
                FM_Thread_Equal(g_last_error_tab[i].owner, self))
            {
                err = (FM_Result)g_last_error_tab[i].err;
                break;
            }
        }
    }
#endif
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

    if (!FM_Storage_IsMounted(storage))
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
/* One buffer for the whole library, handed out by pointer and used    */
/* across several calls (File_Copy, File_Insert / DeleteRange /        */
/* ReplaceRange).  Exclusive use goes through the FM_SCRATCH_IOBUF     */
/* claim: a second task waits, a nested call on the same task gets     */
/* NULL (-> FM_ERR_BUSY).                                              */
/*                                                                     */
/* It used to be `__thread` in threaded builds.  That is one copy per  */
/* thread on a desktop OS only: on arm-none-eabi the TLS block is not  */
/* switched with the RTOS task (the link fails without                 */
/* __aeabi_read_tp, or every task shares one block), so two tasks      */
/* copying at once shared the buffer with nothing guarding it.         */
/* ------------------------------------------------------------------ */
#if FM_USE_INTERNAL_BUFFER
static uint8_t FM_DMA_ALIGNED g_io_buf[FM_INTERNAL_BUFFER_SIZE];
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

uint8_t *FM_InternalBufferAcquire(size_t *size)
{
#if FM_USE_INTERNAL_BUFFER
    if (FM_Scratch_Enter(FM_SCRATCH_IOBUF) != FM_OK)
    {
        return NULL;
    }
    if (size != NULL)
    {
        *size = sizeof(g_io_buf);
    }
    return g_io_buf;
#else
    (void)size;
    return NULL;
#endif
}

void FM_InternalBufferRelease(void)
{
#if FM_USE_INTERNAL_BUFFER
    FM_Scratch_Leave(FM_SCRATCH_IOBUF);
#endif
}
