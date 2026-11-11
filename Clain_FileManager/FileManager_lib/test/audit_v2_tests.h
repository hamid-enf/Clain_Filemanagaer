/*
 * FileManager host tests -- external audit, round 2 (ReportV2.txt).
 *
 * One test (or a small group) per finding that was reproduced.  Each of
 * them fails -- or aborts -- on the code before the fix.  Included by
 * test/main.c; uses its TEST / PASS / FAIL macros.
 */

/* ================================================================ */
/* File part                                                         */
/* ================================================================ */

/* The three backends a host build has: 0 FMFS, 1 LittleFS (both on a
 * RAM disk mounted at /fca), 2 the POSIX root. */
static FM_FS g_av2_fs;

static const char *av2_kind_name(int kind)
{
    return kind == 0 ? "FMFS" : (kind == 1 ? "LittleFS" : "POSIX");
}

static FM_Bool av2_kind_built(int kind)
{
#if !FM_USE_FMFS
    if (kind == 0) return FM_FALSE;
#endif
#if !FM_USE_LITTLEFS
    if (kind == 1) return FM_FALSE;
#endif
    (void)kind;
    return FM_TRUE;
}

static FM_Result av2_mount(int kind, const char **root)
{
    FM_Result r = FM_OK;

    if (kind == 2)
    {
        *root = "";
        return FM_OK;
    }
    *root = "/fca";
#if FM_USE_FMFS || FM_USE_LITTLEFS
    r = fc_storage(0, kind, 1);
    if (r == FM_OK) r = fc_init(kind, &g_av2_fs);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
#else
    r = FM_ERR_UNSUPPORTED;
#endif
    return r;
}

static void av2_unmount(int kind)
{
#if FM_USE_FMFS || FM_USE_LITTLEFS
    if (kind != 2) fc_drop(0);
#else
    (void)kind;
#endif
}

static void av2_path(char *out, size_t n, const char *root, const char *name)
{
    (void)snprintf(out, n, "%s/%s", root, name);
}

/* FILE-01: truncating through a read-only handle */
static FM_Result av2_truncate_readonly(int kind)
{
    const char *root = "";
    char path[64];
    char back[16];
    size_t rd = 0;
    FM_File *f;
    FM_Result r;
    FM_Result t;

    r = av2_mount(kind, &root);
    av2_path(path, sizeof(path), root, "av2_ro.txt");
    if (r == FM_OK) r = File_WriteAll(path, "0123456789", 10);
    if (r == FM_OK)
    {
        f = File_Open(path, FM_OPEN_READ);
        if (f == NULL)
        {
            r = File_GetLastError();
        }
        else
        {
            t = File_Truncate(f, 5);
            if (t != FM_ERR_ACCESS)
            {
                printf("[truncate: %s] ", FM_ResultString(t));
                r = FM_ERR_INTERNAL;
            }
            (void)File_Close(f);
        }
    }
    if (r == FM_OK)
    {
        memset(back, 0, sizeof(back));
        r = File_ReadAll(path, back, sizeof(back) - 1u, &rd);
        if (r == FM_OK && (rd != 10u || memcmp(back, "0123456789", 10) != 0))
        {
            printf("[file changed: %u bytes] ", (unsigned)rd);
            r = FM_ERR_CRC;
        }
    }
    (void)File_Delete(path);
    av2_unmount(kind);
    return r;
}

static FM_Result av2_expect(const char *path, const char *want)
{
    char back[64];
    size_t rd = 0;
    FM_Result r;

    memset(back, 0, sizeof(back));
    r = File_ReadAll(path, back, sizeof(back) - 1u, &rd);
    if (r == FM_OK && (rd != strlen(want) || memcmp(back, want, rd) != 0))
    {
        printf("[got %u bytes \"%.40s\", want \"%s\"] ", (unsigned)rd, back,
               want);
        r = FM_ERR_CRC;
    }
    return r;
}

