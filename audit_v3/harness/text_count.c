/* Audit v3: the exact limit of Text_CountLines, and what happens to the
 * end of a file that Text_Replace rewrites without matching anything. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;
static char       g_buf[4096];

int main(void)
{
    FM_Result r;
    const char *p = "/TC/f.txt";
    unsigned len;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "TC", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("TC");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/TC", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    printf("FM_NAME_MAX=%d -> Text_CountLines' line buffer is %d bytes\n",
           (int)FM_NAME_MAX, (int)(FM_NAME_MAX * 2));
    for (len = 120; len <= 132; len += 1)
    {
        size_t n = 0;

        memset(g_buf, 'a', len);
        g_buf[len] = '\n';
        g_buf[len + 1] = 'b';
        g_buf[len + 2] = '\n';
        (void)File_WriteAll(p, g_buf, len + 3);
        r = Text_CountLines(p, &n);
        printf("  two lines, the first %3u chars: %-20s count=%u\n", len,
               FM_ResultString(r), (unsigned)n);
    }

    printf("\nText_Replace with a needle that is not there:\n");
    {
        static const char *const cases[] = {
            "abc",              /* no final newline at all */
            "abc\r",            /* trailing lone CR, classic Mac line end */
            "a\nb",             /* last line unterminated */
            "a\r\nb\r\n"        /* CRLF, normalisation is documented */
        };
        unsigned i;

        for (i = 0; i < 4; i++)
        {
            size_t got = 0;
            size_t before = strlen(cases[i]);

            (void)File_WriteAll(p, cases[i], before);
            r = Text_Replace(p, "NO-SUCH-TEXT", "x");
            memset(g_buf, 0, sizeof(g_buf));
            (void)File_ReadAll(p, g_buf, sizeof(g_buf), &got);
            printf("  %-22s (%u bytes) -> %s, now %u bytes:", cases[i],
                   (unsigned)before, FM_ResultString(r), (unsigned)got);
            {
                size_t k;

                for (k = 0; k < got; k++) printf(" %02X", (unsigned char)g_buf[k]);
            }
            printf("\n");
        }
    }
    (void)File_Delete(p);
    return 0;
}
