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

/* SUSP-01 and what it led to: a tree deeper than the backend has
 * directory handles (FM_MAX_OPEN_DIRS) is scanned, measured, copied and
 * deleted */
static FM_Result av2_count_cb(const FM_FileInfo *info, void *user)
{
    uint32_t *n = (uint32_t *)user;

    if (info->type == FM_ITEM_FILE) n[0]++; else n[1]++;
    return FM_OK;
}

static FM_Result av2_deep_tree(int kind)
{
    const char *root = "";
    char top[64];
    char copy[64];
    char path[FM_PATH_MAX];
    char file[FM_PATH_MAX];
    uint32_t levels = FM_MAX_OPEN_DIRS + 2u;
    uint32_t n[2] = { 0, 0 };
    uint64_t size = 0;
    FM_Bool exists = FM_TRUE;
    uint32_t i;
    FM_Result r;

    r = av2_mount(kind, &root);
    av2_path(top, sizeof(top), root, "av2_tree");
    av2_path(copy, sizeof(copy), root, "av2_copy");
    (void)Directory_DeleteRecursive(top);
    (void)Directory_DeleteRecursive(copy);
    (void)strcpy(path, top);
    if (r == FM_OK) r = Directory_Create(path);
    for (i = 0; i < levels && r == FM_OK; i++)
    {
        /* a file in front of and one behind the subdirectory, so that
         * coming back up has something left to do */
        (void)strcpy(file, path);
        (void)strcat(file, "/a.txt");
        r = File_WriteAll(file, "12345", 5);
        (void)strcat(path, "/d");
        if (r == FM_OK) r = Directory_Create(path);
        if (r == FM_OK && i == 0u)
        {
            (void)strcpy(file, top);
            (void)strcat(file, "/z.txt");
            r = File_WriteAll(file, "678", 3);
        }
    }
    if (r != FM_OK) printf("[build] ");
    if (r == FM_OK)
    {
        r = Directory_ScanRecursive(top, av2_count_cb, NULL, n, NULL);
        if (r != FM_OK) printf("[scan] ");
        else if (n[0] != levels + 1u || n[1] != levels)
        {
            printf("[scan saw %u files, %u dirs] ", (unsigned)n[0], (unsigned)n[1]);
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = Directory_Size(top, &size);
        if (r != FM_OK) printf("[size] ");
        else if (size != (uint64_t)levels * 5u + 3u)
        {
            printf("[size %u] ", (unsigned)size);
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = Directory_Copy(top, copy, NULL, NULL);
        if (r != FM_OK) printf("[copy] ");
    }
    if (r == FM_OK)
    {
        n[0] = 0;
        n[1] = 0;
        r = Directory_ScanRecursive(copy, av2_count_cb, NULL, n, NULL);
        if (r == FM_OK && (n[0] != levels + 1u || n[1] != levels))
        {
            printf("[copy has %u files, %u dirs] ", (unsigned)n[0], (unsigned)n[1]);
            r = FM_ERR_INTERNAL;
        }
        /* the deepest file made it, with its content */
        (void)strcpy(path, copy);
        for (i = 0; i + 1u < levels; i++) (void)strcat(path, "/d");
        (void)strcat(path, "/a.txt");
        if (r == FM_OK) r = av2_expect(path, "12345");
    }
    if (r == FM_OK)
    {
        r = Directory_DeleteRecursive(top);
        if (r != FM_OK) printf("[delete] ");
    }
    if (r == FM_OK) r = Directory_DeleteRecursive(copy);
    if (r == FM_OK) r = Directory_Exists(top, &exists);
    if (r == FM_OK && exists) r = FM_ERR_EXISTS;
    (void)Directory_DeleteRecursive(top);
    (void)Directory_DeleteRecursive(copy);
    av2_unmount(kind);
    return r;
}

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
    av2_each_backend("tree deeper than FM_MAX_OPEN_DIRS", av2_deep_tree);
#if FM_USE_FMFS && FM_USE_SAFE_WRITE
    av2_safe_write();
#endif
#if FM_USE_EXPLORER
    av2_explorer();
#endif
}

/* ================================================================ */
/* Video part                                                        */
/* ================================================================ */
#if FM_USE_VIDEO
/* VID-01: MP4 container boxes nested without end */
static uint8_t g_av2_mp4[4096];
static uint8_t g_av2_nest[8192];

/* The test MP4 with its "moov" wrapped in `extra` more "moov" boxes */
static FM_Result av2_nested_mp4(uint32_t extra, size_t *len)
{
    size_t mp4_len = 0;
    size_t ftyp;
    size_t moov;
    size_t out = 0;
    uint32_t i;
    FM_Result r;

    g_tst_mp4_len_size = 2u;
    g_tst_mp4_hevc = 1;
    r = build_mp4((const uint8_t *)"SPSx", 4, (const uint8_t *)"PPS", 3,
                  (const uint8_t *)"IDR00", 5, (const uint8_t *)"P1", 2,
                  (const uint8_t *)"P2", 2, g_av2_mp4, &mp4_len);
    g_tst_mp4_len_size = 4u;
    g_tst_mp4_hevc = 0;
    if (r != FM_OK) return r;
    ftyp = ((size_t)g_av2_mp4[2] << 8) | g_av2_mp4[3];
    if (ftyp + 8u > mp4_len || memcmp(g_av2_mp4 + ftyp + 4u, "moov", 4) != 0)
    {
        return FM_ERR_INTERNAL;
    }
    moov = ((size_t)g_av2_mp4[ftyp + 2u] << 8) | g_av2_mp4[ftyp + 3u];
    if (mp4_len + 8u * extra > sizeof(g_av2_nest)) return FM_ERR_TOO_LARGE;
    memcpy(g_av2_nest, g_av2_mp4, ftyp);
    out = ftyp;
    for (i = 0; i < extra; i++)
    {
        size_t size = moov + 8u * (extra - i);

        g_av2_nest[out++] = 0;
        g_av2_nest[out++] = 0;
        g_av2_nest[out++] = (uint8_t)(size >> 8);
        g_av2_nest[out++] = (uint8_t)size;
        memcpy(g_av2_nest + out, "moov", 4);
        out += 4u;
    }
    memcpy(g_av2_nest + out, g_av2_mp4 + ftyp, mp4_len - ftyp);
    out += mp4_len - ftyp;
    *len = out;
    return FM_OK;
}

static void test_audit_v2_video(void)
{
    static FM_Video vid;
    size_t len = 0;
    FM_Result r;
    FM_Result q;

    printf("\n[External audit v2: video]\n");
    restore_posix_root();

    TEST("VID-01: MP4 boxes nested a little still open");
    r = av2_nested_mp4(2u, &len);
    if (r == FM_OK) r = File_WriteAll("/av2_nest.mp4", g_av2_nest, len);
    memset(&vid, 0, sizeof(vid));
    if (r == FM_OK) r = Video_Open("/av2_nest.mp4", &vid);
    if (r == FM_OK)
    {
        if (vid.info.frame_count != 3u) r = FM_ERR_INVALID_FORMAT;
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "nested twice");

    TEST("VID-01: MP4 boxes nested past the limit are refused");
    r = av2_nested_mp4(12u, &len);
    if (r == FM_OK) r = File_WriteAll("/av2_nest.mp4", g_av2_nest, len);
    if (r != FM_OK)
    {
        FAIL("set-up");
    }
    else
    {
        /* the search used to follow the boxes as deep as the file went,
         * one stack frame per level */
        memset(&vid, 0, sizeof(vid));
        q = Video_Open("/av2_nest.mp4", &vid);
        if (q == FM_OK)
        {
            (void)Video_Close(&vid);
            FAIL("opened: the nesting was followed without a limit");
        }
        else
        {
            PASS();
        }
    }

    TEST("VID-01: 750 nested boxes, nothing else");
    {
        uint32_t i;
        const uint32_t n = 750u;

        memset(g_av2_nest, 0, sizeof(g_av2_nest));
        memcpy(g_av2_nest, "\0\0\0\014ftypisom", 12);
        len = 12u;
        for (i = 0; i < n; i++)
        {
            uint32_t size = 8u * (n - i);

            g_av2_nest[len++] = 0;
            g_av2_nest[len++] = 0;
            g_av2_nest[len++] = (uint8_t)(size >> 8);
            g_av2_nest[len++] = (uint8_t)size;
            memcpy(g_av2_nest + len, "moov", 4);
            len += 4u;
        }
        r = File_WriteAll("/av2_nest.mp4", g_av2_nest, len);
        memset(&vid, 0, sizeof(vid));
        q = (r == FM_OK) ? Video_Open("/av2_nest.mp4", &vid) : FM_OK;
        if (r != FM_OK) FAIL("set-up");
        else if (q == FM_OK) { (void)Video_Close(&vid); FAIL("opened"); }
        else PASS();
    }
    (void)File_Delete("/av2_nest.mp4");
}
#endif /* FM_USE_VIDEO */

/* ================================================================ */
/* Image part                                                        */
/* ================================================================ */
#if FM_USE_IMAGE
static uint32_t av2_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

#if FM_USE_PNG
/* IMG-01: the chunks of an encoded PNG must add up to the file, and the
 * picture must come back.  Returns 0, or the step that failed. */
static int av2_png_roundtrip(uint16_t w, uint16_t h)
{
    size_t raw = (size_t)w * h * 3u;
    size_t cap = raw + raw / 1000u + 4096u + (size_t)h * 8u;
    uint8_t *src = (uint8_t *)malloc(raw);
    uint8_t *png = (uint8_t *)malloc(cap);
    uint8_t *out = (uint8_t *)malloc(raw);
    FM_Image img;
    FM_Image dec;
    size_t len = 0;
    size_t pos = 8u;
    size_t i;
    int idat = 0;
    int iend = 0;
    int why = 0;

    if (src == NULL || png == NULL || out == NULL)
    {
        why = 1;
    }
    else
    {
        for (i = 0; i < raw; i++)
        {
            src[i] = (uint8_t)((i * 2654435761u) >> 13);
        }
        memset(&img, 0, sizeof(img));
        img.width = w; img.height = h;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3; img.bit_depth = 24;
        img.data = src; img.data_size = raw; img.data_capacity = raw;
        if (Image_EncodePNG(&img, png, cap, &len) != FM_OK) why = 2;
    }
    /* every chunk: length, type, data, CRC -- and the last one is IEND,
     * ending exactly where the file ends */
    while (why == 0 && !iend)
    {
        uint32_t clen;

        if (pos + 12u > len) { why = 3; break; }
        clen = av2_be32(png + pos);
        if (clen > len - pos - 12u) { why = 4; break; }
        if (memcmp(png + pos + 4u, "IDAT", 4) == 0) idat++;
        else if (memcmp(png + pos + 4u, "IEND", 4) == 0) iend = 1;
        else if (memcmp(png + pos + 4u, "IHDR", 4) != 0) { why = 5; break; }
        pos += 12u + clen;
    }
    if (why == 0 && (idat != 1 || pos != len)) why = 6;
    if (why == 0)
    {
        memset(out, 0, raw);
        memset(&dec, 0, sizeof(dec));
        if (Image_DecodePNG(png, len, &dec, out, raw) != FM_OK) why = 7;
        else if (dec.width != w || dec.height != h) why = 8;
        else if (memcmp(out, src, raw) != 0) why = 9;
    }
    free(src); free(png); free(out);
    return why;
}
#endif /* FM_USE_PNG */

/* IMG-02: a buffer sized from Image_GetInfo holds the picture at full
 * size, in the layout Image_GetInfo announced. */
static int av2_info_matches_load(const char *path)
{
    FM_ImageInfo info;
    FM_Image img;
    uint8_t *buf;
    int why = 0;

    memset(&info, 0, sizeof(info));
    if (Image_GetInfo(path, &info) != FM_OK) return 1;
    if (info.data_size == 0u || info.data_size > (16u << 20)) return 2;
    buf = (uint8_t *)malloc((size_t)info.data_size);
    if (buf == NULL) return 3;
    memset(&img, 0, sizeof(img));
    if (Image_Load(path, &img, buf, (size_t)info.data_size) != FM_OK) why = 4;
    else if (img.width != info.width || img.height != info.height) why = 5;
    else if (img.channels != info.channels) why = 6;
    else if (img.data_size != info.data_size) why = 7;
    free(buf);
    return why;
}

/* IMG-03: an 8-bpp BMP, bottom-up, `used` colour-table entries (0 = the
 * field says 0, 256 written).  kind 0: colour ramp, 1: grey ramp. */
#define AV2_BMP_W 13u
#define AV2_BMP_H 7u
static uint8_t g_av2_bmp[14 + 40 + 256 * 4 + 16 * AV2_BMP_H];

static void av2_pal_color(int kind, uint32_t i, uint8_t *rgb)
{
    if (kind == 1)
    {
        rgb[0] = rgb[1] = rgb[2] = (uint8_t)i;
    }
    else
    {
        rgb[0] = (uint8_t)(3u * i);
        rgb[1] = (uint8_t)(255u - 3u * i);
        rgb[2] = (uint8_t)(5u * i);
    }
}

static uint8_t av2_bmp_index(uint32_t x, uint32_t y)
{
    return (uint8_t)(x * 19u + y * 37u);
}

static size_t av2_build_bmp8(int kind, uint32_t used)
{
    uint32_t entries = (used == 0u) ? 256u : used;
    uint32_t stride = (AV2_BMP_W + 3u) & ~3u;
    uint32_t off = 14u + 40u + entries * 4u;
    uint32_t total = off + stride * AV2_BMP_H;
    uint8_t *p = g_av2_bmp;
    uint32_t i;
    uint32_t x;
    uint32_t y;

    if (entries > 256u) entries = 256u, off = 14u + 40u + 256u * 4u,
                        total = off + stride * AV2_BMP_H;
    memset(p, 0, sizeof(g_av2_bmp));
    p[0] = 'B'; p[1] = 'M';
    p[2] = (uint8_t)total; p[3] = (uint8_t)(total >> 8);
    p[10] = (uint8_t)off; p[11] = (uint8_t)(off >> 8);
    p[14] = 40;
    p[18] = (uint8_t)AV2_BMP_W;
    p[22] = (uint8_t)AV2_BMP_H;
    p[26] = 1;
    p[28] = 8;
    p[46] = (uint8_t)used; p[47] = (uint8_t)(used >> 8);
    for (i = 0; i < entries; i++)
    {
        uint8_t rgb[3];

        av2_pal_color(kind, i, rgb);
        p[54u + i * 4u] = rgb[2];
        p[54u + i * 4u + 1u] = rgb[1];
        p[54u + i * 4u + 2u] = rgb[0];
    }
    for (y = 0; y < AV2_BMP_H; y++)         /* file rows run bottom-up */
    {
        for (x = 0; x < AV2_BMP_W; x++)
        {
            p[off + (AV2_BMP_H - 1u - y) * stride + x] = av2_bmp_index(x, y);
        }
    }
    return total;
}

static uint8_t g_av2_rows[AV2_BMP_W * AV2_BMP_H * 3u];
static uint32_t g_av2_row_count;

static void av2_line_cb(const void *line, uint32_t y, void *user_data)
{
    (void)user_data;
    if (y < AV2_BMP_H)
    {
        memcpy(g_av2_rows + y * AV2_BMP_W * 3u, line, AV2_BMP_W * 3u);
        g_av2_row_count++;
    }
}

/* 0 when `px` holds the picture as RGB888 through the table */
static int av2_bmp_colors_ok(const uint8_t *px, int kind, uint32_t used)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < AV2_BMP_H; y++)
    {
        for (x = 0; x < AV2_BMP_W; x++)
        {
            uint8_t want[3] = { 0, 0, 0 };
            uint8_t i = av2_bmp_index(x, y);

            if (used == 0u || i < used) av2_pal_color(kind, i, want);
            if (memcmp(px + (y * AV2_BMP_W + x) * 3u, want, 3) != 0) return 1;
        }
    }
    return 0;
}

