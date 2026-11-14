/*
 * FileManager host tests -- external audit, round 2 (ReportV2.txt).
 *
 * One test (or a small group) per finding that was reproduced.  Each of
 * them fails -- or aborts -- on the code before the fix.  Included by
 * test/main.c; uses its TEST / PASS / FAIL macros.
 */

#include <time.h>

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
        av2_rename_budget = 0;
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

/* The name filter of Directory_List ('*' only) */
static void av2_wildcard(void)
{
    static const struct { const char *filter; uint32_t files; } k[] = {
        { "*", 5 }, { "*.txt", 2 }, { "a*", 4 }, { "*c*", 3 },
        { "a*b*c.txt", 2 }, { "abc.txt", 1 }, { "**.log**", 1 },
        { "*.tx", 0 }, { "bc.txt", 0 }, { "a*a*b", 0 },
        /* every split of every star: the recursive matcher did not come
         * back from this one */
        { "*a*a*a*a*a*a*a*a*a*a*a*a*b", 0 },
        { "*a*a*a*a*a*a*a*a*a*a*a*a*", 2 }
    };
    char longname[140];
    FM_Result r;
    size_t i;
    int bad = 0;
    char msg[96];

    TEST("Directory_List filter: '*' semantics, and no blow-up");
    (void)Directory_DeleteRecursive("/av2wild");
    r = Directory_Create("/av2wild");
    if (r == FM_OK) r = File_WriteAll("/av2wild/abc.txt", "1", 1);
    if (r == FM_OK) r = File_WriteAll("/av2wild/aXbXc.txt", "1", 1);
    if (r == FM_OK) r = File_WriteAll("/av2wild/xcx.log", "1", 1);
    (void)strcpy(longname, "/av2wild/");
    memset(longname + 9, 'a', 28);
    longname[37] = '\0';
    if (r == FM_OK) r = File_WriteAll(longname, "1", 1);
    longname[36] = 'z';
    if (r == FM_OK) r = File_WriteAll(longname, "1", 1);
    for (i = 0; r == FM_OK && i < sizeof(k) / sizeof(k[0]) && !bad; i++)
    {
        uint32_t n[2] = { 0, 0 };
        clock_t t0 = clock();
        double took;

        r = Directory_List("/av2wild", k[i].filter, av2_count_cb, n);
        took = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        if (r == FM_OK && n[0] != k[i].files)
        {
            bad = 1;
            (void)snprintf(msg, sizeof(msg), "\"%.40s\": %u files, expected %u",
                           k[i].filter, (unsigned)n[0], (unsigned)k[i].files);
        }
        else if (r == FM_OK && took > 2.0)
        {
            /* five short names: this is microseconds of work */
            bad = 1;
            (void)snprintf(msg, sizeof(msg), "\"%.40s\" took %.1f s",
                           k[i].filter, took);
        }
    }
    (void)Directory_DeleteRecursive("/av2wild");
    if (r != FM_OK) { CHECK_RESULT(r, "set-up"); }
    else if (bad) FAIL(msg);
    else PASS();
}

