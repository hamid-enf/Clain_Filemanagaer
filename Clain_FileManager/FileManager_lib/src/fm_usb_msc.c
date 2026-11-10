/*
 * FileManager - USB MSC block bridge
 */

#define FM_DEBUG_TAG "usb"
#include "fm_debug.h"
#include "fm_usb_msc.h"
#include <string.h>

#if FM_USE_USB_MSC

static FM_Result usb_msc_range(const FM_USB_MSC *msc, uint64_t lba,
                               uint32_t block_count, uint64_t *offset,
                               uint32_t *length)
{
    uint64_t count = block_count;

    if (block_count == 0 || msc->block_size == 0 ||
        lba > msc->block_count || count > msc->block_count - lba)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (count > UINT64_MAX / msc->block_size ||
        lba > UINT64_MAX / msc->block_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (count * msc->block_size > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    *offset = lba * msc->block_size;
    *length = (uint32_t)(count * msc->block_size);
    return FM_OK;
}

FM_Result USB_MSC_Init(FM_USB_MSC *msc, FM_Storage *storage,
                       uint32_t usb_block_size)
{
    if (msc == NULL || storage == NULL || storage->capacity == 0 ||
        storage->read == NULL || storage->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (usb_block_size == 0)
    {
        usb_block_size = (storage->block_size != 0) ? storage->block_size : 512u;
    }
    if (usb_block_size == 0 || storage->capacity < usb_block_size)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(msc, 0, sizeof(*msc));
    msc->storage = storage;
    msc->block_size = usb_block_size;
    msc->block_count = storage->capacity / usb_block_size;
    return (msc->block_count == 0) ? FM_ERR_INVALID_ARGUMENT : FM_OK;
}

FM_Result USB_MSC_GetCapacity(const FM_USB_MSC *msc, uint64_t *block_count,
                              uint32_t *block_size)
{
    if (msc == NULL || msc->storage == NULL || block_count == NULL ||
        block_size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *block_count = msc->block_count;
    *block_size = msc->block_size;
    return FM_OK;
}

FM_Result USB_MSC_ReadBlocks(FM_USB_MSC *msc, uint64_t lba,
                             uint32_t block_count, void *data)
{
    uint64_t offset;
    uint32_t length;
    FM_Result r;

    if (msc == NULL || msc->storage == NULL || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!msc->storage->present)
    {
        return FM_ERR_NOT_READY;
    }
    r = usb_msc_range(msc, lba, block_count, &offset, &length);
    if (r != FM_OK)
    {
        return r;
    }
    return msc->storage->read(msc->storage, offset, data, length);
}

FM_Result USB_MSC_WriteBlocks(FM_USB_MSC *msc, uint64_t lba,
                              uint32_t block_count, const void *data)
{
    uint64_t offset;
    uint32_t length;
    FM_Result r;

    if (msc == NULL || msc->storage == NULL || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (msc->storage->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    if (!msc->storage->present)
    {
        return FM_ERR_NOT_READY;
    }
    r = usb_msc_range(msc, lba, block_count, &offset, &length);
    if (r != FM_OK)
    {
        return r;
    }
    return msc->storage->write(msc->storage, offset, data, length);
}

FM_Result USB_MSC_Sync(FM_USB_MSC *msc)
{
    if (msc == NULL || msc->storage == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return (msc->storage->sync == NULL) ? FM_OK :
           msc->storage->sync(msc->storage);
}

FM_Result USB_MSC_Attach(FM_USB_MSC *msc, const FM_USB_MSC_PortOps *ops,
                         void *port_context)
{
    FM_Result r;

    if (msc == NULL || msc->storage == NULL || ops == NULL ||
        ops->start == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (msc->attached)
    {
        return FM_ERR_BUSY;
    }
    r = ops->start(port_context, msc);
    if (r != FM_OK)
    {
        return r;
    }
    msc->port_ops = ops;
    msc->port_context = port_context;
    msc->attached = FM_TRUE;
    return FM_OK;
}

FM_Result USB_MSC_Detach(FM_USB_MSC *msc)
{
    FM_Result r = FM_OK;

    if (msc == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!msc->attached)
    {
        return FM_OK;
    }
    if (msc->port_ops != NULL && msc->port_ops->stop != NULL)
    {
        r = msc->port_ops->stop(msc->port_context);
    }
    msc->port_ops = NULL;
    msc->port_context = NULL;
    msc->attached = FM_FALSE;
    return r;
}

FM_Bool USB_MSC_IsAttached(const FM_USB_MSC *msc)
{
    return (msc != NULL && msc->attached) ? FM_TRUE : FM_FALSE;
}

FM_Result USB_MSC_SetMediaPresent(FM_USB_MSC *msc, FM_Bool present)
{
    if (msc == NULL || msc->storage == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Storage_SetPresent(msc->storage, present);
}

#endif /* FM_USE_USB_MSC */
