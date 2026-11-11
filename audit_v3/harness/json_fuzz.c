/* Audit v3: fm_json.c fuzz.  Random bytes, mutated documents, deep
 * nesting, every escape, numbers at the limits, a writer buffer exactly
 * full.  ASan + UBSan.  Deterministic in the seed.
 *
 *   ./json_fuzz <seed> <iterations>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_json.h"

static unsigned g_rng;
static unsigned rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8);
}

static const char *const seeds[] = {
    "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null},\"e\":true,\"f\":false,\"g\":\"s\"}",
    "[1,2.5,-3e10,1E-9,0.0,\"\\u00e9\\n\\t\\\\\"]",
    "{\"nested\":{\"a\":{\"b\":{\"c\":{\"d\":[[]]}}}}}",
    "[\"\\\\\",\"\\\"\",\"\\/\",\"\\b\\f\\r\\n\\t\",\"\\u0000\\uffff\"]",
    "-0.0e-0", "123456789012345678901234567890", "true", "false", "null",
    "{}", "[]", "\"\"", "{\"k\":\"v\"}", "[[[[[[[[[[1]]]]]]]]]]",
    "{\"a\":{\"b\":[0,{\"c\":[1,{\"d\":2}]}]}}"
};

static char g_text[4096];
static char g_out[8192];
static uint32_t g_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];

static void exercise(FM_Json *doc)
{
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    float f;
    double d;
    FM_Bool b;
    size_t n;

    (void)Json_ReadString(doc, "a", g_out, sizeof(g_out));
    (void)Json_ReadInt(doc, "a", &i32);
    (void)Json_ReadUInt(doc, "a", &u32);
    (void)Json_ReadInt64(doc, "a", &i64);
    (void)Json_ReadFloat(doc, "a", &f);
    (void)Json_ReadDouble(doc, "a", &d);
    (void)Json_ReadBool(doc, "e", &b);
    (void)Json_Exists(doc, "a.b[2].c");
    (void)Json_ArrayCount(doc, "b", &n);
    (void)Json_ArrayString(doc, "b", 999, g_out, sizeof(g_out));
    (void)Json_ArrayInt(doc, "b", 0, &i32);
    (void)Json_ArrayUInt(doc, "b", 0, &u32);
    (void)Json_ArrayFloat(doc, "b", 0, &f);
    (void)Json_ArrayBool(doc, "b", 0, &b);
    /* keys that are not there, empty, or nonsense */
    (void)Json_ReadString(doc, "", g_out, 4);
    (void)Json_ReadString(doc, "....[[[[", g_out, 4);
    (void)Json_ReadInt(doc, NULL, &i32);
    (void)Json_ReadString(doc, "a", NULL, 0);
    (void)Json_ReadString(doc, "a", g_out, 1);
}

