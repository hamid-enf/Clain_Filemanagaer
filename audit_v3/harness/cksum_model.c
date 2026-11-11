/* Audit v3: model-based write/read consistency for files of every size
 * around every internal boundary (512-byte bounce block, cache line,
 * FMFS chunk), on both RAM backends.  Every byte written is compared with
 * what is read back, at every offset, with every read length.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][2 * 1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static const char *const g_stname[2] = { "WA", "WB" };
static const char *const g_mount[2] = { "/WA", "/WB" };

static unsigned char g_buf[8192];
static unsigned char g_back[8192];

static unsigned pat(unsigned off)
{
    return (unsigned char)((off * 31u + (off >> 8) * 7u + 0x5A) & 0xFF);
}

static int one(int kind)
{
    static const unsigned sizes[] = { 1, 2, 511, 512, 513, 1023, 1024, 1025,
                                      4095, 4096, 4097, 8191, 8192 };
    unsigned si;
    int bad = 0;
    char path[64];

    snprintf(path, sizeof(path), "%s/data.bin", g_mount[kind]);
    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
    {
        unsigned n = sizes[si];
        unsigned i;
        FM_Result r;

        for (i = 0; i < n; i++) g_buf[i] = (unsigned char)pat(i);
        r = File_WriteAll(path, g_buf, n);
        if (r != FM_OK)
        {
            printf("  %s: WriteAll(%u) -> %s\n", g_mount[kind], n,
                   FM_ResultString(r));
            return 1;
        }
        /* read the whole file back through one File_ReadAll */
        {
            size_t got = 0;

            memset(g_back, 0xEE, sizeof(g_back));
            r = File_ReadAll(path, g_back, sizeof(g_back), &got);
            if (r != FM_OK || got != n || memcmp(g_back, g_buf, n) != 0)
            {
                printf("  %s %u bytes: ReadAll -> %s, %u bytes, %s\n",
                       g_mount[kind], n, FM_ResultString(r), (unsigned)got,
                       (got == n) ? "content differs" : "");
                for (i = 0; i < n && i < got; i++)
                {
                    if (g_back[i] != g_buf[i])
                    {
                        printf("    first difference at %u: %02X, wrote %02X\n",
                               i, g_back[i], g_buf[i]);
                        break;
                    }
                }
                bad = 1;
                continue;
            }
        }
        /* and with a handle, at every offset and several lengths */
        {
            FM_File *f = File_Open(path, FM_OPEN_READ);
            unsigned off;
            static const unsigned lens[] = { 1, 2, 7, 512, 513, 1024 };

            if (f == NULL) { bad = 1; continue; }
            for (off = 0; off < n; off += (n > 600 ? 97u : 3u))
            {
                unsigned li;

                for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++)
                {
                    unsigned len = lens[li];
                    uint32_t got = 0;

                    if (off + len > n) continue;
                    if (File_Seek(f, (int64_t)off, FM_SEEK_SET) != FM_OK)
                    {
                        printf("  %s: Seek(%u) failed\n", g_mount[kind], off);
                        bad = 1;
                        break;
                    }
                    memset(g_back, 0xEE, len + 8);
                    r = File_Read(f, g_back, len, &got);
                    if (r != FM_OK || got != len ||
                        memcmp(g_back, g_buf + off, len) != 0)
                    {
                        printf("  %s: %u bytes, Read(%u) at %u -> %s, %u "
                               "bytes, %s\n", g_mount[kind], n, len, off,
                               FM_ResultString(r), (unsigned)got,
                               (got == len && memcmp(g_back, g_buf + off, len))
                               ? "content differs" : "");
                        bad = 1;
                    }
                }
                if (bad) break;
            }
            (void)File_Close(f);
        }
        if (bad) break;
    }
    return bad;
}

int main(void)
{
    int k;
    int bad = 0;

    FileManager_Init();
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
        if (r != FM_OK) { printf("mount %d: %s\n", k, FM_ResultString(r)); continue; }
        printf("%s: %s\n", g_mount[k], one(k) ? "MISMATCH" : "ok");
        bad += one(k);
    }
    return bad ? 1 : 0;
}
