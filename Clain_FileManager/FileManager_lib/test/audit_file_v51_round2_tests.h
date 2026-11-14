static FM_Bool v51_cancel_transfer(uint64_t done, uint64_t total, void *user)
{ (void)done; (void)total; (void)user; return FM_FALSE; }
#if FM_USE_SAFE_WRITE
static FM_FS_DeleteFn v51_saved_delete;
static FM_Result v51_fail_backup_delete(FM_FS *fs, const char *path)
{
    if (strstr(path, "v51_dst.fm_tmp.bak") != NULL) return FM_ERR_IO;
    return v51_saved_delete(fs, path);
}
#endif
static FM_Result v51_truncate_case(const char *path)
{
    FM_File *h;
    FM_Result r = File_WriteText(path, "0123456789");
    uint32_t written = 0;
    uint64_t pos = 0;
    char text[32];
    size_t n = 0;
    if (r != FM_OK) return r;
    h = File_Open(path, FM_OPEN_READ | FM_OPEN_WRITE);
    if (!h) return File_GetLastError();
    r = File_Seek(h, 0, FM_SEEK_END);
    if (r == FM_OK) r = File_Truncate(h, 4);
    if (r == FM_OK) r = File_Tell(h, &pos);
    if (r == FM_OK && pos != 4) r = FM_ERR_INTERNAL;
    if (r == FM_OK) r = File_Write(h, "XY", 2, &written);
    if (r == FM_OK && written != 2) r = FM_ERR_INTERNAL;
    if (File_Close(h) != FM_OK && r == FM_OK) r = FM_ERR_IO;
    if (r == FM_OK) r = File_ReadText(path, text, sizeof(text), &n);
    if (r == FM_OK && strcmp(text, "0123XY") != 0) r = FM_ERR_INTERNAL;
    (void)File_Delete(path);
    return r;
}
typedef struct { const char *path; FM_Result result; } V51PublishRace;
static FM_Bool v51_create_destination(uint64_t done, uint64_t total, void *ctx)
{
    V51PublishRace *race = (V51PublishRace *)ctx;
    (void)done; (void)total;
    race->result = File_WriteText(race->path, "RIVAL");
    return FM_TRUE;
}
static FM_Result v51_no_replace_case(const char *src, const char *dst)
{
    FM_TransferOpts opts;
    V51PublishRace race;
    FM_Result r;
    char buf[32];
    size_t n;
    memset(&opts, 0, sizeof(opts));
    opts.no_overwrite = FM_TRUE;
    opts.progress = v51_create_destination;
    opts.progress_user = &race;
    race.path = dst;
    race.result = FM_ERR_INTERNAL;
    (void)File_Delete(dst);
    r = File_WriteText(src, "SOURCE");
    if (r == FM_OK) r = FM_Transfer_CopyEx(src, dst, &opts, NULL);
    if (r != FM_ERR_EXISTS || race.result != FM_OK) r = FM_ERR_INTERNAL;
    else {
        r = File_ReadText(dst, buf, sizeof(buf), &n);
        if (r == FM_OK && strcmp(buf, "RIVAL") != 0) r = FM_ERR_INTERNAL;
    }
    {
        char temp[FM_PATH_MAX];
        if (strlen(dst) + sizeof(".fm_tmp") <= sizeof(temp)) {
            strcpy(temp, dst); strcat(temp, ".fm_tmp"); (void)File_Delete(temp);
        }
    }
    (void)File_Delete(src);
    (void)File_Delete(dst);
    return r;
}
#if FM_USE_DATA_LOGGER
static FM_Result v51_logger_publish_case(const char *dir)
{
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_Result r;
    char final[FM_PATH_MAX], temp[FM_PATH_MAX], buf[32];
    size_t n;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crash_safe = FM_TRUE;
    r = Logger_Init(&lg, dir, "v51publish", FM_LOG_FORMAT_CSV, &cfg);
    if (r == FM_OK) r = Logger_Start(&lg);
    if (r != FM_OK) return r;
    snprintf(final, sizeof(final), "%s", lg.info.file_name);
    if (strlen(final) + sizeof(".fm_tmp") > sizeof(temp)) return FM_ERR_TOO_LARGE;
    strcpy(temp, final);
    strcat(temp, ".fm_tmp");
    r = Logger_WriteRaw(&lg, "LOG", 3);
    if (r == FM_OK) r = File_WriteText(final, "RIVAL");
    if (r == FM_OK && Logger_Stop(&lg) != FM_ERR_EXISTS) r = FM_ERR_INTERNAL;
    if (r == FM_OK) r = File_ReadText(final, buf, sizeof(buf), &n);
    if (r == FM_OK && strcmp(buf, "RIVAL") != 0) r = FM_ERR_INTERNAL;
    if (r == FM_OK) r = File_ReadText(temp, buf, sizeof(buf), &n);
    if (r == FM_OK && strcmp(buf, "LOG") != 0) r = FM_ERR_INTERNAL;
    (void)File_Delete(final);
    (void)File_Delete(temp);
    return r;
}
#endif
#if FM_USE_POSIX
static FM_FS_RenameFn v51_rename_real;
static int v51_rename_injected;
static FM_Result v51_rename_commit_then_error(FM_FS *fs, const char *from, const char *to)
{
    FM_Result r = v51_rename_real(fs, from, to);
    if (r == FM_OK && strstr(from, "v51_ambiguous.fm_tmp") != NULL) {
        const char *name = from;
        FILE *f;
        while (*name == '/') name++;
        f = fopen(name, "wb");
        if (!f) return FM_ERR_INTERNAL;
        if (fwrite("RIVAL", 1, 5, f) != 5 || fclose(f) != 0) return FM_ERR_INTERNAL;
        v51_rename_injected = 1;
        return FM_ERR_IO;
    }
    return r;
}
#endif
/* V51 ownership regressions. Included by the host suite. */
static void test_audit_file_v51_round2(void)
{
    char buf[64];
    size_t n = 0;
    FM_Result r;
    FM_File *f;
#if FM_USE_JSON
    TEST("V51: JSON unicode escapes, key lookup, and surrogate pair");
    {
        const char *json = "{\"\\u0061\":\"x\\u0041\\u00e9\\uD83D\\uDE00\"}";
        uint32_t tokens[128];
        FM_Json doc;
        char value[32];
        r = Json_OpenBuffer(json, strlen(json), &doc, tokens, sizeof(tokens));
        if (r == FM_OK) r = Json_ReadString(&doc, "a", value, sizeof(value));
        if (r == FM_OK && strcmp(value, "xA\xc3\xa9\xf0\x9f\x98\x80") == 0) PASS();
        else FAIL("unicode decode / key lookup");
    }
    TEST("V51: JSON NUL and truncated UTF-8 never silently change value");
    {
        const char *json = "{\"nul\":\"x\\u0000y\",\"utf\":\"\\u00e9x\"}";
        uint32_t tokens[128]; FM_Json doc; char small[2] = {1,1};
        r = Json_OpenBuffer(json, strlen(json), &doc, tokens, sizeof(tokens));
        if (r == FM_OK && Json_ReadString(&doc, "nul", small, sizeof(small)) != FM_ERR_INVALID_FORMAT) r = FM_ERR_INTERNAL;
        if (r == FM_OK && (Json_ReadString(&doc, "utf", small, sizeof(small)) != FM_ERR_TOO_LARGE || small[0] != '\0')) r = FM_ERR_INTERNAL;
        CHECK_RESULT(r, "embedded NUL / UTF-8 boundary");
    }
    TEST("V51: JSON validates raw UTF-8 and preserves codepoint boundary");
    {
        const char valid[] = "{\"a\":\"\xc3\xa9x\"}";
        const char *bad[] = {"{\"a\":\"\xc0\x80\"}", "{\"a\":\"\xed\xa0\x80\"}",
            "{\"a\":\"\xf4\x90\x80\x80\"}", "{\"a\":\"\xe2\x82\"}",
            "{\"a\":\"\x80\"}", "{\"\xc0\x80\":1}"};
        uint32_t tokens[128]; FM_Json doc; char out[2] = {1,1}; size_t j;
        r = Json_OpenBuffer(valid, strlen(valid), &doc, tokens, sizeof(tokens));
        if (r == FM_OK && (Json_ReadString(&doc, "a", out, sizeof(out)) != FM_ERR_TOO_LARGE || out[0] != '\0')) r = FM_ERR_INTERNAL;
        for (j = 0; r == FM_OK && j < sizeof(bad)/sizeof(bad[0]); j++)
            if (Json_OpenBuffer(bad[j], strlen(bad[j]), &doc, tokens, sizeof(tokens)) != FM_ERR_INVALID_FORMAT) r = FM_ERR_INTERNAL;
        CHECK_RESULT(r, "raw UTF-8");
    }
    TEST("V51: JSON rejects invalid escapes and surrogate pairs");
    {
        const char *bad[] = {"{\"a\":\"\\q\"}", "{\"a\":\"\\uD800\"}",
            "{\"a\":\"\\uDC00\"}", "{\"a\":\"\\u12X4\"}", "{\"\\q\":1}",
            "{\"a\":\"unescaped\nnewline\"}"};
        uint32_t tokens[128]; FM_Json doc; size_t j;
        for (j = 0; j < sizeof(bad)/sizeof(bad[0]); j++)
            if (Json_OpenBuffer(bad[j], strlen(bad[j]), &doc, tokens, sizeof(tokens)) != FM_ERR_INVALID_FORMAT) break;
        if (j == sizeof(bad)/sizeof(bad[0])) PASS(); else FAIL("invalid escape accepted");
    }
#if FM_JSON_MAX_DEPTH >= 8
    TEST("V51: JSON writer and parser accept eight nested containers");
    {
        char json[128]; uint32_t tokens[128]; FM_Json doc; FM_JsonWriter w; int j;
        r = JsonWriter_Init(&w, json, sizeof(json));
        for (j = 0; j < 8 && r == FM_OK; j++) r = JsonWriter_BeginArray(&w);
        if (r == FM_OK) r = JsonWriter_Int(&w, 7);
        for (j = 0; j < 8 && r == FM_OK; j++) r = JsonWriter_EndArray(&w);
        if (r == FM_OK) r = Json_OpenBuffer(json, strlen(json), &doc, tokens, sizeof(tokens));
        CHECK_RESULT(r, "nested JSON");
    }
#endif /* FM_JSON_MAX_DEPTH >= 8 */
#endif /* FM_USE_JSON */
#if FM_USE_POSIX && FM_NAME_MAX < 250
    TEST("V51: directory oversized-entry error stays sticky until close");
    {
        char path[300];
        FM_DirEntry entry;
        FM_Dir *dir;
        FILE *native;
        FM_Result first = FM_OK, after = FM_OK, rewind_result = FM_OK, again = FM_OK;
        size_t i;
        (void)Directory_Create("/v51_longdir");
        strcpy(path, "v51_longdir/");
        i = strlen(path);
        memset(path + i, 'x', FM_NAME_MAX + 1u);
        path[i + FM_NAME_MAX + 1u] = '\0';
        native = fopen(path, "wb");
        if (native) (void)fclose(native);
        dir = native ? Directory_Open("/v51_longdir") : NULL;
        if (dir) {
            do { first = Directory_Read(dir, &entry); } while (first == FM_OK);
            after = Directory_Read(dir, &entry);
            rewind_result = Directory_Rewind(dir);
            again = Directory_Read(dir, &entry);
            (void)Directory_Close(dir);
        }
        if (dir && first == FM_ERR_TOO_LARGE && after == FM_ERR_TOO_LARGE &&
            rewind_result == FM_OK && again == FM_ERR_TOO_LARGE) PASS();
        else FAIL("oversized entry disappeared without error");
        if (native) (void)remove(path);
        (void)Directory_Delete("/v51_longdir");
    }
#endif
#if FM_USE_POSIX
    TEST("V51: ambiguous rename failure cannot delete a replacement temp");
    {
        FM_FS *fs;
        (void)File_WriteText("/v51_ambiguous_src", "NEW");
        (void)File_WriteText("/v51_ambiguous", "OLD");
        (void)File_Delete("/v51_ambiguous.fm_tmp");
        f = File_Open("/v51_ambiguous", FM_OPEN_READ);
        if (!f) FAIL("setup");
        else {
            fs = f->fs;
            (void)File_Close(f);
            v51_rename_real = fs->ops.rename;
            v51_rename_injected = 0;
            fs->ops.rename = v51_rename_commit_then_error;
            r = File_Copy("/v51_ambiguous_src", "/v51_ambiguous", NULL, NULL);
            fs->ops.rename = v51_rename_real;
            if (r == FM_ERR_IO && v51_rename_injected &&
                File_ReadText("/v51_ambiguous", buf, sizeof(buf), &n) == FM_OK && strcmp(buf, "NEW") == 0 &&
                File_ReadText("/v51_ambiguous.fm_tmp", buf, sizeof(buf), &n) == FM_OK && strcmp(buf, "RIVAL") == 0)
                PASS(); else FAIL("unowned temp deleted after ambiguous commit");
        }
        (void)File_Delete("/v51_ambiguous_src");
        (void)File_Delete("/v51_ambiguous");
        (void)File_Delete("/v51_ambiguous.fm_tmp");
    }
#endif
    TEST("V51: POSIX no-overwrite publication wins race");
    r = v51_no_replace_case("/v51_race_src", "/v51_race_dst");
    CHECK_RESULT(r, "no-overwrite collision");
#if FM_USE_DATA_LOGGER
    TEST("V51: POSIX logger does not overwrite late final file");
    r = v51_logger_publish_case("/");
    CHECK_RESULT(r, "logger collision");
#endif
    TEST("V51: POSIX truncate clamps backend position");
    r = v51_truncate_case("/v51_truncate");
    CHECK_RESULT(r, "truncate position");
    TEST("V51: exclusive create preserves existing file");
    (void)File_WriteText("/v51_owned", "USER");
    f = File_Open("/v51_owned", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
    r = File_GetLastError();
    if (f) (void)File_Close(f);
    if (!f && r == FM_ERR_EXISTS && File_ReadText("/v51_owned", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "USER") == 0) PASS(); else FAIL("exclusive create collision");
    (void)File_Delete("/v51_owned");

#if FM_USE_FMFS || FM_USE_LITTLEFS
    {
        int kind;
        for (kind = 0; kind < 2; kind++) {
#if !FM_USE_FMFS
            if (kind == 0) continue;
#endif
#if !FM_USE_LITTLEFS
            if (kind == 1) continue;
#endif
            TEST(kind == 0 ? "V51: FMFS exclusive staging collision" : "V51: LittleFS exclusive staging collision");
            r = av2_mount(kind, &(const char *){ NULL });
            if (r == FM_OK) r = File_WriteText("/fca/v51_src", "NEW");
            if (r == FM_OK) r = File_WriteText("/fca/v51_dst", "OLD");
            if (r == FM_OK) r = File_WriteText("/fca/v51_dst.fm_tmp", "USER");
            if (r == FM_OK && File_Copy("/fca/v51_src", "/fca/v51_dst", NULL, NULL) != FM_ERR_EXISTS) r = FM_ERR_INTERNAL;
            if (r == FM_OK) r = av2_expect("/fca/v51_dst", "OLD");
            if (r == FM_OK) r = av2_expect("/fca/v51_dst.fm_tmp", "USER");
            if (r == FM_OK) r = v51_no_replace_case("/fca/race_src", "/fca/race_dst");
#if FM_USE_DATA_LOGGER
            if (r == FM_OK) r = v51_logger_publish_case("/fca");
#endif
            if (r == FM_OK) r = v51_truncate_case("/fca/v51_truncate");
            av2_unmount(kind);
            CHECK_RESULT(r, "staging collision / truncate position");
        }
    }
#endif

#if FM_USE_DATA_LOGGER
    TEST("V51: logger recovery refuses unowned matching file");
    (void)Directory_Create("/v51logs");
    (void)File_WriteText("/v51logs/lg_user.fm_tmp", "USER");
    r = Logger_Recover("/v51logs", "lg");
    if (r == FM_ERR_EXISTS && File_ReadText("/v51logs/lg_user.fm_tmp", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "USER") == 0) PASS(); else FAIL("logger recovery collision");
    (void)File_Delete("/v51logs/lg_user.fm_tmp");
    (void)Directory_Delete("/v51logs");
#endif

    TEST("V51: failed directory rewind keeps its handle reserved");
    {
        FM_Dir *d1, *d2;
        FM_Result rewind_result;
        (void)Directory_Create("/v51_dir1");
        (void)Directory_Create("/v51_dir2");
        d1 = Directory_Open("/v51_dir1");
        if (d1) (void)Directory_Delete("/v51_dir1");
        rewind_result = d1 ? Directory_Rewind(d1) : FM_OK;
        r = File_GetLastError();
        d2 = Directory_Open("/v51_dir2");
        if (d1 && d2 && d1 != d2 && rewind_result == FM_ERR_NOT_FOUND && r == rewind_result &&
            Directory_Close(d1) == FM_OK && d2->used && Directory_Close(d2) == FM_OK)
            PASS();
        else { FAIL("rewind reused or invalidated a live slot"); if (d1 && d1->used) (void)Directory_Close(d1); if (d2 && d2->used) (void)Directory_Close(d2); }
        (void)Directory_Delete("/v51_dir1");
        (void)Directory_Delete("/v51_dir2");
    }

    TEST("V51: copy refuses occupied staging without modifying either file");
    (void)File_WriteText("/v51_src", "NEW");
    (void)File_WriteText("/v51_dst", "OLD");
    (void)File_WriteText("/v51_dst.fm_tmp", "USER");
    r = File_Copy("/v51_src", "/v51_dst", NULL, NULL);
    if (r == FM_ERR_EXISTS && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "OLD") == 0 && File_ReadText("/v51_dst.fm_tmp", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "USER") == 0) PASS(); else FAIL("copy collision");
#if FM_USE_SAFE_WRITE
    TEST("V51: safe replace refuses occupied staging and recover preserves it");
    r = File_SafeReplace("/v51_dst", "NEW", 3);
    if (r == FM_ERR_EXISTS && File_Recover("/v51_dst") == FM_ERR_EXISTS &&
        File_ReadText("/v51_dst.fm_tmp", buf, sizeof(buf), &n) == FM_OK && strcmp(buf, "USER") == 0 &&
        File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK && strcmp(buf, "OLD") == 0)
        PASS(); else FAIL("safe replace collision");
    TEST("V51: transaction refuses occupied backup");
    (void)File_Delete("/v51_dst.fm_tmp");
    (void)File_WriteText("/v51_dst.fm_tmp.bak", "USER");
    r = File_Transaction("/v51_dst", "NEW", 3);
    if (r == FM_ERR_EXISTS && File_ReadText("/v51_dst.fm_tmp.bak", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "USER") == 0 && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "OLD") == 0) PASS(); else FAIL("backup collision");
    (void)File_Delete("/v51_dst.fm_tmp.bak");
    TEST("V51: transaction reports post-commit cleanup failure");
    f = File_Open("/v51_dst", FM_OPEN_READ);
    if (f) {
        FM_FS *fs = f->fs;
        (void)File_Close(f);
        v51_saved_delete = fs->ops.delete_file;
        fs->ops.delete_file = v51_fail_backup_delete;
        r = File_Transaction("/v51_dst", "NEW", 3);
        fs->ops.delete_file = v51_saved_delete;
        if (r == FM_ERR_CLEANUP && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
            strcmp(buf, "NEW") == 0 && File_ReadText("/v51_dst.fm_tmp.bak", buf, sizeof(buf), &n) == FM_OK &&
            strcmp(buf, "OLD") == 0) PASS(); else FAIL("cleanup status");
        (void)File_Delete("/v51_dst.fm_tmp.bak");
        (void)File_WriteText("/v51_dst", "OLD");
    } else FAIL("cleanup setup");
