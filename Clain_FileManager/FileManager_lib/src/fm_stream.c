/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_stream.c - Generic stream abstraction
 */

#define FM_DEBUG_TAG "stream"
#include "fm_debug.h"
#include "fm_stream.h"
#include "fm_file.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_STREAM

#define FM_STREAM_POOL 4

/* ------------------------------------------------------------------ */
/* File-backed stream                                                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_Stream *stream; /* owning FM_Stream (NULL = free) */
    FM_File   *file;
    FM_Bool    owner;  /* close the file when the stream is closed */
    FM_Bool    busy;
} FM_FileStreamCtx;

static FM_FileStreamCtx g_file_pool[FM_STREAM_POOL];

static FM_Result fs_read(FM_Stream *s, void *buf, size_t len, size_t *read)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;
    uint32_t rd = 0;
    FM_Result r;

    if (len > 0xFFFFFFFFu)
    {
        len = 0xFFFFFFFFu;
    }
    r = File_Read(c->file, buf, (uint32_t)len, &rd);
    if (r == FM_ERR_EOF)
    {
        r = FM_OK; /* EOF is not an error for streams */
    }
    if (read != NULL)
    {
        *read = rd;
    }
    return r;
}

static FM_Result fs_write(FM_Stream *s, const void *buf, size_t len,
                          size_t *written)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;
    uint32_t wr = 0;
    FM_Result r;

    if (len > 0xFFFFFFFFu)
    {
        len = 0xFFFFFFFFu;
    }
    r = File_Write(c->file, buf, (uint32_t)len, &wr);
    if (written != NULL)
    {
        *written = wr;
    }
    return r;
}

static FM_Result fs_seek(FM_Stream *s, int64_t offset, FM_SeekOrigin origin)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    return File_Seek(c->file, offset, origin);
}

static FM_Result fs_tell(FM_Stream *s, uint64_t *pos)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    return File_Tell(c->file, pos);
}

static FM_Result fs_size(FM_Stream *s, uint64_t *size)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    return File_Size(c->file, size);
}

static FM_Result fs_flush(FM_Stream *s)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    return File_Flush(c->file);
}

static FM_Bool fs_eof(FM_Stream *s)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    return File_EOF(c->file);
}

static void fs_close(FM_Stream *s)
{
    FM_FileStreamCtx *c = (FM_FileStreamCtx *)s->ctx;

    if (c->owner)
    {
        (void)File_Close(c->file);
    }
    FM_Lock();
    c->stream = NULL;
    c->file = NULL;
    c->owner = FM_FALSE;
    c->busy = FM_FALSE;
    FM_Unlock();
    memset(s, 0, sizeof(*s));
}

/* Defined after g_mem_pool; see below. */
static FM_Bool stream_is_bound(const FM_Stream *s);

FM_Result Stream_InitFile(FM_Stream *s, FM_File *f)
{
    uint32_t i;
    FM_FileStreamCtx *ctx = NULL;

    if (s == NULL || f == NULL || !File_IsOpen(f))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (stream_is_bound(s))
    {
        /* Re-initializing an open stream would leak the occupied context
         * slot; require Stream_Close first. */
        return FM_ERR_BUSY;
    }
    memset(s, 0, sizeof(*s));
    FM_Lock();
    for (i = 0; i < FM_STREAM_POOL; i++)
    {
        if (!g_file_pool[i].busy)
        {
            ctx = &g_file_pool[i];
            ctx->busy = FM_TRUE;
                    break;
        }
    }
    FM_Unlock();
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    ctx->file = f;
    ctx->owner = FM_FALSE;
    s->ctx = ctx;
    s->read = fs_read;
    s->write = fs_write;
    s->seek = fs_seek;
    s->tell = fs_tell;
    s->size = fs_size;
    s->flush = fs_flush;
    s->eof = fs_eof;
    s->close = fs_close;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Memory-backed streams                                               */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_Stream    *stream;   /* owning FM_Stream (NULL = free) */
    const uint8_t *data;
    uint8_t *wdata;
    size_t size;
    size_t cap;
    size_t pos;
    FM_Bool writable;
    FM_Bool busy;
} FM_MemStreamCtx;

static FM_MemStreamCtx g_mem_pool[FM_STREAM_POOL];

/* Detect a stream object that is already bound to a context slot.  This
 * looks at the pools rather than the (possibly uninitialized) FM_Stream
 * memory, so it is safe for stack-allocated objects and catches a
 * re-init-without-close that would otherwise leak a slot. */
static FM_Bool stream_is_bound(const FM_Stream *s)
{
    uint32_t i;
    FM_Bool bound = FM_FALSE;

    /* the .stream back-pointers are written under FM_Lock by the
     * Stream_Init* functions and the close handlers of other tasks */
    FM_Lock();
    for (i = 0; i < FM_STREAM_POOL; i++)
    {
        if (g_file_pool[i].stream == s || g_mem_pool[i].stream == s)
        {
            bound = FM_TRUE;
            break;
        }
    }
    FM_Unlock();
    return bound;
}

static FM_Result ms_read(FM_Stream *s, void *buf, size_t len, size_t *read)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;
    size_t avail = (c->pos < c->size) ? c->size - c->pos : 0;
    size_t n = (len < avail) ? len : avail;

    if (read != NULL)
    {
        *read = 0;
    }
    if (n > 0)
    {
        memcpy(buf, c->data + c->pos, n);
        c->pos += n;
        if (read != NULL)
        {
            *read = n;
        }
    }
    return FM_OK;
}