/* FILE-02: the file grew behind an append handle's back */
static FM_Result av2_append_after_grow(int kind)
{
    const char *root = "";
    char path[64];
    FM_File *h;
    FM_File *other;
    uint32_t wr = 0;
    uint64_t pos = 0;
    FM_Result r;

    r = av2_mount(kind, &root);
    av2_path(path, sizeof(path), root, "av2_app.txt");
    (void)File_Delete(path);
    if (r == FM_OK) r = File_WriteAll(path, "AAAA", 4);
    h = NULL;
    if (r == FM_OK)
    {
        h = File_Open(path, FM_OPEN_WRITE | FM_OPEN_APPEND);
        if (h == NULL) r = File_GetLastError();
    }
    if (r == FM_OK)
    {
        /* somebody else -- a second task, the logger -- adds four bytes */
        other = File_Open(path, FM_OPEN_WRITE | FM_OPEN_APPEND);
        if (kind == 1)
        {
            /* littlefs cannot share a file between two writers; the
             * adapter says so instead of losing one of the two writes */
            if (other != NULL || File_GetLastError() != FM_ERR_BUSY)
            {
                printf("[second writer: %s] ", other != NULL ? "opened" :
                       FM_ResultString(File_GetLastError()));
                r = FM_ERR_INTERNAL;
            }
            if (other != NULL) (void)File_Close(other);
            (void)File_Close(h);
            (void)File_Delete(path);
            av2_unmount(kind);
            return r;
        }
        if (other == NULL)
        {
            r = File_GetLastError();
        }
        else
        {
            r = File_Write(other, "CCCC", 4, &wr);
            if (r == FM_OK) r = File_Close(other); else (void)File_Close(other);
        }
    }
    if (r == FM_OK) r = File_Append(h, "B", 1, &wr);
    if (r == FM_OK && wr != 1u) r = FM_ERR_IO;
    if (r == FM_OK) r = File_Tell(h, &pos);
    if (r == FM_OK && pos != 9u)
    {
        printf("[position %u, want 9] ", (unsigned)pos);
        r = FM_ERR_INTERNAL;
    }
    if (h != NULL)
    {
        FM_Result c = File_Close(h);
        if (r == FM_OK) r = c;
    }
    if (r == FM_OK) r = av2_expect(path, "AAAACCCCB");
    (void)File_Delete(path);
    av2_unmount(kind);
    return r;
}

/* FILE-02: two append handles taking turns */
static FM_Result av2_append_two_handles(int kind)
{
    const char *root = "";
    char path[64];
    FM_File *a = NULL;
    FM_File *b = NULL;
    FM_Result r;

    r = av2_mount(kind, &root);
    av2_path(path, sizeof(path), root, "av2_two.txt");
    (void)File_Delete(path);
    if (r == FM_OK)
    {
        a = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_APPEND);
        if (a == NULL) r = File_GetLastError();
    }
    if (r == FM_OK)
    {
        b = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_APPEND);
        if (b == NULL) r = File_GetLastError();
    }
    if (kind == 1)
    {
        if (a == NULL || b != NULL || r != FM_ERR_BUSY)
        {
            printf("[second writer: %s] ", b != NULL ? "opened" :
                   FM_ResultString(r));
            r = FM_ERR_INTERNAL;
        }
        else
        {
            r = FM_OK;
        }
        if (a != NULL) (void)File_Close(a);
        if (b != NULL) (void)File_Close(b);
        (void)File_Delete(path);
        av2_unmount(kind);
        return r;
    }
    if (r == FM_OK) r = File_Write(a, "1111", 4, NULL);
    if (r == FM_OK) r = File_Flush(a);
    if (r == FM_OK) r = File_Write(b, "2222", 4, NULL);
    if (r == FM_OK) r = File_Flush(b);
    if (r == FM_OK) r = File_Write(a, "3333", 4, NULL);
    if (a != NULL)
    {
        FM_Result c = File_Close(a);
        if (r == FM_OK) r = c;
    }
    if (b != NULL)
    {
        FM_Result c = File_Close(b);
        if (r == FM_OK) r = c;
    }
    if (r == FM_OK) r = av2_expect(path, "111122223333");
    (void)File_Delete(path);
    av2_unmount(kind);
    return r;
}