static void test_audit_v2_file(void)
{
    printf("\n[External audit v2: files]\n");
    restore_posix_root();

    av2_wildcard();
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
#if FM_USE_VIDEO && FM_USE_H264     /* build_mp4() is the H.264 test helper */
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
#if FM_USE_PNG
static uint32_t av2_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

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

/* ================================================================ */
/* Audio part                                                        */
/* ================================================================ */
#if FM_USE_AUDIO
static uint8_t g_av2_abuf[64 * 1024];
static int16_t g_av2_pcm[8192];

static void av2_wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void av2_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t av2_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A WAV file of `format` in g_av2_abuf; returns its length */
static size_t av2_build_wav(uint16_t format, uint16_t channels, uint32_t rate,
                            uint16_t bits, uint16_t block_align,
                            const uint8_t *data, uint32_t data_len)
{
    uint8_t *p = g_av2_abuf;

    memcpy(p, "RIFF", 4); av2_wr32(p + 4, 36u + data_len);
    memcpy(p + 8, "WAVEfmt ", 8); av2_wr32(p + 16, 16);
    av2_wr16(p + 20, format); av2_wr16(p + 22, channels);
    av2_wr32(p + 24, rate); av2_wr32(p + 28, rate * block_align);
    av2_wr16(p + 32, block_align); av2_wr16(p + 34, bits);
    memcpy(p + 36, "data", 4); av2_wr32(p + 40, data_len);
    memcpy(p + 44, data, data_len);
    return 44u + data_len;
}

/* Decode a whole file through Audio_Read in chunks of `chunk` samples;
 * returns the result that ended the loop, *frames = frames delivered */
static FM_Result av2_decode_all(const char *path, size_t chunk,
                                uint64_t *frames, FM_AudioInfo *info)
{
    static FM_Audio a;
    FM_Result r;
    size_t got = 0;
    uint64_t total = 0;

    memset(&a, 0, sizeof(a));
    *frames = 0;
    r = Audio_Open(path, &a);
    if (r != FM_OK) return r;
    if (info != NULL) *info = a.info;
    while ((r = Audio_Read(&a, g_av2_pcm, chunk, &got)) == FM_OK && got > 0)
    {
        total += got;
    }
    *frames = total / a.info.channels;
    (void)Audio_Close(&a);
    return r;
}

static FM_Result av2_load(const char *path, size_t *len)
{
    size_t got = 0;
    FM_Result r = File_ReadAll(path, g_av2_abuf, sizeof(g_av2_abuf), &got);

    *len = got;
    return r;
}

#define AV2_RESULT(cond, r, what)                                          \
    do {                                                                   \
        if (cond) { PASS(); }                                              \
        else                                                               \
        {                                                                  \
            char m_[124];                                                  \
            (void)snprintf(m_, sizeof(m_), "%.100s (result %d)", (what),   \
                           (int)(r));                                      \
            FAIL(m_);                                                      \
        }                                                                  \
    } while (0)

/* ---- allocator that remembers the largest request (F14) ---- */
static size_t g_av2_max_alloc;
static unsigned long g_av2_alloc_calls;
static long g_av2_alloc_live;
static void *av2_alloc(size_t n, void *ctx)
{
    void *q;

    (void)ctx;
    if (n > g_av2_max_alloc) g_av2_max_alloc = n;
    g_av2_alloc_calls++;
    q = malloc(n);
    if (q != NULL) g_av2_alloc_live++;
    return q;
}
static void *av2_realloc(void *q, size_t n, void *ctx)
{
    (void)ctx;
    if (n > g_av2_max_alloc) g_av2_max_alloc = n;
    return realloc(q, n);
}
static void av2_free(void *q, void *ctx)
{
    (void)ctx;
    if (q != NULL) g_av2_alloc_live--;
    free(q);
}

/* ---- AAC plug-in stand-in (F5, F6, F16, F17) ---- */
static int      g_av2_aac_inits;
static int      g_av2_aac_closes;
static int      g_av2_aac_header_ok;    /* frames that arrived with header */
static int      g_av2_aac_frames;
static size_t   g_av2_aac_claim;        /* samples to report, 0 = honest   */

static FM_Result av2_aac_init(void **ctx, const FM_AudioInfo *info)
{
    (void)info;
    g_av2_aac_inits++;
    *ctx = &g_av2_aac_inits;
    return FM_OK;
}

static FM_Result av2_aac_decode(void *ctx, const uint8_t *in, size_t in_size,
                                int16_t *out, size_t out_count,
                                size_t *out_samples)
{
    size_t i;
    size_t n = (out_count < 64u) ? out_count : 64u;

    (void)ctx;
    g_av2_aac_frames++;
    if (in_size >= 7u && in[0] == 0xFFu && (in[1] & 0xF6u) == 0xF0u)
    {
        g_av2_aac_header_ok++;
    }
    for (i = 0; i < n; i++) out[i] = (int16_t)(100 + (int)i);
    *out_samples = (g_av2_aac_claim != 0u) ? g_av2_aac_claim : n;
    return FM_OK;
}

static void av2_aac_close(void *ctx)
{
    (void)ctx;
    g_av2_aac_closes++;
}

static const FM_AudioDecoderOps g_av2_aac_ops = {
    "AAC", av2_aac_init, av2_aac_decode, av2_aac_close
};

/* `frames` ADTS frames (mono, 44.1 kHz), 16 payload bytes each */
static size_t av2_build_adts(uint32_t frames)
{
    uint32_t i;
    size_t n = 0;

    for (i = 0; i < frames; i++)
    {
        uint8_t *h = g_av2_abuf + n;
        uint32_t flen = 7u + 16u;

        h[0] = 0xFF; h[1] = 0xF1;
        h[2] = (uint8_t)((1u << 6) | (4u << 2));        /* LC, 44.1 kHz */
        h[3] = (uint8_t)((1u << 6) | (flen >> 11));     /* 1 channel    */
        h[4] = (uint8_t)(flen >> 3);
        h[5] = (uint8_t)((flen << 5) | 0x1F);
        h[6] = 0xFC;
        memset(h + 7, (int)(i + 1u), 16);
        n += flen;
    }
    return n;
}

#if FM_USE_FMFS && FM_USE_SAFE_WRITE     /* av2_faulty_mount() */
/* ---- a mount whose reads / seeks can be made to fail (F6, F17) ---- */
static FM_FS_FileReadFn av2_real_read;
static FM_FS_FileSeekFn av2_real_seek;
static long av2_read_budget;
static long av2_seek_budget;

static FM_Result av2_bad_read(FM_FS *fs, void *fh, void *buf, uint32_t len,
                              uint32_t *got)
{
    if (av2_read_budget == 0)
    {
        if (got != NULL) *got = 0;
        return FM_ERR_IO;
    }
    if (av2_read_budget > 0) av2_read_budget--;
    return av2_real_read(fs, fh, buf, len, got);
}

static FM_Result av2_bad_seek(FM_FS *fs, void *fh, int64_t offset,
                              FM_SeekOrigin origin)
{
    if (av2_seek_budget == 0) return FM_ERR_IO;
    if (av2_seek_budget > 0) av2_seek_budget--;
    return av2_real_seek(fs, fh, offset, origin);
}

static FM_Result av2_faulty_read_mount(void)
{
    FM_Result r = av2_faulty_mount();

    av2_read_budget = -1;
    av2_seek_budget = -1;
    if (r == FM_OK)
    {
        /* the mount copied the ops: patch the mounted instance */
        FM_FS *m = &g_av2_fs;

        av2_real_read = m->ops.file_read;
        av2_real_seek = m->ops.file_seek;
        m->ops.file_read = av2_bad_read;
        m->ops.file_seek = av2_bad_seek;
    }
    return r;
}
#endif /* FM_USE_FMFS && FM_USE_SAFE_WRITE */

static void av2_stream_cb(const int16_t *samples, size_t count, void *user)
{
    (void)samples; (void)count; (void)user;
}

/* ---- DMA example (F13, F25, F30..F33) ---- */
static int16_t g_av2_tap_pre_first;
static int16_t g_av2_tap_post_first;
static int     g_av2_tap_pre_calls;

static void av2_tap_pre(void *ctx, int16_t *pcm, size_t samples,
                        uint32_t rate, uint16_t channels)
{
    (void)ctx; (void)rate; (void)channels;
    if (samples > 0u && g_av2_tap_pre_calls++ == 0) g_av2_tap_pre_first = pcm[0];
}

/* a data callback that makes the first sample loud (saturation test) */
static void av2_tap_loud(void *ctx, int16_t *pcm, size_t samples,
                         uint32_t rate, uint16_t channels)
{
    (void)ctx; (void)rate; (void)channels;
    if (samples > 1u) { pcm[0] = 30000; pcm[1] = -30000; }
}

static int  g_av2_track_starts;
static char g_av2_track_path[64];
static void av2_track_start(void *ctx, const char *path,
                            const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    g_av2_track_starts++;
    (void)snprintf(g_av2_track_path, sizeof(g_av2_track_path), "%s",
                   (path != NULL) ? path : "");
}

static void av2_tap_post(void *ctx, int16_t *pcm, size_t samples,
                         uint32_t rate, uint16_t channels)
{
    static int calls;

    (void)rate; (void)channels;
    if (ctx != NULL) { calls = 0; return; }
    if (samples > 0u && calls++ == 0) g_av2_tap_post_first = pcm[0];
}

static FM_Result av2_write_ramp_wav(const char *dir, const char *path,
                                    uint32_t samples)
{
    FM_AudioWriter w;
    FM_Result r;
    uint32_t i;

    (void)Directory_DeleteRecursive(dir);
    r = Directory_Create(dir);
    if (r == FM_OK) r = AudioWriter_Create(path, 8000, 2, 16, &w);
    for (i = 0; r == FM_OK && i < samples; i += 500u)
    {
        int16_t chunk[500];
        size_t written = 0;
        uint32_t k;

        for (k = 0; k < 500u; k++) chunk[k] = (int16_t)((i + k) * 4u);
        r = AudioWriter_Write(&w, chunk, 500, &written);
    }
    if (r == FM_OK) r = AudioWriter_Close(&w);
    return r;
}

#if FM_USE_THREADS && FM_HOST_BUILD
/* F4: two tasks, each playing its own WAV file */
typedef struct
{
    const char *path;
    uint8_t     law;        /* every byte of the file's audio */
    int16_t     expect;     /* ...and what it decodes to      */
    int         wrong;
    FM_Result   result;
} Av2WavTask;

static void *av2_wav_task(void *arg)
{
    Av2WavTask *t = (Av2WavTask *)arg;
    int16_t out[1024];
    int pass;

    for (pass = 0; pass < 40 && t->result == FM_OK; pass++)
    {
        FM_Audio au;
        size_t got = 0;
        size_t i;

        memset(&au, 0, sizeof(au));
        t->result = Audio_Open(t->path, &au);
        if (t->result != FM_OK) break;
        while (Audio_Read(&au, out, 1024, &got) == FM_OK && got > 0)
        {
            for (i = 0; i < got; i++)
            {
                if (out[i] != t->expect) t->wrong++;
            }
        }
        (void)Audio_Close(&au);
    }
    return NULL;
}
#endif

static void test_audit_v2_audio(void)
{
    static FM_Audio a;
    FM_AudioInfo info;
    FM_Result r;
    FM_Result q;
    uint64_t frames = 0;
    size_t len = 0;
    size_t got = 0;
    char msg[128];

    printf("\n[External audit v2: audio]\n");
    restore_posix_root();
    /* helpers whose every user sits behind an FM_USE_* switch */
    (void)av2_rd32;
    (void)av2_realloc;
    (void)av2_load;
    (void)av2_count_cb;
    (void)len;
    (void)frames;
    (void)info;

#if FM_USE_VORBIS
    /* ---------------- F1 ---------------- */
    {
        static const struct { const char *name; uint32_t count; uint32_t first; int set_first; } k[] = {
            { "F1: Vorbis comment count 0x20000001 is refused", 0x20000001u, 0, 0 },
            { "F1: Vorbis comment count 0x10000000 is refused", 0x10000000u, 0, 0 },
            { "F1: Vorbis comment length 0xFFFFFFFF is refused", 3u, 0xFFFFFFFFu, 1 },
            { "F1: Vorbis comment count 0xFFFFFFFF is refused", 0xFFFFFFFFu, 0, 0 }
        };
        size_t i;

        for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        {
            size_t c = 0;
            size_t j;

            TEST(k[i].name);
            r = av2_load("/test/vectors/test_vorbis.ogg", &len);
            for (j = 0; r == FM_OK && j + 7u < len; j++)
            {
                if (memcmp(g_av2_abuf + j, "\003vorbis", 7) == 0)
                {
                    c = j + 11u + av2_rd32(g_av2_abuf + j + 7u);
                    break;
                }
            }
            if (r != FM_OK || c == 0 || c + 8u > len) { FAIL("set-up"); continue; }
            av2_wr32(g_av2_abuf + c, k[i].count);
            if (k[i].set_first) av2_wr32(g_av2_abuf + c + 4u, k[i].first);
            r = File_WriteAll("/av2_bad.ogg", g_av2_abuf, (uint32_t)len);
            memset(&a, 0, sizeof(a));
            /* used to write past the list, or free what was never set */
            q = (r == FM_OK) ? Audio_Open("/av2_bad.ogg", &a) : FM_OK;
            if (q == FM_OK) (void)Audio_Close(&a);
            AV2_RESULT(r == FM_OK && q != FM_OK, q, "opened");
        }
    }

    TEST("F18: Vorbis cut off anywhere in its setup header (leaks: asan)");
    {
        size_t cut;
        size_t setup = 0;
        size_t j;
        unsigned tried = 0;

        r = av2_load("/test/vectors/test_vorbis.ogg", &len);
        for (j = 0; r == FM_OK && j + 7u < len; j++)
        {
            if (memcmp(g_av2_abuf + j, "\005vorbis", 7) == 0) { setup = j; break; }
        }
        for (cut = setup; r == FM_OK && setup != 0 && cut < len - 1u; cut += 7u)
        {
            r = File_WriteAll("/av2_bad.ogg", g_av2_abuf, (uint32_t)cut);
            memset(&a, 0, sizeof(a));
            tried++;
            if (r == FM_OK && Audio_Open("/av2_bad.ogg", &a) == FM_OK)
            {
                /* cut in the audio: plays what is there, and that is fine */
                (void)Audio_Close(&a);
            }
        }
        AV2_RESULT(r == FM_OK && setup != 0 && tried != 0u, r, "set-up");
    }
    (void)File_Delete("/av2_bad.ogg");
#endif /* FM_USE_VORBIS */

#if FM_USE_MP3
    /* ---------------- F2 ---------------- */
    TEST("F2: MP3 seek into frames that use the bit reservoir");
    memset(&a, 0, sizeof(a));
    r = Audio_Open("/test/vectors/mp3_m2_22k_32k.mp3", &a);
    if (r != FM_OK) { CHECK_RESULT(r, "open"); }
    else
    {
        static const double at[] = { 0.5, 1.5, 2.5, 1.0 };
        size_t i;
        int bad = 0;

        for (i = 0; i < sizeof(at) / sizeof(at[0]) && !bad; i++)
        {
            uint64_t want = (uint64_t)(at[i] * 22050.0);

            q = Audio_Seek(&a, at[i]);
            got = 0;
            if (q == FM_OK) q = Audio_Read(&a, g_av2_pcm, 2048, &got);
            /* the position is where the audio is: at or a little after
             * the request, never more than a quarter second away */
            /* constant bit rate: the position is the request, exactly */
            if (q != FM_OK || got != 2048u || a.frame_index != want + 2048u)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg),
                               "seek %.1f: result %d, %u samples, frame %lu",
                               at[i], (int)q, (unsigned)got,
                               (unsigned long)a.frame_index);
            }
        }
        (void)Audio_Close(&a);
        if (bad) FAIL(msg); else PASS();
    }

    TEST("F22: an MP3 seek (constant bit rate) lands on the sample");
    {
        static int16_t whole[70000];
        static const double at[] = { 0.01, 0.2, 1.234, 2.9, 0.0, 2.0 };
        size_t n = 0;
        size_t i;
        int bad = 0;

        memset(&a, 0, sizeof(a));
        r = Audio_Open("/test/vectors/mp3_m2_22k_32k.mp3", &a);
        while (r == FM_OK && n + 4096u <= sizeof(whole) / sizeof(whole[0]) &&
               Audio_Read(&a, whole + n, 4096, &got) == FM_OK && got > 0)
        {
            n += got;
        }
        for (i = 0; r == FM_OK && i < sizeof(at) / sizeof(at[0]) && !bad; i++)
        {
            size_t want = (size_t)(at[i] * 22050.0);
            size_t k;
            long worst = 0;

            q = Audio_Seek(&a, at[i]);
            if (q == FM_OK && a.frame_index != want) q = FM_ERR_INTERNAL;
            if (q == FM_OK) q = Audio_Read(&a, g_av2_pcm, 2048, &got);
            for (k = 0; q == FM_OK && k < got && want + k < n; k++)
            {
                long d = (long)g_av2_pcm[k] - (long)whole[want + k];

                if (d < 0) d = -d;
                if (d > worst) worst = d;
            }
            /* the same samples a decode from the start gives */
            if (q != FM_OK || got != 2048u || worst > 2)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg),
                               "seek %.3f: result %d, frame %lu (want %lu), off by %ld",
                               at[i], (int)q, (unsigned long)a.frame_index,
                               (unsigned long)want, worst);
            }
        }
        if (r == FM_OK) (void)Audio_Close(&a);
        if (r != FM_OK || n < 60000u) { CHECK_RESULT(FM_ERR_INTERNAL, "set-up"); }
        else if (bad) FAIL(msg);
        else PASS();
    }

    TEST("F2: one damaged MP3 frame header costs a few frames, not the track");
    {
        uint64_t whole = 0;
        uint64_t damaged = 0;
        size_t at = 0;
        size_t j;

        r = av2_decode_all("/test/vectors/mp3_m2_22k_32k.mp3", 4096, &whole, NULL);
        if (r == FM_ERR_EOF) r = av2_load("/test/vectors/mp3_m2_22k_32k.mp3", &len);
        /* a frame header in the middle of the file */
        for (j = len / 2u; r == FM_OK && j + 4u < len; j++)
        {
            if (g_av2_abuf[j] == 0xFFu && (g_av2_abuf[j + 1u] & 0xE0u) == 0xE0u &&
                memcmp(g_av2_abuf + j, g_av2_abuf, 2) == 0)
            {
                at = j;
                break;
            }
        }
        if (r == FM_OK && at != 0)
        {
            memset(g_av2_abuf + at, 0, 4);
            r = File_WriteAll("/av2_bad.mp3", g_av2_abuf, (uint32_t)len);
        }
        if (r == FM_OK) r = av2_decode_all("/av2_bad.mp3", 4096, &damaged, NULL);
        (void)snprintf(msg, sizeof(msg), "%lu of %lu frames played",
                       (unsigned long)damaged, (unsigned long)whole);
        AV2_RESULT(r == FM_ERR_EOF && at != 0 && whole > 60000u &&
                   damaged + 8u * 576u >= whole, r, msg);
        (void)File_Delete("/av2_bad.mp3");
    }

    TEST("F2: a file that is not MP3 at all is refused");
    memset(g_av2_abuf, 0, sizeof(g_av2_abuf));
    r = File_WriteAll("/av2_zero.mp3", g_av2_abuf, (uint32_t)sizeof(g_av2_abuf));
    memset(&a, 0, sizeof(a));
    q = (r == FM_OK) ? Audio_Open("/av2_zero.mp3", &a) : FM_OK;
    if (q == FM_OK) (void)Audio_Close(&a);
    AV2_RESULT(r == FM_OK && q == FM_ERR_INVALID_FORMAT, q, "open");
    (void)File_Delete("/av2_zero.mp3");

    /* ---------------- F21 ---------------- */
    TEST("F21: the Xing/Info frame is not played as audio");
    r = av2_decode_all("/test/vectors/mp3_info_44k.mp3", 4096, &frames, &info);
    (void)snprintf(msg, sizeof(msg), "%lu frames decoded, header says %lu",
                   (unsigned long)frames, (unsigned long)info.total_frames);
    AV2_RESULT(r == FM_ERR_EOF && info.total_frames != 0u &&
               frames == info.total_frames, r, msg);
