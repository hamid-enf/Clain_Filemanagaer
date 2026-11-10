/*
 * Example 05 — JSON
 *
 * نوشتن: JsonWriter_Init -> BeginObject -> Key -> Value -> EndObject
 * خواندن: Json_LoadFile -> Json_ReadString/Int/Bool/Float
 *
 *   dotted path:  "device.name"     آبجکت تودرتو
 *   array index:  "sensors[0]"      عنصر آرایه
 */
#include <stdio.h>
#include <stdint.h>
#include "file_manager.h"

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

int main(void)
{
    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    /* 1. نوشتن */
    {
        char buf[256];
        FM_JsonWriter w;
        JsonWriter_Init(&w, buf, sizeof(buf));

        JsonWriter_BeginObject(&w);
          JsonWriter_Key(&w, "name");     JsonWriter_String(&w, "STM32");
          JsonWriter_Key(&w, "baudrate"); JsonWriter_Int(&w, 115200);
          JsonWriter_Key(&w, "debug");    JsonWriter_Bool(&w, FM_TRUE);
          JsonWriter_Key(&w, "sensors");
          JsonWriter_BeginArray(&w);
            JsonWriter_Float(&w, 23.5f, 1);
            JsonWriter_Float(&w, 24.1f, 1);
            JsonWriter_Float(&w, 22.8f, 1);
          JsonWriter_EndArray(&w);
        JsonWriter_EndObject(&w);

        File_WriteText("/config.json", JsonWriter_StringValue(&w));
        printf("Written:\n%s\n\n", JsonWriter_StringValue(&w));
    }

    /* 2. خواندن
     *
     * نکته‌ی مهم برای embedded: token buffer در FileManager بزرگه
     * (FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE = 4 KB پیش‌فرض).
     * اگه از main معمولی با stack محدود صدا می‌زنید، این بافر رو در
     * BSS / external RAM بذارید.  اینجا به‌خاطر سادگی stack-static
     * شده ولی در main واقعی MCU توصیه می‌شه:
     *
     *   static uint32_t s_tokens[FM_JSON_TOKEN_BUFFER_SIZE/4];
     *
     * یا استفاده از API جدید Json_LoadInt / Json_LoadString /
     * Json_LoadFloat که بافرها رو از caller می‌گیرن.
     */
    {
        FM_Json doc;
        char text[256];
        uint32_t tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
        char name[32];
        int32_t baud;
        FM_Bool debug;
        size_t count;
        float v0, v1, v2;

        Json_LoadFile("/config.json", &doc, text, sizeof(text),
                      tokens, sizeof(tokens));

        Json_ReadString(&doc, "name", name, sizeof(name));
        Json_ReadInt(&doc, "baudrate", &baud);
        Json_ReadBool(&doc, "debug", &debug);
        Json_ArrayCount(&doc, "sensors", &count);
        Json_ArrayFloat(&doc, "sensors", 0, &v0);
        Json_ArrayFloat(&doc, "sensors", 1, &v1);
        Json_ArrayFloat(&doc, "sensors", 2, &v2);

        printf("name    = %s\n", name);
        printf("baud    = %d\n", (int)baud);
        printf("debug   = %s\n", debug ? "true" : "false");
        printf("sensors = [%lu] %.1f, %.1f, %.1f\n", (unsigned long)count, v0, v1, v2);
    }

    /* 3. روش Embedded-friendly با API جدید (بدون allocation روی stack) */
    {
        /* بافرها در BSS (global static) — هیچ فشاری روی stack نیست */
        static char     s_text[256];
        static uint32_t s_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
        char  name[32];
        int32_t baud;

        /* یک خط JSON convenience — caller مالک بافرهاست */
        Json_LoadString("/config.json", "name", name, sizeof(name),
                        s_text, sizeof(s_text),
                        s_tokens, sizeof(s_tokens));
        printf("[api] name = %s\n", name);

        Json_LoadInt("/config.json", "baudrate", &baud,
                     s_text, sizeof(s_text),
                     s_tokens, sizeof(s_tokens));
        printf("[api] baud = %d\n", (int)baud);
    }

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}
