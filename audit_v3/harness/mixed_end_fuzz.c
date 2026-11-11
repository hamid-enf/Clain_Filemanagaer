/* Audit v3 FILE3-06: a randomized check that the three rewrite helpers
 * are transparent.  For each case a random file (random lines, random
 * endings: LF, CRLF, no ending on the last line, lone CRs inside lines,
 * empty lines) is edited with one helper, and the result is compared with
 * a reference that does the same edit on the in-memory text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_text.h"

static uint8_t g_mem[1024 * 1024];
static FM_Storage g_st;
static FM_FS g_fs;
static char g_file[65536];
static char g_want[65536];
static char g_got[65536];
static unsigned g_seed = 1;

static unsigned rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 16) & 0x7FFF; }

/* build a random file; returns its length and the line boundaries */
static size_t build(char *out, size_t *line_start, size_t *line_len,
                    int *line_end, int *nlines)
{
    size_t n = 0;
    int lines = 1 + (int)(rnd() % 6);
    int i;

    for (i = 0; i < lines; i++)
    {
        size_t len = rnd() % 12;
        size_t j;

        line_start[i] = n;
        for (j = 0; j < len; j++)
        {
            /* letters, and an occasional lone CR inside the line */
            /* a lone CR is only unambiguous away from the line end */
            out[n++] = (j + 1 < len && rnd() % 20 == 0) ? '\r'
                                                        : (char)('a' + rnd() % 4);
        }
        line_len[i] = n - line_start[i];
        /* ending: 0 = LF, 1 = CRLF, 2 = none (only legal on the last) */
        line_end[i] = (i == lines - 1) ? (int)(rnd() % 3) : (int)(rnd() % 2);
        if (i == lines - 1 && line_end[i] == 2 && line_len[i] == 0)
        {
            lines--;      /* an empty last line with no ending is not a line */
            break;
        }
        if (line_end[i] == 0) out[n++] = '\n';
        else if (line_end[i] == 1) { out[n++] = '\r'; out[n++] = '\n'; }
    }
    *nlines = lines;
    return n;
}

/* copy the same text, but with the line `skip` left out */
static size_t ref_delete(const char *t, size_t tn, size_t *ls, size_t *ll,
                         int *le, int nlines, int skip, char *out)
{
    size_t n = 0;
    int i;

    for (i = 0; i < nlines; i++)
    {
        if (i == skip) continue;
        memcpy(out + n, t + ls[i], ll[i]);
        n += ll[i];
        if (le[i] == 0) out[n++] = '\n';
        else if (le[i] == 1) { out[n++] = '\r'; out[n++] = '\n'; }
    }
    return n;
}

int main(void)
{
    const char *p = "/ME/f.txt";
    int cases = 400;
    int c;
    int bad = 0;
    FM_Result r;

    FileManager_Init();
    (void)RamDisk_Init(&g_st, "ME", g_mem, sizeof(g_mem), 512);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("ME");
    (void)FMFS_Init(&g_fs);
    r = FS_Mount("/ME", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    for (c = 0; c < cases; c++)
    {
        size_t ls[8], ll[8];
        int le[8], nlines = 0;
        size_t tn = build(g_file, ls, ll, le, &nlines);
        size_t wn = 0, gn = 0;
        int which = (int)(rnd() % 3);

        (void)File_WriteAll(p, g_file, tn);

        if (which == 0)          /* delete a line */
        {
            int skip = (int)(rnd() % nlines);

            wn = ref_delete(g_file, tn, ls, ll, le, nlines, skip, g_want);
            r = Text_DeleteLine(p, (size_t)skip + 1);
        }
        else if (which == 1)     /* replace a word that is not there */
        {
            memcpy(g_want, g_file, tn);
            wn = tn;
            r = Text_Replace(p, "ZZZ", "q");
        }
        else                     /* insert a line in front of line 1 */
        {
            /* the inserted line takes the ending of the line it precedes */
            wn = 0;
            memcpy(g_want + wn, "NEW", 3); wn += 3;
            if (le[0] == 1) { g_want[wn++] = '\r'; }
            g_want[wn++] = '\n';
            memcpy(g_want + wn, g_file, tn); wn += tn;
            r = Text_InsertLine(p, 1, "NEW");
        }

        (void)File_ReadAll(p, g_got, sizeof(g_got) - 1, &gn);
        if (r != FM_OK || gn != wn || memcmp(g_want, g_got, wn) != 0)
        {
            size_t i;

            bad++;
            if (bad <= 3)
            {
                printf("case %d (which %d) -> %s: got %u bytes, want %u\n", c,
                       which, FM_ResultString(r), (unsigned)gn, (unsigned)wn);
                printf("  file:"); for (i = 0; i < tn && i < 40; i++) printf(" %02X", (unsigned char)g_file[i]);
                printf("\n  got :"); for (i = 0; i < gn && i < 40; i++) printf(" %02X", (unsigned char)g_got[i]);
                printf("\n  want:"); for (i = 0; i < wn && i < 40; i++) printf(" %02X", (unsigned char)g_want[i]);
                printf("\n");
            }
        }
        (void)File_Delete(p);
    }
    printf("mixed_end_fuzz: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