#endif /* FM_USE_MP3 */

    /* ---------------- F3 ---------------- */
    TEST("F3: stereo IMA ADPCM decodes to the reference samples");
    {
        static uint8_t ref[16 * 1024];
        static int16_t out[8 * 1024];
        size_t ref_len = 0;
        size_t n = 0;

        r = File_ReadAll("/test/vectors/wav_adpcm_ima_stereo_8k.s16", ref,
                         sizeof(ref), &ref_len);
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/test/vectors/wav_adpcm_ima_stereo_8k.wav", &a);
        if (r == FM_OK)
        {
            /* an awkward chunk size, so blocks are split across reads */
            while (n + 250u <= sizeof(out) / sizeof(out[0]) &&
                   (q = Audio_Read(&a, out + n, 250, &got)) == FM_OK && got > 0)
            {
                n += got;
            }
            info = a.info;
            (void)Audio_Close(&a);
        }
        if (r != FM_OK) { CHECK_RESULT(r, "open"); }
        else if (n * 2u != ref_len)
        {
            (void)snprintf(msg, sizeof(msg), "%u samples, reference has %u",
                           (unsigned)n, (unsigned)(ref_len / 2u));
            FAIL(msg);
        }
        else if (memcmp(out, ref, ref_len) != 0) FAIL("samples differ");
        else if (info.total_frames != n / 2u) FAIL("total_frames");
        else PASS();
    }

    TEST("F3: Microsoft ADPCM (format 2) has its own decoder, not the IMA one");
    {
        /* A block of zeros: predictor 0, both start samples 0, every
         * nibble 0 -> 2 + (256 - 7) * 2 = 500 frames of silence.  (The
         * IMA decoder made 505 samples of it; the reference vectors are
         * in test/audit_v3_tests.h.) */
        static const uint8_t blk[256] = { 0 };
        static int16_t pcm[600];
        size_t i;
        int quiet = 1;

        memset(&info, 0, sizeof(info));
        memset(pcm, 0x55, sizeof(pcm));
        got = 0;
        len = av2_build_wav(2, 1, 8000, 4, 256, blk, sizeof(blk));
        r = File_WriteAll("/av2.wav", g_av2_abuf, (uint32_t)len);
        memset(&a, 0, sizeof(a));
        q = (r == FM_OK) ? Audio_Open("/av2.wav", &a) : FM_OK;
        if (r == FM_OK && q == FM_OK)
        {
            (void)Audio_GetInfo(&a, &info);
            q = Audio_Read(&a, pcm, 600, &got);
            (void)Audio_Close(&a);
        }
        for (i = 0; i < got && i < 600u; i++)
        {
            if (pcm[i] != 0) quiet = 0;
        }
        (void)snprintf(msg, sizeof(msg), "%lu samples, total_frames %lu, quiet %d",
                       (unsigned long)got, (unsigned long)info.total_frames,
                       quiet);
        AV2_RESULT(r == FM_OK && q == FM_OK && got == 500u &&
                   info.total_frames == 500u && quiet, q, msg);
    }

    /* ---------------- F10 ---------------- */
    TEST("F10: A-law polarity (G.711: 0xD5 is +8, 0x55 is -8)");
    {
        static const uint8_t law[4] = { 0xD5, 0x55, 0xAA, 0x2A };
        int16_t out4[4] = { 0, 0, 0, 0 };

        len = av2_build_wav(6, 1, 8000, 8, 1, law, sizeof(law));
        r = File_WriteAll("/av2.wav", g_av2_abuf, (uint32_t)len);
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/av2.wav", &a);
        if (r == FM_OK)
        {
            r = Audio_Read(&a, out4, 4, &got);
            (void)Audio_Close(&a);
        }
        (void)snprintf(msg, sizeof(msg), "got %d %d %d %d", out4[0], out4[1],
                       out4[2], out4[3]);
        AV2_RESULT(r == FM_OK && out4[0] == 8 && out4[1] == -8 &&
                   out4[2] == 32256 && out4[3] == -32256, r, msg);
    }

