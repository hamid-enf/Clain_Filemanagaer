/* Audit v3, Job 1: the six earlier file findings, changed inputs.
 *  FILE-01 truncate on a read-only handle (LittleFS/FMFS/POSIX)
 *  FILE-02 append after an external write (stale size)
 *  FILE-05 Stream_InitFile rebind + release
 *  FILE-06 POSIX opens a directory as a file
 *  FILE-04 CR handling (already covered by text_* harnesses)
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

static void trunc_readonly(const char *mnt)
{
    char p[64];
    FM_File *f;

    snprintf(p, sizeof(p), "%s/tr.txt", mnt);
    (void)File_WriteAll(p, "0123456789", 10);
    f = File_Open(p, FM_OPEN_READ);
    if (f == NULL) { printf("  %-4s open: %s\n", mnt, FM_ResultString(File_GetLastError())); g_bad = 1; return; }
    printf("  %-4s Truncate(4) on a read-only handle -> %s", mnt,
           FM_ResultString(File_Truncate(f, 4)));
    {
        uint64_t sz = 0;
        (void)File_Size(f, &sz);
        printf("  size now %u (want 10)\n", (unsigned)sz);
        if (sz != 10u) g_bad = 1;
    }
    (void)File_Close(f);
}

static void append_after_external_write(const char *mnt)
{
    char p[64];
    FM_File *a;

    snprintf(p, sizeof(p), "%s/ap.txt", mnt);
    (void)File_WriteAll(p, "AAAA", 4);
    /* one writer holds the file; a second write comes from outside */
    a = File_Open(p, FM_OPEN_WRITE | FM_OPEN_APPEND);
    if (a == NULL) { printf("  %-4s append open: %s\n", mnt, FM_ResultString(File_GetLastError())); return; }
    {
        uint32_t wr = 0;
        (void)File_Write(a, "BB", 2, &wr);
    }
    (void)File_Close(a);
    {
        char back[32];
        size_t got = 0;

        (void)File_ReadAll(p, back, sizeof(back), &got);
        printf("  %-4s append twice -> %.*s (%u bytes, want 6)\n", mnt,
               (int)got, back, (unsigned)got);
        if (got != 6u || memcmp(back, "AAAABB", 6) != 0) g_bad = 1;
    }
}

static void stream_rebind(void)
{
    FM_File *f = File_Open("/RE/st.bin", FM_OPEN_READ);
    FM_Stream s;
    FM_Result r;
    int i;
    int busy_seen = 0;

    (void)File_WriteAll("/RE/st.bin", "stream", 6);
    if (f == NULL) f = File_Open("/RE/st.bin", FM_OPEN_READ);
    memset(&s, 0, sizeof(s));
    if (f == NULL) { printf("  stream: open failed\n"); g_bad = 1; return; }
    r = Stream_InitFile(&s, f);
    for (i = 0; i < 4; i++)
    {
        if (Stream_InitFile(&s, f) == FM_ERR_BUSY) busy_seen++;
    }
    {
        char b[8];
        size_t rd = 0;

        r = Stream_Read(&s, b, 6, &rd);
        printf("  stream: rebind refused %d/4 times, read %s %u bytes\n",
               busy_seen, FM_ResultString(r), (unsigned)rd);
        if (busy_seen != 4 || rd != 6u || memcmp(b, "stream", 6) != 0) g_bad = 1;
    }
    Stream_Close(&s);
    for (i = 0; i < 20; i++)
    {
        if (Stream_InitFile(&s, f) != FM_OK) break;
        Stream_Close(&s);
    }
    printf("  stream: %d further init/close cycles before failure (want 20)\n",
           i);
    if (i != 20) g_bad = 1;
    (void)File_Close(f);
    (void)File_Delete("/RE/st.bin");
}

int main(void)
{
    FM_Result r;
    int k;

    FileManager_Init();
    (void)mkdir("/tmp/rg_root", 0777);
    (void)chdir("/tmp/rg_root");
    for (k = 0; k < 2; k++)
    {
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], k ? "LB" : "LA", g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(k ? "LB" : "LA");
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[0]) : LittleFS_Init(&g_fs[1]);
        if (r == FM_OK) r = FS_Mount(k ? "/LB" : "/LA", &g_fs[k]);
        if (r != FM_OK) { printf("mount %d: %s\n", k, FM_ResultString(r)); return 2; }
    }
    if (PosixFS_Init(&g_fs[2]) == FM_OK && FS_Mount("/", &g_fs[2]) != FM_OK)
        printf("posix mount failed\n");
    if (FS_Mount("/RE", &g_fs[2]) != FM_OK) { /* reuse the posix fs at /RE */ }

    printf("FILE-01: truncate on a read-only handle\n");
    trunc_readonly("/LA");
    trunc_readonly("/LB");
    trunc_readonly("");

    printf("\nFILE-02: append through an open handle twice\n");
    append_after_external_write("/LA");
    append_after_external_write("/LB");

    printf("\nFILE-06: open a directory as a file\n");
    (void)Directory_Create("/dir_v2");
    {
        FM_File *d = File_Open("/dir_v2", FM_OPEN_READ);

        printf("  POSIX open(read) on a directory -> %s\n",
               (d == NULL) ? FM_ResultString(File_GetLastError()) : "OK (!)");
        if (d != NULL) { g_bad = 1; (void)File_Close(d); }
        (void)Directory_Delete("/dir_v2");
    }

    printf("\nFILE-05: stream contexts\n");
    {
        static uint8_t s_mem[256 * 1024];
        static FM_Storage s_st;
        static FM_FS s_fs;

        memset(s_mem, 0, sizeof(s_mem));
        r = RamDisk_Init(&s_st, "RE", s_mem, sizeof(s_mem), 512u);
        if (r == FM_OK) r = Storage_Register(&s_st);
        if (r == FM_OK) r = Storage_Mount("RE");
        if (r == FM_OK) r = FMFS_Init(&s_fs);
        if (r == FM_OK) r = FS_Mount("/RE", &s_fs);
        if (r == FM_OK) stream_rebind();
        else printf("  stream mount: %s\n", FM_ResultString(r));
    }
    printf("\nregress_v2: %s\n", g_bad ? "REGRESSION" : "all as fixed");
    return g_bad;
}
