/* Audit v3: the open-mode matrix and the path edge cases, on every
 * backend, printed side by side.  Each row is one API call on a fresh
 * volume; a row where the three columns differ is a candidate finding.
 *
 *   ./matrix_fs            (all backends)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

#define NB 3
static uint8_t    g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static FM_FS      g_posix;
static const char *const g_stname[2] = { "MA", "MB" };
static const char *const g_mount[NB] = { "/MA", "/MB", "" };
static int g_live[NB];
static char g_row[128];

static const char *bn(int k)
{
    return k == 0 ? "FMFS" : (k == 1 ? "lfs " : "POSIX");
}

static void p(char *out, size_t n, int kind, const char *rel)
{
    if (g_mount[kind][0] == 0) snprintf(out, n, "%s", rel);
    else snprintf(out, n, "%s%s", g_mount[kind], rel);
}

static void row(const char *label, const FM_Result *r, const char *detail)
{
    printf("%-52s | %-16s | %-16s | %-16s | %s\n", label,
           g_live[0] ? FM_ResultString(r[0]) : "-",
           g_live[1] ? FM_ResultString(r[1]) : "-",
           g_live[2] ? FM_ResultString(r[2]) : "-", detail ? detail : "");
}

/* Run fn on every backend and print one row. */
static void each(const char *label,
                 FM_Result (*fn)(int kind, char *detail, size_t dn))
{
    FM_Result r[NB];
    char det[NB][96];
    int k;

    for (k = 0; k < NB; k++)
    {
        det[k][0] = 0;
        if (!g_live[k]) { r[k] = FM_OK; continue; }
        r[k] = fn(k, det[k], sizeof(det[k]));
    }
    printf("%-52s | %-16s | %-16s | %-16s | %s\n", label,
           g_live[0] ? FM_ResultString(r[0]) : "-",
           g_live[1] ? FM_ResultString(r[1]) : "-",
           g_live[2] ? FM_ResultString(r[2]) : "-",
           (g_live[0] && det[0][0]) ? det[0] : "");
    if (g_live[0] && g_live[1] && (r[0] != r[1] || strcmp(det[0], det[1])))
    {
        printf("%52s !  FMFS %s %s / lfs %s %s\n", "", FM_ResultString(r[0]),
               det[0], FM_ResultString(r[1]), det[1]);
    }
    if (g_live[0] && g_live[2] && (r[0] != r[2] || strcmp(det[0], det[2])))
    {
        printf("%52s !  FMFS %s %s / POSIX %s %s\n", "", FM_ResultString(r[0]),
               det[0], FM_ResultString(r[2]), det[2]);
    }
}

/* Empty the root of one backend (the mount root itself cannot be
 * removed), then put the standard x.txt in place. */
static FM_Result fresh(int kind)
{
    char root[128];
    char a[128];
    FM_Dir *d;
    FM_DirEntry e;

    p(root, sizeof(root), kind, "");
    if (root[0] == 0) snprintf(root, sizeof(root), "/");
    for (;;)
    {
        FM_Bool removed = FM_FALSE;

        d = Directory_Open(root);
        if (d == NULL) break;
        while (Directory_Read(d, &e) == FM_OK)
        {
            char full[256];

            snprintf(full, sizeof(full), "%s/%s", root, e.name);
            if (e.type == FM_ITEM_DIR) (void)Directory_DeleteRecursive(full);
            else (void)File_Delete(full);
            removed = FM_TRUE;
            break;                      /* the listing moved under us */
        }
        (void)Directory_Close(d);
        if (!removed) break;
    }
    p(a, sizeof(a), kind, "/x.txt");
    (void)File_WriteAll(a, "0123456789", 10);
    return FM_OK;
}

/* ---- the individual checks ---- */

#define CHECK(name) static FM_Result name(int kind, char *det, size_t dn)

CHECK(c_open_read_missing)
{
    char a[128];

    p(a, sizeof(a), kind, "/nope.txt");
    (void)File_Open(a, FM_OPEN_READ);
    snprintf(det, dn, "%s", FM_ResultString(File_GetLastError()));
    return File_GetLastError();
}

CHECK(c_open_write_missing_nocreate)
{
    char a[128];

    p(a, sizeof(a), kind, "/nope.txt");
    (void)File_Open(a, FM_OPEN_WRITE);
    return File_GetLastError();
}

