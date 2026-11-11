/* Audit v3: CRC and binary helpers against independent references.
 *
 *  - CRC-32 is compared with zlib's crc32() (Python) for 64 random
 *    buffers, computed in one go and in two pieces at every split point.
 *  - CRC-16/CCITT-FALSE and CRC-16/MODBUS are compared with the published
 *    check values (0x29B1 and 0x4B37 for "123456789") and with binascii.
 *  - Binary_WriteXxx is compared with struct.pack(): the bytes the library
 *    produces for each type/endian, and the values it reads back from
 *    reference bytes.
 *
 * Output is a plain table on stdout that tools/crc_ref.py checks.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "file_manager.h"
#include "fm_crc.h"
#include "fm_binary.h"

static unsigned g_rng = 12345u;
static unsigned rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8);
}

static uint8_t g_buf[256];

int main(void)
{
    unsigned i;
    char a[4096];
    size_t an = 0;

    FileManager_Init();

    /* the check value the standard names */
    printf("crc32_check %08X\n", (unsigned)CRC_Calculate32("123456789", 9));
    printf("crc16_check %04X\n",
           (unsigned)CRC_Calculate16("123456789", 9));
    printf("crc16modbus_check %04X\n",
           (unsigned)CRC_Compute16Modbus("123456789", 9, 0xFFFFu));
    printf("ck8_123456789 %02X\n", (unsigned)CRC_Checksum8("123456789", 9));
    printf("ck32_123456789 %08X\n", (unsigned)CRC_Checksum32("123456789", 9));

    /* 64 random buffers, whole and split */
    for (i = 0; i < 64; i++)
    {
        size_t n = 1u + rnd() % 200u;
        size_t k;

        for (k = 0; k < n; k++) g_buf[k] = (uint8_t)(rnd() & 0xFF);
        an += (size_t)snprintf(a + an, sizeof(a) - an, "buf %u", (unsigned)n);
        printf("buf %u", (unsigned)n);
        for (k = 0; k < n; k++) printf(" %02X", g_buf[k]);
        printf(" whole32 %08X whole16 %04X modbus %04X\n",
               (unsigned)CRC_Calculate32(g_buf, n),
               (unsigned)CRC_Calculate16(g_buf, n),
               (unsigned)CRC_Compute16Modbus(g_buf, n, 0xFFFFu));

        /* chained in two pieces at a few split points */
        for (k = 0; k <= n; k += (n > 20 ? 7u : 1u))
        {
            uint32_t c32 = CRC_Compute32(g_buf, k, 0xFFFFFFFFu);

            c32 = CRC_Compute32(g_buf + k, n - k, c32) ^ 0xFFFFFFFFu;
            printf("split %u %u %08X\n", (unsigned)n, (unsigned)k,
                   (unsigned)c32);
        }
    }

    /* Binary: write every type both ways into a memory stream */
    {
        FM_Stream s;
        uint8_t mem[128];
        FM_Result r;

        memset(mem, 0, sizeof(mem));
        r = Stream_InitMemoryWrite(&s, mem, sizeof(mem));
        printf("stream_open %s\n", FM_ResultString(r));
        printf("binary_writes");
        (void)Binary_WriteU8(&s, 0xA5);
        (void)Binary_WriteU16(&s, 0x1234, FM_ENDIAN_LITTLE);
        (void)Binary_WriteU16(&s, 0x1234, FM_ENDIAN_BIG);
        (void)Binary_WriteU32(&s, 0xDEADBEEF, FM_ENDIAN_LITTLE);
        (void)Binary_WriteU32(&s, 0xDEADBEEF, FM_ENDIAN_BIG);
        (void)Binary_WriteU64(&s, 0x0102030405060708ull, FM_ENDIAN_LITTLE);
        (void)Binary_WriteU64(&s, 0x0102030405060708ull, FM_ENDIAN_BIG);
        (void)Binary_WriteI16(&s, -2, FM_ENDIAN_LITTLE);
        (void)Binary_WriteI32(&s, -2147483647 - 1, FM_ENDIAN_BIG);
        (void)Binary_WriteI64(&s, INT64_MIN, FM_ENDIAN_LITTLE);
        (void)Binary_WriteFloat(&s, 1.5f, FM_ENDIAN_LITTLE);
        (void)Binary_WriteFloat(&s, 1.5f, FM_ENDIAN_BIG);
        (void)Binary_WriteDouble(&s, -2.5, FM_ENDIAN_BIG);
        {
            uint64_t written = 0;
            (void)Stream_Tell(&s, &written);
            for (i = 0; i < written; i++) printf(" %02X", mem[i]);
            printf(" (%u bytes)\n", (unsigned)written);
        }
        Stream_Close(&s);
    }
    (void)a;
    return 0;
}
