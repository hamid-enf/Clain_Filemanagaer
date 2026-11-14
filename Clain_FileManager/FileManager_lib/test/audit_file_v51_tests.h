/* ReportV51 regressions: host tests, injected short successful writes. */
#if FM_USE_TEXT || FM_USE_DATA_LOGGER
static FM_FS_FileWriteFn v51_original_write;
static int v51_write_limit;
static FM_Result v51_short_write(FM_FS *fs, void *fh, const void *buf,
                                 uint32_t len, uint32_t *written)
{
    if (v51_write_limit >= 0 && len > (uint32_t)v51_write_limit)
        len = (uint32_t)v51_write_limit;
    return v51_original_write(fs, fh, buf, len, written);
}
#endif

static void test_audit_file_v51(void)
{
    char buf[64];
    FM_File *f;
    FM_File ex;
    FM_Result r;
    size_t n;
    TEST("V51: read-only truncate rejected without data loss");
    (void)File_WriteText("/v51_open", "valuable");
    f = File_Open("/v51_open", FM_OPEN_READ | FM_OPEN_TRUNCATE);
    r = File_GetLastError();
    if (f) (void)File_Close(f);
    File_Init(&ex);
    if (f == NULL && r == FM_ERR_ACCESS &&
        File_OpenEx(&ex, "/v51_open", FM_OPEN_READ | FM_OPEN_TRUNCATE) == FM_ERR_ACCESS &&
        File_ReadText("/v51_open", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "valuable") == 0) PASS(); else FAIL("truncate/read-only");
    (void)File_Delete("/v51_open");

#if FM_USE_LITTLEFS
    TEST("V51: LittleFS rename uses native replacement and type checks");
    {
        FM_Bool dir = FM_FALSE;
        r = av2_mount(1, &(const char *){ NULL });
        if (r == FM_OK) r = Directory_Create("/fca/v51dir");
        if (r == FM_OK) r = File_WriteText("/fca/v51file", "valuable");
        if (r == FM_OK && g_av2_fs.ops.rename(&g_av2_fs, "/v51file", "/v51dir") != FM_ERR_IS_DIRECTORY)
            r = FM_ERR_INTERNAL;
        if (r == FM_OK && g_av2_fs.ops.rename(&g_av2_fs, "/v51dir", "/v51file") != FM_ERR_NOT_DIRECTORY)
            r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Directory_Exists("/fca/v51dir", &dir);
        if (r == FM_OK && !dir) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = av2_expect("/fca/v51file", "valuable");
        if (r == FM_OK) r = File_WriteText("/fca/v51other", "new");
        if (r == FM_OK) r = File_Rename("/fca/v51other", "/fca/v51file");
        if (r == FM_OK) r = av2_expect("/fca/v51file", "new");
        av2_unmount(1);
        CHECK_RESULT(r, "LittleFS rename");
    }
#endif

#if FM_USE_TEXT
    TEST("V51: text short writes and zero-progress detection");
    f = File_Open("/v51_text", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    r = FM_ERR_INTERNAL;
    if (f) {
        v51_original_write = f->fs->ops.file_write;
        f->fs->ops.file_write = v51_short_write;
        v51_write_limit = 3;
        r = Text_WriteLine(f, "ABCDEFGH");
        v51_write_limit = 0;
        if (Text_WriteLine(f, "X") != FM_ERR_IO) r = FM_ERR_INTERNAL;
        f->fs->ops.file_write = v51_original_write;
        (void)File_Close(f);
    }
    if (r == FM_OK && File_ReadText("/v51_text", buf, sizeof(buf), &n) == FM_OK &&
        strcmp(buf, "ABCDEFGH\n") == 0) PASS(); else FAIL("short text write");
    (void)File_Delete("/v51_text");
#endif
#if FM_USE_DATA_LOGGER
    TEST("V51: logger short-write retry does not repeat prefix");
    {
        FM_Logger lg;
        FM_LoggerConfig cfg;
        char name[FM_PATH_MAX];
        FM_Result first = FM_ERR_INTERNAL;
        memset(&cfg, 0, sizeof(cfg));
        cfg.flush_records = 1;
        r = Logger_Init(&lg, "/", "v51log", FM_LOG_FORMAT_CSV, &cfg);
        if (r == FM_OK) r = Logger_Start(&lg);
        if (r == FM_OK) {
            snprintf(name, sizeof(name), "%s", lg.info.file_name);
            v51_original_write = lg.file.fs->ops.file_write;
            lg.file.fs->ops.file_write = v51_short_write;
            v51_write_limit = 3;
            first = Logger_WriteRaw(&lg, "ABCDEFGH", 8);
            lg.file.fs->ops.file_write = v51_original_write;
            r = Logger_Flush(&lg);
            if (Logger_Stop(&lg) != FM_OK) r = FM_ERR_IO;
            if (first == FM_ERR_IO && r == FM_OK &&
                File_ReadText(name, buf, sizeof(buf), &n) == FM_OK &&
                strcmp(buf, "ABCDEFGH") == 0) PASS(); else FAIL("short logger retry");
            (void)File_Delete(name);
        } else FAIL("logger setup");
    }
#endif
}
