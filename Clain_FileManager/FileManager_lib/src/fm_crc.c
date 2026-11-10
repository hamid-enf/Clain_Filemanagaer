/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_crc.c - CRC16 / CRC32 / checksum implementations (table-less)
 */

#include "fm_crc.h"

#if FM_USE_CRC

uint16_t CRC_Compute16(const void *data, size_t len, uint16_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < len; i++)
    {
        uint8_t j;
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x8000u)
            {
                crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021u);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t CRC_Compute16Modbus(const void *data, size_t len, uint16_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < len; i++)
    {
        uint8_t j;
        crc ^= p[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x0001u)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

uint32_t CRC_Compute32(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    /* Standard IEEE 802.3 (zlib-compatible) CRC-32: reflected polynomial
     * 0xEDB88320, LSB-first.  The non-chaining wrapper CRC_Calculate32
     * applies the standard init (0xFFFFFFFF) and final XOR, so its check
     * value for "123456789" is the canonical 0xCBF43926. */
    for (i = 0; i < len; i++)
    {
        uint8_t j;

        crc ^= p[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 1u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint8_t CRC_Checksum8(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < len; i++)
    {
        sum ^= p[i];
    }
    return sum;
}

uint32_t CRC_Checksum32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < len; i++)
    {
        sum += p[i];
    }
    return sum;
}

#endif /* FM_USE_CRC */
