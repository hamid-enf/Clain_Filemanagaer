/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_async.h - Cooperative asynchronous operations (optional)
 *
 * Long operations (copy, move, recursive delete, scan) run as chunked
 * state machines driven by Async_Poll().  This requires no RTOS and no
 * threads: the caller invokes Poll() from its main loop / super-loop.
 *
 * With FM_USE_THREADS == 1 the same object can be driven from a worker
 * task; the core still never spawns its own threads.
 */

#ifndef FM_ASYNC_H
#define FM_ASYNC_H

#include "fm_types.h"
#include "fm_file.h"
#include "fm_directory.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_ASYNC_IDLE = 0,
    FM_ASYNC_RUNNING,
    FM_ASYNC_DONE,
    FM_ASYNC_CANCELLED,
    FM_ASYNC_ERROR
} FM_AsyncState;

typedef struct FM_Async FM_Async;

typedef void (*FM_AsyncDoneCb)(FM_Async *a, FM_Result result, void *user_data);
typedef void (*FM_AsyncProgressCb)(FM_Async *a, uint64_t current,
                                   uint64_t total, void *user_data);

struct FM_Async
{
    uint32_t      magic;          /* initialized by Async_Init        */
    FM_AsyncState state;
    uint8_t       op;             /* internal opcode                  */
    char          src[FM_PATH_MAX];
    char          dst[FM_PATH_MAX];
    uint64_t      total;
    uint64_t      current;
    FM_Result     result;
    FM_Bool       cancel;
    FM_AsyncDoneCb     done_cb;
    FM_AsyncProgressCb progress_cb;
    FM_ItemCallback    item_cb;
    void         *user_data;

    /* internal state */
    FM_File  srcf;
    FM_File  dstf;
    uint8_t  chunk[FM_ASYNC_CHUNK_SIZE];
    char     scan_path[FM_PATH_MAX];
    FM_Dir   scan_dir;
    FM_DirEntry scan_entry;
    char     stack[FM_DIR_MAX_DEPTH][FM_NAME_MAX]; /* dir-name stack */
    uint32_t scan_index[FM_DIR_MAX_DEPTH + 1];      /* entries consumed */
    uint32_t depth;
    uint64_t item_size;
    uint8_t  phase;
    uint8_t  temp_owned;
    char     temp_path[FM_PATH_MAX];
};

/* Initialize a user-owned async operation object. */
void Async_Init(FM_Async *a);

/* Start operations (returns FM_OK, state becomes RUNNING).
 * Copy and cross-mount move stage to an exclusively-created temporary file;
 * same-mount moves use the backend rename on the first poll. Cancel before
 * that poll to prevent it. Rename/sync failures have backend-dependent commit
 * status; failed publication may leave staging behind for inspection.
 * No crash-atomicity or external-actor protection is implied. */
FM_Result Async_StartCopy(FM_Async *a, const char *src, const char *dst,
                          FM_AsyncDoneCb done, FM_AsyncProgressCb progress,
                          void *user_data);
FM_Result Async_StartMove(FM_Async *a, const char *src, const char *dst,
                          FM_AsyncDoneCb done, FM_AsyncProgressCb progress,
                          void *user_data);
FM_Result Async_StartDeleteRecursive(FM_Async *a, const char *path,
                                     FM_AsyncDoneCb done,
                                     FM_AsyncProgressCb progress,
                                     void *user_data);
FM_Result Async_StartScan(FM_Async *a, const char *path,
                          FM_ItemCallback item_cb, FM_AsyncDoneCb done,
                          void *user_data);

/* Drive the operation.  Returns the current state.  Call repeatedly
 * until state != RUNNING. */
FM_AsyncState Async_Poll(FM_Async *a);

FM_Result Async_Cancel(FM_Async *a);
FM_Bool  Async_IsDone(FM_Async *a);
FM_AsyncState Async_GetState(const FM_Async *a);
FM_Result Async_GetResult(const FM_Async *a);
FM_Result Async_Reset(FM_Async *a);   /* back to IDLE, releases files */

FM_END_DECLS

#endif /* FM_ASYNC_H */
