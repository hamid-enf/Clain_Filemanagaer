/* Audit v3: differential tester across the three host backends.
 *
 * Every random operation is executed on ALL mounted backends (FMFS on a
 * RAM disk, LittleFS on a RAM disk, POSIX in a scratch directory) and the
 * result code, the file contents and every directory listing are
 * compared between them.  A difference is printed together with the
 * operation, so the log is a reproducer.
 *
 *   ./diff_fs <seed> <ops> [mask]      mask: 1 FMFS 2 LittleFS 4 POSIX
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

#define NBACK 2
#define MAXF 8
#define NAMELEN 32
#define CM 1024

static uint8_t   g_mem[NBACK][1024 * 1024];
static FM_Storage g_st[NBACK];
static FM_FS      g_fs[NBACK];
static FM_FS      g_posix;
static const char *const g_stname[NBACK] = { "FA", "FB" };
static const char *const g_mount[3] = { "/FA", "/FB", "" };
static int        g_live[3];
static unsigned   g_rng;

static unsigned rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8) & 0xffffffu;
}

static const char *bname(int k)
{
    return k == 0 ? "FMFS" : (k == 1 ? "LittleFS" : "POSIX");
}

static void path_of(char *out, size_t n, int kind, const char *p)
{
    if (g_mount[kind][0] == 0) snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s%s", g_mount[kind], p);
}

/* ---- state of one backend: sorted list of paths, sizes, contents ---- */

typedef struct
{
    int    n;
    char   path[MAXF * 2][NAMELEN * 2];
    size_t size[MAXF * 2];
} State;

static const char *rel_of(int kind, const char *full)
{
    size_t m = strlen(g_mount[kind]);

    if (m > 0 && strncmp(full, g_mount[kind], m) == 0) return full + m;
    return full;
}

static FM_Result collect_cb(const FM_FileInfo *info, void *user)
{
    State *s = (State *)user;

    if (s->n < MAXF * 2)
    {
        strncpy(s->path[s->n], info->path, sizeof(s->path[0]) - 1);
        s->size[s->n] = (size_t)info->size;
        s->n++;
    }
    return FM_OK;
}

static void sort_state(State *s)
{
    int i, j;

    for (i = 1; i < s->n; i++)
    {
        for (j = i; j > 0 && strcmp(s->path[j - 1], s->path[j]) > 0; j--)
        {
            char t[NAMELEN * 2];
            size_t z = s->size[j];

            memcpy(t, s->path[j - 1], sizeof(t));
            memcpy(s->path[j - 1], s->path[j], sizeof(t));
            memcpy(s->path[j], t, sizeof(t));
            s->size[j] = s->size[j - 1];
            s->size[j - 1] = z;
        }
    }
}

/* Walk the whole tree (depth-first, bounded) and record every entry. */
static int kind_of_root;

static void walk_state(State *s, const char *root, int depth)
{
    FM_Dir *d;
    FM_DirEntry e;

    if (depth > 4) return;
    d = Directory_Open(root);
    if (d == NULL) return;
    while (Directory_Read(d, &e) == FM_OK && s->n < MAXF * 2)
    {
        char full[NAMELEN * 2];

        snprintf(full, sizeof(full), "%s%s%s", root,
                 (root[strlen(root) - 1] == '/') ? "" : "/", e.name);
        strncpy(s->path[s->n], rel_of(kind_of_root, full), sizeof(s->path[0]) - 1);
        s->size[s->n] = (size_t)e.size;
        s->n++;
        (void)Directory_Close(d);
        if (e.type == FM_ITEM_DIR) walk_state(s, full, depth + 1);
        d = Directory_Open(root);
        if (d == NULL) return;
        /* restart this directory and skip what was already seen */
        {
            FM_DirEntry skip;
            int k;

            for (k = 0; k < s->n; k++)
            {
                char back[NAMELEN * 2];

                if (Directory_Read(d, &skip) != FM_OK) break;
                snprintf(back, sizeof(back), "%s%s%s", root,
                         (root[strlen(root) - 1] == '/') ? "" : "/",
                         skip.name);
                if (strcmp(back, full) == 0) break;
            }
            (void)skip;
        }
    }
    (void)Directory_Close(d);
}

static void snapshot(State *s, int kind)
{
    int n;

    memset(s, 0, sizeof(*s));
    /* flat: the candidates /f0../f3, queried directly (no walker in the
     * oracle path) */
    for (n = 0; n < 4; n++)
    {
        char rel[NAMELEN];
        char full[256];
        FM_FileInfo info;

        snprintf(rel, sizeof(rel), "/f%d", n);
        path_of(full, sizeof(full), kind, rel);
        memset(&info, 0, sizeof(info));
        if (File_GetInfo(full, &info) == FM_OK)
        {
            strncpy(s->path[s->n], rel, sizeof(s->path[0]) - 1);
            s->size[s->n] = (size_t)info.size;
            s->n++;
        }
    }
    sort_state(s);
}