#if FM_USE_THREADS && FM_HOST_BUILD
    /* ---------------- F4 ---------------- */
    TEST("F4: two tasks play two WAV files, each hears its own");
    {
        static uint8_t law[16000];
        static Av2WavTask task[2] = {
            { "/av2_t0.wav", 0xFF, 0, 0, FM_OK },       /* mu-law, silence   */
            { "/av2_t1.wav", 0x00, 0, 0, FM_OK }        /* mu-law, full scale */
        };
        pthread_t th[2];
        int started = 0;
        int i;

        r = FM_OK;
        for (i = 0; i < 2 && r == FM_OK; i++)
        {
            task[i].wrong = 0;
            task[i].result = FM_OK;
            memset(law, task[i].law, sizeof(law));
            len = av2_build_wav(7, 1, 8000, 8, 1, law, sizeof(law));
            r = File_WriteAll(task[i].path, g_av2_abuf, (uint32_t)len);
            /* what one task alone hears */
            memset(&a, 0, sizeof(a));
            if (r == FM_OK) r = Audio_Open(task[i].path, &a);
            if (r == FM_OK)
            {
                r = Audio_Read(&a, g_av2_pcm, 1, &got);
                task[i].expect = g_av2_pcm[0];
                (void)Audio_Close(&a);
            }
        }
        if (r == FM_OK && task[0].expect == task[1].expect) r = FM_ERR_INTERNAL;
        for (i = 0; i < 2 && r == FM_OK; i++)
        {
            if (pthread_create(&th[i], NULL, av2_wav_task, &task[i]) != 0)
            {
                r = FM_ERR_INTERNAL;
                break;
            }
            started++;
        }
        for (i = 0; i < started; i++) (void)pthread_join(th[i], NULL);
        (void)snprintf(msg, sizeof(msg),
                       "results %d %d, samples of the other file: %d and %d",
                       (int)task[0].result, (int)task[1].result,
                       task[0].wrong, task[1].wrong);
        AV2_RESULT(r == FM_OK && task[0].result == FM_OK &&
                   task[1].result == FM_OK && task[0].wrong == 0 &&
                   task[1].wrong == 0, r, msg);
        (void)File_Delete("/av2_t0.wav");
        (void)File_Delete("/av2_t1.wav");
    }
