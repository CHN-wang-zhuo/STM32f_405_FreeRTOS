#ifndef UPDATE_STORAGE_LAYOUT_H
#define UPDATE_STORAGE_LAYOUT_H
#include "boot_protocol.h"

/* W25Q16 byte OFFSETS, never STM32 memory-mapped addresses. */
#define UPDATE_EXT_CAPACITY       UINT32_C(0x200000)
#define UPDATE_EXT_SECTOR_SIZE    UINT32_C(0x1000)
#define UPDATE_EXT_REQUEST_OFFSET UINT32_C(0x000000)
#define UPDATE_EXT_IMAGE_OFFSET   UINT32_C(0x010000)
#define UPDATE_EXT_IMAGE_CAPACITY UINT32_C(0x060000)
#define UPDATE_EXT_STORAGE_MAGIC  UINT32_C(0x31505557) /* little-endian WUP1 */
#define UPDATE_EXT_LAYOUT_VERSION UINT32_C(1)

typedef struct {
    BootImageManifest image; /* commit at +24 is programmed LAST */
    uint32_t storage_magic;
    uint32_t layout_version;
    uint32_t image_offset;
    uint32_t image_capacity;
    uint32_t apply_mode; /* 0 = next reset, 1 = request reset after ACK */
    uint32_t reserved;
    uint32_t metadata_crc; /* CRC-32 over bytes 0..23 followed by 28..55 */
    uint32_t reserved_tail;
} UpdateStorageRecord;

_Static_assert(sizeof(UpdateStorageRecord) == 64U, "external metadata wire size");
_Static_assert(offsetof(UpdateStorageRecord, metadata_crc) == 56U, "metadata CRC offset");
_Static_assert(UPDATE_EXT_IMAGE_CAPACITY == BOOT_IMAGE_CAPACITY, "APP capacity agreement");
_Static_assert(UPDATE_EXT_IMAGE_OFFSET + UPDATE_EXT_IMAGE_CAPACITY <= UPDATE_EXT_CAPACITY,
               "external image partition bounds");
#endif
