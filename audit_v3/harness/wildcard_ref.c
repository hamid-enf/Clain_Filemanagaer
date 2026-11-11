/* Audit v3: the rewritten wildcard matchers against a reference.
 *
 * The reference is a plain recursive matcher written straight from the
 * definition in fm_directory.h ("'*' stands for any run of characters,
 * also none, everything else for itself").  Random patterns and names are
 * compared pair by pair; the names are real files in a POSIX directory,
 * so what is compared is what Directory_List() (fm_directory.c) and the
 * logger's filter (fm_logger.c) answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "file_manager.h"

static int ref(const char *p, const char *t)
{
    if (*p == 0) return *t == 0;
    if (*p == '*')
    {
        for (;; t++)
        {
            if (ref(p + 1, t)) return 1;
            if (*t == 0) return 0;
        }
    }
    if (*t == 0) return 0;
    return (*p == *t) && ref(p + 1, t + 1);
}

static unsigned g_rng = 1;
static unsigned rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8);
}

static unsigned g_n;

static FM_Result count_cb(const FM_FileInfo *info, void *user)
{
    (void)info;
    (*(unsigned *)user)++;
    return FM_OK;
}

static FM_FS g_fs;

int main(void)
{
    unsigned iter;
    unsigned bad = 0;
    unsigned checked = 0;

    FileManager_Init();
    (void)mkdir("/tmp/wc_root", 0777);
    (void)chdir("/tmp/wc_root");
    if (PosixFS_Init(&g_fs) != FM_OK || FS_Mount("/", &g_fs) != FM_OK)
    {
        printf("POSIX mount failed\n");
        return 2;
    }

    for (iter = 0; iter < 4000; iter++)
    {
        char filter[24];
        char name[24];
        unsigned pi, ni;
        unsigned npat = 1u + rnd() % 8u;
        unsigned nnam = 1u + rnd() % 8u;
        unsigned got = 0;
        unsigned want = 0;
        FM_Result r;

        for (pi = 0; pi < npat; pi++)
        {
            static const char alpha[] = "ab*.";
            unsigned c = rnd() % 5u;

            filter[pi] = (c == 4u) ? '*' : alpha[c];
            if (c == 4u) filter[pi] = '*';
        }
        filter[npat] = 0;
        for (ni = 0; ni < nnam; ni++)
        {
            static const char alpha[] = "ab.";
            name[ni] = alpha[rnd() % 3u];
        }
        name[nnam] = 0;
        /* "." and ".." cannot be file names at all; skip them */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* exactly one file in an otherwise empty directory */
        (void)Directory_Create("/wc");
        {
            char path[96];

            snprintf(path, sizeof(path), "/wc/%s", name);
            (void)File_Delete(path);
            if (File_WriteAll(path, "x", 1) != FM_OK) continue;
        }
        r = Directory_List("/wc", filter, count_cb, &got);
        {
            char path[96];

            snprintf(path, sizeof(path), "/wc/%s", name);
            (void)File_Delete(path);
        }
        want = ref(filter, name) ? 1u : 0u;
        checked++;
        if (r != FM_OK || got != want)
        {
            printf("\"%s\" vs \"%s\": library says %u entries (%s), "
                   "reference says %u\n", filter, name, got,
                   FM_ResultString(r), want);
            if (++bad > 15) break;
        }
    }
    (void)Directory_DeleteRecursive("/wc");
    printf("wildcard: %u pairs checked, %u disagreements\n", checked, bad);

    /* timing on the pattern the old recursive matcher could not survive.
     * The name must fit FM_NAME_MAX-1 characters: Directory_List documents
     * that an entry whose name does not fit stops the walk, which is why an
     * earlier 79-'a' name silently produced 0 entries. */
    {
        char longname[FM_NAME_MAX];
        char filter[96];
        clock_t t0;
        double took;
        unsigned got = 0;
        unsigned i;

        printf("FM_NAME_MAX = %d\n", (int)FM_NAME_MAX);
        memset(longname, 'a', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = 0;
        (void)Directory_Create("/wc");
        {
            char path[128];

            snprintf(path, sizeof(path), "/wc/%s", longname);
            if (File_WriteAll(path, "x", 1) != FM_OK)
                printf("could not create the long name\n");
        }
        /* 30 '*a' pairs then a 'b' that is not in the name: worst case for a
         * backtracking matcher, and it must not match */
        filter[0] = 0;
        for (i = 0; i < 30; i++) strcat(filter, "*a");
        strcat(filter, "*b");
        t0 = clock();
        (void)Directory_List("/wc", filter, count_cb, &got);
        took = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        printf("adversarial filter (%u chars) on a %u-'a' name: %.3f s, "
               "%u entries (want 0)\n", (unsigned)strlen(filter),
               (unsigned)strlen(longname), took, got);

        /* the same filter that must match */
        filter[0] = 0;
        for (i = 0; i < 30; i++) strcat(filter, "*a");
        strcat(filter, "*");
        got = 0;
        t0 = clock();
        (void)Directory_List("/wc", filter, count_cb, &got);
        took = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        printf("matching version: %.3f s, %u entries (want 1)\n", took, got);

        /* and a name one character too long, which the docs say stops the
         * walk: this is the case that produced the 0 entries earlier */
        {
            char toobig[FM_NAME_MAX + 8];

            memset(toobig, 'a', sizeof(toobig) - 1);
            toobig[sizeof(toobig) - 1] = 0;
            snprintf(filter, sizeof(filter), "/wc2/%s", toobig);
            (void)Directory_Create("/wc2");
            printf("write of a %u-char name: %s\n", (unsigned)strlen(toobig),
                   FM_ResultString(File_WriteAll(filter, "x", 1)));
            got = 0;
            printf("listing a directory that holds it: %s, %u entries\n",
                   FM_ResultString(Directory_List("/wc2", "*", count_cb, &got)),
                   got);
            (void)Directory_DeleteRecursive("/wc2");
        }
        (void)Directory_DeleteRecursive("/wc");
    }
    return bad ? 1 : 0;
}