static FM_Result ms_write(FM_Stream *s, const void *buf, size_t len,
                          size_t *written)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;
    size_t avail;

    if (written != NULL)
    {
        *written = 0;
    }
    if (!c->writable)
    {
        return FM_ERR_ACCESS;
    }
    if (c->pos > c->cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    avail = c->cap - c->pos;
    if (len > avail)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (len > 0)
    {
        memcpy(c->wdata + c->pos, buf, len);
        c->pos += len;
        if (c->pos > c->size)
        {
            c->size = c->pos;
        }
        if (written != NULL)
        {
            *written = len;
        }
    }
    return FM_OK;
}

static FM_Result ms_seek(FM_Stream *s, int64_t offset, FM_SeekOrigin origin)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;
    int64_t base = 0;

    if (origin == FM_SEEK_CUR)
    {
        if (c->pos > (size_t)INT64_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        base = (int64_t)c->pos;
    }
    else if (origin == FM_SEEK_END)
    {
        if (c->size > (size_t)INT64_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        base = (int64_t)c->size;
    }
    else if (origin != FM_SEEK_SET)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
    {
        return FM_ERR_TOO_LARGE;
    }
    if (base + offset < 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if ((uint64_t)(base + offset) > (uint64_t)SIZE_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (c->writable && (uint64_t)(base + offset) > (uint64_t)c->cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    c->pos = (size_t)(base + offset);
    return FM_OK;
}

static FM_Result ms_tell(FM_Stream *s, uint64_t *pos)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;

    *pos = c->pos;
    return FM_OK;
}

static FM_Result ms_size(FM_Stream *s, uint64_t *size)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;

    *size = c->size;
    return FM_OK;
}

static FM_Result ms_flush(FM_Stream *s)
{
    (void)s;
    return FM_OK;
}

static FM_Bool ms_eof(FM_Stream *s)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;

    return (c->pos >= c->size) ? FM_TRUE : FM_FALSE;
}

static void ms_close(FM_Stream *s)
{
    FM_MemStreamCtx *c = (FM_MemStreamCtx *)s->ctx;

    FM_Lock();
    c->stream = NULL;
    c->busy = FM_FALSE;
    FM_Unlock();
    memset(s, 0, sizeof(*s));
}

static FM_MemStreamCtx *mem_acquire(FM_Stream *s)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_STREAM_POOL; i++)
    {
        if (!g_mem_pool[i].busy)
        {
            g_mem_pool[i].busy = FM_TRUE;
            g_mem_pool[i].stream = s;
            FM_Unlock();
            return &g_mem_pool[i];
        }
    }
    FM_Unlock();
    return NULL;
}

FM_Result Stream_InitMemoryRead(FM_Stream *s, const void *data, size_t size)
{
    FM_MemStreamCtx *ctx;

    if (s == NULL || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (stream_is_bound(s))
    {
        return FM_ERR_BUSY; /* close the previous stream first */
    }
    memset(s, 0, sizeof(*s));
    ctx = mem_acquire(s);
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    ctx->data = (const uint8_t *)data;
    ctx->wdata = NULL;
    ctx->size = size;
    ctx->cap = size;
    ctx->pos = 0;
    ctx->writable = FM_FALSE;
    s->ctx = ctx;
    s->read = ms_read;
    s->write = ms_write;
    s->seek = ms_seek;
    s->tell = ms_tell;
    s->size = ms_size;
    s->flush = ms_flush;
    s->eof = ms_eof;
    s->close = ms_close;
    return FM_OK;
}

FM_Result Stream_InitMemoryWrite(FM_Stream *s, void *data, size_t capacity)
{
    FM_MemStreamCtx *ctx;

    if (s == NULL || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (stream_is_bound(s))
    {
        return FM_ERR_BUSY; /* close the previous stream first */
    }
    memset(s, 0, sizeof(*s));
    ctx = mem_acquire(s);
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    ctx->data = (const uint8_t *)data;
    ctx->wdata = (uint8_t *)data;
    ctx->size = 0;
    ctx->cap = capacity;
    ctx->pos = 0;
    ctx->writable = FM_TRUE;
    s->ctx = ctx;
    s->read = ms_read;
    s->write = ms_write;
    s->seek = ms_seek;
    s->tell = ms_tell;
    s->size = ms_size;
    s->flush = ms_flush;
    s->eof = ms_eof;
    s->close = ms_close;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Public wrappers                                                     */
/* ------------------------------------------------------------------ */
FM_Result Stream_Read(FM_Stream *s, void *buf, size_t len, size_t *read)
{
    if (s == NULL || s->read == NULL || buf == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->read(s, buf, len, read);
}

FM_Result Stream_Write(FM_Stream *s, const void *buf, size_t len,
                       size_t *written)
{
    if (s == NULL || s->write == NULL || buf == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->write(s, buf, len, written);
}

FM_Result Stream_Seek(FM_Stream *s, int64_t offset, FM_SeekOrigin origin)
{
    if (s == NULL || s->seek == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->seek(s, offset, origin);
}

FM_Result Stream_Tell(FM_Stream *s, uint64_t *pos)
{
    if (s == NULL || s->tell == NULL || pos == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->tell(s, pos);
}

FM_Result Stream_Size(FM_Stream *s, uint64_t *size)
{
    if (s == NULL || s->size == NULL || size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->size(s, size);
}

FM_Result Stream_Flush(FM_Stream *s)
{
    if (s == NULL || s->flush == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->flush(s);
}

FM_Bool Stream_EOF(FM_Stream *s)
{
    if (s == NULL || s->eof == NULL)
    {
        return FM_TRUE;
    }
    return s->eof(s);
}

void Stream_Close(FM_Stream *s)
{
    if (s != NULL && s->close != NULL)
    {
        s->close(s);
    }
}

#endif /* FM_USE_STREAM */
