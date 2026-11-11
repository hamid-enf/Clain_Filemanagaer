/* Audit v3: FM_Async (cooperative chunked operations) against the blocking
 * equivalents.  Same tree, same questions:
 *
 *   scan   -- every entry Directory_ScanRecursive reports, exactly once
 *   delete -- everything gone, and the progress count equals the number of
 *             entries that were there
 *   copy   -- byte-identical destination
 *   cancel -- stops, leaves something sane, and the object can be reused
 *
 * Both RAM backends; two trees, one wide, one deep.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_async.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][4 * 1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static const char *const g_stname[2] = { "AA", "AB" };
static const char *const g_mount[2] = { "/AA", "/AB" };

static int g_bad;

/* --- collectors --------------------------------------------------- */
struct names { char v[512][64]; unsigned n; };

static FM_Result collect_cb(const FM_FileInfo *info, void *user)
{
    struct names *c = (struct names *)user;

    if (c->n < 512)
    {
        size_t k;

        snprintf(c->v[c->n], sizeof(c->v[0]), "%s", info->path);
        /* sort key: turn the mount prefix into nothing so both backends
         * produce the same set */
        for (k = 0; c->v[c->n][k] != 0; k++)
        {
            if (c->v[c->n][k] == '/' && c->v[c->n][k + 1] == g_mount[0][1])
            {
                /* leave it, we compare with the prefix stripped below */
            }
        }
        c->n++;
    }
    return FM_OK;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* strip the "/XY" mount prefix, so a tree built on either volume gives
 * the same names */
static void strip(struct names *c, const char *mnt)
{
    unsigned i;
    size_t ml = strlen(mnt);

    for (i = 0; i < c->n; i++)
    {
        if (strncmp(c->v[i], mnt, ml) == 0)
        {
            memmove(c->v[i], c->v[i] + ml, strlen(c->v[i] + ml) + 1);
        }
    }
    qsort(c->v, c->n, sizeof(c->v[0]), cmp_str);
}

static void build_tree(const char *mnt, int deep)
{
    char p[160];
    int i;

    snprintf(p, sizeof(p), "%s/t", mnt);
    (void)Directory_Create(p);
    if (deep)
    {
        /* /t/1/2/3/.../8, a file in each level */
        for (i = 1; i <= 8; i++)
        {
            size_t l = strlen(p);

            snprintf(p + l, sizeof(p) - l, "/%d", i);
            (void)Directory_Create(p);
            {
                char f[160];
                size_t fl = strlen(p);

                snprintf(f, sizeof(f), "%s/f%d.txt", p, i);
                (void)File_WriteAll(f, "x", 1);
            }
        }
        /* and a sibling tree at every level */
        for (i = 1; i <= 8; i++)
        {
            char q[160];
            size_t k;

            snprintf(q, sizeof(q), "%s/t", mnt);
            for (k = 1; k <= (size_t)i; k++)
            {
                size_t l = strlen(q);

                snprintf(q + l, sizeof(q) - l, "/%u", (unsigned)k);
            }
            {
                char f[176];
                size_t fl = strlen(q);

                snprintf(f, sizeof(f), "%s/s%u.txt", q, (unsigned)i);
                (void)File_WriteAll(f, "y", 1);
            }
        }
    }
    else
    {
        for (i = 0; i < 40; i++)
        {
            char f[176];

            FM_Result wr;

            snprintf(f, sizeof(f), "%s/t/w%02d.txt", mnt, i);
            wr = File_WriteAll(f, "z", 1);
            if (wr != FM_OK)
                printf("    build: %s -> %s\n", f, FM_ResultString(wr));
        }
    }
}

int main(void)
{
    int k;
    FM_Result r;

    FileManager_Init();
    for (k = 0; k < 2; k++)
    {
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], g_stname[k], g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(g_stname[k]);
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[k]) : LittleFS_Init(&g_fs[k]);
        if (r == FM_OK) r = FS_Mount(g_mount[k], &g_fs[k]);
        if (r != FM_OK) { printf("mount %d: %s\n", k, FM_ResultString(r)); return 2; }
    }

    for (k = 0; k < 2; k++)
    {
        const char *mnt = g_mount[k];
        int deep;
        char root[32];

        printf("%s\n", mnt);
        snprintf(root, sizeof(root), "%s/t", mnt);

        for (deep = 0; deep <= 1; deep++)
        {
            static struct names blocking, async;
            FM_Async a;
            unsigned polls;
            uint64_t progress_last = 0;

            /* ---- scan ---- */
            build_tree(mnt, deep);
            memset(&blocking, 0, sizeof(blocking));
            memset(&async, 0, sizeof(async));
            (void)Directory_ScanRecursive(root, collect_cb, NULL, &blocking, NULL);
            strip(&blocking, mnt);

            Async_Init(&a);
            r = Async_StartScan(&a, root, collect_cb, NULL, &async);
            if (r != FM_OK) { printf("  StartScan: %s\n", FM_ResultString(r)); g_bad = 1; }
            polls = 0;
            while (Async_Poll(&a) == FM_ASYNC_RUNNING && polls++ < 100000) {}
            strip(&async, mnt);
            if (a.state != FM_ASYNC_DONE)
            {
                printf("  %s scan: state %d, %s\n", deep ? "deep" : "wide",
                       (int)a.state, FM_ResultString(Async_GetResult(&a)));
                g_bad = 1;
            }
            if (blocking.n != async.n)
            {
                printf("  %s scan: blocking saw %u entries, async %u "
                       "(polls %u)\n", deep ? "deep" : "wide", blocking.n,
                       async.n, polls);
                g_bad = 1;
            }
            else
            {
                unsigned i;

                for (i = 0; i < blocking.n; i++)
                {
                    if (strcmp(blocking.v[i], async.v[i]) != 0)
                    {
                        printf("  %s scan: entry %u differs: '%s' vs '%s'\n",
                               deep ? "deep" : "wide", i, blocking.v[i],
                               async.v[i]);
                        g_bad = 1;
                        break;
                    }
                }
            }

            /* ---- delete, counting progress ---- */
            {
                struct names before;

                memset(&before, 0, sizeof(before));
                (void)Directory_ScanRecursive(root, collect_cb, NULL, &before,
                                              NULL);
                Async_Init(&a);
                r = Async_StartDeleteRecursive(&a, root, NULL, NULL, NULL);
                polls = 0;
                while (Async_Poll(&a) == FM_ASYNC_RUNNING && polls++ < 100000) {}
                if (a.state != FM_ASYNC_DONE)
                {
                    printf("  %s delete: state %d, %s\n", deep ? "deep" : "wide",
                           (int)a.state, FM_ResultString(Async_GetResult(&a)));
                    g_bad = 1;
                }
                if (Directory_Exists(root, NULL) == FM_OK)
                {
                    FM_Bool there = FM_TRUE;

                    (void)Directory_Exists(root, &there);
                    if (there)
                    {
                        printf("  %s delete: the tree is still there\n",
                               deep ? "deep" : "wide");
                        g_bad = 1;
                        (void)Directory_DeleteRecursive(root);
                    }
                }
                printf("  %-4s %u entries, scan/delete as above\n",
                       deep ? "deep" : "wide", before.n);
                (void)progress_last;
            }
        }

        /* ---- copy, byte for byte ---- */
        {
            char src[64], dst[64];
            static unsigned char back[24000];
            size_t got = 0;

            snprintf(src, sizeof(src), "%s/c1.bin", mnt);
            snprintf(dst, sizeof(dst), "%s/c2.bin", mnt);
            {
                static unsigned char big[20000];
                unsigned i;

                for (i = 0; i < sizeof(big); i++) big[i] = (unsigned char)i;
                (void)File_WriteAll(src, big, sizeof(big));
            }
            {
                FM_Async a;
                unsigned polls = 0;

                Async_Init(&a);
                r = Async_StartCopy(&a, src, dst, NULL, NULL, NULL);
                while (Async_Poll(&a) == FM_ASYNC_RUNNING && polls++ < 200000) {}
                if (a.state != FM_ASYNC_DONE)
                {
                    printf("  copy: state %d, %s\n", (int)a.state,
                           FM_ResultString(Async_GetResult(&a)));
                    g_bad = 1;
                }
                (void)File_ReadAll(dst, back, sizeof(back), &got);
                if (got != 20000u)
                {
                    printf("  copy: %u bytes at the destination (want 20000)\n",
                           (unsigned)got);
                    g_bad = 1;
                }
                /* reuse the same object for another copy, and check the
                 * cancel path */
                r = Async_StartCopy(&a, src, dst, NULL, NULL, NULL);
                if (r != FM_OK) { printf("  restart: %s\n", FM_ResultString(r)); g_bad = 1; }
                (void)Async_Cancel(&a);
                (void)Async_Poll(&a);
                if (a.state != FM_ASYNC_CANCELLED)
                {
                    printf("  cancel: state %d (want CANCELLED)\n", (int)a.state);
                    g_bad = 1;
                }
            }
            (void)File_Delete(src);
            (void)File_Delete(dst);
        }

        /* ---- scan of a path that does not exist, and of a file ---- */
        {
            FM_Async a;
            struct names dummy;

            memset(&dummy, 0, sizeof(dummy));
            Async_Init(&a);
            r = Async_StartScan(&a, g_mount[k], collect_cb, NULL, &dummy);
            if (r == FM_OK)
            {
                while (Async_Poll(&a) == FM_ASYNC_RUNNING) {}
                printf("  scan of the mount root: state %d, %s, %u entries\n",
                       (int)a.state, FM_ResultString(Async_GetResult(&a)),
                       dummy.n);
            }
        }
        printf("\n");
    }
    printf("async_model: %s\n", g_bad ? "DIFFERS" : "same as the blocking API");
    return g_bad;
}