#endif
    (void)File_Delete("/v51_dst.fm_tmp");
    TEST("V51: cancelled transfer preserves existing destination");
    {
        FM_TransferOpts opts;
        FM_Bool exists = FM_TRUE;
        memset(&opts, 0, sizeof(opts));
        opts.progress = v51_cancel_transfer;
        r = FM_Transfer_CopyEx("/v51_src", "/v51_dst", &opts, NULL);
        if (r == FM_ERR_CANCELLED && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
            strcmp(buf, "OLD") == 0 && File_Exists("/v51_dst.fm_tmp", &exists) == FM_OK && !exists)
            PASS(); else FAIL("transfer cancellation");
    }
#if FM_USE_TEXT || FM_USE_DATA_LOGGER
    TEST("V51: transfer write failure preserves existing destination");
    f = File_Open("/v51_dst", FM_OPEN_READ);
    if (f) {
        FM_FS *fs = f->fs;
        (void)File_Close(f);
        v51_original_write = fs->ops.file_write;
        v51_write_limit = 0;
        fs->ops.file_write = v51_short_write;
        r = FM_Transfer_CopyEx("/v51_src", "/v51_dst", NULL, NULL);
        fs->ops.file_write = v51_original_write;
        if (r != FM_OK && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
            strcmp(buf, "OLD") == 0) PASS(); else FAIL("transfer error");
    } else FAIL("transfer setup");
#endif
#if FM_USE_ASYNC
    TEST("V51: async copy cancellation preserves destination");
    {
        FM_Async a;
        FM_Bool exists = FM_TRUE;
        Async_Init(&a);
        r = Async_StartCopy(&a, "/v51_src", "/v51_dst", NULL, NULL, NULL);
        if (r == FM_OK && Async_Poll(&a) == FM_ASYNC_RUNNING) {
            (void)Async_Cancel(&a);
            if (Async_Poll(&a) != FM_ASYNC_CANCELLED) r = FM_ERR_INTERNAL;
        } else r = FM_ERR_INTERNAL;
        if (r == FM_OK && File_ReadText("/v51_dst", buf, sizeof(buf), &n) == FM_OK &&
            strcmp(buf, "OLD") == 0 && File_Exists("/v51_dst.fm_tmp", &exists) == FM_OK && !exists)
            PASS(); else FAIL("async cancel");
    }
#endif
    (void)File_Delete("/v51_dst");
    (void)File_Delete("/v51_src");
}
