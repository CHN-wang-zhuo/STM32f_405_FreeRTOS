#include "fw_update_port.h"
#include "update_storage_layout.h"
#include "update_protocol.h"
#include "w25q16.h"
#include <string.h>

static uint32_t write_limit;

static uint32_t RecordCrc(const UpdateStorageRecord *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = Update_CrcContinue(UINT32_MAX, bytes, 24U);
    return ~Update_CrcContinue(crc, bytes + 28U, 28U);
}

static bool WriteVerified(uint32_t address, const uint8_t *data, uint32_t size)
{
    if (!W25Q16_Program(address, data, size)) { return false; }
    uint8_t verify[64];
    for (uint32_t offset = 0; offset < size; offset += sizeof(verify)) {
        const uint32_t count = size - offset < sizeof(verify) ? size - offset : sizeof(verify);
        if (!W25Q16_Read(address + offset, verify, count) ||
            memcmp(verify, data + offset, count) != 0) { return false; }
    }
    return true;
}

bool FwPort_Init(void) { write_limit = 0; return W25Q16_Init(); }

bool FwPort_Begin(uint32_t image_size)
{
    if (image_size < 8U || image_size > UPDATE_EXT_IMAGE_CAPACITY) { return false; }
    write_limit = 0;
    /* Invalidate any old pending request before erasing its staged data. */
    if (!W25Q16_EraseSector(UPDATE_EXT_REQUEST_OFFSET)) { return false; }
    for (uint32_t offset = 0; offset < image_size; offset += UPDATE_EXT_SECTOR_SIZE) {
        if (!W25Q16_EraseSector(UPDATE_EXT_IMAGE_OFFSET + offset)) { return false; }
    }
    write_limit = image_size;
    return true;
}

bool FwPort_Cancel(void)
{
    write_limit = 0;
    return W25Q16_EraseSector(UPDATE_EXT_REQUEST_OFFSET);
}

bool FwPort_Write(uint32_t offset, const uint8_t *data, uint32_t size)
{
    if ((offset & 3U) != 0U || data == NULL || size == 0U ||
        offset >= write_limit || size > write_limit - offset) { return false; }
    return WriteVerified(UPDATE_EXT_IMAGE_OFFSET + offset, data, size);
}

bool FwPort_Read(uint32_t offset, void *data, uint32_t size)
{
    return offset < UPDATE_EXT_IMAGE_CAPACITY && size <= UPDATE_EXT_IMAGE_CAPACITY - offset &&
           W25Q16_Read(UPDATE_EXT_IMAGE_OFFSET + offset, data, size);
}

bool FwPort_ReadRequest(BootImageManifest *manifest)
{
    UpdateStorageRecord record;
    if (!W25Q16_Read(UPDATE_EXT_REQUEST_OFFSET, &record, sizeof(record))) { return false; }
    memset(manifest, 0xFF, sizeof(*manifest));
    if (record.storage_magic == UPDATE_EXT_STORAGE_MAGIC &&
        record.layout_version == UPDATE_EXT_LAYOUT_VERSION &&
        record.image_offset == UPDATE_EXT_IMAGE_OFFSET &&
        record.image_capacity == UPDATE_EXT_IMAGE_CAPACITY && record.apply_mode <= 1U &&
        record.reserved == UINT32_MAX && record.reserved_tail == UINT32_MAX &&
        record.metadata_crc == RecordCrc(&record)) { *manifest = record.image; }
    return true;
}

bool FwPort_Commit(const BootImageManifest *manifest, uint8_t mode)
{
    if (manifest == NULL || mode > 1U || manifest->image_size < 8U ||
        manifest->image_size > UPDATE_EXT_IMAGE_CAPACITY || manifest->image_size != write_limit ||
        manifest->commit != BOOT_IMAGE_COMMITTED || manifest->consumed != BOOT_ERASED_WORD) { return false; }
    UpdateStorageRecord record;
    if (!W25Q16_Read(UPDATE_EXT_REQUEST_OFFSET, &record, sizeof(record))) { return false; }
    for (uint32_t i = 0; i < sizeof(record); ++i) {
        if (((const uint8_t *)&record)[i] != 0xFF) { return false; }
    }
    memset(&record, 0xFF, sizeof(record));
    record.image = *manifest;
    record.storage_magic = UPDATE_EXT_STORAGE_MAGIC;
    record.layout_version = UPDATE_EXT_LAYOUT_VERSION;
    record.image_offset = UPDATE_EXT_IMAGE_OFFSET;
    record.image_capacity = UPDATE_EXT_IMAGE_CAPACITY;
    record.apply_mode = mode;
    record.metadata_crc = RecordCrc(&record);
    const uint8_t *bytes = (const uint8_t *)&record;
    if (!WriteVerified(UPDATE_EXT_REQUEST_OFFSET, bytes, 24U) ||
        !WriteVerified(UPDATE_EXT_REQUEST_OFFSET + 28U, bytes + 28U, sizeof(record) - 28U)) { return false; }
    /* No STM32 internal Flash is erased or programmed by this storage backend. */
    return WriteVerified(UPDATE_EXT_REQUEST_OFFSET + 24U, bytes + 24U, 4U);
}