CHECK(c_open_trunc_missing)
{
    char a[128];
    FM_File *f;

    p(a, sizeof(a), kind, "/nope2.txt");
    f = File_Open(a, FM_OPEN_WRITE | FM_OPEN_TRUNCATE);
    if (f == NULL) return File_GetLastError();
    (void)File_Close(f);
    snprintf(det, dn, "created");
    return FM_OK;
}

CHECK(c_open_write_only_then_read)
{
    char a[128];
    FM_File *f;
    uint32_t got = 0;
    char b[8];

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_WRITE);
    if (f == NULL) return File_GetLastError();
    {
        FM_Result r = File_Read(f, b, 4, &got);

        (void)File_Close(f);
        snprintf(det, dn, "read got %u", (unsigned)got);
        return r;
    }
}

CHECK(c_open_read_only_then_write)
{
    char a[128];
    FM_File *f;
    uint32_t wr = 0;

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_READ);
    if (f == NULL) return File_GetLastError();
    {
        FM_Result r = File_Write(f, "zz", 2, &wr);

        (void)File_Close(f);
        snprintf(det, dn, "wrote %u", (unsigned)wr);
        return r;
    }
}

CHECK(c_append_after_seek)
{
    char a[128];
    FM_File *f;
    char b[32];
    size_t got = 0;
    uint32_t wr = 0;
    FM_Result r;

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_READ | FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (f == NULL) return File_GetLastError();
    r = File_Seek(f, 0, FM_SEEK_SET);
    if (r == FM_OK) r = File_Write(f, "XY", 2, &wr);
    (void)File_Close(f);
    memset(b, 0, sizeof(b));
    (void)File_ReadAll(a, b, sizeof(b) - 1, &got);
    snprintf(det, dn, "%u bytes '%.20s'", (unsigned)got, b);
    return r;
}

CHECK(c_seek_negative)
{
    char a[128];
    FM_File *f;
    FM_Result r;

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_READ);
    if (f == NULL) return File_GetLastError();
    r = File_Seek(f, -1, FM_SEEK_SET);
    (void)File_Close(f);
    return r;
}

CHECK(c_seek_past_end_read)
{
    char a[128];
    FM_File *f;
    FM_Result r;
    uint32_t got = 0;
    char b[8];

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_READ);
    if (f == NULL) return File_GetLastError();
    r = File_Seek(f, 100, FM_SEEK_SET);
    if (r == FM_OK) r = File_Read(f, b, 4, &got);
    (void)File_Close(f);
    snprintf(det, dn, "got %u", (unsigned)got);
    return r;
}

CHECK(c_truncate_grow)
{
    char a[128];
    FM_File *f;
    uint64_t sz = 0;
    FM_Result r;

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_READ | FM_OPEN_WRITE);
    if (f == NULL) return File_GetLastError();
    r = File_Truncate(f, 20);
    (void)File_Close(f);
    if (r == FM_OK) r = File_SizeOf(a, &sz);
    snprintf(det, dn, "size %u", (unsigned)sz);
    return r;
}

CHECK(c_rename_onto_existing)
{
    char a[128], b[128];
    char buf[32];
    size_t got = 0;

    p(a, sizeof(a), kind, "/x.txt");
    p(b, sizeof(b), kind, "/y.txt");
    (void)File_WriteAll(b, "YYY", 3);
    {
        FM_Result r = File_Rename(a, b);

        memset(buf, 0, sizeof(buf));
        (void)File_ReadAll(b, buf, sizeof(buf) - 1, &got);
        snprintf(det, dn, "'%s'", buf);
        return r;
    }
}

CHECK(c_rename_onto_self)
{
    char a[128];

    p(a, sizeof(a), kind, "/x.txt");
    return File_Rename(a, a);
}

CHECK(c_rename_missing)
{
    char a[128], b[128];

    p(a, sizeof(a), kind, "/nope.txt");
    p(b, sizeof(b), kind, "/z.txt");
    return File_Rename(a, b);
}

CHECK(c_delete_missing)
{
    char a[128];

    p(a, sizeof(a), kind, "/nope.txt");
    return File_Delete(a);
}

CHECK(c_delete_directory_as_file)
{
    char a[128];

    p(a, sizeof(a), kind, "/d");
    (void)Directory_Create(a);
    return File_Delete(a);
}

CHECK(c_mkdir_over_file)
{
    char a[128];

    p(a, sizeof(a), kind, "/x.txt");
    return Directory_Create(a);
}

