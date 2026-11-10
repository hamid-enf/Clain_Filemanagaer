/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_crc.h - Optional integrity primitives (CRC16 / CRC32 / checksums)
 *
 * All functions are table-less (bit-wise) implementations so no static
 * tables are linked.  They are deterministic and small.
 */

#ifndef FM_CRC_H
#define FM_CRC_H

#include "fm_types.h"

FM_BEGIN_DECLS

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */
uint16_t CRC_Compute16(const void *data, size_t len, uint16_t crc);

/* CRC-16/MODBUS (poly 0x8005, init 0xFFFF, reflected) */
uint16_t CRC_Compute16Modbus(const void *data, size_t len, uint16_t crc);

/* CRC-32 (IEEE 802.3, zlib-compatible: reflected poly 0xEDB88320,
 * init 0xFFFFFFFF, final XOR 0xFFFFFFFF) */
uint32_t CRC_Compute32(const void *data, size_t len, uint32_t crc);

/* Simple 8-bit XOR checksum */
uint8_t  CRC_Checksum8(const void *data, size_t len);

/* Simple 32-bit additive checksum */
uint32_t CRC_Checksum32(const void *data, size_t len);

/* Convenience wrappers (no chaining) */
static inline uint16_t CRC_Calculate16(const void *d, size_t n)
{ return CRC_Compute16(d, n, 0xFFFFu); }

static inline uint32_t CRC_Calculate32(const void *d, size_t n)
{ return CRC_Compute32(d, n, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; }

FM_END_DECLS

#endif /* FM_CRC_H */
