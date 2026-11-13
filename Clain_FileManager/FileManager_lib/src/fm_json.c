/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_json.c - Compact buffer-based JSON parser / writer
 *
 * Token layout: one token per JSON value; tokens form a tree via
 * parent / first-child / next-sibling indices.  No allocation.
 */

#define FM_DEBUG_TAG "json"
#include "fm_debug.h"
#include "fm_json.h"
#include "fm_file.h"
#include "fm_safe.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_JSON

/* ------------------------------------------------------------------ */
/* Token table                                                         */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t  type;         /* FM_JsonType                    */
    uint8_t  parent;       /* parent token index, 0xFF = none */
    uint8_t  first_child;  /* 0xFF = none                     */
    uint8_t  next_sibling; /* 0xFF = none                     */
    uint16_t key_start;    /* offset of member key            */
    uint16_t key_len;
    uint16_t val_start;    /* offset of value (raw text)      */
    uint16_t val_len;
    uint8_t  pad[4];
} FM_JsonToken; /* sizeof == 16 == FM_JSON_TOKEN_SIZE */

#define NO_TOKEN ((uint8_t)0xFFu)

typedef struct
{
    const char *text;
    size_t      len;
    size_t      pos;
    FM_JsonToken *tokens;
    size_t      token_cap;
    size_t      token_count;
    FM_Result   err;
} Parser;

static uint8_t token_new(Parser *p)
{
    uint8_t idx;

    if (p->token_count >= p->token_cap || p->token_count >= 254)
    {
        p->err = FM_ERR_TOO_LARGE;
        return NO_TOKEN;
    }
    idx = (uint8_t)p->token_count;
    memset(&p->tokens[idx], 0, sizeof(FM_JsonToken));
    p->tokens[idx].parent = NO_TOKEN;
    p->tokens[idx].first_child = NO_TOKEN;
    p->tokens[idx].next_sibling = NO_TOKEN;
    p->token_count++;
    return idx;
}

static void token_link_child(FM_JsonToken *tokens, uint8_t parent,
                             uint8_t child)
{
    uint8_t last;

    tokens[child].parent = parent;
    last = tokens[parent].first_child;
    if (last == NO_TOKEN)
    {
        tokens[parent].first_child = child;
        return;
    }
    while (tokens[last].next_sibling != NO_TOKEN)
    {
        last = tokens[last].next_sibling;
    }
    tokens[last].next_sibling = child;
}

static void skip_ws(Parser *p)
{
    while (p->pos < p->len)
    {
        char c = p->text[p->pos];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            p->pos++;
        }
        else
        {
            break;
        }
    }
}

static FM_Bool peek(Parser *p, char c)
{
    return p->pos < p->len && p->text[p->pos] == c;
}

static FM_Result parse_value(Parser *p, uint8_t parent, int depth,
                             uint8_t *out_tok);

static FM_Result parse_string(Parser *p, uint8_t tok)
{
    size_t start;

    if (!peek(p, '"'))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    p->pos++;
    start = p->pos;
    while (p->pos < p->len)
    {
        char c = p->text[p->pos];

        if (c == '\\')
        {
            p->pos += 2;
            continue;
        }
        if (c == '"')
        {
            p->tokens[tok].val_start = (uint16_t)start;
            p->tokens[tok].val_len = (uint16_t)(p->pos - start);
            p->pos++;
            return FM_OK;
        }
        p->pos++;
    }
    return FM_ERR_INVALID_FORMAT;
}