CHECK(c_open_directory)
{
    char a[128];
    FM_File *f;

    p(a, sizeof(a), kind, "/d");
    (void)Directory_Create(a);
    f = File_Open(a, FM_OPEN_READ);
    if (f != NULL) { (void)File_Close(f); snprintf(det, dn, "opened"); return FM_OK; }
    return File_GetLastError();
}

CHECK(c_rmdir_nonempty)
{
    char a[128], b[128];

    p(a, sizeof(a), kind, "/d");
    p(b, sizeof(b), kind, "/d/f.txt");
    (void)Directory_Create(a);
    (void)File_WriteAll(b, "1", 1);
    return Directory_Delete(a);
}

CHECK(c_rmdir_missing)
{
    char a[128];

    p(a, sizeof(a), kind, "/nope");
    return Directory_Delete(a);
}

CHECK(c_rmdir_root)
{
    p(g_row, sizeof(g_row), kind, "");
    return Directory_Delete(g_row);
}

CHECK(c_list_file)
{
    char a[128];
    FM_Dir *d;

    p(a, sizeof(a), kind, "/x.txt");
    d = Directory_Open(a);
    if (d != NULL) { (void)Directory_Close(d); snprintf(det, dn, "opened"); return FM_OK; }
    return File_GetLastError();
}

CHECK(c_double_slash)
{
    char a[128];
    FM_Bool ex = FM_FALSE;
    FM_Result r;

    p(a, sizeof(a), kind, "//x.txt");
    r = File_Exists(a, &ex);
    snprintf(det, dn, "exists %d", (int)ex);
    return r;
}

CHECK(c_dot_component)
{
    char a[128];
    FM_Bool ex = FM_FALSE;
    FM_Result r;

    p(a, sizeof(a), kind, "/./x.txt");
    r = File_Exists(a, &ex);
    snprintf(det, dn, "exists %d", (int)ex);
    return r;
}

CHECK(c_trailing_slash_file)
{
    char a[128];
    FM_Bool ex = FM_FALSE;
    FM_Result r;

    p(a, sizeof(a), kind, "/x.txt/");
    r = File_Exists(a, &ex);
    snprintf(det, dn, "exists %d", (int)ex);
    return r;
}

CHECK(c_relative_path)
{
    FM_File *f = File_Open("x.txt", FM_OPEN_READ);

    if (f != NULL) { (void)File_Close(f); return FM_OK; }
    return File_GetLastError();
}

CHECK(c_too_long_path)
{
    char a[512];
    FM_Bool ex = FM_FALSE;
    FM_Result r;
    size_t i;

    p(a, sizeof(a), kind, "");
    for (i = strlen(a); i < 200; i++) a[i] = 'a';
    a[200] = 0;
    r = File_Exists(a, &ex);
    return r;
}

CHECK(c_write_zero_bytes)
{
    char a[128];

    p(a, sizeof(a), kind, "/zero.txt");
    return File_WriteAll(a, "", 0);
}

CHECK(c_write_null)
{
    char a[128];

    p(a, sizeof(a), kind, "/null.txt");
    return File_WriteAll(a, NULL, 4);
}

CHECK(c_read_all_missing)
{
    char a[128];
    char b[8];
    size_t got = 0;

    p(a, sizeof(a), kind, "/nope.txt");
    return File_ReadAll(a, b, sizeof(b), &got);
}

CHECK(c_copy_to_self)
{
    char a[128];

    p(a, sizeof(a), kind, "/x.txt");
    return File_Copy(a, a, NULL, NULL);
}

CHECK(c_move_to_existing_dir)
{
    char a[128], b[128];

    p(a, sizeof(a), kind, "/x.txt");
    p(b, sizeof(b), kind, "/d");
    (void)Directory_Create(b);
    {
        FM_Result r = File_Move(a, b, NULL, NULL);
        snprintf(det, dn, "%s", FM_ResultString(r));
        return r;
    }
}

CHECK(c_dir_copy_into_self)
{
    char a[128], b[128];

    p(a, sizeof(a), kind, "/d");
    p(b, sizeof(b), kind, "/d/sub");
    (void)Directory_Create(a);
    return Directory_Copy(a, b, NULL, NULL);
}