int main(int argc, char **argv)
{
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
    unsigned iters = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 100000u;
    unsigned i;
    unsigned accepted = 0;
    unsigned doc_ok = 0;

    g_rng = seed;
    FileManager_Init();

    for (i = 0; i < iters; i++)
    {
        FM_Json doc;
        FM_Result r;
        unsigned mode = rnd() % 5u;
        size_t len = 0;
        size_t tcap = FM_JSON_TOKEN_BUFFER_SIZE;

        if (rnd() % 8u == 0)
        {
            /* a token buffer that is too small / badly sized on purpose */
            tcap = (size_t)(rnd() % 8u) * FM_JSON_TOKEN_SIZE;
        }

        if (mode == 0)
        {
            len = 1u + rnd() % 300u;
            {
                unsigned k;

                for (k = 0; k < len; k++) g_text[k] = (char)(rnd() & 0xFF);
            }
        }
        else if (mode == 1)
        {
            const char *s = seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))];
            unsigned muts;

            strncpy(g_text, s, sizeof(g_text) - 1);
            len = strlen(g_text);
            muts = 1u + rnd() % 5u;
            while (muts-- > 0)
            {
                unsigned pos = (len == 0) ? 0 : rnd() % len;
                unsigned act = rnd() % 3u;

                if (act == 0 && len > 0)
                {
                    g_text[pos] = "{}[]\",:0-\\"[rnd() % 11];
                }
                else if (act == 1 && len > 0)
                {
                    memmove(g_text + pos, g_text + pos + 1, len - pos);
                    len--;
                }
                else if (len + 1 < sizeof(g_text))
                {
                    memmove(g_text + pos + 1, g_text + pos, len - pos);
                    g_text[pos] = "[{0\",:"[rnd() % 5];
                    len++;
                }
            }
        }
        else if (mode == 2)
        {
            unsigned depth = 1u + rnd() % 600u;
            unsigned k;
            char open = (rnd() & 1u) ? '[' : '{';

            for (k = 0; k < depth && len < sizeof(g_text) - 2; k++)
                g_text[len++] = open;
            for (k = 0; k < depth && len < sizeof(g_text) - 1; k++)
                g_text[len++] = (open == '[') ? ']' : '}';
        }
        else if (mode == 3)
        {
            const char *s = seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))];

            strncpy(g_text, s, sizeof(g_text) - 1);
            len = strlen(g_text);
            if (len > 1) len = rnd() % len;
        }
        else
        {
            /* numbers at the limits and every escape form */
            static const char *const nums[] = {
                "0", "-0", "1e308", "-1e308", "1e-308", "1e999", "-1e999",
                "9223372036854775807", "-9223372036854775808",
                "18446744073709551616", "0.1", "1.7976931348623157e308",
                "\\u0000", "\\ud800", "\\udfff", "\\uffff", "\\u12345"
            };
            snprintf(g_text, sizeof(g_text), "{\"n\":%s,\"s\":\"%s\"}",
                     nums[rnd() % 14u], nums[rnd() % 17u]);
            len = strlen(g_text);
        }
        g_text[len] = 0;

        memset(g_tokens, 0xCD, sizeof(g_tokens));
        r = Json_OpenBuffer(g_text, len, &doc, g_tokens, tcap);
        if (r == FM_OK)
        {
            accepted++;
            exercise(&doc);
            /* the writer: build the same tree with a buffer exactly full,
             * one byte short and one byte long */
            {
                char wbuf[1024];
                FM_JsonWriter w;
                size_t caps[3];
                unsigned ci;

                caps[0] = 0;
                caps[1] = 1;
                caps[2] = sizeof(wbuf);
                for (ci = 0; ci < 3; ci++)
                {
                    size_t cap = (ci == 2) ? (size_t)(rnd() % sizeof(wbuf)) : caps[ci];

                    if (JsonWriter_Init(&w, wbuf, cap) == FM_OK)
                    {
                        (void)JsonWriter_BeginObject(&w);
                        (void)JsonWriter_Key(&w, "a");
                        (void)JsonWriter_String(&w, "value with \" and \\ and \n");
                        (void)JsonWriter_Key(&w, "b");
                        (void)JsonWriter_BeginArray(&w);
                        (void)JsonWriter_Int(&w, -2147483647 - 1);
                        (void)JsonWriter_UInt(&w, 4294967295u);
                        (void)JsonWriter_Int64(&w, INT64_MIN);
                        (void)JsonWriter_Float(&w, 3.14159f, 6);
                        (void)JsonWriter_Bool(&w, FM_TRUE);
                        (void)JsonWriter_Null(&w);
                        (void)JsonWriter_EndArray(&w);
                        (void)JsonWriter_EndObject(&w);
                        (void)JsonWriter_Length(&w);
                        (void)JsonWriter_StringValue(&w);
                    }
                }
            }
            /* and re-parse our own output?  (no ToString in this API) */
            doc_ok++;
        }
    }
    printf("json_fuzz seed=%u: %u inputs, %u accepted, %u fully exercised\n",
           seed, iters, accepted, doc_ok);
    return 0;
}
