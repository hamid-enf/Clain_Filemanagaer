/* Audit v3: the iterative tree walks.
 *
 *  1. delete / copy while the tree changes under the walk (the documented
 *     rule says callbacks must not, but the *library's own* delete walk
 *     changes the tree it walks: it deletes files as it meets them);
 *  2. a file that appears in a directory the walk is coming back into;
 *  3. depth exactly FM_DIR_MAX_DEPTH and one more, on three backends;
 *  4. cancellation in the middle of a walk.
 *
 * Build: ./build_walk.sh ; run: /tmp/walk
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t  g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static const char *const g_name[2] = { "FA", "FB" };
static const char *const g_mnt[2]  = { "/FA", "/FB" };

static FM_Result mount_kind(int kind)
{
    FM_Result r;

    memset(g_mem[kind], kind ? 0xFF : 0, sizeof(g_mem[kind]));
    r = RamDisk_Init(&g_st[kind], g_name[kind], g_mem[kind],
                     sizeof(g_mem[kind]), kind ? 4096u : 512u);
    if (r == FM_OK) r = Storage_Register(&g_st[kind]);
    if (r == FM_OK) r = Storage_Mount(g_name[kind]);
    if (r == FM_OK)
    {
#if FM_USE_FMFS
        if (kind == 0) r = FMFS_Init(&g_fs[kind]);
#endif
#if FM_USE_LITTLEFS
        if (kind == 1) r = LittleFS_Init(&g_fs[kind]);
#endif
    }
    if (r == FM_OK) r = FS_Mount(g_mnt[kind], &g_fs[kind]);
    return r;
}

static void unmount_kind(int kind)
{
    (void)FS_Unmount(g_mnt[kind]);
    (void)Storage_Unmount(g_name[kind]);
    (void)Storage_Unregister(g_name[kind]);
}

/* callback that deletes the file it is shown */
static FM_Result cb_delete(const FM_FileInfo *info, void *user)
{
    (void)user;
    if (info->type == FM_ITEM_FILE)
    {
        (void)File_Delete(info->path);
    }
    return FM_OK;
}

static uint32_t g_seen;
static FM_Result cb_count(const FM_FileInfo *info, void *user)
{
    (void)info;
    (void)user;
    g_seen++;
    return FM_OK;
}

static void show_tree(const char *what, const char *root)
{
    uint32_t n = 0;
    FM_Result r = Directory_ScanRecursive(root, cb_count, NULL, NULL, NULL);

    printf("    %-28s %-14s entries=%u\n", what, FM_ResultString(r),
           (unsigned)g_seen);
    g_seen = 0;
}