/* FM_OPEN_APPEND: every write goes to the end, whatever the position */
static FM_Result av2_append_ignores_seek(int kind)
{
    const char *root = "";
    char path[64];
    FM_File *h = NULL;
    uint64_t pos = 0;
    FM_Result r;

    r = av2_mount(kind, &root);
    av2_path(path, sizeof(path), root, "av2_seek.txt");
    (void)File_Delete(path);
    if (r == FM_OK) r = File_WriteAll(path, "abcdef", 6);
    if (r == FM_OK)
    {
        h = File_Open(path, FM_OPEN_READ | FM_OPEN_WRITE | FM_OPEN_APPEND);
        if (h == NULL) r = File_GetLastError();
    }
    if (r == FM_OK) r = File_Seek(h, 0, FM_SEEK_SET);
    if (r == FM_OK) r = File_Write(h, "XY", 2, NULL);
    if (r == FM_OK) r = File_Tell(h, &pos);
    if (r == FM_OK && pos != 8u)
    {
        printf("[position %u, want 8] ", (unsigned)pos);
        r = FM_ERR_INTERNAL;
    }
    if (h != NULL)
    {
        FM_Result c = File_Close(h);
        if (r == FM_OK) r = c;
    }
    if (r == FM_OK) r = av2_expect(path, "abcdefXY");
    (void)File_Delete(path);
    av2_unmount(kind);
    return r;
}

static void av2_each_backend(const char *what, FM_Result (*fn)(int))
{
    int kind;

    for (kind = 0; kind < 3; kind++)
    {
        char name[64];

        if (!av2_kind_built(kind)) continue;
        (void)snprintf(name, sizeof(name), "%s (%s)", what,
                       av2_kind_name(kind));
        TEST(name);
        {
            FM_Result r = fn(kind);
            CHECK_RESULT(r, "see above");
        }
    }
}

#if FM_USE_LITTLEFS
/* The rule the LittleFS adapter enforces: a file open for writing has
 * one handle; read-only handles can be many. */
