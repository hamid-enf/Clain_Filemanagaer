/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_stream.h - Generic stream abstraction
 *
 * Streams decouple media codecs and file editing from the filesystem.
 * A stream can wrap an open FM_File or a memory buffer.
 */

#ifndef FM_STREAM_H
#define FM_STREAM_H

#include "fm_types.h"
#include "fm_file.h"

FM_BEGIN_DECLS

typedef struct FM_Stream FM_Stream;

typedef FM_Result (*FM_StreamReadFn)(FM_Stream *s, void *buf, size_t len,
                                     size_t *read);
typedef FM_Result (*FM_StreamWriteFn)(FM_Stream *s, const void *buf,
                                      size_t len, size_t *written);
typedef FM_Result (*FM_StreamSeekFn)(FM_Stream *s, int64_t offset,
                                     FM_SeekOrigin origin);
typedef FM_Result (*FM_StreamTellFn)(FM_Stream *s, uint64_t *pos);
typedef FM_Result (*FM_StreamSizeFn)(FM_Stream *s, uint64_t *size);
typedef FM_Result (*FM_StreamFlushFn)(FM_Stream *s);
typedef FM_Bool   (*FM_StreamEofFn)(FM_Stream *s);
typedef void      (*FM_StreamCloseFn)(FM_Stream *s);

struct FM_Stream
{
    void *ctx;                       /* underlying object (FM_File etc) */
    uint8_t in_use;                  /* internal: context slot in use   */
    FM_StreamReadFn  read;
    FM_StreamWriteFn write;
    FM_StreamSeekFn  seek;
    FM_StreamTellFn  tell;
    FM_StreamSizeFn  size;
    FM_StreamFlushFn flush;
    FM_StreamEofFn   eof;
    FM_StreamCloseFn close;
};

FM_Result Stream_Read(FM_Stream *s, void *buf, size_t len, size_t *read);
FM_Result Stream_Write(FM_Stream *s, const void *buf, size_t len,
                       size_t *written);
FM_Result Stream_Seek(FM_Stream *s, int64_t offset, FM_SeekOrigin origin);
FM_Result Stream_Tell(FM_Stream *s, uint64_t *pos);
FM_Result Stream_Size(FM_Stream *s, uint64_t *size);
FM_Result Stream_Flush(FM_Stream *s);
FM_Bool  Stream_EOF(FM_Stream *s);
void     Stream_Close(FM_Stream *s);

/* Wrappers */
FM_Result Stream_InitFile(FM_Stream *s, FM_File *f);
FM_Result Stream_InitMemoryRead(FM_Stream *s, const void *data, size_t size);
FM_Result Stream_InitMemoryWrite(FM_Stream *s, void *data, size_t capacity);

FM_END_DECLS

#endif /* FM_STREAM_H */