#endif

    /* ---------------- F8 + G.711 seek ---------------- */
    TEST("F8: duration and seek of G.711 count one byte a sample");
    {
        static uint8_t law[8000];
        size_t i;

        for (i = 0; i < sizeof(law); i++) law[i] = (uint8_t)(0xD5u ^ (i / 1000u));
        len = av2_build_wav(7, 1, 8000, 8, 1, law, sizeof(law));
        r = File_WriteAll("/av2.wav", g_av2_abuf, (uint32_t)len);
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/av2.wav", &a);
        if (r == FM_OK)
        {
            int16_t at_start[1];
            int16_t at_half[1];
            uint64_t total = a.info.total_frames;

            q = Audio_Read(&a, at_start, 1, &got);
            if (q == FM_OK) q = Audio_Seek(&a, 0.5);
            /* half a second in is byte 4000: a different value than at
             * byte 8000, where the seek used to land (and found the end) */
            if (q == FM_OK) q = Audio_Read(&a, at_half, 1, &got);
            (void)snprintf(msg, sizeof(msg), "%lu frames, frame_index %lu",
                           (unsigned long)total, (unsigned long)a.frame_index);
            AV2_RESULT(q == FM_OK && total == 8000u && a.frame_index == 4001u &&
                       at_half[0] != at_start[0], q, msg);
            (void)Audio_Close(&a);
        }
        else
        {
            CHECK_RESULT(r, "open");
        }
    }

    TEST("F8: Player_GetDuration for ADPCM, G.711, FLAC, Vorbis, MP3");
    {
        static const struct { const char *name; double want; } k[] = {
            { "wav_adpcm_ima_stereo_8k.wav", 2034.0 / 8000.0 },
            { "wav_alaw_8k.wav", 0.0 },         /* filled in below */
#if FM_USE_FLAC
            { "flac_6ch_8k.flac", 0.2 },
#endif
#if FM_USE_VORBIS
            { "test_vorbis.ogg", 1.0 },
#endif
#if FM_USE_MP3
            { "mp3_info_44k.mp3", 1.0 },
#endif
        };
        static FM_Player pl;
        size_t i;
        int bad = 0;

        (void)Directory_DeleteRecursive("/av2dur");
        r = Directory_Create("/av2dur");
        for (i = 0; r == FM_OK && i < sizeof(k) / sizeof(k[0]); i++)
        {
            char from[96];
            char to[96];

            (void)snprintf(from, sizeof(from), "/test/vectors/%s", k[i].name);
            (void)snprintf(to, sizeof(to), "/av2dur/%02u_%s", (unsigned)i, k[i].name);
            r = File_Copy(from, to, NULL, NULL);
        }
        if (r == FM_OK) r = Player_Init(&pl);
        if (r == FM_OK) r = Player_ScanFolder(&pl, "/av2dur", FM_PLAYER_KIND_AUDIO);
        if (r == FM_OK && Player_Count(&pl) != sizeof(k) / sizeof(k[0])) r = FM_ERR_INTERNAL;
        for (i = 0; r == FM_OK && i < sizeof(k) / sizeof(k[0]) && !bad; i++)
        {
            double d;
            double want = k[i].want;
            uint64_t decoded = 0;

            r = Player_Play(&pl, (uint32_t)i);
            if (r != FM_OK) break;
            d = Player_GetDuration(&pl);
            (void)Player_Stop(&pl);
            /* the truth: what a full decode delivers */
            q = av2_decode_all(Player_ItemPath(&pl, (uint32_t)i), 3072,
                               &decoded, &info);
            if (want == 0.0) want = (double)decoded / (double)info.sample_rate;
            if (q != FM_ERR_EOF || d < want - 0.002 || d > want + 0.002 ||
                (double)decoded / (double)info.sample_rate < want - 0.002 ||
                (double)decoded / (double)info.sample_rate > want + 0.002)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg), "%s: %.3f s reported, %.3f s decoded",
                               k[i].name, d,
                               (double)decoded / (double)info.sample_rate);
            }
        }
        Player_Deinit(&pl);
        (void)Directory_DeleteRecursive("/av2dur");
        if (r != FM_OK) { CHECK_RESULT(r, "set-up"); }
        else if (bad) FAIL(msg);
        else PASS();
    }

    /* ---------------- F24 ---------------- */
    TEST("F24: a scanned folder lists .opus and not .m4a");
    {
        static FM_Player pl;
        uint32_t i;
        int opus = 0;
        int m4a = 0;

        (void)Directory_DeleteRecursive("/av2scan");
        r = Directory_Create("/av2scan");
        if (r == FM_OK) r = File_WriteAll("/av2scan/a.opus", "x", 1);
        if (r == FM_OK) r = File_WriteAll("/av2scan/b.m4a", "x", 1);
        if (r == FM_OK) r = File_WriteAll("/av2scan/c.wav", "x", 1);
        if (r == FM_OK) r = Player_Init(&pl);
        if (r == FM_OK) r = Player_ScanFolder(&pl, "/av2scan", FM_PLAYER_KIND_AUDIO);
        for (i = 0; r == FM_OK && i < Player_Count(&pl); i++)
        {
            const char *path = Player_ItemPath(&pl, i);

            if (strstr(path, ".opus") != NULL) opus++;
            if (strstr(path, ".m4a") != NULL) m4a++;
        }
        Player_Deinit(&pl);
        (void)Directory_DeleteRecursive("/av2scan");
        (void)snprintf(msg, sizeof(msg), ".opus %d, .m4a %d", opus, m4a);
        AV2_RESULT(r == FM_OK && opus == 1 && m4a == 0, r, msg);
    }

    TEST("F9: Player_Seek / SeekFwd / SeekBack refuse NaN");
    {
        static FM_Player pl;
        volatile double zero = 0.0;
        double nan_value = zero / zero;
        FM_Result s1 = FM_OK;
        FM_Result s2 = FM_OK;
        FM_Result s3 = FM_OK;

        (void)Directory_DeleteRecursive("/av2scan");
        r = Directory_Create("/av2scan");
        if (r == FM_OK) r = File_Copy("/test/vectors/wav_alaw_8k.wav", "/av2scan/a.wav", NULL, NULL);
        if (r == FM_OK) r = Player_Init(&pl);
        if (r == FM_OK) r = Player_ScanFolder(&pl, "/av2scan", FM_PLAYER_KIND_AUDIO);
        if (r == FM_OK) r = Player_Play(&pl, 0);
        if (r == FM_OK) r = Player_Seek(&pl, 1.0);
        if (r == FM_OK)
        {
            s1 = Player_Seek(&pl, nan_value);
            s2 = Player_SeekFwd(&pl, nan_value);
            s3 = Player_SeekBack(&pl, nan_value);   /* used to rewind to 0 */
        }
        (void)snprintf(msg, sizeof(msg), "results %d %d %d, position %.2f",
                       (int)s1, (int)s2, (int)s3, Player_GetPosition(&pl));
        AV2_RESULT(r == FM_OK && s1 == FM_ERR_INVALID_ARGUMENT &&
                   s2 == FM_ERR_INVALID_ARGUMENT && s3 == FM_ERR_INVALID_ARGUMENT &&
                   Player_GetPosition(&pl) > 0.99, r, msg);
        Player_Deinit(&pl);
        (void)Directory_DeleteRecursive("/av2scan");
    }

    /* ---------------- F9 ---------------- */
    TEST("F9: Audio_Seek(NaN) is an argument error in every format");
    {
        static const char *const files[] = {
            "/test/vectors/wav_alaw_8k.wav",
            "/test/vectors/wav_adpcm_ima_22k.wav",
#if FM_USE_FLAC
            "/test/vectors/test_flac.flac",
#endif
#if FM_USE_VORBIS
            "/test/vectors/test_vorbis.ogg",
#endif
#if FM_USE_MP3
            "/test/vectors/mp3_m2_22k_32k.mp3",
#endif
        };
        size_t i;
        int bad = 0;
        volatile double zero = 0.0;
        double nan_value = zero / zero;

        for (i = 0; i < sizeof(files) / sizeof(files[0]) && !bad; i++)
        {
            memset(&a, 0, sizeof(a));
            r = Audio_Open(files[i], &a);
            q = (r == FM_OK) ? Audio_Seek(&a, nan_value) : r;
            if (r == FM_OK) (void)Audio_Close(&a);
            if (q != FM_ERR_INVALID_ARGUMENT)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg), "%s: result %d", files[i], (int)q);
            }
        }
        if (bad) FAIL(msg); else PASS();
    }

    TEST("F9: a seek far past the end is the end, not the start");
    {
        static const char *const files[] = {
            "/test/vectors/wav_alaw_8k.wav",
            "/test/vectors/wav_adpcm_ima_22k.wav",
#if FM_USE_FLAC
            "/test/vectors/test_flac.flac",
#endif
#if FM_USE_VORBIS
            "/test/vectors/test_vorbis.ogg",
#endif
#if FM_USE_MP3
            "/test/vectors/mp3_m2_22k_32k.mp3",
#endif
        };
        static const double far_away[] = { 1e30, 1e9, 0.0 /* infinity */ };
        size_t i;
        size_t j;
        int bad = 0;
        volatile double zero = 0.0;

        for (i = 0; i < sizeof(files) / sizeof(files[0]) && !bad; i++)
        {
            for (j = 0; j < 3u && !bad; j++)
            {
                double t = (j == 2u) ? 1.0 / zero : far_away[j];

                memset(&a, 0, sizeof(a));
                r = Audio_Open(files[i], &a);
                q = (r == FM_OK) ? Audio_Seek(&a, t) : r;
                got = 99;
                /* either the seek says "past the end", or the read does */
                if (q == FM_OK) q = Audio_Read(&a, g_av2_pcm, 256, &got);
                if (r == FM_OK) (void)Audio_Close(&a);
                if (q != FM_ERR_EOF)
                {
                    bad = 1;
                    (void)snprintf(msg, sizeof(msg), "%s, case %u: result %d",
                                   files[i], (unsigned)j, (int)q);
                }
            }
        }
        if (bad) FAIL(msg); else PASS();
    }

#if FM_USE_FLAC
    /* ---------------- F11 ---------------- */
    TEST("F11: Audio_ReadMono refuses a 6-channel stream");
    memset(&a, 0, sizeof(a));
    r = Audio_Open("/test/vectors/flac_6ch_8k.flac", &a);
    if (r != FM_OK) { CHECK_RESULT(r, "open"); }
    else
    {
        got = 99;
        q = Audio_ReadMono(&a, g_av2_pcm, 600, &got);
        (void)snprintf(msg, sizeof(msg), "%u \"mono\" samples", (unsigned)got);
        AV2_RESULT(q == FM_ERR_UNSUPPORTED && got == 0u, q, msg);
        (void)Audio_Close(&a);
    }

    TEST("F11: a 6-channel stream is read in whole frames");
    r = av2_decode_all("/test/vectors/flac_6ch_8k.flac", 1000, &frames, &info);
    {
        int whole = 1;

        memset(&a, 0, sizeof(a));
        q = Audio_Open("/test/vectors/flac_6ch_8k.flac", &a);
        while (q == FM_OK && Audio_Read(&a, g_av2_pcm, 1000, &got) == FM_OK)
        {
            if (got % 6u != 0u) whole = 0;      /* 1000 is not a multiple */
        }
        if (q == FM_OK) (void)Audio_Close(&a);
        (void)snprintf(msg, sizeof(msg), "%lu frames", (unsigned long)frames);
        AV2_RESULT(r == FM_ERR_EOF && q == FM_OK && whole && frames == 1600u,
                   r, msg);
    }

    /* ---------------- F14 ---------------- */
    TEST("F14: FLAC without a declared length does not cost megabytes");
    {
        FM_Allocator saved = *FM_Allocator_Get();
        FM_Allocator mine = { av2_alloc, NULL, av2_realloc, av2_free, NULL };
        uint64_t declared = 0;

        r = av2_decode_all("/test/vectors/test_flac.flac", 4096, &declared, NULL);
        if (r == FM_ERR_EOF) r = av2_load("/test/vectors/test_flac.flac", &len);
        /* STREAMINFO: the 36-bit sample count -> 0 = "not known" */
        if (r == FM_OK)
        {
            g_av2_abuf[21] &= 0xF0u;
            memset(g_av2_abuf + 22, 0, 4);
            r = File_WriteAll("/av2.flac", g_av2_abuf, (uint32_t)len);
        }
        g_av2_max_alloc = 0;
        (void)FM_Allocator_Set(&mine);
        if (r == FM_OK) r = av2_decode_all("/av2.flac", 4096, &frames, &info);
        (void)FM_Allocator_Set(&saved);
        (void)snprintf(msg, sizeof(msg), "%lu frames (%lu), largest block %lu",
                       (unsigned long)frames, (unsigned long)declared,
                       (unsigned long)g_av2_max_alloc);
        /* the whole track is under 100 KB of PCM */
        AV2_RESULT(r == FM_ERR_EOF && frames == declared && frames != 0u &&
                   info.total_frames == frames &&
                   g_av2_max_alloc < 512u * 1024u, r, msg);
        (void)File_Delete("/av2.flac");
    }
