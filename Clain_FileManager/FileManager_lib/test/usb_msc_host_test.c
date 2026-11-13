/*
 * usb_msc_host_test.c -- the USB mass-storage glue (examples/usb_msc.h)
 * on a PC.
 *
 * FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 *
 * The glue sits between the Cube USB stack and an FM_Storage and moves
 * every request through its own 32-byte-aligned bounce buffer
 * (USB_MSC_BOUNCE_SECTORS blocks, 4 KB).  The read side cut a request
 * into pieces of that size; the write side did not and copied
 * block_count * 512 bytes into the 4 KB buffer (report V5, CRIT-01).
 * With the Cube default MSC_MEDIA_PACKET = 512 the stack asks for one
 * block per call, so the board never showed it; a project that raises
 * the packet size (the usual speed-up) overwrote what lies behind the
 * buffer on every write.
 *
 * What is checked, with a RAM disk as the medium:
 *   - a write and a read of 1, 8, 9, 64 and 200 blocks reach the driver
 *     in pieces of at most USB_MSC_BOUNCE_SECTORS blocks;
 *   - the blocks arrive at the right place with the right content;
 *   - nothing happens before UsbMsc_Begin and after UsbMsc_End.
 * The makefile builds this file with AddressSanitizer, which reports
 * the overflow of the bounce buffer itself when the limit is missing.
 *
 *     make usbmsctest
 */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_manager.h"
#include "fm_storage.h"
#include "stm32h7xx_hal.h"

#define USB_MSC_DEFINE_CONTEXT
#include "usb_msc.h"

static int fails;
static int checks;

static void ok(const char *what, int cond)
{
    checks++;
    if (!cond)
    {
        printf("  %-62s FAIL\n", what);
        fails++;
    }
}

#define DISK_BLOCKS 512u
static uint8_t disk[DISK_BLOCKS * 512u];
static FM_Storage ram;
static FM_Storage_WriteFn real_write;
static FM_Storage_ReadFn real_read;
static uint32_t max_write;
static uint32_t max_read;
static uint32_t n_write;
static uint32_t n_read;

static FM_Result spy_write(FM_Storage *s, uint64_t offset, const void *buf,
                           uint32_t len)
{
    if (len > max_write) max_write = len;
    n_write++;
    return real_write(s, offset, buf, len);
}

static FM_Result spy_read(FM_Storage *s, uint64_t offset, void *buf,
                          uint32_t len)
{
    if (len > max_read) max_read = len;
    n_read++;
    return real_read(s, offset, buf, len);
}

static FM_Result owner_ok(void *ctx)
{
    (void)ctx;
    return FM_OK;
}

/* what block `lba` holds in pass `salt` */
static uint8_t pat(uint32_t lba, uint32_t i, uint32_t salt)
{
    return (uint8_t)(lba * 31u + i * 7u + salt * 101u + (i >> 8));
}

int main(void)
{
    static const uint16_t counts[] = { 1u, 8u, 9u, 64u, 200u };
    static uint8_t usb[200u * 512u];
    char line[96];
    uint32_t c;

    printf("usb_msc.h host test\n");
    printf("===================\n");
    ok("the library starts", FileManager_Init() == FM_OK);
    ok("the RAM disk is there",
       RamDisk_Init(&ram, "USBT", disk, sizeof(disk), 512u) == FM_OK);
    real_write = ram.write;
    real_read = ram.read;
    ram.write = spy_write;
    ram.read = spy_read;

    ok("before Begin a write is refused",
       UsbMsc_StorageWrite(0u, usb, 0u, 1u) == USB_MSC_FAIL);
    ok("before Begin a read is refused",
       UsbMsc_StorageRead(0u, usb, 0u, 1u) == USB_MSC_FAIL);
    ok("Begin", UsbMsc_Begin(&ram, owner_ok, owner_ok, NULL) == FM_OK);

    for (c = 0; c < (uint32_t)(sizeof(counts) / sizeof(counts[0])); c++)
    {
        uint16_t n = counts[c];
        uint32_t lba = 3u + c * 5u;
        uint32_t b;
        uint32_t i;
        uint32_t want_calls =
            ((uint32_t)n + USB_MSC_BOUNCE_SECTORS - 1u) / USB_MSC_BOUNCE_SECTORS;
        int same = 1;
        int around = 1;

        memset(disk, 0xEE, sizeof(disk));
        for (b = 0; b < n; b++)
        {
            for (i = 0; i < 512u; i++)
            {
                usb[b * 512u + i] = pat(lba + b, i, c);
            }
        }
        max_write = 0u;
        n_write = 0u;
        (void)snprintf(line, sizeof(line), "write of %u blocks is accepted",
                       (unsigned)n);
        ok(line, UsbMsc_StorageWrite(0u, usb, lba, n) == USB_MSC_OK);
        (void)snprintf(line, sizeof(line),
                       "write of %u blocks: no piece above %u blocks",
                       (unsigned)n, (unsigned)USB_MSC_BOUNCE_SECTORS);
        ok(line, max_write <= (uint32_t)USB_MSC_BOUNCE_SECTORS * 512u &&
                 max_write > 0u);
        (void)snprintf(line, sizeof(line), "write of %u blocks: %u pieces",
                       (unsigned)n, (unsigned)want_calls);
        ok(line, n_write == want_calls);
        for (b = 0; b < n; b++)
        {
            for (i = 0; i < 512u; i++)
            {
                if (disk[(lba + b) * 512u + i] != pat(lba + b, i, c)) same = 0;
            }
        }
        (void)snprintf(line, sizeof(line),
                       "write of %u blocks: every byte is on the medium",
                       (unsigned)n);
        ok(line, same);
        for (i = 0; i < 512u; i++)
        {
            if (disk[(lba - 1u) * 512u + i] != 0xEEu) around = 0;
            if (disk[(lba + n) * 512u + i] != 0xEEu) around = 0;
        }
        (void)snprintf(line, sizeof(line),
                       "write of %u blocks: the blocks around are untouched",
                       (unsigned)n);
        ok(line, around);

        memset(usb, 0, sizeof(usb));
        max_read = 0u;
        n_read = 0u;
        (void)snprintf(line, sizeof(line), "read of %u blocks is accepted",
                       (unsigned)n);
        ok(line, UsbMsc_StorageRead(0u, usb, lba, n) == USB_MSC_OK);
        (void)snprintf(line, sizeof(line),
                       "read of %u blocks: no piece above %u blocks",
                       (unsigned)n, (unsigned)USB_MSC_BOUNCE_SECTORS);
        ok(line, max_read <= (uint32_t)USB_MSC_BOUNCE_SECTORS * 512u &&
                 n_read == want_calls);
        same = 1;
        for (b = 0; b < n; b++)
        {
            for (i = 0; i < 512u; i++)
            {
                if (usb[b * 512u + i] != pat(lba + b, i, c)) same = 0;
            }
        }
        (void)snprintf(line, sizeof(line),
                       "read of %u blocks gives back what was written",
                       (unsigned)n);
        ok(line, same);
    }

    ok("a write past the end of the medium fails",
       UsbMsc_StorageWrite(0u, usb, DISK_BLOCKS - 4u, 16u) == USB_MSC_FAIL);
    ok("End", UsbMsc_End() == FM_OK);
    ok("after End a write is refused",
       UsbMsc_StorageWrite(0u, usb, 0u, 1u) == USB_MSC_FAIL);

    printf("\nResults: %d checks, %s\n", checks,
           fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_usbmsc_t;
#endif /* USE_HAL_DRIVER */