CHECK(c_delete_open_writer_result)
{
    char a[128];
    FM_File *f;
    FM_Result r;

    p(a, sizeof(a), kind, "/open.txt");
    (void)File_WriteAll(a, "abc", 3);
    f = File_Open(a, FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (f == NULL) return File_GetLastError();
    r = File_Delete(a);
    (void)File_Close(f);
    return r;
}

CHECK(c_size_after_close)
{
    char a[128];
    FM_File *f;
    uint64_t sz = 0;
    FM_Result r;

    p(a, sizeof(a), kind, "/x.txt");
    f = File_Open(a, FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (f == NULL) return File_GetLastError();
    {
        uint32_t wr = 0;

        r = File_Write(f, "abcd", 4, &wr);
    }
    if (r == FM_OK) r = File_Close(f);
    if (r == FM_OK) r = File_SizeOf(a, &sz);
    snprintf(det, dn, "size %u", (unsigned)sz);
    return r;
}

int main(void)
{
    int k;

    if (FileManager_Init() != FM_OK) return 1;
    (void)mkdir("/tmp/matrix_root", 0777);
    (void)chdir("/tmp/matrix_root");
    if (PosixFS_Init(&g_posix) == FM_OK && FS_Mount("/", &g_posix) == FM_OK)
        g_live[2] = 1;
    for (k = 0; k < 2; k++)
    {
        FM_Result r;

        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], g_stname[k], g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(g_stname[k]);
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[k]) : LittleFS_Init(&g_fs[k]);
        if (r == FM_OK) r = FS_Mount(g_mount[k], &g_fs[k]);
        if (r == FM_OK) g_live[k] = 1;
    }

    printf("%-52s | %-16s | %-16s | %-16s | %s\n", "check", "FMFS",
           "LittleFS", "POSIX", "detail (FMFS)");
    printf("-----------------------------------------------------+"
           "------------------+------------------+------------------+---\n");

    /* each check starts from the same state on every backend */
    (void)fresh(0);
    (void)fresh(1);
    (void)fresh(2);

    each("open(read) of a missing file", c_open_read_missing);
    each("open(write) of a missing file, no CREATE", c_open_write_missing_nocreate);
    each("open(write|truncate) of a missing file", c_open_trunc_missing);
    (void)fresh(0); (void)fresh(1); (void)fresh(2);
    each("File_Read on a write-only handle", c_open_write_only_then_read);
    each("File_Write on a read-only handle", c_open_read_only_then_write);
    each("APPEND handle after Seek(0)+Write", c_append_after_seek);
    each("Seek(-1, SET)", c_seek_negative);
    each("Seek(100)+Read", c_seek_past_end_read);
    each("Truncate to a larger size", c_truncate_grow);
    (void)fresh(0); (void)fresh(1); (void)fresh(2);
    each("Rename onto an existing file", c_rename_onto_existing);
    (void)fresh(0); (void)fresh(1); (void)fresh(2);
    each("Rename onto itself", c_rename_onto_self);
    each("Rename of a missing file", c_rename_missing);
    each("Delete of a missing file", c_delete_missing);
    each("File_Delete on a directory", c_delete_directory_as_file);
    each("Directory_Create over a file", c_mkdir_over_file);
    each("File_Open on a directory", c_open_directory);
    each("Directory_Delete of a non-empty dir", c_rmdir_nonempty);
    each("Directory_Delete of a missing dir", c_rmdir_missing);
    (void)fresh(0); (void)fresh(1); (void)fresh(2);
    each("Directory deleted: the mount root", c_rmdir_root);
    each("Directory_Open on a file", c_list_file);
    each("double slash '//x.txt'", c_double_slash);
    each("dot component '/./x.txt'", c_dot_component);
    each("trailing slash on a file '/x.txt/'", c_trailing_slash_file);
    each("relative path 'x.txt'", c_relative_path);
    each("200-character path", c_too_long_path);
    each("WriteAll of 0 bytes", c_write_zero_bytes);
    each("WriteAll with a NULL buffer", c_write_null);
    each("ReadAll of a missing file", c_read_all_missing);
    each("File_Copy onto itself", c_copy_to_self);
    each("File_Move onto an existing directory", c_move_to_existing_dir);
    each("Directory_Copy into itself", c_dir_copy_into_self);
    each("File_Delete of a file open for writing", c_delete_open_writer_result);
    (void)fresh(0); (void)fresh(1); (void)fresh(2);
    each("Write through an APPEND handle, then SizeOf", c_size_after_close);
    return 0;
}