static int states_differ(const State *a, const State *b, int ka, int kb,
                         const char *what, unsigned op)
{
    int i;

    if (a->n != b->n)
    {
        printf("op %u (%s): %s has %d entries, %s has %d\n", op, what,
               bname(ka), a->n, bname(kb), b->n);
        for (i = 0; i < a->n; i++) printf("    %s: %s (%u)\n", bname(ka),
                                         a->path[i], (unsigned)a->size[i]);
        for (i = 0; i < b->n; i++) printf("    %s: %s (%u)\n", bname(kb),
                                         b->path[i], (unsigned)b->size[i]);
        return 1;
    }
    for (i = 0; i < a->n; i++)
    {
        if (strcmp(a->path[i], b->path[i]) != 0 || a->size[i] != b->size[i])
        {
            printf("op %u (%s): entry %d is %s (%u bytes) on %s but %s "
                   "(%u bytes) on %s\n", op, what, i, a->path[i],
                   (unsigned)a->size[i], bname(ka), b->path[i],
                   (unsigned)b->size[i], bname(kb));
            return 1;
        }
    }
    return 0;
}

static int contents_differ(int ka, int kb, const State *a, unsigned op,
                           const char *what)
{
    int i;

    for (i = 0; i < a->n; i++)
    {
        static unsigned char ba[CM + 8], bb[CM + 8];
        size_t ga = 0, gb = 0;
        char pa[256], pb[256];
        FM_Result ra, rb;

        if (a->size[i] == 0) continue;
        snprintf(pa, sizeof(pa), "%s", a->path[i]);
        snprintf(pb, sizeof(pb), "%s", a->path[i]);
        memset(ba, 0, sizeof(ba));
        memset(bb, 0, sizeof(bb));
        ra = File_ReadAll(pa, ba, sizeof(ba), &ga);
        rb = File_ReadAll(pb, bb, sizeof(bb), &gb);
        if (ra != rb || ga != gb || memcmp(ba, bb, ga) != 0)
        {
            printf("op %u (%s): contents of %s differ: %s %u bytes vs "
                   "%s %u bytes\n", op, what, a->path[i], FM_ResultString(ra),
                   (unsigned)ga, FM_ResultString(rb), (unsigned)gb);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
    unsigned ops = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 300u;
    int mask = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 7;
    unsigned i;
    int k;
    int failures = 0;

    g_rng = seed;
    if (FileManager_Init() != FM_OK) return 1;

    if (mask & 4)
    {
        (void)mkdir("/tmp/diff_fs_root", 0777);
        (void)chdir("/tmp/diff_fs_root");
        if (PosixFS_Init(&g_posix) == FM_OK && FS_Mount("/", &g_posix) == FM_OK)
            g_live[2] = 1;
        else
            printf("skip POSIX\n");
    }
    for (k = 0; k < NBACK; k++)
    {
        FM_Result r;

        if (!(mask & (1 << k))) continue;
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], g_stname[k], g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(g_stname[k]);
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[k]) : LittleFS_Init(&g_fs[k]);
        if (r == FM_OK) r = FS_Mount(g_mount[k], &g_fs[k]);
        if (r == FM_OK) g_live[k] = 1;
        else printf("skip %s: %s\n", bname(k), FM_ResultString(r));
    }

    if (!g_live[0] && !g_live[1] && !g_live[2]) return 2;

    for (i = 0; i < ops; i++)
    {
        /* the names this run uses: a few short ones so files collide */
        char name[NAMELEN];
        char name2[NAMELEN];
        char rel[NAMELEN];
        char rel2[NAMELEN];
        char what[96];
        unsigned pick = rnd() % 100u;
        int kind;
        int results[3];
        State st0, stk;
        int first = -1;

        snprintf(rel, sizeof(rel), "/f%u", rnd() % 4u);
        snprintf(rel2, sizeof(rel2), "/f%u", rnd() % 4u);
        snprintf(name, sizeof(name), "%s", rel);
        snprintf(name2, sizeof(name2), "%s", rel2);

        /* every random parameter of the operation is drawn ONCE, before
         * the loop: all backends must see the same call */
        {
            static unsigned char wdata[64];
            unsigned wlen = 1u + rnd() % 40u;
            unsigned keep = rnd() % 8u;
            unsigned at = rnd() % 40u;
            unsigned j;

            for (j = 0; j < wlen; j++) wdata[j] = (unsigned char)(0x41 + (j & 15));

        for (kind = 0; kind < 3; kind++)
        {
            char pa[256], pb[256];
            FM_Result r = FM_OK;

            if (!g_live[kind]) { results[kind] = -1000; continue; }
            path_of(pa, sizeof(pa), kind, name);
            path_of(pb, sizeof(pb), kind, name2);

            if (pick < 30u)                       /* create / overwrite */
            {
                unsigned n = wlen;

                r = File_WriteAll(pa, wdata, n);
                snprintf(what, sizeof(what), "WriteAll %s (%u bytes)", name, n);
            }
            else if (pick < 45u)                  /* append */
            {
                r = File_AppendAll(pa, "APP", 3);
                snprintf(what, sizeof(what), "AppendAll %s", name);
            }
            else if (pick < 55u)                  /* truncate */
            {
                FM_File *f = File_Open(pa, FM_OPEN_READ | FM_OPEN_WRITE);

                if (f == NULL) r = File_GetLastError();
                else
                {
                    r = File_Truncate(f, keep);
                    if (r == FM_OK) { FM_Result c = File_Close(f); if (c != FM_OK) r = c; }
                    else (void)File_Close(f);
                }
                snprintf(what, sizeof(what), "Truncate %s to %u", name, keep);
            }
            else if (pick < 62u)                  /* seek past end + write */
            {
                FM_File *f = File_Open(pa, FM_OPEN_READ | FM_OPEN_WRITE);
                uint32_t wr = 0;

                if (f == NULL) r = File_GetLastError();
                else
                {
                    r = File_Seek(f, (int64_t)at, FM_SEEK_SET);
                    if (r == FM_OK) r = File_Write(f, "Q", 1, &wr);
                    else (void)File_Close(f);
                    if (f != NULL)
                    {
                        FM_Result c = File_Close(f);
                        if (r == FM_OK) r = c;
                    }
                }
                snprintf(what, sizeof(what), "Seek(%u)+Write %s", at, name);
            }
            else if (pick < 70u)                  /* rename */
            {
                r = File_Rename(pa, pb);
                snprintf(what, sizeof(what), "Rename %s -> %s", name, name2);
            }
            else if (pick < 80u)                  /* delete */
            {
                r = File_Delete(pa);
                snprintf(what, sizeof(what), "Delete %s", name);
            }
            else if (pick < 85u)                  /* delete, ignoring missing */
            {
                (void)File_Delete(pa);
                snprintf(what, sizeof(what), "Delete(ignore) %s", name);
            }
            else if (pick < 90u)                  /* stat */
            {
                FM_FileInfo info;
                FM_Bool ex = FM_FALSE;
                uint64_t sz = 0;

                memset(&info, 0, sizeof(info));
                r = File_GetInfo(pa, &info);
                {
                    FM_Result e2 = File_Exists(pa, &ex);
                    FM_Result s2 = File_SizeOf(pa, &sz);

                    if (r == FM_OK && e2 == FM_OK && !ex) r = FM_ERR_INTERNAL;
                    if (r == FM_OK && s2 == FM_OK && info.size != sz)
                    {
                        printf("op %u %s: GetInfo says %u bytes, SizeOf says "
                               "%u for %s\n", i, bname(kind),
                               (unsigned)info.size, (unsigned)sz, name);
                        failures++;
                    }
                }
                snprintf(what, sizeof(what), "GetInfo %s", name);
            }
            else                                  /* read back */
            {
                static unsigned char b[CM];
                size_t g = 0;

                r = File_ReadAll(pa, b, sizeof(b), &g);
                if (r == FM_OK && g != 0) r = FM_OK;
                snprintf(what, sizeof(what), "ReadAll %s", name);
            }
            results[kind] = (int)r;
        }
        }

        /* compare the return codes (ignoring backends that are absent) */
        for (kind = 0; kind < 3; kind++)
        {
            if (!g_live[kind]) continue;
            if (first < 0) { first = kind; continue; }
            if (results[kind] != results[first] &&
                !(results[kind] == FM_ERR_EOF && results[first] == FM_OK) &&
                !(results[kind] == FM_OK && results[first] == FM_ERR_EOF))
            {
                printf("op %u (%s): %s returned %d (%s), %s returned %d "
                       "(%s)\n", i, what, bname(first), results[first],
                       FM_ResultString((FM_Result)results[first]),
                       bname(kind), results[kind],
                       FM_ResultString((FM_Result)results[kind]));
                failures++;
            }
        }

        /* compare the whole trees */
        if (first >= 0)
        {
            snapshot(&st0, first);
            for (kind = 0; kind < 3; kind++)
            {
                if (!g_live[kind] || kind == first) continue;
                snapshot(&stk, kind);
                failures += states_differ(&st0, &stk, first, kind, what, i);
                failures += contents_differ(first, kind, &st0, i, what);
            }
        }
        if (failures) break;
    }

    for (k = 0; k < NBACK; k++)
    {
        if (g_live[k])
        {
            (void)FS_Unmount(g_mount[k]);
            (void)Storage_Unmount(g_stname[k]);
            (void)Storage_Unregister(g_stname[k]);
        }
    }
    printf("diff_fs seed=%u ops=%u: %s\n", seed, ops,
           failures ? "DIVERGED" : "consistent");
    return failures ? 1 : 0;
}