static FM_Result parse_number(Parser *p, uint8_t tok)
{
    size_t start = p->pos;

    if (p->pos < p->len && p->text[p->pos] == '-')
    {
        p->pos++;
    }
    if (p->pos >= p->len)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (p->text[p->pos] == '0')
    {
        p->pos++;
    }
    else if (p->text[p->pos] >= '1' && p->text[p->pos] <= '9')
    {
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9')
        {
            p->pos++;
        }
    }
    else
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (p->pos < p->len && p->text[p->pos] == '.')
    {
        size_t fraction_start;

        p->pos++;
        fraction_start = p->pos;
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9')
        {
            p->pos++;
        }
        if (p->pos == fraction_start)
        {
            return FM_ERR_INVALID_FORMAT;
        }
    }
    if (p->pos < p->len && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E'))
    {
        size_t exponent_start;

        p->pos++;
        if (p->pos < p->len &&
            (p->text[p->pos] == '+' || p->text[p->pos] == '-'))
        {
            p->pos++;
        }
        exponent_start = p->pos;
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9')
        {
            p->pos++;
        }
        if (p->pos == exponent_start)
        {
            return FM_ERR_INVALID_FORMAT;
        }
    }
    p->tokens[tok].val_start = (uint16_t)start;
    p->tokens[tok].val_len = (uint16_t)(p->pos - start);
    return FM_OK;
}

static FM_Result parse_object(Parser *p, uint8_t tok, int depth)
{
    if (depth > 64)
    {
        return FM_ERR_TOO_LARGE;
    }
    p->pos++; /* '{' */
    skip_ws(p);
    if (peek(p, '}'))
    {
        p->pos++;
        return FM_OK;
    }
    for (;;)
    {
        uint8_t val_tok;
        size_t kstart;
        size_t klen;

        skip_ws(p);
        if (!peek(p, '"'))
        {
            return FM_ERR_INVALID_FORMAT;
        }
        p->pos++;
        kstart = p->pos;
        while (p->pos < p->len && p->text[p->pos] != '"')
        {
            if (p->text[p->pos] == '\\')
            {
                p->pos++;
            }
            p->pos++;
        }
        if (p->pos >= p->len)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        klen = p->pos - kstart;
        p->pos++; /* closing quote */
        skip_ws(p);
        if (!peek(p, ':'))
        {
            return FM_ERR_INVALID_FORMAT;
        }
        p->pos++;
        skip_ws(p);

        {
            FM_Result r = parse_value(p, tok, depth + 1, &val_tok);

            if (r != FM_OK)
            {
                return r;
            }
        }
        p->tokens[val_tok].key_start = (uint16_t)kstart;
        p->tokens[val_tok].key_len = (uint16_t)klen;

        skip_ws(p);
        if (peek(p, ','))
        {
            p->pos++;
            continue;
        }
        if (peek(p, '}'))
        {
            p->pos++;
            return FM_OK;
        }
        return FM_ERR_INVALID_FORMAT;
    }
}

static FM_Result parse_array(Parser *p, uint8_t tok, int depth)
{
    if (depth > 64)
    {
        return FM_ERR_TOO_LARGE;
    }
    p->pos++; /* '[' */
    skip_ws(p);
    if (peek(p, ']'))
    {
        p->pos++;
        return FM_OK;
    }
    for (;;)
    {
        uint8_t val_tok;
        FM_Result r;

        r = parse_value(p, tok, depth + 1, &val_tok);
        if (r != FM_OK)
        {
            return r;
        }
        skip_ws(p);
        if (peek(p, ','))
        {
            p->pos++;
            continue;
        }
        if (peek(p, ']'))
        {
            p->pos++;
            return FM_OK;
        }
        return FM_ERR_INVALID_FORMAT;
    }
}

static FM_Result parse_value(Parser *p, uint8_t parent, int depth,
                             uint8_t *out_tok)
{
    uint8_t tok;
    FM_Result r;

    skip_ws(p);
    if (p->pos >= p->len)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    tok = token_new(p);
    if (tok == NO_TOKEN)
    {
        return p->err;
    }

    switch (p->text[p->pos])
    {
    case '{':
        p->tokens[tok].type = (uint8_t)FM_JSON_OBJECT;
        r = parse_object(p, tok, depth);
        break;
    case '[':
        p->tokens[tok].type = (uint8_t)FM_JSON_ARRAY;
        r = parse_array(p, tok, depth);
        break;
    case '"':
        p->tokens[tok].type = (uint8_t)FM_JSON_STRING;
        r = parse_string(p, tok);
        break;
    case 't':
        if (p->len - p->pos >= 4 && strncmp(p->text + p->pos, "true", 4) == 0)
        {
            p->tokens[tok].type = (uint8_t)FM_JSON_BOOL;
            p->tokens[tok].val_start = (uint16_t)p->pos;
            p->tokens[tok].val_len = 4;
            p->pos += 4;
            r = FM_OK;
        }
        else
        {
            return FM_ERR_INVALID_FORMAT;
        }
        break;
    case 'f':
        if (p->len - p->pos >= 5 &&
            strncmp(p->text + p->pos, "false", 5) == 0)
        {
            p->tokens[tok].type = (uint8_t)FM_JSON_BOOL;
            p->tokens[tok].val_start = (uint16_t)p->pos;
            p->tokens[tok].val_len = 5;
            p->pos += 5;
            r = FM_OK;
        }
        else
        {
            return FM_ERR_INVALID_FORMAT;
        }
        break;
    case 'n':
        if (p->len - p->pos >= 4 && strncmp(p->text + p->pos, "null", 4) == 0)
        {
            p->tokens[tok].type = (uint8_t)FM_JSON_NULL;
            p->tokens[tok].val_start = (uint16_t)p->pos;
            p->tokens[tok].val_len = 4;
            p->pos += 4;
            r = FM_OK;
        }
        else
        {
            return FM_ERR_INVALID_FORMAT;
        }
        break;
    default:
        p->tokens[tok].type = (uint8_t)FM_JSON_NUMBER;
        r = parse_number(p, tok);
        break;
    }
    if (r != FM_OK)
    {
        return r;
    }
    if (parent != NO_TOKEN)
    {
        token_link_child(p->tokens, parent, tok);
    }
    if (out_tok != NULL)
    {
        *out_tok = tok;
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Public parse entry points                                           */
/* ------------------------------------------------------------------ */
FM_Result Json_OpenBuffer(const char *text, size_t len, FM_Json *doc,
                          void *token_buffer, size_t token_buffer_size)
{
    Parser p;

    if (text == NULL || doc == NULL || token_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* The token table contains 16-bit fields, so the buffer must be at
     * least 2-byte aligned.  Unaligned halfword access hard-faults on
     * Cortex-M0-class cores, so reject misaligned buffers up front. */
    if (((uintptr_t)token_buffer % 4u) != 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (token_buffer_size < FM_JSON_TOKEN_SIZE ||
        token_buffer_size % FM_JSON_TOKEN_SIZE != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (len == 0)
    {
        len = strlen(text);
    }
    if (len > UINT16_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }

    memset(doc, 0, sizeof(*doc));
    doc->text = text;
    doc->len = len;
    doc->tokens = token_buffer;
    doc->token_cap = token_buffer_size / FM_JSON_TOKEN_SIZE;
    if (doc->token_cap > FM_JSON_MAX_TOKENS)
    {
        doc->token_cap = FM_JSON_MAX_TOKENS;
    }

    memset(&p, 0, sizeof(p));
    p.text = text;
    p.len = len;
    p.tokens = (FM_JsonToken *)token_buffer;
    p.token_cap = doc->token_cap;
    p.err = FM_OK;

    {
        uint8_t root_tok;
        FM_Result r = parse_value(&p, NO_TOKEN, 0, &root_tok);

        doc->token_count = p.token_count;
        doc->parse_result = r;
        (void)root_tok;
        if (r != FM_OK)
        {
            return r;
        }
        skip_ws(&p);
        if (p.pos < p.len)
        {
            doc->parse_result = FM_ERR_INVALID_FORMAT;
            return FM_ERR_INVALID_FORMAT;
        }
    }
    return FM_OK;
}

FM_Result Json_LoadFile(const char *path, FM_Json *doc, char *text_buffer,
                        size_t text_buffer_size, void *token_buffer,
                        size_t token_buffer_size)
{
    size_t rd = 0;
    FM_Result r;

    if (path == NULL || doc == NULL || text_buffer == NULL ||
        text_buffer_size == 0 || token_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_ReadAll(path, text_buffer, text_buffer_size - 1, &rd);
    if (r != FM_OK)
    {
        return r;
    }
    text_buffer[rd] = '\0';
    return Json_OpenBuffer(text_buffer, rd, doc, token_buffer,
                           token_buffer_size);
}

FM_Result Json_SaveFile(const char *path, const char *json_text)
{
#if FM_USE_SAFE_WRITE
    return File_SafeWriteText(path, json_text);
#else
    /* Without the safe-write module fall back to a plain write. */
    if (json_text == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return File_WriteAll(path, json_text, strlen(json_text));
#endif
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */
static const FM_JsonToken *token_root(const FM_Json *doc)
{
    return (const FM_JsonToken *)doc->tokens;
}

static const FM_JsonToken *token_find_member(const FM_Json *doc,
                                             const FM_JsonToken *parent,
                                             const char *name,
                                             size_t name_len)
{
    const FM_JsonToken *tokens = token_root(doc);
    uint8_t child;

    if (parent == NULL || parent->first_child == NO_TOKEN)
    {
        return NULL;
    }
    child = parent->first_child;
    while (child != NO_TOKEN)
    {
        if (tokens[child].key_len == name_len &&
            strncmp(doc->text + tokens[child].key_start, name, name_len) == 0)
        {
            return &tokens[child];
        }
        child = tokens[child].next_sibling;
    }
    return NULL;
}

static const FM_JsonToken *token_find_index(const FM_Json *doc,
                                            const FM_JsonToken *array,
                                            size_t index)
{
    const FM_JsonToken *tokens = token_root(doc);
    uint8_t child;
    size_t n = 0;

    if (array == NULL || array->type != (uint8_t)FM_JSON_ARRAY)
    {
        return NULL;
    }
    child = array->first_child;
    while (child != NO_TOKEN)
    {
        if (n == index)
        {
            return &tokens[child];
        }
        n++;
        child = tokens[child].next_sibling;
    }
    return NULL;
}

/* Resolve a dotted path with optional [n] indices, e.g.
 * "sensor[2].calibration".  out_token receives the value token. */
static FM_Result token_resolve(const FM_Json *doc, const char *key,
                               const FM_JsonToken **out_token)
{
    const FM_JsonToken *tokens;
    const FM_JsonToken *cur;
    const char *p;
    char name[FM_NAME_MAX];

    if (doc == NULL || key == NULL || out_token == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (doc->tokens == NULL || doc->token_count == 0 || key[0] == '\0')
    {
        return FM_ERR_NOT_FOUND;
    }
    tokens = token_root(doc);
    cur = &tokens[0];
    p = key;

    while (*p != '\0')
    {
        if (*p == '[')
        {
            size_t idx = 0;

            p++;
            if (*p < '0' || *p > '9')
            {
                return FM_ERR_INVALID_PATH;
            }
            while (*p >= '0' && *p <= '9')
            {
                size_t digit = (size_t)(*p - '0');

                /* an index too large for size_t names no element; it
                 * used to wrap around, so "a[4294967296]" on a 32-bit
                 * target was "a[0]" */
                if (idx > ((size_t)-1 - digit) / 10u)
                {
                    return FM_ERR_NOT_FOUND;
                }
                idx = idx * 10u + digit;
                p++;
            }
            if (*p != ']')
            {
                return FM_ERR_INVALID_PATH;
            }
            p++;
            cur = token_find_index(doc, cur, idx);
            if (cur == NULL)
            {
                return FM_ERR_NOT_FOUND;
            }
            if (*p == '\0')
            {
                break;
            }
            if (*p != '.')
            {
                return FM_ERR_INVALID_PATH;
            }
            p++;
            if (*p == '\0')
            {
                return FM_ERR_INVALID_PATH;
            }
            continue;
        }
        else
        {
            size_t n = 0;

            while (*p != '\0' && *p != '.' && *p != '[' && n + 1 <
                   sizeof(name))
            {
                name[n++] = *p;
                p++;
            }
            if (n == 0)
            {
                return FM_ERR_INVALID_PATH;
            }
            if (n + 1 >= sizeof(name) && *p != '\0' && *p != '.' &&
                *p != '[')
            {
                return FM_ERR_TOO_LARGE;
            }
            name[n] = '\0';
            cur = token_find_member(doc, cur, name, n);
            if (cur == NULL)
            {
                return FM_ERR_NOT_FOUND;
            }
            if (*p == '\0')
            {
                break;
            }
            if (*p == '.')
            {
                p++;
                if (*p == '\0')
                {
                    return FM_ERR_INVALID_PATH;
                }
            }
        }
    }

    *out_token = cur;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Value extraction                                                    */
/* ------------------------------------------------------------------ */
static FM_Result token_string_value(const FM_Json *doc,
                                    const FM_JsonToken *tok, char *out,
                                    size_t out_size)
{
    size_t i;
    size_t n = 0;
    size_t src = tok->val_start;
    size_t end = src + tok->val_len;
    FM_Bool truncated = FM_FALSE;

    if (tok->type != (uint8_t)FM_JSON_STRING)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (i = src; i < end; i++)
    {
        char c = doc->text[i];

        if (c == '\\' && i + 1 < end)
        {
            i++;
            switch (doc->text[i])
            {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            default:  c = doc->text[i]; break;
            }
        }
        if (n + 1 < out_size)
        {
            out[n++] = c;
        }
        else
        {
            truncated = FM_TRUE;
        }
    }
    out[n] = '\0';
    return truncated ? FM_ERR_TOO_LARGE : FM_OK;
}

static FM_Bool parse_int_text(const char *text, size_t len, int64_t *out)
{
    uint64_t value = 0;
    uint64_t limit;
    size_t i = 0;
    FM_Bool negative = FM_FALSE;

    if (text == NULL || out == NULL || len == 0)
    {
        return FM_FALSE;
    }
    if (text[0] == '-')
    {
        negative = FM_TRUE;
        i = 1;
    }
    if (i == len)
    {
        return FM_FALSE;
    }
    limit = negative ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    for (; i < len; i++)
    {
        uint64_t digit;

        if (text[i] < '0' || text[i] > '9')
        {
            return FM_FALSE;
        }
        digit = (uint64_t)(text[i] - '0');
        if (value > (limit - digit) / 10u)
        {
            return FM_FALSE;
        }
        value = value * 10u + digit;
    }
    if (negative)
    {
        if (value == (uint64_t)INT64_MAX + 1u)
        {
            *out = INT64_MIN;
        }
        else
        {
            *out = -(int64_t)value;
        }
    }
    else
    {
        *out = (int64_t)value;
    }
    return FM_TRUE;
}

static FM_Bool parse_float_text(const char *text, size_t len, double *out)
{
    /* Deterministic, allocation-free decimal parser (no strtod). */
    double v = 0.0;
    double frac = 0.1;
    FM_Bool neg = FM_FALSE;
    size_t i = 0;

    if (len == 0)
    {
        return FM_FALSE;
    }
    if (text[0] == '-')
    {
        neg = FM_TRUE;
        i = 1;
    }
    for (; i < len; i++)
    {
        char c = text[i];

        if (c >= '0' && c <= '9')
        {
            v = v * 10.0 + (double)(c - '0');
        }
        else if (c == '.')
        {
            i++;
            while (i < len && text[i] >= '0' && text[i] <= '9')
            {
                v += (double)(text[i] - '0') * frac;
                frac *= 0.1;
                i++;
            }
            break;
        }
        else
        {
            break;
        }
    }
    /* exponent */
    for (; i < len; i++)
    {
        if (text[i] == 'e' || text[i] == 'E')
        {
            int64_t exp = 0;
            FM_Bool eneg = FM_FALSE;
            size_t j = i + 1;

            if (j < len && (text[j] == '+' || text[j] == '-'))
            {
                eneg = (text[j] == '-');
                j++;
            }
            while (j < len && text[j] >= '0' && text[j] <= '9')
            {
                /* Accumulate in 64-bit and clamp early so a hostile
                 * exponent like 1e999999999999 can neither overflow the
                 * counter nor spin the scaling loops below. */
                if (exp < 100000)
                {
                    exp = exp * 10 + (int64_t)(text[j] - '0');
                }
                j++;
            }
            if (eneg)
            {
                exp = -exp;
            }
            /* Beyond these bounds every double is inf (or denormal);
             * saturating keeps the behavior deterministic. */
            if (exp > 308)
            {
                exp = 308;
            }
            else if (exp < -324)
            {
                exp = -324;
            }
            while (exp > 0)
            {
                v *= 10.0;
                exp--;
            }
            while (exp < 0)
            {
                v *= 0.1;
                exp++;
            }
            break;
        }
    }
    *out = neg ? -v : v;
    return FM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */
FM_Result Json_ReadString(const FM_Json *doc, const char *key, char *out,
                          size_t out_size)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (doc == NULL || key == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    return token_string_value(doc, tok, out, out_size);
}

FM_Result Json_ReadInt(const FM_Json *doc, const char *key, int32_t *out)
{
    int64_t v;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Json_ReadInt64(doc, key, &v);

    if (r == FM_OK)
    {
        if (v < INT32_MIN || v > INT32_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        *out = (int32_t)v;
    }
    return r;
}

FM_Result Json_ReadUInt(const FM_Json *doc, const char *key, uint32_t *out)
{
    int64_t v;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Json_ReadInt64(doc, key, &v);
    if (r == FM_OK)
    {
        if (v < 0 || (uint64_t)v > UINT32_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        *out = (uint32_t)v;
    }
    return r;
}

FM_Result Json_ReadInt64(const FM_Json *doc, const char *key, int64_t *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (doc == NULL || key == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_NUMBER)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (!parse_int_text(doc->text + tok->val_start, tok->val_len, out))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    return FM_OK;
}

FM_Result Json_ReadFloat(const FM_Json *doc, const char *key, float *out)
{
    double v;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Json_ReadDouble(doc, key, &v);

    if (r == FM_OK)
    {
        *out = (float)v;
    }
    return r;
}

FM_Result Json_ReadDouble(const FM_Json *doc, const char *key, double *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (doc == NULL || key == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_NUMBER)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (!parse_float_text(doc->text + tok->val_start, tok->val_len, out))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    return FM_OK;
}

FM_Result Json_ReadBool(const FM_Json *doc, const char *key, FM_Bool *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (doc == NULL || key == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_BOOL)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    *out = (tok->val_len == 4) ? FM_TRUE : FM_FALSE;
    return FM_OK;
}

FM_Bool Json_Exists(const FM_Json *doc, const char *key)
{
    const FM_JsonToken *tok;

    return token_resolve(doc, key, &tok) == FM_OK;
}

/* ------------------------------------------------------------------ */
/* Arrays                                                              */
/* ------------------------------------------------------------------ */
static FM_Result json_array_token(const FM_Json *doc, const char *key,
                                  size_t index, const FM_JsonToken **out)
{
    const FM_JsonToken *arr;
    FM_Result r;

    if (doc == NULL || key == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &arr);
    if (r != FM_OK)
    {
        return r;
    }
    if (arr->type != (uint8_t)FM_JSON_ARRAY)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    *out = token_find_index(doc, arr, index);
    if (*out == NULL)
    {
        return FM_ERR_NOT_FOUND;
    }
    return FM_OK;
}

FM_Result Json_ArrayCount(const FM_Json *doc, const char *key, size_t *count)
{
    const FM_JsonToken *arr;
    const FM_JsonToken *tokens;
    uint8_t child;
    size_t n = 0;
    FM_Result r;

    if (doc == NULL || key == NULL || count == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = token_resolve(doc, key, &arr);
    if (r != FM_OK)
    {
        return r;
    }
    if (arr->type != (uint8_t)FM_JSON_ARRAY)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    tokens = token_root(doc);
    child = arr->first_child;
    while (child != NO_TOKEN)
    {
        n++;
        child = tokens[child].next_sibling;
    }
    *count = n;
    return FM_OK;
}

FM_Result Json_ArrayString(const FM_Json *doc, const char *key, size_t index,
                           char *out, size_t out_size)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = json_array_token(doc, key, index, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    return token_string_value(doc, tok, out, out_size);
}

FM_Result Json_ArrayInt(const FM_Json *doc, const char *key, size_t index,
                        int32_t *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = json_array_token(doc, key, index, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_NUMBER)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    {
        int64_t v;

        if (!parse_int_text(doc->text + tok->val_start, tok->val_len, &v))
        {
            return FM_ERR_INVALID_FORMAT;
        }
        if (v < INT32_MIN || v > INT32_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        *out = (int32_t)v;
    }
    return FM_OK;
}

FM_Result Json_ArrayUInt(const FM_Json *doc, const char *key, size_t index,
                         uint32_t *out)
{
    const FM_JsonToken *tok;
    int64_t value;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = json_array_token(doc, key, index, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_NUMBER ||
        !parse_int_text(doc->text + tok->val_start, tok->val_len, &value))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (value < 0 || (uint64_t)value > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    *out = (uint32_t)value;
    return FM_OK;
}

FM_Result Json_ArrayFloat(const FM_Json *doc, const char *key, size_t index,
                          float *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = json_array_token(doc, key, index, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_NUMBER)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    {
        double v;

        if (!parse_float_text(doc->text + tok->val_start, tok->val_len, &v))
        {
            return FM_ERR_INVALID_FORMAT;
        }
        *out = (float)v;
    }
    return FM_OK;
}

FM_Result Json_ArrayBool(const FM_Json *doc, const char *key, size_t index,
                         FM_Bool *out)
{
    const FM_JsonToken *tok;
    FM_Result r;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = json_array_token(doc, key, index, &tok);
    if (r != FM_OK)
    {
        return r;
    }
    if (tok->type != (uint8_t)FM_JSON_BOOL)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    *out = (tok->val_len == 4) ? FM_TRUE : FM_FALSE;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Convenience helpers backed by the same full parser as the advanced
 * API.  This keeps the one-call API predictable for dotted paths and
 * array indices instead of doing a fragile textual search. */
static FM_Result json_load_convenience(const char *path, FM_Json *doc,
                                       char *text, size_t text_size,
                                       void *tokens, size_t token_size)
{
    size_t rd = 0;
    FM_Result r;

    if (path == NULL || doc == NULL || text == NULL || text_size < 2 ||
        tokens == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_ReadAll(path, text, text_size - 1, &rd);
    if (r != FM_OK)
    {
        return r;
    }
    text[rd] = '\0';
    return Json_OpenBuffer(text, rd, doc, tokens, token_size);
}

FM_Result Json_LoadString(const char *path, const char *key, char *out,
                          size_t out_size,
                          char *text_buffer, size_t text_buffer_size,
                          void *token_buffer, size_t token_buffer_size)
{
    FM_Json doc;
    FM_Result r;

    r = json_load_convenience(path, &doc, text_buffer, text_buffer_size,
                              token_buffer, token_buffer_size);
    if (r != FM_OK)
    {
        return r;
    }
    return Json_ReadString(&doc, key, out, out_size);
}

FM_Result Json_LoadInt(const char *path, const char *key, int32_t *out,
                       char *text_buffer, size_t text_buffer_size,
                       void *token_buffer, size_t token_buffer_size)
{
    FM_Json doc;
    FM_Result r;

    r = json_load_convenience(path, &doc, text_buffer, text_buffer_size,
                              token_buffer, token_buffer_size);
    if (r != FM_OK)
    {
        return r;
    }
    return Json_ReadInt(&doc, key, out);
}

FM_Result Json_LoadFloat(const char *path, const char *key, float *out,
                         char *text_buffer, size_t text_buffer_size,
                         void *token_buffer, size_t token_buffer_size)
{
    FM_Json doc;
    FM_Result r;

    r = json_load_convenience(path, &doc, text_buffer, text_buffer_size,
                              token_buffer, token_buffer_size);
    if (r != FM_OK)
    {
        return r;
    }
    return Json_ReadFloat(&doc, key, out);
}

/* Writer                                                              */
/* ------------------------------------------------------------------ */
FM_Result JsonWriter_Init(FM_JsonWriter *w, char *buffer, size_t cap)
{
    if (w == NULL || buffer == NULL || cap == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(w, 0, sizeof(*w));
    w->buf = buffer;
    w->cap = cap;
    w->len = 0;
    w->err = FM_OK;
    buffer[0] = '\0';
    return FM_OK;
}

static FM_Result w_put(FM_JsonWriter *w, char c)
{
    if (w->err != FM_OK)
    {
        return w->err;
    }
    if (w->len + 1 >= w->cap)
    {
        w->err = FM_ERR_TOO_LARGE;
        return w->err;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
    return FM_OK;
}

static FM_Result w_put_str(FM_JsonWriter *w, const char *s)
{
    while (*s != '\0')
    {
        if (w_put(w, *s) != FM_OK)
        {
            return w->err;
        }
        s++;
    }
    return FM_OK;
}

/* Called before any value (including nested objects/arrays).  In an
 * array context it emits the element separator; in an object context
 * the separator was already emitted by JsonWriter_Key. */
static FM_Result w_value_start(FM_JsonWriter *w)
{
    int d = w->depth;

    if (d == 0)
    {
        if (w->root_written)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        w->root_written = FM_TRUE;
        return FM_OK;
    }
    if (w->container[d - 1] == (uint8_t)FM_JSON_ARRAY)
    {
        if (!w->first[d - 1])
        {
            if (w_put(w, ',') != FM_OK)
            {
                return w->err;
            }
        }
        w->first[d - 1] = FM_FALSE;
    }
    else
    {
        if (!w->pending[d - 1])
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        w->pending[d - 1] = FM_FALSE;
    }
    return FM_OK;
}

FM_Result JsonWriter_BeginObject(FM_JsonWriter *w)
{
    if (w == NULL || w->depth >= 8)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }

    if (w_put(w, '{') != FM_OK)
    {
        return w->err;
    }
    w->container[w->depth] = (uint8_t)FM_JSON_OBJECT;
    w->first[w->depth] = FM_TRUE;
    w->depth++;
    return FM_OK;
}

FM_Result JsonWriter_EndObject(FM_JsonWriter *w)
{
    if (w == NULL || w->depth == 0 ||
        w->container[w->depth - 1] != (uint8_t)FM_JSON_OBJECT ||
        w->pending[w->depth - 1])
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    w->depth--;
    if (w_put(w, '}') != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

FM_Result JsonWriter_BeginArray(FM_JsonWriter *w)
{
    if (w == NULL || w->depth >= 8)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }

    if (w_put(w, '[') != FM_OK)
    {
        return w->err;
    }
    w->container[w->depth] = (uint8_t)FM_JSON_ARRAY;
    w->first[w->depth] = FM_TRUE;
    w->depth++;
    return FM_OK;
}

FM_Result JsonWriter_EndArray(FM_JsonWriter *w)
{
    if (w == NULL || w->depth == 0 ||
        w->container[w->depth - 1] != (uint8_t)FM_JSON_ARRAY)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    w->depth--;
    if (w_put(w, ']') != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

FM_Result JsonWriter_Key(FM_JsonWriter *w, const char *key)
{
    size_t i;
    int d;

    if (w == NULL || key == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    d = w->depth;
    if (d == 0 || w->container[d - 1] != (uint8_t)FM_JSON_OBJECT ||
        w->pending[d - 1])
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!w->first[d - 1])
    {
        (void)w_put(w, ',');
    }
    w->first[d - 1] = FM_FALSE;
    w->pending[d - 1] = FM_TRUE;
    if (w->err != FM_OK)
    {
        return w->err;
    }
    if (w_put(w, '"') != FM_OK)
    {
        return w->err;
    }
    for (i = 0; key[i] != '\0'; i++)
    {
        if ((unsigned char)key[i] < 0x20u)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        if (key[i] == '"' || key[i] == '\\')
        {
            if (w_put(w, '\\') != FM_OK)
            {
                return w->err;
            }
        }
        if (w_put(w, key[i]) != FM_OK)
        {
            return w->err;
        }
    }
    if (w_put(w, '"') != FM_OK || w_put(w, ':') != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

FM_Result JsonWriter_String(FM_JsonWriter *w, const char *value)
{
    size_t i;

    if (w == NULL || value == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    if (w_put(w, '"') != FM_OK)
    {
        return w->err;
    }
    for (i = 0; value[i] != '\0'; i++)
    {
        char c = value[i];

        if (c == '"' || c == '\\')
        {
            if (w_put(w, '\\') != FM_OK)
            {
                return w->err;
            }
        }
        else if (c == '\n')
        {
            if (w_put(w, '\\') != FM_OK || w_put(w, 'n') != FM_OK)
            {
                return w->err;
            }
            continue;
        }
        else if (c == '\r')
        {
            if (w_put(w, '\\') != FM_OK || w_put(w, 'r') != FM_OK)
            {
                return w->err;
            }
            continue;
        }
        else if (c == '\t')
        {
            if (w_put(w, '\\') != FM_OK || w_put(w, 't') != FM_OK)
            {
                return w->err;
            }
            continue;
        }
        else if (c == '\b' || c == '\f')
        {
            if (w_put(w, '\\') != FM_OK ||
                w_put(w, (c == '\b') ? 'b' : 'f') != FM_OK)
            {
                return w->err;
            }
            continue;
        }
        else if ((unsigned char)c < 0x20u)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        if (w_put(w, c) != FM_OK)
        {
            return w->err;
        }
    }
    if (w_put(w, '"') != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

static void u32_to_str(uint32_t v, char *out)
{
    char tmp[12];
    int n = 0;

    if (v == 0)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0)
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    {
        int i;

        for (i = 0; i < n; i++)
        {
            out[i] = tmp[n - 1 - i];
        }
        out[n] = '\0';
    }
}

FM_Result JsonWriter_Int(FM_JsonWriter *w, int32_t value)
{
    char buf[16];

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    if (value < 0)
    {
        if (w_put(w, '-') != FM_OK)
        {
            return w->err;
        }
        u32_to_str((uint32_t)(-(int64_t)value), buf);
    }
    else
    {
        u32_to_str((uint32_t)value, buf);
    }
    if (w_put_str(w, buf) != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

FM_Result JsonWriter_UInt(FM_JsonWriter *w, uint32_t value)
{
    char buf[16];

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    u32_to_str(value, buf);
    if (w_put_str(w, buf) != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

/* FLT_MAX without <float.h> */
#define FM_JSON_FLT_MAX 3.402823466e+38f

static void u64_to_str(uint64_t v, char *out)
{
    char tmp[24];
    int n = 0;

    if (v == 0)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0)
    {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    {
        int i;

        for (i = 0; i < n; i++)
        {
            out[i] = tmp[n - 1 - i];
        }
        out[n] = '\0';
    }
}

FM_Result JsonWriter_Int64(FM_JsonWriter *w, int64_t value)
{
    uint64_t uv;
    char buf[24];

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    if (value < 0)
    {
        if (w_put(w, '-') != FM_OK)
        {
            return w->err;
        }
        uv = (uint64_t)0 - (uint64_t)value;
    }
    else
    {
        uv = (uint64_t)value;
    }
    u64_to_str(uv, buf);
    if (w_put_str(w, buf) != FM_OK)
    {
        return w->err;
    }
    return FM_OK;
}

FM_Result JsonWriter_Float(FM_JsonWriter *w, float value, int decimals)
{
    double mult = 1.0;
    double scaled_d;
    int64_t scaled;
    int64_t div = 1;
    int64_t ip;
    int64_t fp;
    char int_s[24];
    char fp_s[16];
    int d;
    int i;

    if (w == NULL || decimals < 0 || decimals > 6)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    /* JSON has no number for "not a number" or for infinity; the value
     * is written as null, which is what every reader expects there.
     * (The cast to an integer below is undefined for them: NaN came out
     * as 1511828489.4294966488, infinity as a finite number.) */
    if (value != value || value > FM_JSON_FLT_MAX || value < -FM_JSON_FLT_MAX)
    {
        (void)w_put_str(w, "null");
        return w->err;
    }
    if (value < 0)
    {
        if (w_put(w, '-') != FM_OK)
        {
            return w->err;
        }
        value = -value;
    }
    if (value > 9000000000.0f)
    {
        value = 9000000000.0f; /* clamp: keep scaled value in int64 range */
    }
    for (i = 0; i < decimals; i++)
    {
        mult *= 10.0;
    }
    scaled_d = (double)value * mult + 0.5;
    scaled = (int64_t)scaled_d;
    for (d = 0; d < decimals; d++)
    {
        div *= 10;
    }
    ip = scaled / div;
    fp = scaled % div;
    /* the integer part reaches 9e9: 64 bits (it was printed through 32,
     * so every value from 4294967296 up came out modulo 2^32) */
    u64_to_str((uint64_t)ip, int_s);
    (void)w_put_str(w, int_s);
    if (decimals > 0)
    {
        size_t l;

        (void)w_put(w, '.');
        u32_to_str((uint32_t)fp, fp_s);
        l = strlen(fp_s);
        for (i = (int)l; i < decimals; i++)
        {
            (void)w_put(w, '0');
        }
        (void)w_put_str(w, fp_s);
    }
    return w->err;
}

FM_Result JsonWriter_Bool(FM_JsonWriter *w, FM_Bool value)
{
    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    if (value)
    {
        (void)w_put_str(w, "true");
    }
    else
    {
        (void)w_put_str(w, "false");
    }
    return w->err;
}

FM_Result JsonWriter_Null(FM_JsonWriter *w)
{
    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = w_value_start(w);
        if (r != FM_OK) return r;
    }
    (void)w_put_str(w, "null");
    return w->err;
}

const char *JsonWriter_StringValue(FM_JsonWriter *w)
{
    if (w == NULL)
    {
        return NULL;
    }
    return w->buf;
}

size_t JsonWriter_Length(FM_JsonWriter *w)
{
    if (w == NULL)
    {
        return 0;
    }
    return w->len;
}

#endif /* FM_USE_JSON */