int main(void)
{
    FM_Result r;
    char path[FM_PATH_MAX];
    int kind;
    uint32_t i;

    if (FileManager_Init() != FM_OK) return 1;

    for (kind = 0; kind < 2; kind++)
    {
        printf("\n=== backend %d (%s) ===\n", kind, g_name[kind]);
        r = mount_kind(kind);
        printf("  mount: %s\n", FM_ResultString(r));
        if (r != FM_OK) continue;

        /* ---- 1. depth exactly FM_DIR_MAX_DEPTH, and one more ---- */
        for (i = 0; i < 2; i++)
        {
            uint32_t levels = (i == 0) ? FM_DIR_MAX_DEPTH : FM_DIR_MAX_DEPTH + 1u;
            uint32_t k;

            (void)Directory_DeleteRecursive(
                kind == 0 ? "/FA/dp" : "/FB/dp");

            (void)strcpy(path, kind == 0 ? "/FA/dp" : "/FB/dp");
            r = Directory_Create(path);
            for (k = 0; k < levels && r == FM_OK; k++)
            {
                (void)strcat(path, "/d");
                r = Directory_Create(path);
            }
            (void)strcat(path, "/f.txt");
            if (r == FM_OK) r = File_WriteAll(path, "x", 1);
            {
                uint64_t sz = 0;
                FM_Result s = Directory_Size(kind == 0 ? "/FA/dp" : "/FB/dp", &sz);

                printf("  depth %2u: create=%s size=%s (%u) ",
                       (unsigned)levels, FM_ResultString(r),
                       FM_ResultString(s), (unsigned)sz);
                s = Directory_ScanRecursive(kind == 0 ? "/FA/dp" : "/FB/dp",
                                            cb_count, NULL, NULL, NULL);
                printf("scan=%s(%u) ", FM_ResultString(s), (unsigned)g_seen);
                g_seen = 0;
                s = Directory_DeleteRecursive(kind == 0 ? "/FA/dp" : "/FB/dp");
                printf("delete=%s\n", FM_ResultString(s));
            }
        }


        /* ---- 2. the library's own delete walk on a wide tree ---- */
        {
            uint32_t k;

            (void)Directory_DeleteRecursive(kind == 0 ? "/FA/wide" : "/FB/wide");
            (void)strcpy(path, kind == 0 ? "/FA/wide" : "/FB/wide");
            (void)Directory_Create(path);
            for (k = 0; k < 40; k++)
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/sub%02u", path, (unsigned)k);
                (void)Directory_Create(f);
                (void)snprintf(f, sizeof(f), "%s/sub%02u/a.bin", path, (unsigned)k);
                {
                    FM_Result q = File_WriteAll(f, "12345", 5);

                    if (q != FM_OK)
                    {
                        printf("    write %u failed: %s (%s)\n", (unsigned)k,
                               FM_ResultString(q), f);
                    }
                }
            }
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/single.txt",
                               kind == 0 ? "/FA/wide" : "/FB/wide");
                (void)File_WriteAll(f, "1", 1);
            }
            if (kind == 1)
            {
                FM_Dir *dd = Directory_Open("/FB/wide");
                FM_DirEntry ee;
                unsigned n = 0;

                if (dd != NULL)
                {
                    while (Directory_Read(dd, &ee) == FM_OK && n < 100) n++;
                    (void)Directory_Close(dd);
                }
printf("  list /FB/wide -> %u entries, open=%p\n", n, (void *)dd);
            }
            r = Directory_DeleteRecursive(kind == 0 ? "/FA/wide" : "/FB/wide");
            {
                FM_Bool ex = FM_TRUE;

                (void)Directory_Exists(kind == 0 ? "/FA/wide" : "/FB/wide", &ex);
                printf("  wide delete (40 dirs): %s, still there=%d\n",
                       FM_ResultString(r), (int)ex);
            }
        }

        /* ---- 3. ScanRecursive whose callback deletes what it sees ---- */
        {
            uint32_t k;

            (void)strcpy(path, kind == 0 ? "/FA/chg" : "/FB/chg");
            (void)Directory_DeleteRecursive(path);
            (void)Directory_Create(path);
            for (k = 0; k < 8; k++)
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/f%02u.bin", path, (unsigned)k);
                (void)File_WriteAll(f, "abc", 3);
            }
            r = Directory_ScanRecursive(path, cb_delete, NULL, NULL, NULL);
            printf("  scan-with-delete: %s\n", FM_ResultString(r));
            show_tree("after (0 expected)", path);
            (void)Directory_DeleteRecursive(path);
        }

        /* ---- 4. a file that appears in a directory being returned to ---- */
        {
            uint32_t k;

            (void)strcpy(path, kind == 0 ? "/FA/grow" : "/FB/grow");
            (void)Directory_DeleteRecursive(path);
            (void)Directory_Create(path);
            for (k = 0; k < 4; k++)
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/d%u", path, (unsigned)k);
                (void)Directory_Create(f);
                (void)snprintf(f, sizeof(f), "%s/d%u/in.bin", path, (unsigned)k);
                (void)File_WriteAll(f, "ab", 2);
            }
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/zz.bin",
                               kind == 0 ? "/FA/grow" : "/FB/grow");
                (void)File_WriteAll(f, "z", 1);
            }
            g_seen = 0;
            r = Directory_ScanRecursive(kind == 0 ? "/FA/grow" : "/FB/grow",
                                        cb_count, NULL, NULL, NULL);
            printf("  tree of 4 dirs + 5 files: %s, seen=%u\n",
                   FM_ResultString(r), (unsigned)g_seen);
            g_seen = 0;
            (void)Directory_DeleteRecursive(kind == 0 ? "/FA/grow" : "/FB/grow");
        }

        /* ---- 5. cancel in the middle ---- */
        {
            volatile FM_Bool cancel = FM_FALSE;
            uint32_t k;

            (void)strcpy(path, kind == 0 ? "/FA/can" : "/FB/can");
            (void)Directory_DeleteRecursive(path);
            (void)Directory_Create(path);
            for (k = 0; k < 6; k++)
            {
                char f[FM_PATH_MAX];

                (void)snprintf(f, sizeof(f), "%s/f%u.bin", path, (unsigned)k);
                (void)File_WriteAll(f, "x", 1);
            }
            cancel = FM_TRUE;
            g_seen = 0;
            r = Directory_ScanRecursive(path, cb_count, NULL, NULL, &cancel);
            printf("  cancel before the first entry: %s, seen=%u\n",
                   FM_ResultString(r), (unsigned)g_seen);
            g_seen = 0;
            cancel = FM_FALSE;
            r = Directory_ScanRecursive(path, cb_count, NULL, NULL, &cancel);
            printf("  no cancel: %s, seen=%u\n", FM_ResultString(r),
                   (unsigned)g_seen);
            g_seen = 0;
            (void)Directory_DeleteRecursive(path);
        }

        unmount_kind(kind);
    }
    return 0;
}