#endif /* FM_USE_FLAC */

    /* ---------------- F12 / F15 ---------------- */
    TEST("F12: a read smaller than one frame is an argument error");
    {
        static const uint8_t pcm8[400] = { 0 };

        len = av2_build_wav(1, 2, 8000, 16, 4, pcm8, sizeof(pcm8));
        r = File_WriteAll("/av2.wav", g_av2_abuf, (uint32_t)len);
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/av2.wav", &a);
        if (r == FM_OK)
        {
            FM_Result one;
            FM_Result three;
            size_t got3 = 99;

            got = 99;
            one = Audio_Read(&a, g_av2_pcm, 1, &got);
            three = Audio_Read(&a, g_av2_pcm, 3, &got3);    /* one frame */
            (void)snprintf(msg, sizeof(msg), "count 1: %d/%u, count 3: %d/%u",
                           (int)one, (unsigned)got, (int)three, (unsigned)got3);
            AV2_RESULT(one == FM_ERR_INVALID_ARGUMENT && got == 0u &&
                       three == FM_OK && got3 == 2u, one, msg);
            (void)Audio_Close(&a);
        }
        else
        {
            CHECK_RESULT(r, "open");
        }
    }

    TEST("F12: Audio_Stream with a chunk smaller than one frame");
    memset(&a, 0, sizeof(a));
    r = Audio_Open("/av2.wav", &a);
    q = (r == FM_OK) ? Audio_Stream(&a, av2_stream_cb, NULL, 1) : r;
    if (r == FM_OK) (void)Audio_Close(&a);
    AV2_RESULT(q == FM_ERR_INVALID_ARGUMENT, q, "result");

    TEST("F15: the sample count is written on every path");
    {
        size_t n1 = 0xDEADu;
        size_t n2 = 0xDEADu;
        FM_Result r1;
        FM_Result r2 = FM_OK;

        memset(&a, 0, sizeof(a));
        r = Audio_Open("/test/vectors/wav_adpcm_ima_22k.wav", &a);
        if (r == FM_OK) r = Audio_Seek(&a, 1e9);
        r1 = (r == FM_OK) ? Audio_Read(&a, g_av2_pcm, 64, &n1) : r;
        if (r == FM_OK) r2 = Audio_ReadFrame(&a, g_av2_pcm, &n2);
        if (r == FM_OK) (void)Audio_Close(&a);
        (void)snprintf(msg, sizeof(msg), "read %d/%lu, frame %d/%lu", (int)r1,
                       (unsigned long)n1, (int)r2, (unsigned long)n2);
        AV2_RESULT(r1 == FM_ERR_EOF && n1 == 0u && r2 == FM_ERR_EOF && n2 == 0u,
                   r, msg);
    }
    (void)File_Delete("/av2.wav");

    /* ---------------- F5 / F16 / F6 / F17: AAC ---------------- */
    r = Audio_RegisterDecoder("AAC", &g_av2_aac_ops);
    len = av2_build_adts(40);
    if (r == FM_OK) r = File_WriteAll("/av2.aac", g_av2_abuf, (uint32_t)len);

    TEST("F16: the AAC decoder is given the frame with its ADTS header");
    g_av2_aac_frames = 0; g_av2_aac_header_ok = 0; g_av2_aac_claim = 0;
    q = (r == FM_OK) ? av2_decode_all("/av2.aac", 512, &frames, NULL) : r;
    (void)snprintf(msg, sizeof(msg), "%d of %d frames had a header, %lu samples",
                   g_av2_aac_header_ok, g_av2_aac_frames, (unsigned long)frames);
    AV2_RESULT(q == FM_ERR_EOF && g_av2_aac_frames == 40 &&
               g_av2_aac_header_ok == 40 && frames == 40u * 64u, q, msg);

    TEST("F5: an AAC decoder that reports more than it was given room for");
    g_av2_aac_claim = 6000u;
    memset(&a, 0, sizeof(a));
    q = (r == FM_OK) ? Audio_Open("/av2.aac", &a) : r;
    if (q == FM_OK)
    {
        got = 0;
        q = Audio_Read(&a, g_av2_pcm, 8192, &got);
        (void)Audio_Close(&a);
    }
    g_av2_aac_claim = 0;
    (void)snprintf(msg, sizeof(msg), "%u samples delivered", (unsigned)got);
    AV2_RESULT(q == FM_ERR_INTERNAL && got == 0u, q, msg);

    TEST("F6: junk between AAC frames is skipped, the track goes on");
    {
        /* 20 frames, 11 bytes that are no frame, 20 frames */
        size_t half = 20u * 23u;

        len = av2_build_adts(40);
        memmove(g_av2_abuf + half + 11u, g_av2_abuf + half, len - half);
        memset(g_av2_abuf + half, 0x11, 11);
        q = File_WriteAll("/av2j.aac", g_av2_abuf, (uint32_t)(len + 11u));
        g_av2_aac_frames = 0;
        if (q == FM_OK) q = av2_decode_all("/av2j.aac", 512, &frames, NULL);
        (void)snprintf(msg, sizeof(msg), "%d frames decoded", g_av2_aac_frames);
        AV2_RESULT(r == FM_OK && q == FM_ERR_EOF && g_av2_aac_frames == 40, q, msg);
        (void)File_Delete("/av2j.aac");
    }

#if FM_USE_FMFS && FM_USE_SAFE_WRITE
    TEST("F6: a read error is reported as one, not as the end (AAC, MP3)");
    {
        static const char *const names[] = {
            "/av2.aac",
#if FM_USE_MP3
            "/test/vectors/mp3_m2_22k_32k.mp3",
#endif
        };
        size_t i;
        int bad = 0;

        for (i = 0; i < sizeof(names) / sizeof(names[0]) && !bad; i++)
        {
            char path[96];
            uint64_t clean = 0;
            long budget;
            int seen_io = 0;

            q = av2_load(names[i], &len);
            if (q == FM_OK) q = av2_faulty_read_mount();
            (void)strcpy(path, g_fc_mount[0]);
            (void)strcat(path, "/t.bin");
            /* the extension decides the decoder */
            (void)strcpy(path + strlen(path) - 3u, (i == 0u) ? "aac" : "mp3");
            if (q == FM_OK) q = File_WriteAll(path, g_av2_abuf, (uint32_t)len);
            if (q == FM_OK) q = av2_decode_all(path, 512, &clean, NULL);
            if (q != FM_ERR_EOF)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg), "%s: set-up %d", names[i], (int)q);
            }
            /* fail the n-th read of the storage, for every n that matters */
            for (budget = 1; !bad && budget < 40; budget++)
            {
                uint64_t part = 0;

                av2_read_budget = budget;
                q = av2_decode_all(path, 512, &part, NULL);
                av2_read_budget = -1;
                if (q == FM_ERR_EOF && part == clean) break;    /* never hit */
                if (q == FM_ERR_IO) seen_io = 1;
                if (q == FM_ERR_EOF || q == FM_OK)
                {
                    /* a short track that claims to have ended */
                    bad = 1;
                    (void)snprintf(msg, sizeof(msg),
                                   "%s: read %ld failed, %lu of %lu frames, result %d",
                                   names[i], budget, (unsigned long)part,
                                   (unsigned long)clean, (int)q);
                }
            }
            if (!bad && !seen_io)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg), "%s: no read was failed", names[i]);
            }
            fc_drop(0);
        }
        if (bad) FAIL(msg); else PASS();
    }

    TEST("F17: every AAC decoder that is created is closed again");
    {
        long budget;
        int bad = 0;
        char path[96];

        len = av2_build_adts(40);
        q = av2_faulty_read_mount();
        (void)strcpy(path, g_fc_mount[0]);
        (void)strcat(path, "/t.aac");
        if (q == FM_OK) q = File_WriteAll(path, g_av2_abuf, (uint32_t)len);
        g_av2_aac_inits = 0;
        g_av2_aac_closes = 0;
        for (budget = 0; q == FM_OK && budget < 12; budget++)
        {
            FM_Result o;

            av2_seek_budget = budget;
            memset(&a, 0, sizeof(a));
            o = Audio_Open(path, &a);
            av2_seek_budget = -1;
            if (o == FM_OK) (void)Audio_Close(&a);
        }
        if (q != FM_OK || g_av2_aac_inits == 0 ||
            g_av2_aac_inits != g_av2_aac_closes)
        {
            bad = 1;
        }
        (void)snprintf(msg, sizeof(msg), "%d created, %d closed",
                       g_av2_aac_inits, g_av2_aac_closes);
        fc_drop(0);
        AV2_RESULT(!bad, q, msg);
    }
