/*
 * FileManager - USB Mass Storage bridge
 *
 * This module is transport-neutral.  It maps a registered FM_Storage to
 * USB MSC block callbacks.  A TinyUSB, STM32 USB Device, or other USB
 * port adapter calls USB_MSC_ReadBlocks/WriteBlocks from its SCSI/BOT
 * callbacks; the core does not depend on one USB stack or RTOS.
 */

#ifndef FM_USB_MSC_H
#define FM_USB_MSC_H

#include "fm_storage.h"

FM_BEGIN_DECLS

typedef struct FM_USB_MSC FM_USB_MSC;

typedef FM_Result (*FM_USB_MSC_PortStartFn)(void *context,
                                            FM_USB_MSC *msc);
typedef FM_Result (*FM_USB_MSC_PortStopFn)(void *context);

typedef struct
{
    FM_USB_MSC_PortStartFn start;
    FM_USB_MSC_PortStopFn  stop;
} FM_USB_MSC_PortOps;

struct FM_USB_MSC
{
    FM_Storage              *storage;
    uint32_t                 block_size;
    uint64_t                 block_count;
    const FM_USB_MSC_PortOps *port_ops;
    void                    *port_context;
    FM_Bool                  attached;
};

/* usb_block_size may be 0 to use the storage block size, or 512 when the
 * target USB stack expects standard 512-byte MSC sectors. */
FM_Result USB_MSC_Init(FM_USB_MSC *msc, FM_Storage *storage,
                       uint32_t usb_block_size);
FM_Result USB_MSC_GetCapacity(const FM_USB_MSC *msc, uint64_t *block_count,
                              uint32_t *block_size);
FM_Result USB_MSC_ReadBlocks(FM_USB_MSC *msc, uint64_t lba,
                             uint32_t block_count, void *data);
FM_Result USB_MSC_WriteBlocks(FM_USB_MSC *msc, uint64_t lba,
                              uint32_t block_count, const void *data);
FM_Result USB_MSC_Sync(FM_USB_MSC *msc);

/* Connect/disconnect through the selected MCU/USB-stack port adapter. */
FM_Result USB_MSC_Attach(FM_USB_MSC *msc, const FM_USB_MSC_PortOps *ops,
                         void *port_context);
FM_Result USB_MSC_Detach(FM_USB_MSC *msc);
FM_Bool   USB_MSC_IsAttached(const FM_USB_MSC *msc);
FM_Result USB_MSC_SetMediaPresent(FM_USB_MSC *msc, FM_Bool present);

FM_END_DECLS

#endif /* FM_USB_MSC_H */
