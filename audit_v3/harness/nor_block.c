/* Audit v3: the Tier-3 device adapters under a strict NOR model.
 *
 * FM_FlashDevice (page-program, sector-erase, bits only cleared) ->
 * NorBlockDevice (512-byte logical blocks with read-modify-erase-program)
 * -> FMFS on top.  The model refuses a program to a byte that was not
 * erased first, which is the datasheet rule; any such attempt is counted.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_device.h"

#define NOR_SIZE (256u * 1024u)
#define NOR_SECT 4096u
#define NOR_PAGE 256u

static uint8_t  g_flash[NOR_SIZE];
static unsigned g_prog_violations;
static unsigned g_erase_calls;
static unsigned g_prog_calls;
static unsigned g_read_calls;

static FM_Result f_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
    (void)ctx;
    if (addr > NOR_SIZE || len > NOR_SIZE - addr) return FM_ERR_INVALID_ARGUMENT;
    g_read_calls++;
    memcpy(data, g_flash + addr, len);
    return FM_OK;
}

static FM_Result f_prog(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    (void)ctx;
    if (addr > NOR_SIZE || len > NOR_SIZE - addr) return FM_ERR_INVALID_ARGUMENT;
    g_prog_calls++;
    for (i = 0; i < len; i++)
    {
        if ((g_flash[addr + i] & p[i]) != p[i])
        {
            g_prog_violations++;      /* bits set that were already 0 */
            return FM_ERR_IO;
        }
        g_flash[addr + i] &= p[i];
    }
    return FM_OK;
}

static FM_Result f_erase(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    if ((addr % NOR_SECT) != 0u || (len % NOR_SECT) != 0u ||
        addr > NOR_SIZE || len > NOR_SIZE - addr)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    g_erase_calls++;
    memset(g_flash + addr, 0xFF, len);
    return FM_OK;
}

static uint8_t g_scratch[NOR_SECT];

int main(void)
{
    FM_FlashDeviceConfig fc;
    FM_NorBlockDevice nor;
    FM_Storage st;
    FM_FS fs;
    FM_Result r;
    static uint8_t back[8192];
    size_t got = 0;
    unsigned round;

    FileManager_Init();
    memset(g_flash, 0xFF, sizeof(g_flash));

    memset(&fc, 0, sizeof(fc));
    fc.name = "QSPI";
    fc.type = FM_STORAGE_QSPI;
    fc.capacity = NOR_SIZE;
    fc.page_size = NOR_PAGE;
    fc.erase_size = NOR_SECT;
    fc.present = FM_TRUE;
    fc.read = f_read;
    fc.program = f_prog;
    fc.erase = f_erase;

    r = NorBlockDevice_Init(&nor, &fc, 512u, g_scratch, sizeof(g_scratch));
    printf("NorBlockDevice_Init: %s\n", FM_ResultString(r));
    if (r != FM_OK) return 2;
    r = Storage_InitNorBlockDevice(&st, &nor);
    printf("Storage_InitNorBlockDevice: %s\n", FM_ResultString(r));
    if (r != FM_OK) return 2;
    r = Storage_Register(&st);
    if (r == FM_OK) r = Storage_Mount("QSPI");
    printf("Storage_Mount: %s\n", FM_ResultString(r));
    if (r != FM_OK) return 2;

    r = FMFS_Init(&fs);
    if (r == FM_OK) r = FS_Mount("/QSPI", &fs);
    printf("FS_Mount(FMFS on NOR): %s\n", FM_ResultString(r));
    if (r != FM_OK) return 2;
    printf("power-safe layout: %d\n", (int)FMFS_IsPowerSafe(&fs));

    for (round = 0; round < 6; round++)
    {
        char p[64];
        unsigned char data[3000];
        unsigned i;

        for (i = 0; i < sizeof(data); i++)
            data[i] = (unsigned char)(i * (round + 1u) + round);
        snprintf(p, sizeof(p), "/QSPI/f%u.bin", round);
        r = File_WriteAll(p, data, sizeof(data));
        if (r != FM_OK)
        {
            printf("  write %s: %s\n", p, FM_ResultString(r));
            return 1;
        }
        memset(back, 0, sizeof(back));
        got = 0;
        r = File_ReadAll(p, back, sizeof(back), &got);
        if (r != FM_OK || got != sizeof(data) || memcmp(back, data, got) != 0)
        {
            printf("  read back %s: %s, %u bytes\n", p, FM_ResultString(r),
                   (unsigned)got);
            return 1;
        }
    }
    /* rewrite over the same files: this is where read-modify-erase matters */
    for (round = 0; round < 6; round++)
    {
        char p[64];
        unsigned char data[17];

        memset(data, (int)(0x5A + round), sizeof(data));
        snprintf(p, sizeof(p), "/QSPI/f%u.bin", round);
        r = File_WriteAll(p, data, sizeof(data));
        if (r != FM_OK) { printf("  rewrite %s: %s\n", p, FM_ResultString(r)); return 1; }
        memset(back, 0, sizeof(back));
        got = 0;
        r = File_ReadAll(p, back, sizeof(back), &got);
        if (r != FM_OK || got != sizeof(data) || memcmp(back, data, got) != 0)
        {
            printf("  read back after rewrite %s: %s, %u bytes\n", p,
                   FM_ResultString(r), (unsigned)got);
            return 1;
        }
    }
    /* delete then create again: the freed sector is reused and programmed */
    r = File_Delete("/QSPI/f0.bin");
    if (r == FM_OK) r = File_WriteAll("/QSPI/new0.bin", "abc", 3);
    if (r == FM_OK) r = File_WriteAll("/QSPI/new1.bin", "abcdefghij", 10);
    printf("delete + recreate: %s\n", FM_ResultString(r));

    printf("reads %u, programs %u, erases %u, program violations %u\n",
           g_read_calls, g_prog_calls, g_erase_calls, g_prog_violations);

    /* remount: the volume must still be there */
    (void)FS_Unmount("/QSPI");
    (void)Storage_Unmount("QSPI");
    r = Storage_Mount("QSPI");
    if (r == FM_OK) r = FS_Mount("/QSPI", &fs);
    printf("remount: %s\n", FM_ResultString(r));
    if (r == FM_OK) r = File_ReadAll("/QSPI/new1.bin", back, sizeof(back), &got);
    printf("after remount new1.bin: %s, %u bytes '%.*s'\n", FM_ResultString(r),
           (unsigned)got, (int)got, (char *)back);

    printf("\nnor_block: %s\n", g_prog_violations ? "MODEL VIOLATED" : "clean");
    return g_prog_violations ? 1 : 0;
}