#endif /* FM_USE_FMFS && FM_USE_SAFE_WRITE */
    (void)Audio_UnregisterDecoder("AAC");
    (void)File_Delete("/av2.aac");

#if FM_USE_OPUS && FM_OPUS_USE_LIBOPUS
    /* ---------------- F7 / F19 ---------------- */
    TEST("F7: Opus with more than two channels is refused");
    r = av2_load("/test/vectors/test_opus.ogg", &len);
    if (r == FM_OK && memcmp(g_av2_abuf + 28, "OpusHead", 8) == 0)
    {
        g_av2_abuf[28 + 9] = 6;         /* channels */
        g_av2_abuf[28 + 18] = 1;        /* mapping family 1 */
        r = File_WriteAll("/av2.opus", g_av2_abuf, (uint32_t)len);
    }
    else if (r == FM_OK) r = FM_ERR_INTERNAL;
    memset(&a, 0, sizeof(a));
    q = (r == FM_OK) ? Audio_Open("/av2.opus", &a) : FM_OK;
    if (q == FM_OK) (void)Audio_Close(&a);
    AV2_RESULT(r == FM_OK && q == FM_ERR_UNSUPPORTED, q, "open");

    TEST("F19: the Opus header gain is applied (-6 dB halves the level)");
    {
        double e_plain = 0.0;
        double e_gain = 0.0;
        int pass;

        for (pass = 0; pass < 2; pass++)
        {
            double e = 0.0;
            size_t i;

            r = av2_load("/test/vectors/test_opus.ogg", &len);
            if (r != FM_OK) break;
            if (pass == 1)
            {
                g_av2_abuf[28 + 16] = 0x00;     /* -6 * 256 = 0xFA00 */
                g_av2_abuf[28 + 17] = 0xFA;
            }
            r = File_WriteAll("/av2.opus", g_av2_abuf, (uint32_t)len);
            memset(&a, 0, sizeof(a));
            if (r == FM_OK) r = Audio_Open("/av2.opus", &a);
            if (r != FM_OK) break;
            while (Audio_Read(&a, g_av2_pcm, 4096, &got) == FM_OK && got > 0)
            {
                for (i = 0; i < got; i++) e += (double)g_av2_pcm[i] * g_av2_pcm[i];
            }
            (void)Audio_Close(&a);
            if (pass == 0) e_plain = e; else e_gain = e;
        }
        /* energy ratio of -6 dB is 0.2512 */
        (void)snprintf(msg, sizeof(msg), "energy ratio %.4f",
                       (e_plain > 0.0) ? e_gain / e_plain : 0.0);
        AV2_RESULT(r == FM_OK && e_plain > 0.0 && e_gain > 0.24 * e_plain &&
                   e_gain < 0.26 * e_plain, r, msg);
    }
    (void)File_Delete("/av2.opus");
#endif

    /* ---------------- F28 ---------------- */
    TEST("F28: the decoders take their memory from the library allocator");
    {
        static const struct { const char *path; unsigned long least; } k[] = {
#if FM_USE_FLAC
            /* file, decoder state, PCM, stream: was file, PCM, stream */
            { "/test/vectors/test_flac.flac", 4 },
#endif
#if FM_USE_VORBIS
            /* codebooks, floors, residues, windows: hundreds of blocks */
            { "/test/vectors/test_vorbis.ogg", 50 },
#endif
#if FM_USE_OPUS && FM_OPUS_USE_LIBOPUS
            { "/test/vectors/test_opus.ogg", 0 },
#endif
            { "/test/vectors/wav_alaw_8k.wav", 0 }
        };
        FM_Allocator saved = *FM_Allocator_Get();
        /* no realloc: what a simple heap has to offer */
        FM_Allocator mine = { av2_alloc, NULL, NULL, av2_free, NULL };
        size_t i;
        int bad = 0;

        for (i = 0; i < sizeof(k) / sizeof(k[0]) && !bad; i++)
        {
            g_av2_alloc_calls = 0;
            g_av2_alloc_live = 0;
            (void)FM_Allocator_Set(&mine);
            r = av2_decode_all(k[i].path, 4096, &frames, NULL);
            (void)FM_Allocator_Set(&saved);
            if (r != FM_ERR_EOF || frames == 0u ||
                g_av2_alloc_calls < k[i].least || g_av2_alloc_live != 0)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg),
                               "%s: result %d, %lu blocks asked for, %ld not freed",
                               k[i].path, (int)r, g_av2_alloc_calls,
                               g_av2_alloc_live);
            }
        }
        if (bad) FAIL(msg); else PASS();
    }

#if FM_USE_MEDIA_DETECT
    /* ---------------- F26 ---------------- */
    TEST("F26: UTF-16 text is not detected as MP3");
    {
        static const uint8_t bom_short[8] = { 0xFF, 0xFE, 'H', 0, 'i', 0, '\r', 0 };
        static uint8_t bom_long[512];
        FM_MediaType t1;
        FM_MediaType t2;
        FM_MediaType t3;
        size_t i;

        bom_long[0] = 0xFF; bom_long[1] = 0xFE;
        for (i = 2; i < sizeof(bom_long); i += 2u)
        {
            bom_long[i] = (uint8_t)('A' + (i / 2u) % 26u);
            bom_long[i + 1u] = 0;
        }
        t1 = Media_DetectBuffer(bom_short, sizeof(bom_short), NULL);
        t2 = Media_DetectBuffer(bom_long, sizeof(bom_long), NULL);
        /* and a real MP3 still is one, with a long and with a short look */
        r = av2_load("/test/vectors/mp3_m2_22k_32k.mp3", &len);
        t3 = Media_DetectBuffer(g_av2_abuf, 512, NULL);
        (void)snprintf(msg, sizeof(msg), "text %d %d, mp3 %d %d", (int)t1,
                       (int)t2, (int)t3,
                       (int)Media_DetectBuffer(g_av2_abuf, 4, NULL));
        AV2_RESULT(r == FM_OK && t1 != FM_MEDIA_MP3 && t2 != FM_MEDIA_MP3 &&
                   t3 == FM_MEDIA_MP3 &&
                   Media_DetectBuffer(g_av2_abuf, 4, NULL) == FM_MEDIA_MP3 &&
                   File_DetectType("/test/vectors/tone_stereo.mp3") == FM_MEDIA_MP3,
                   r, msg);
    }

    TEST("F26: extensions .m4a .opus .oga .wave, and a bare extension hint");
    {
        FM_MediaType m4a = Media_DetectByExtension("/x.m4a");
        FM_MediaType opus = Media_DetectByExtension("/x.OPUS");
        FM_MediaType oga = Media_DetectByExtension("/x.oga");
        FM_MediaType wave = Media_DetectByExtension("/x.wave");
        FM_MediaType bare = Media_DetectBuffer(NULL, 0, "mp3");
        FM_MediaType dotted = Media_DetectBuffer(NULL, 0, ".mp3");
        FM_MediaType named = Media_DetectBuffer(NULL, 0, "a.mp3");

        (void)snprintf(msg, sizeof(msg), "%d %d %d %d, hints %d %d %d", (int)m4a,
                       (int)opus, (int)oga, (int)wave, (int)bare, (int)dotted,
                       (int)named);
        AV2_RESULT(m4a == FM_MEDIA_MP4 && opus == FM_MEDIA_OGG &&
                   oga == FM_MEDIA_OGG && wave == FM_MEDIA_WAV &&
                   bare == FM_MEDIA_MP3 && dotted == FM_MEDIA_MP3 &&
                   named == FM_MEDIA_MP3, FM_OK, msg);
    }