static void test_audit_v2_image(void)
{
    static uint8_t px[AV2_BMP_W * AV2_BMP_H * 3u];
    FM_Image img;
    FM_ImageInfo info;
    FM_Result r;
    size_t len;
    char msg[96];

    printf("\n[External audit v2: image]\n");
    restore_posix_root();

#if FM_USE_PNG
    {
        static const uint16_t dims[][2] = {
            { 640, 480 }, { 800, 600 }, { 1280, 800 }, { 4369, 9 },
            { 1, 1 }, { 5461, 3 }, { 300, 219 }
        };
        size_t i;

        for (i = 0; i < sizeof(dims) / sizeof(dims[0]); i++)
        {
            int why;
            char name[64];

            (void)snprintf(name, sizeof(name),
                           "IMG-01: PNG encode %ux%u is a valid file",
                           (unsigned)dims[i][0], (unsigned)dims[i][1]);
            TEST(name);
            why = av2_png_roundtrip(dims[i][0], dims[i][1]);
            if (why != 0)
            {
                (void)snprintf(msg, sizeof(msg), "step %d", why);
                FAIL(msg);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("IMG-02: a buffer sized from Image_GetInfo holds every PNG");
    {
        static const char *const names[] = {
            "big_rgb8_a7", "ga16_a7", "ga8", "gray1", "gray16", "gray2_a7",
            "gray4", "gray8_a7", "pal1_a7", "pal2", "pal4_a7", "pal8_a7",
            "rgb16", "rgb8_a7", "rgba16_a7", "rgba8_a7", "tall_gray4_a7"
        };
        size_t i;
        int why = 0;

        for (i = 0; i < sizeof(names) / sizeof(names[0]) && why == 0; i++)
        {
            char path[96];

            (void)snprintf(path, sizeof(path), "/test/vectors/png/%s.png",
                           names[i]);
            why = av2_info_matches_load(path);
            if (why != 0)
            {
                (void)snprintf(msg, sizeof(msg), "%s: step %d", names[i], why);
            }
        }
        if (why != 0) FAIL(msg); else PASS();
    }
#endif /* FM_USE_PNG */

    TEST("IMG-02: the same for BMP and JPEG");
    {
        int why = av2_info_matches_load("/test/vectors/tiny64.bmp");

#if FM_USE_JPEG
        if (why == 0)
        {
            why = 10 * av2_info_matches_load("/test/vectors/tiny64.jpg");
        }
#endif
        if (why != 0)
        {
            (void)snprintf(msg, sizeof(msg), "step %d", why);
            FAIL(msg);
        }
        else
        {
            PASS();
        }
    }

    TEST("IMG-03: 8-bpp BMP with a colour table comes out in its colours");
    len = av2_build_bmp8(0, 0u);
    memset(&img, 0, sizeof(img));
    memset(px, 0xEE, sizeof(px));
    r = Image_DecodeBMP(g_av2_bmp, len, &img, px, sizeof(px));
    if (r != FM_OK) { CHECK_RESULT(r, "decode"); }
    else if (img.format != FM_PIXEL_RGB888 || img.channels != 3u)
    {
        FAIL("indices returned as GRAY8");
    }
    else if (av2_bmp_colors_ok(px, 0, 0u) != 0) FAIL("wrong colours");
    else PASS();

    TEST("IMG-03: Image_GetInfo and Image_Load agree on that file");
    r = File_WriteAll("/av2_pal.bmp", g_av2_bmp, len);
    memset(&info, 0, sizeof(info));
    if (r == FM_OK) r = Image_GetInfo("/av2_pal.bmp", &info);
    if (r != FM_OK) { CHECK_RESULT(r, "info"); }
    else if (info.channels != 3u || info.bit_depth != 24u ||
             info.data_size != (uint64_t)AV2_BMP_W * AV2_BMP_H * 3u)
    {
        FAIL("reported as one channel");
    }
    else
    {
        int why = av2_info_matches_load("/av2_pal.bmp");

        if (why != 0)
        {
            (void)snprintf(msg, sizeof(msg), "step %d", why);
            FAIL(msg);
        }
        else
        {
            PASS();
        }
    }

    TEST("IMG-03: the same picture row by row (Image_StreamDecode)");
    {
        FM_Stream st;

        memset(&img, 0, sizeof(img));
        memset(g_av2_rows, 0xEE, sizeof(g_av2_rows));
        g_av2_row_count = 0;
        r = Stream_InitMemoryRead(&st, g_av2_bmp, len);
        if (r == FM_OK)
        {
            r = Image_StreamDecode(&st, &img, av2_line_cb, NULL, NULL);
            Stream_Close(&st);
        }
        if (r != FM_OK) { CHECK_RESULT(r, "stream decode"); }
        else if (g_av2_row_count != AV2_BMP_H || img.channels != 3u ||
                 av2_bmp_colors_ok(g_av2_rows, 0, 0u) != 0)
        {
            FAIL("rows are not the picture");
        }
        else PASS();
    }

    TEST("IMG-03: scaled down to fit a small buffer, still in colour");
    {
        /* 6x3 at most: every output pixel is one of the source pixels */
        static uint8_t small[6u * 3u * 3u + 2u];

        memset(&img, 0, sizeof(img));
        r = Image_DecodeBMP(g_av2_bmp, len, &img, small, sizeof(small));
        if (r != FM_OK) { CHECK_RESULT(r, "decode small"); }
        else if (img.channels != 3u || img.width == 0u ||
                 img.width > AV2_BMP_W || img.height > AV2_BMP_H)
        {
            FAIL("layout");
        }
        else
        {
            uint8_t want[3];

            av2_pal_color(0, av2_bmp_index(0, 0), want);
            if (memcmp(small, want, 3) != 0) FAIL("first pixel");
            else PASS();
        }
    }

    TEST("IMG-03: short colour table: missing entries are black");
    len = av2_build_bmp8(0, 16u);
    memset(&img, 0, sizeof(img));
    r = Image_DecodeBMP(g_av2_bmp, len, &img, px, sizeof(px));
    if (r != FM_OK) { CHECK_RESULT(r, "decode"); }
    else if (img.channels != 3u || av2_bmp_colors_ok(px, 0, 16u) != 0)
    {
        FAIL("wrong colours");
    }
    else PASS();

    TEST("IMG-03: grey-ramp colour table is still GRAY8, bytes untouched");
    len = av2_build_bmp8(1, 0u);
    memset(&img, 0, sizeof(img));
    memset(px, 0xEE, sizeof(px));
    r = Image_DecodeBMP(g_av2_bmp, len, &img, px, sizeof(px));
    if (r != FM_OK) { CHECK_RESULT(r, "decode"); }
    else if (img.format != FM_PIXEL_GRAY8 || img.channels != 1u ||
             img.data_size != (uint64_t)AV2_BMP_W * AV2_BMP_H)
    {
        FAIL("layout");
    }
    else
    {
        uint32_t x;
        uint32_t y;
        int bad = 0;

        for (y = 0; y < AV2_BMP_H; y++)
        {
            for (x = 0; x < AV2_BMP_W; x++)
            {
                if (px[y * AV2_BMP_W + x] != av2_bmp_index(x, y)) bad = 1;
            }
        }
        if (bad || px[AV2_BMP_W * AV2_BMP_H] != 0xEE) FAIL("pixels");
        else PASS();
    }

    TEST("IMG-03: colour table larger than 256 entries is refused");
    len = av2_build_bmp8(0, 0u);
    g_av2_bmp[46] = 0x2C; g_av2_bmp[47] = 0x01;         /* 300 */
    memset(&img, 0, sizeof(img));
    r = Image_DecodeBMP(g_av2_bmp, len, &img, px, sizeof(px));
    if (r == FM_ERR_CORRUPTED) PASS();
    else { (void)snprintf(msg, sizeof(msg), "got %d", (int)r); FAIL(msg); }

    (void)File_Delete("/av2_pal.bmp");
}
#endif /* FM_USE_IMAGE */