static void av2_lfs_one_writer(void)
{
    const char *root = "";
    FM_File *w = NULL;
    FM_File *r1 = NULL;
    FM_File *r2 = NULL;
    FM_File *x;
    FM_Result r;
    int bad = 0;

    TEST("LittleFS: one writer or many readers per file");
    r = av2_mount(1, &root);
    if (r == FM_OK) r = File_WriteAll("/fca/a.txt", "aaaa", 4);
    if (r == FM_OK) r = File_WriteAll("/fca/b.txt", "bbbb", 4);
    if (r != FM_OK)
    {
        FAIL("set-up");
        av2_unmount(1);
        return;
    }
    /* two readers */
    r1 = File_Open("/fca/a.txt", FM_OPEN_READ);
    r2 = File_Open("/fca/a.txt", FM_OPEN_READ);
    if (r1 == NULL || r2 == NULL) { printf("[two readers] "); bad = 1; }
    /* a writer next to them: no, and the truncation it asked for did
     * not happen either */
    x = File_Open("/fca/a.txt", FM_OPEN_WRITE | FM_OPEN_TRUNCATE);
    if (x != NULL || File_GetLastError() != FM_ERR_BUSY)
    {
        printf("[writer next to readers] ");
        bad = 1;
    }
    if (x != NULL) (void)File_Close(x);
    /* another file is nobody's business */
    w = File_Open("/fca/b.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (w == NULL) { printf("[writer on another file] "); bad = 1; }
    if (r1 != NULL) (void)File_Close(r1);
    if (r2 != NULL) (void)File_Close(r2);
    if (av2_expect("/fca/a.txt", "aaaa") != FM_OK) bad = 1;
    /* a reader next to the writer: no */
    x = File_Open("/fca/b.txt", FM_OPEN_READ);
    if (x != NULL || File_GetLastError() != FM_ERR_BUSY)
    {
        printf("[reader next to a writer] ");
        bad = 1;
    }
    if (x != NULL) (void)File_Close(x);
    /* a file that does not exist yet, created while others are open */
    x = File_Open("/fca/c.txt", FM_OPEN_WRITE | FM_OPEN_CREATE);
    if (x == NULL) { printf("[create] "); bad = 1; }
    if (x != NULL) (void)File_Close(x);
    if (w != NULL)
    {
        if (File_Write(w, "BB", 2, NULL) != FM_OK) bad = 1;
        if (File_Close(w) != FM_OK) bad = 1;
    }
    /* the writer is gone: everything is allowed again */
    if (av2_expect("/fca/b.txt", "bbbbBB") != FM_OK) bad = 1;
    x = File_Open("/fca/b.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (x == NULL) { printf("[writer after close] "); bad = 1; }
    if (x != NULL) (void)File_Close(x);
    av2_unmount(1);
    if (bad) FAIL("see above"); else PASS();
}
#endif

#if FM_USE_TEXT
/* FILE-04: a CR that is not part of a line end is text */
static void av2_text_cr(void)
{
    static const char text[] = "a\rb\r\nc\r\r\n\r\nplain\nd\r";
    static const char *const want[] = { "a\rb", "c\r", "", "plain", "d" };
    FM_File *f;
    FM_Result r;
    char line[32];
    size_t n = 0;
    size_t i;
    int bad = 0;

    TEST("FILE-04: Text_ReadLine keeps a CR inside a line");
    r = File_WriteAll("/av2_cr.txt", text, sizeof(text) - 1u);
    f = (r == FM_OK) ? File_Open("/av2_cr.txt", FM_OPEN_READ) : NULL;
    if (f == NULL)
    {
        FAIL("set-up");
    }
    else
    {
        for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        {
            r = Text_ReadLine(f, line, sizeof(line), &n);
            if (r != FM_OK || n != strlen(want[i]) ||
                memcmp(line, want[i], n) != 0)
            {
                printf("[line %u] ", (unsigned)i);
                bad = 1;
            }
        }
        if (Text_ReadLine(f, line, sizeof(line), &n) != FM_ERR_EOF) bad = 1;
        (void)File_Close(f);
        if (bad) FAIL("wrong lines"); else PASS();
    }

    TEST("FILE-04: a CR that does not fit is reported, not dropped");
    {
        char small[3];

        r = File_WriteAll("/av2_cr.txt", "ab\rc\n", 5);
        f = (r == FM_OK) ? File_Open("/av2_cr.txt", FM_OPEN_READ) : NULL;
        if (f == NULL)
        {
            FAIL("set-up");
        }
        else
        {
            r = Text_ReadLine(f, small, sizeof(small), &n);
            (void)File_Close(f);
            if (r == FM_ERR_TOO_LARGE && n == 2u && memcmp(small, "ab", 3) == 0)
                PASS();
            else
                FAIL("expected FM_ERR_TOO_LARGE and \"ab\"");
        }
    }

    TEST("FILE-04: Text_Replace does not delete a lone CR");
    {
        char back[32];
        size_t rd = 0;

        r = File_WriteAll("/av2_cr.txt", "k=1\rx\nname=old\n", 15);
        if (r == FM_OK) r = Text_Replace("/av2_cr.txt", "old", "new");
        memset(back, 0, sizeof(back));
        if (r == FM_OK) r = File_ReadAll("/av2_cr.txt", back, sizeof(back) - 1u, &rd);
        if (r == FM_OK && rd == 15u && memcmp(back, "k=1\rx\nname=new\n", 15) == 0)
            PASS();
        else
            FAIL("content changed outside the replacement");
    }
    (void)File_Delete("/av2_cr.txt");
}
#endif /* FM_USE_TEXT */

/* FILE-05: the re-initialisation guard of file streams */
static void av2_stream_rebind(void)
{
    FM_File *f;
    FM_Stream s;
    FM_Result r;
    int i;
    int bad = 0;

    TEST("FILE-05: Stream_InitFile on a bound stream is refused");
    memset(&s, 0, sizeof(s));
    r = File_WriteAll("/av2_st.bin", "stream", 6);
    f = (r == FM_OK) ? File_Open("/av2_st.bin", FM_OPEN_READ) : NULL;
    if (f == NULL)
    {
        FAIL("set-up");
        return;
    }
    r = Stream_InitFile(&s, f);
    if (r != FM_OK) bad = 1;
    for (i = 0; i < 6 && !bad; i++)
    {
        /* used to succeed four times, each taking one of the four
         * contexts for good, and then fail with FM_ERR_NO_MEMORY */
        if (Stream_InitFile(&s, f) != FM_ERR_BUSY) bad = 1;
    }
    if (!bad)
    {
        char b[8];
        size_t rd = 0;

        r = Stream_Read(&s, b, 6, &rd);
        if (r != FM_OK || rd != 6u || memcmp(b, "stream", 6) != 0) bad = 1;
    }
    Stream_Close(&s);
    if (bad) FAIL("guard did not hold"); else PASS();

    TEST("FILE-05: the context comes back with Stream_Close");
    bad = 0;
    for (i = 0; i < 12 && !bad; i++)
    {
        r = Stream_InitFile(&s, f);
        if (r != FM_OK) bad = 1;
        Stream_Close(&s);
    }
    (void)File_Close(f);
    (void)File_Delete("/av2_st.bin");
    if (bad) FAIL("stream contexts leaked"); else PASS();
}

/* FILE-06: a directory is not a file, on the POSIX adapter either */
static void av2_posix_open_dir(void)
{
    FM_File *f;
    FM_Result r;

    TEST("FILE-06: File_Open on a directory (POSIX)");
    r = Directory_Create("/av2_dir");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        FAIL("set-up");
        return;
    }
    f = File_Open("/av2_dir", FM_OPEN_READ);
    if (f != NULL)
    {
        (void)File_Close(f);
        FAIL("a directory was opened as a file");
    }
    else if (File_GetLastError() != FM_ERR_IS_DIRECTORY)
    {
        FAIL(FM_ResultString(File_GetLastError()));
    }
    else
    {
        PASS();
    }
    (void)Directory_Delete("/av2_dir");
}

/* Seeking in front of the first byte: one answer on every backend */
static FM_Result av2_seek_before_start(int kind)
{
    const char *root = "";
    char path[64];
    FM_File *h = NULL;
    uint64_t pos = 0;
    FM_Result r;
    FM_Result q;

    r = av2_mount(kind, &root);
    av2_path(path, sizeof(path), root, "av2_neg.txt");
    if (r == FM_OK) r = File_WriteAll(path, "0123456789", 10);
    if (r == FM_OK)
    {
        h = File_Open(path, FM_OPEN_READ);
        if (h == NULL) r = File_GetLastError();
    }
    if (r == FM_OK) r = File_Seek(h, 4, FM_SEEK_SET);
    if (r == FM_OK)
    {
        q = File_Seek(h, -1, FM_SEEK_SET);
        if (q != FM_ERR_INVALID_ARGUMENT) { printf("[SET: %s] ", FM_ResultString(q)); r = FM_ERR_INTERNAL; }
        q = File_Seek(h, -5, FM_SEEK_CUR);
        if (q != FM_ERR_INVALID_ARGUMENT) { printf("[CUR: %s] ", FM_ResultString(q)); r = FM_ERR_INTERNAL; }
        q = File_Seek(h, -11, FM_SEEK_END);
        if (q != FM_ERR_INVALID_ARGUMENT) { printf("[END: %s] ", FM_ResultString(q)); r = FM_ERR_INTERNAL; }
    }
    if (r == FM_OK) r = File_Tell(h, &pos);
    if (r == FM_OK && pos != 4u)
    {
        printf("[position %u, want 4] ", (unsigned)pos);
        r = FM_ERR_INTERNAL;
    }
    if (h != NULL) (void)File_Close(h);
    (void)File_Delete(path);
    av2_unmount(kind);
    return r;
}

#if FM_USE_FMFS && FM_USE_SAFE_WRITE
/* SUSP-02 / SUSP-04: an FMFS volume whose adapter can be told to fail */
static FM_FS_FileCloseFn av2_real_close;
static FM_FS_FileWriteFn av2_real_write;
static FM_FS_RenameFn    av2_real_rename;
static int  av2_close_fails;     /* the next close reports an I/O error */
static long av2_write_budget;    /* writes left before they fail; <0 off */
static long av2_rename_budget;   /* the same for renames */

static FM_Result av2_bad_close(FM_FS *fs, void *fh)
{
    FM_Result r = av2_real_close(fs, fh);

    if (av2_close_fails)
    {
        av2_close_fails = 0;
        return FM_ERR_IO;
    }
    return r;
}

static FM_Result av2_bad_write(FM_FS *fs, void *fh, const void *buf,
                               uint32_t len, uint32_t *put)
{
    if (av2_write_budget == 0)
    {
        if (put != NULL) *put = 0;
        return FM_ERR_IO;
    }
    if (av2_write_budget > 0) av2_write_budget--;
    return av2_real_write(fs, fh, buf, len, put);
}

static FM_Result av2_bad_rename(FM_FS *fs, const char *from, const char *to)
{
    if (av2_rename_budget == 0) return FM_ERR_IO;
    if (av2_rename_budget > 0) av2_rename_budget--;
    return av2_real_rename(fs, from, to);
}

static FM_Result av2_faulty_mount(void)
{
    FM_Result r;

    av2_close_fails = 0;
    av2_write_budget = -1;
    av2_rename_budget = -1;
    r = fc_storage(0, 0, 1);
    if (r == FM_OK) r = fc_init(0, &g_av2_fs);
    if (r == FM_OK)
    {
        av2_real_close = g_av2_fs.ops.file_close;
        av2_real_write = g_av2_fs.ops.file_write;
        av2_real_rename = g_av2_fs.ops.rename;
        g_av2_fs.ops.file_close = av2_bad_close;
        g_av2_fs.ops.file_write = av2_bad_write;
        g_av2_fs.ops.rename = av2_bad_rename;
        r = FS_Mount(g_fc_mount[0], &g_av2_fs);
    }
    return r;
}

static void av2_safe_write(void)
{
    FM_Result r;
    FM_Result q;
    FM_Bool exists = FM_TRUE;

    TEST("SUSP-02: SafeReplace keeps the file when the close fails");
    r = av2_faulty_mount();
    if (r == FM_OK) r = File_WriteAll("/fca/cfg.txt", "good", 4);
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        av2_close_fails = 1;
        q = File_SafeReplace("/fca/cfg.txt", "NEWDATA", 7);
        av2_close_fails = 0;
        r = av2_expect("/fca/cfg.txt", "good");
        if (r == FM_OK) r = File_Exists("/fca/cfg.txt.fm_tmp", &exists);
        if (q == FM_OK) FAIL("reported success although the close failed");
        else if (r != FM_OK) FAIL("the good file was replaced");
        else if (exists) FAIL("temp file left behind");
        else PASS();
    }
    fc_drop(0);

    TEST("SUSP-04: a failed Transaction leaves the original in place");
    r = av2_faulty_mount();
    if (r == FM_OK) r = File_WriteAll("/fca/cfg.txt", "original", 8);
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        /* one write for the backup copy, then the volume refuses */
        av2_write_budget = 1;
        q = File_Transaction("/fca/cfg.txt", "replacement", 11);
        av2_write_budget = -1;
        r = av2_expect("/fca/cfg.txt", "original");
        if (q == FM_OK) FAIL("reported success");
        else if (r != FM_OK) FAIL("original lost");
        else PASS();
    }
    fc_drop(0);

    TEST("SUSP-04: the backup survives a restore that fails too");
    r = av2_faulty_mount();
    if (r == FM_OK) r = File_WriteAll("/fca/cfg.txt", "original", 8);
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        /* the backup copy gets its write and its rename; after that
         * nothing can be written and nothing renamed */
        av2_write_budget = 1;
        av2_rename_budget = 1;
        q = File_Transaction("/fca/cfg.txt", "replacement", 11);
        av2_write_budget = -1;
        av2_rename_budget = -1;
        r = av2_expect("/fca/cfg.txt.fm_tmp.bak", "original");
        if (q == FM_OK) FAIL("reported success");
        else if (r != FM_OK) FAIL("the last copy of the data was deleted");
        else PASS();
    }
    fc_drop(0);
}
#endif /* FM_USE_FMFS && FM_USE_SAFE_WRITE */

