/* Audit v3: File_Insert / File_DeleteRange / File_ReplaceRange against an
 * in-RAM model, on both RAM backends, with random op sequences and a full
 * read-back compared byte for byte after each op.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][2 * 1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static const char *const g_stname[2] = { "EA", "EB" };
static const char *const g_mount[2] = { "/EA", "/EB" };

static unsigned g_rng = 1;
static unsigned rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8);
}

static unsigned char g_model[8192];
static size_t        g_len;
static unsigned char g_back[8192];

static int verify(const char *mnt, unsigned step, const char *what)
{
    size_t got = 0;
    char p[64];
    FM_Result r;

    snprintf(p, sizeof(p), "%s/f.bin", mnt);
    memset(g_back, 0, sizeof(g_back));
    r = File_ReadAll(p, g_back, sizeof(g_back), &got);
    if (r != FM_OK || got != g_len || memcmp(g_back, g_model, g_len) != 0)
    {
        size_t i;

        printf("  %s step %u (%s): read %s, %u bytes, model %u bytes\n", mnt,
               step, what, FM_ResultString(r), (unsigned)got, (unsigned)g_len);
        for (i = 0; i < g_len && i < got; i++)
        {
            if (g_back[i] != g_model[i])
            {
                printf("    first difference at %u: %02X, model %02X\n",
                       (unsigned)i, g_back[i], g_model[i]);
                break;
            }
        }
        return 1;
    }
    return 0;
}

static int run(const char *mnt, unsigned seed)
{
    char p[64];
    unsigned step;
    int bad = 0;

    g_rng = seed;
    g_len = 0;
    snprintf(p, sizeof(p), "%s/f.bin", mnt);
    (void)File_Delete(p);
    (void)File_WriteAll(p, "", 0);

    for (step = 0; step < 300 && !bad; step++)
    {
        unsigned kind = rnd() % 3u;
        size_t off = (g_len == 0) ? 0 : rnd() % (g_len + 1);
        size_t n = rnd() % 64u;
        unsigned char data[64];
        unsigned i;
        FM_Result r;

        for (i = 0; i < n; i++) data[i] = (unsigned char)(rnd() & 0xFF);

        if (kind == 0)
        {
            r = File_Insert(p, off, data, n);
            if (r == FM_OK)
            {
                memmove(g_model + off + n, g_model + off, g_len - off);
                memcpy(g_model + off, data, n);
                g_len += n;
            }
            bad |= verify(mnt, step, "insert");
        }
        else if (kind == 1)
        {
            if (g_len == 0) continue;
            if (off + n > g_len) n = g_len - off;
            r = File_DeleteRange(p, off, n);
            if (r == FM_OK)
            {
                memmove(g_model + off, g_model + off + n, g_len - off - n);
                g_len -= n;
            }
            bad |= verify(mnt, step, "delete range");
        }
        else
        {
            if (off + n > g_len) n = g_len - off;
            r = File_ReplaceRange(p, off, n, data, n);
            if (r == FM_OK) memcpy(g_model + off, data, n);
            bad |= verify(mnt, step, "replace range");
        }
        if (bad)
        {
            printf("  (seed %u, op kind %u at %u, n=%u, result %s)\n", seed,
                   kind, (unsigned)off, (unsigned)n, FM_ResultString(r));
            break;
        }
    }
    (void)File_Delete(p);
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
        if (r != FM_OK) { printf("mount %d: %s\n", k, FM_ResultString(r)); bad++; }
    }
    {
        unsigned seed;

        for (seed = 1; seed <= 12; seed++)
        {
            bad += run("/EA", seed);
            bad += run("/EB", seed);
        }
    }
    printf("edit_model: %s\n", bad ? "MISMATCH" : "300 ops x 12 seeds x 2 backends, all identical to the model");
    return bad ? 1 : 0;
}