#endif /* FM_USE_MEDIA_DETECT */

    /* ---------------- F23 ---------------- */
    TEST("F23: a recorder that cannot report its file name creates none");
    {
        FM_AudioWriter w;
        char small[8];
        uint32_t count[2] = { 0, 0 };
        FM_Result r3 = FM_OK;
        int i;

        (void)Directory_DeleteRecursive("/av2rec");
        for (i = 0; i < 3; i++)
        {
            r3 = AudioRecorder_Open("/av2rec", 8000, 1, 16, &w, small, sizeof(small));
            if (r3 == FM_OK) (void)AudioWriter_Close(&w);
        }
        (void)Directory_ScanRecursive("/av2rec", av2_count_cb, NULL, count, NULL);
        (void)snprintf(msg, sizeof(msg), "%u files left", (unsigned)count[0]);
        AV2_RESULT(r3 == FM_ERR_TOO_LARGE && count[0] == 0u, r3, msg);
        (void)Directory_DeleteRecursive("/av2rec");
    }

    /* ---------------- F13 / F25: the DMA example ---------------- */
    TEST("F13: DMA player, odd half size and a stereo track");
    {
        static AudioDma x;
        static int16_t dma[2 * 255] __attribute__((aligned(32)));

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        memset(dma, 0x55, sizeof(dma));
        if (r == FM_OK)
        {
            r = AudioDma_StartEx(&x, "/av2dma", &audio_dma_test_port, NULL,
                                     dma, 255, NULL);
        }
        /* half 0: samples 0..253 and one silent slot; half 1 goes on with
         * sample 254 -- it used to be silence, the track "had ended" */
        (void)snprintf(msg, sizeof(msg), "half 0 ends %d %d, half 1 starts %d %d",
                       dma[253], dma[254], dma[255], dma[256]);
        AV2_RESULT(r == FM_OK && dma[253] == 253 * 4 && dma[254] == 0 &&
                   dma[255] == 254 * 4 && dma[256] == 255 * 4, r, msg);
        (void)AudioDma_Stop(&x);
    }

    TEST("F25: DMA player, the pre-gain tap sees the samples before the gain");
    {
        static AudioDma x;
        static int16_t dma[2 * 256] __attribute__((aligned(32)));

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        if (r == FM_OK)
        {
            r = AudioDma_StartEx(&x, "/av2dma", &audio_dma_test_port, NULL,
                                     dma, 256, NULL);
        }
        g_av2_tap_pre_calls = 0;
        g_av2_tap_pre_first = -1;
        g_av2_tap_post_first = -1;
        av2_tap_post(&x, NULL, 0, 0, 0);            /* reset its counter */
        if (r == FM_OK)
        {
            AudioDma_SetVolume(&x, 16383);
            AudioDma_SetDataCallback(&x, av2_tap_pre, NULL);
            AudioDma_SetMonitorCallback(&x, av2_tap_post, NULL);
            AudioDma_HalfCallback(&x);          /* refill half 0 */
            r = AudioDma_Poll(&x);
        }
        /* the refill starts at sample 512 of the ramp: 2048 */
        (void)snprintf(msg, sizeof(msg), "pre tap %d, post tap %d",
                       g_av2_tap_pre_first, g_av2_tap_post_first);
        AV2_RESULT(r == FM_OK && g_av2_tap_pre_first == 2048 &&
                   g_av2_tap_post_first >= 1022 && g_av2_tap_post_first <= 1025,
                   r, msg);
        (void)AudioDma_Stop(&x);
    }

    TEST("F30: DMA player, a configuration is in place before the first buffer");
    {
        static AudioDma x;
        static int16_t dma[2 * 256] __attribute__((aligned(32)));
        AudioDma_Config cfg;

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        g_av2_tap_pre_calls = 0;
        g_av2_tap_pre_first = -1;
        g_av2_track_starts = 0;
        g_av2_track_path[0] = '\0';
        AudioDma_ConfigDefaults(&cfg);
        cfg.data_cb = av2_tap_pre;
        cfg.track_start_cb = av2_track_start;
        cfg.play_mode = FM_PLAY_MODE_STOP;
        if (r == FM_OK)
        {
            r = AudioDma_StartConfig(&x, "/av2dma", &audio_dma_test_port,
                                     NULL, dma, 256, &cfg);
        }
        (void)snprintf(msg, sizeof(msg),
                       "%d data calls, first %d, %d starts '%s', mode %d",
                       g_av2_tap_pre_calls, g_av2_tap_pre_first,
                       g_av2_track_starts, g_av2_track_path,
                       (int)AudioDma_GetPlayMode(&x));
        AV2_RESULT(r == FM_OK && g_av2_tap_pre_calls >= 1 &&
                   g_av2_tap_pre_first == 0 && g_av2_track_starts == 1 &&
                   strcmp(g_av2_track_path, "/av2dma/ramp.wav") == 0 &&
                   AudioDma_GetPlayMode(&x) == FM_PLAY_MODE_STOP, r, msg);
        /* the same callback again is not a second "start" */
        AudioDma_SetTrackStartCallback(&x, av2_track_start, NULL);
        TEST("F30: installing the same start callback again reports nothing");
        (void)snprintf(msg, sizeof(msg), "%d starts", g_av2_track_starts);
        AV2_RESULT(g_av2_track_starts == 1, FM_OK, msg);
        (void)AudioDma_Stop(&x);
    }

    TEST("F31: DMA player, output = speaker does not call the callbacks");
    {
        static AudioDma x;
        static int16_t dma[2 * 256] __attribute__((aligned(32)));
        AudioDma_Config cfg;

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        g_av2_tap_pre_calls = 0;
        memset(dma, 0x55, sizeof(dma));
        AudioDma_ConfigDefaults(&cfg);
        cfg.data_cb = av2_tap_pre;
        cfg.monitor_cb = av2_tap_pre;
        cfg.output = FM_AUDIO_OUT_SPEAKER;
        if (r == FM_OK)
        {
            r = AudioDma_StartConfig(&x, "/av2dma", &audio_dma_test_port,
                                     NULL, dma, 256, &cfg);
        }
        (void)snprintf(msg, sizeof(msg), "%d calls, dma[10] %d, dma[300] %d",
                       g_av2_tap_pre_calls, dma[10], dma[300]);
        AV2_RESULT(r == FM_OK && g_av2_tap_pre_calls == 0 && dma[10] == 40 &&
                   dma[300] == 1200, r, msg);
        (void)AudioDma_Stop(&x);
    }

    TEST("F32: DMA player, output = callback feeds the callback and plays silence");
    {
        static AudioDma x;
        static int16_t dma[2 * 256] __attribute__((aligned(32)));
        AudioDma_Config cfg;
        int silent = 1;
        size_t k;

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        g_av2_tap_pre_calls = 0;
        g_av2_tap_pre_first = -1;
        memset(dma, 0x55, sizeof(dma));
        AudioDma_ConfigDefaults(&cfg);
        cfg.data_cb = av2_tap_pre;
        cfg.output = FM_AUDIO_OUT_CALLBACK;
        if (r == FM_OK)
        {
            r = AudioDma_StartConfig(&x, "/av2dma", &audio_dma_test_port,
                                     NULL, dma, 256, &cfg);
        }
        for (k = 0; k < 2u * 256u; k++)
        {
            if (dma[k] != 0) silent = 0;
        }
        (void)snprintf(msg, sizeof(msg), "%d calls, first %d, silent %d",
                       g_av2_tap_pre_calls, g_av2_tap_pre_first, silent);
        AV2_RESULT(r == FM_OK && g_av2_tap_pre_calls >= 1 &&
                   g_av2_tap_pre_first == 0 && silent, r, msg);

        TEST("F32: back to speaker + callback, the next half is audible");
        if (r == FM_OK)
        {
            AudioDma_SetOutput(&x, FM_AUDIO_OUT_BOTH);
            AudioDma_HalfCallback(&x);          /* refill half 0 */
            r = AudioDma_Poll(&x);
        }
        (void)snprintf(msg, sizeof(msg), "dma[0] %d, output %d", dma[0],
                       (int)AudioDma_GetOutput(&x));
        AV2_RESULT(r == FM_OK && dma[0] == 2048 &&
                   AudioDma_GetOutput(&x) == FM_AUDIO_OUT_BOTH, r, msg);
        (void)AudioDma_Stop(&x);
    }

    TEST("F33: DMA player, a volume above 100 % boosts and saturates");
    {
        static AudioDma x;
        static int16_t dma[2 * 256] __attribute__((aligned(32)));

        r = av2_write_ramp_wav("/av2dma", "/av2dma/ramp.wav", 4000);
        if (r == FM_OK)
        {
            r = AudioDma_StartEx(&x, "/av2dma", &audio_dma_test_port, NULL,
                                 dma, 256, NULL);
        }
        if (r == FM_OK)
        {
            AudioDma_SetVolume(&x, 2 * 0x7FFF);
            AudioDma_SetDataCallback(&x, av2_tap_loud, NULL);
            AudioDma_HalfCallback(&x);          /* refill half 0 */
            r = AudioDma_Poll(&x);
        }
        /* samples 512.. of the ramp: 2056 at index 2 becomes about 4112 */
        (void)snprintf(msg, sizeof(msg), "%d %d %d", dma[0], dma[1], dma[2]);
        AV2_RESULT(r == FM_OK && dma[0] == 32767 && dma[1] == -32768 &&
                   dma[2] >= 4110 && dma[2] <= 4112, r, msg);
        (void)AudioDma_Stop(&x);
        (void)Directory_DeleteRecursive("/av2dma");
    }
}
#endif /* FM_USE_AUDIO */
