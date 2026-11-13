/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_json.h - JSON plugin (optional)
 *
 * A compact, buffer-based JSON reader and writer.  The document is
 * parsed into a token table stored in a user-provided buffer, so no
 * allocation happens at runtime.  Keys are addressed with dot paths:
 *
 *   "device.baudrate", "sensors[2].value"
 *
 * Reading:  Json_LoadFile("/config.json", &doc, tokens, size);
 *           Json_ReadInt(&doc, "device.baudrate", &baud);
 *
 * Writing:  JsonWriter_* builds a JSON string in a user buffer, then
 *           Json_SaveFile writes it atomically.
 */

#ifndef FM_JSON_H
#define FM_JSON_H

#include "fm_types.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Document model                                                      */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_JSON_NULL = 0,
    FM_JSON_OBJECT = 1,
    FM_JSON_ARRAY = 2,
    FM_JSON_STRING = 3,
    FM_JSON_NUMBER = 4,
    FM_JSON_BOOL = 5
} FM_JsonType;

/* One token table entry; the token buffer handed to Json_LoadFile /
 * Json_OpenBuffer must be a multiple of this size and at least 4-byte
 * aligned (declare it as uint32_t tokens[N*FM_JSON_TOKEN_SIZE/4]).
 * Misaligned or non-multiple buffers are rejected with
 * FM_ERR_INVALID_ARGUMENT, because unaligned 16-bit accesses hard-fault
 * on Cortex-M0-class cores. */
#define FM_JSON_TOKEN_SIZE 16u

typedef struct
{
    const char *text;        /* raw JSON text being parsed   */
    size_t      len;         /* length of the text           */
    void       *tokens;      /* internal token table         */
    size_t      token_cap;   /* capacity (in tokens)         */
    size_t      token_count; /* tokens actually used         */
    FM_Result   parse_result;
} FM_Json;

/* ------------------------------------------------------------------ */
/* Loading / saving                                                    */
/* ------------------------------------------------------------------ */
/* Parse text from a file.  The raw text is copied into text_buffer
 * (and stays referenced by doc->text while reading values), and the
 * token table is built inside token_buffer (token_buffer_size must be
 * a multiple of FM_JSON_TOKEN_SIZE, up to FM_JSON_MAX_TOKENS). */
FM_Result Json_LoadFile(const char *path, FM_Json *doc, char *text_buffer,
                        size_t text_buffer_size, void *token_buffer,
                        size_t token_buffer_size);
FM_Result Json_OpenBuffer(const char *text, size_t len, FM_Json *doc,
                          void *token_buffer, size_t token_buffer_size);

/* Write a JSON string to a file atomically (temp + rename). */
FM_Result Json_SaveFile(const char *path, const char *json_text);

/* Accessors; key = "a.b[2].c" ; returns FM_ERR_NOT_FOUND if absent. */
FM_Result Json_ReadString(const FM_Json *doc, const char *key, char *out,
                          size_t out_size);
FM_Result Json_ReadInt(const FM_Json *doc, const char *key, int32_t *out);
FM_Result Json_ReadUInt(const FM_Json *doc, const char *key, uint32_t *out);
FM_Result Json_ReadInt64(const FM_Json *doc, const char *key, int64_t *out);
FM_Result Json_ReadFloat(const FM_Json *doc, const char *key, float *out);
FM_Result Json_ReadDouble(const FM_Json *doc, const char *key, double *out);
FM_Result Json_ReadBool(const FM_Json *doc, const char *key, FM_Bool *out);
FM_Bool  Json_Exists(const FM_Json *doc, const char *key);

/* Arrays */
FM_Result Json_ArrayCount(const FM_Json *doc, const char *key, size_t *count);
FM_Result Json_ArrayString(const FM_Json *doc, const char *key, size_t index,
                           char *out, size_t out_size);
FM_Result Json_ArrayInt(const FM_Json *doc, const char *key, size_t index,
                        int32_t *out);
FM_Result Json_ArrayUInt(const FM_Json *doc, const char *key, size_t index,
                         uint32_t *out);
FM_Result Json_ArrayFloat(const FM_Json *doc, const char *key, size_t index,
                          float *out);
FM_Result Json_ArrayBool(const FM_Json *doc, const char *key, size_t index,
                         FM_Bool *out);

/* Convenience whole-config helpers.  Because the parse token table is
 * large (FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE = up to 4 KB), these
 * functions take the token buffer from the caller.  On a desktop host a
 * 4 KB stack frame is fine; on a Cortex-M the caller can place the
 * buffer in a global, a static pool, or external RAM (SDRAM/PSRAM) and
 * stay within its task stack budget.  The text buffer is similarly
 * caller-provided; pass the same size you would use with Json_LoadFile. */
FM_Result Json_LoadString(const char *path, const char *key, char *out,
                          size_t out_size,
                          char *text_buffer, size_t text_buffer_size,
                          void *token_buffer, size_t token_buffer_size);
FM_Result Json_LoadInt(const char *path, const char *key, int32_t *out,
                       char *text_buffer, size_t text_buffer_size,
                       void *token_buffer, size_t token_buffer_size);
FM_Result Json_LoadFloat(const char *path, const char *key, float *out,
                         char *text_buffer, size_t text_buffer_size,
                         void *token_buffer, size_t token_buffer_size);

/* Convenience macro to size a token buffer for the full default
 * capacity.  Declare as:  uint32_t tokens[FM_JSON_TOKEN_BUFFER_SIZE/4]; */
#define FM_JSON_TOKEN_BUFFER_SIZE \
    (FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE)

/* ------------------------------------------------------------------ */
/* Writer (builds JSON text in a user buffer)                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    char  *buf;
    size_t cap;
    size_t len;
    int    depth;
    FM_Bool first[8];     /* first-member tracking per depth */
    FM_Bool pending[8];   /* object key is waiting for a value  */
    FM_Bool root_written;
    uint8_t container[8]; /* FM_JsonType of each open container */
    FM_Result err;
} FM_JsonWriter;

FM_Result JsonWriter_Init(FM_JsonWriter *w, char *buffer, size_t cap);
FM_Result JsonWriter_BeginObject(FM_JsonWriter *w);
FM_Result JsonWriter_EndObject(FM_JsonWriter *w);
FM_Result JsonWriter_BeginArray(FM_JsonWriter *w);
FM_Result JsonWriter_EndArray(FM_JsonWriter *w);
FM_Result JsonWriter_Key(FM_JsonWriter *w, const char *key);
FM_Result JsonWriter_String(FM_JsonWriter *w, const char *value);
FM_Result JsonWriter_Int(FM_JsonWriter *w, int32_t value);
FM_Result JsonWriter_UInt(FM_JsonWriter *w, uint32_t value);
FM_Result JsonWriter_Int64(FM_JsonWriter *w, int64_t value);
/* 0..6 decimals.  NaN and +-infinity are written as null (JSON has no
 * number for them); a value beyond +-9e9 is written as +-8999999488,
 * the float next to 9e9. */
FM_Result JsonWriter_Float(FM_JsonWriter *w, float value, int decimals);
FM_Result JsonWriter_Bool(FM_JsonWriter *w, FM_Bool value);
FM_Result JsonWriter_Null(FM_JsonWriter *w);
const char *JsonWriter_StringValue(FM_JsonWriter *w);   /* NUL-terminated */
size_t JsonWriter_Length(FM_JsonWriter *w);

FM_END_DECLS

#endif /* FM_JSON_H */