#if FM_USE_EXPLORER
/* SUSP-03 */
static void av2_explorer(void)
{
    static FM_Explorer ex;
    FM_Result r = FM_OK;
    char path[FM_PATH_MAX];
    uint32_t i;
    int bad = 0;

    TEST("SUSP-03: Explorer says when the listing is cut short");
    (void)Directory_DeleteRecursive("/av2_ex");
    r = Directory_Create("/av2_ex");
    for (i = 0; i < FM_MAX_EXPLORER_ITEMS + 3u && r == FM_OK; i++)
    {
        (void)snprintf(path, sizeof(path), "/av2_ex/f%03u.txt", (unsigned)i);
        r = File_WriteAll(path, "x", 1);
    }
    if (r == FM_OK) r = Explorer_Open(&ex, "/av2_ex");
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        if (Explorer_GetItemCount(&ex) != FM_MAX_EXPLORER_ITEMS) bad = 1;
        if (!Explorer_IsTruncated(&ex)) bad = 1;
        /* a directory that fits is not flagged */
        (void)snprintf(path, sizeof(path), "/av2_ex/f%03u.txt", 0u);
        for (i = 0; i < 3u; i++)
        {
            (void)snprintf(path, sizeof(path), "/av2_ex/f%03u.txt", (unsigned)i);
            (void)File_Delete(path);
        }
        if (Explorer_Refresh(&ex) != FM_OK) bad = 1;
        if (Explorer_GetItemCount(&ex) != FM_MAX_EXPLORER_ITEMS) bad = 1;
        if (Explorer_IsTruncated(&ex)) bad = 1;
        if (bad) FAIL("truncation flag wrong"); else PASS();
    }
    (void)Directory_DeleteRecursive("/av2_ex");

    TEST("SUSP-03: Explorer_Back below the history limit");
    bad = 0;
    (void)Directory_DeleteRecursive("/av2_dp");
    (void)strcpy(path, "/av2_dp");
    r = Directory_Create(path);
    for (i = 0; i < FM_EXPLORER_MAX_DEPTH + 2u && r == FM_OK; i++)
    {
        (void)strcat(path, "/d");
        r = Directory_Create(path);
    }
    if (r == FM_OK) r = Explorer_Open(&ex, "/av2_dp");
    for (i = 0; i < FM_EXPLORER_MAX_DEPTH + 2u && r == FM_OK; i++)
    {
        r = Explorer_Enter(&ex);
    }
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        char want[FM_PATH_MAX];
        char got[FM_PATH_MAX];

        /* one step back is one level up, also past the history limit */
        (void)strcpy(want, path);
        want[strlen(want) - 2u] = '\0';
        if (Explorer_Back(&ex) != FM_OK) bad = 1;
        if (Explorer_GetCurrentPath(&ex, got, sizeof(got)) != FM_OK) bad = 1;
        if (!bad && strcmp(got, want) != 0)
        {
            printf("[at %s] ", got);
            bad = 1;
        }
        if (bad) FAIL("Back did not return to the previous directory");
        else PASS();
    }
    (void)Directory_DeleteRecursive("/av2_dp");
}
#endif

static void test_audit_v2_file(void)
{
    printf("\n[External audit v2: files]\n");
    restore_posix_root();

    av2_each_backend("FILE-01: truncate, read-only handle", av2_truncate_readonly);
    av2_each_backend("FILE-02: append after the file grew", av2_append_after_grow);
    av2_each_backend("FILE-02: two append handles", av2_append_two_handles);
    av2_each_backend("FM_OPEN_APPEND writes at the end", av2_append_ignores_seek);
#if FM_USE_LITTLEFS
    av2_lfs_one_writer();
#endif
#if FM_USE_TEXT
    av2_text_cr();
#endif
    av2_stream_rebind();
    av2_posix_open_dir();
    av2_each_backend("seek in front of the first byte", av2_seek_before_start);
#if FM_USE_FMFS && FM_USE_SAFE_WRITE
    av2_safe_write();
#endif
#if FM_USE_EXPLORER
    av2_explorer();
#endif
}
