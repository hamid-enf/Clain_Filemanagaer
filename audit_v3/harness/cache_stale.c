/* Audit v3: the read cache must never serve bytes that are no longer in
 * the file.  Every way the file can change is tried with the cache warm.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[3];
static int        g_bad;

static unsigned char g_back[4096];

/* read the whole file through the cached path and compare */
static void expect(const char *mnt, const char *what, const char *path,
                   const char *want, size_t wn)
{
    size_t got = 0;
    FM_Result r;

    memset(g_back, 0xEE, sizeof(g_back));
    r = File_ReadAll(path, g_back, sizeof(g_back), &got);
    if (r != FM_OK || got != wn || memcmp(g_back, want, wn) != 0)
    {
        printf("STALE %-4s %-46s %s, %u bytes (want %u)\n", mnt, what,
               FM_ResultString(r), (unsigned)got, (unsigned)wn);
        printf("      got : %.*s\n", (int)got, (char *)g_back);
        printf("      want: %.*s\n", (int)wn, want);
        g_bad = 1;
    }
}

static void warm(const char *path)
{
    /* read in cache-line sized pieces so the cache really holds data */
    FM_File *f = File_Open(path, FM_OPEN_READ);
    unsigned char buf[64];
    uint32_t got = 0;
    int i;

    if (f == NULL) return;
    for (i = 0; i < 40; i++)
    {
        if (File_Read(f, buf, sizeof(buf), &got) != FM_OK) break;
    }
    (void)File_Close(f);
}

static void one(const char *mnt)
{
    char a[64], b[64];
    FM_File *f;
    uint32_t got = 0;

    snprintf(a, sizeof(a), "%s/a.txt", mnt);
    snprintf(b, sizeof(b), "%s/b.txt", mnt);

    /* 1. rewrite through File_WriteAll (truncate) with the cache warm */
    (void)File_WriteAll(a, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 32);
    warm(a);
    (void)File_WriteAll(a, "BBB", 3);
    expect(mnt, "truncating rewrite after a cached read", a, "BBB", 3);

    /* 2. append after a cached read */
    (void)File_WriteAll(a, "AAAA", 4);
    warm(a);
    (void)File_AppendText(a, "BBBB");
    expect(mnt, "append after a cached read", a, "AAAABBBB", 8);

    /* 3. write through an open handle after a cached read */
    (void)File_WriteAll(a, "AAAAAAAA", 8);
    warm(a);
    f = File_Open(a, FM_OPEN_WRITE);
    if (f != NULL)
    {
        (void)File_Write(f, "ZZ", 2, &got);
        (void)File_Close(f);
        expect(mnt, "write through a second handle", a, "ZZAAAAAA", 8);
    }

    /* 4. rename onto the name, after caching both names */
    (void)File_WriteAll(a, "AAAA", 4);
    (void)File_WriteAll(b, "BBBB", 4);
    warm(b);
    (void)File_Rename(a, b);
    expect(mnt, "rename over a cached name", b, "AAAA", 4);
    (void)File_Rename(b, a);

    /* 5. delete and recreate under the same name */
    (void)File_WriteAll(a, "AAAA", 4);
    warm(a);
    (void)File_Delete(a);
    (void)File_WriteAll(a, "CC", 2);
    expect(mnt, "delete then recreate", a, "CC", 2);

    /* 6. File_Copy over a cached destination */
    (void)File_WriteAll(a, "SRC", 3);
    (void)File_WriteAll(b, "DDDDDD", 6);
    warm(b);
    (void)File_Copy(a, b, NULL, NULL);
    expect(mnt, "copy over a cached destination", b, "SRC", 3);

    /* 7. File_ReplaceRange with the cache warm */
    (void)File_WriteAll(a, "0123456789", 10);
    warm(a);
    (void)File_ReplaceRange(a, 0, 3, "ABC", 3);
    expect(mnt, "replace range at the start", a, "ABC3456789", 10);

    /* 8. File_Insert at the end with the cache warm */
    (void)File_WriteAll(a, "0123", 4);
    warm(a);
    (void)File_Insert(a, 4, "XY", 2);
    expect(mnt, "insert at the end", a, "0123XY", 6);

    /* 9. and the same file read twice in a row must be stable */
    (void)File_WriteAll(a, "stable", 6);
    warm(a);
    expect(mnt, "second read after a warm cache", a, "stable", 6);
    expect(mnt, "third read after a warm cache", a, "stable", 6);

    (void)File_Delete(a);
    (void)File_Delete(b);
}

int main(void)
{
    int k;
    FM_Result r;

    FileManager_Init();
    (void)mkdir("/tmp/cs_root", 0777);
    (void)chdir("/tmp/cs_root");
    for (k = 0; k < 2; k++)
    {
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], k ? "CB" : "CA", g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(k ? "CB" : "CA");
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[0]) : LittleFS_Init(&g_fs[1]);
        if (r == FM_OK) r = FS_Mount(k ? "/CB" : "/CA", &g_fs[k]);
        if (r != FM_OK) printf("mount %d: %s\n", k, FM_ResultString(r));
    }
    if (PosixFS_Init(&g_fs[2]) == FM_OK && FS_Mount("/", &g_fs[2]) != FM_OK)
        printf("posix mount failed\n");
    one("/CA");
    one("/CB");
    one("");
    printf("cache_stale: %s\n", g_bad ? "STALE DATA SERVED" : "clean on 3 backends");
    return g_bad;
}
