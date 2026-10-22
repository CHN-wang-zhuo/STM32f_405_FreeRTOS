#include "fw_update_port.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static bool Open(void)
{
    vTaskSuspendAll();
    if (HAL_FLASH_Unlock() != HAL_OK) { (void)xTaskResumeAll(); return false; }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    return true;
}

static bool Close(bool result)
{
    (void)HAL_FLASH_Lock();
    (void)xTaskResumeAll();
    return result;
}

static bool Erase(uint32_t sector, uint32_t count)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = UINT32_MAX;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = sector;
    erase.NbSectors = count;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    return HAL_FLASHEx_Erase(&erase, &error) == HAL_OK && error == UINT32_MAX;
}

static bool Word(uint32_t address, uint32_t value)
{
    /* This APP can only publish requests or write staging, never running code. */
    const bool allowed = (address >= BOOT_STAGE_ADDRESS &&
                          address <= BOOT_STAGE_ADDRESS + BOOT_IMAGE_CAPACITY - 4U) ||
                         (address >= BOOT_REQUEST_ADDRESS &&
                          address <= BOOT_REQUEST_ADDRESS + offsetof(BootImageManifest, commit));
    if (!allowed || (address & 3U) != 0U ||
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, value) != HAL_OK) { return false; }
    FLASH_FlushCaches();
    return *(const volatile uint32_t *)(uintptr_t)address == value;
}

bool FwPort_Begin(void)
{
    if (!Open()) { return false; }
    const bool ok = Erase(FLASH_SECTOR_3, 1U) && Erase(FLASH_SECTOR_8, 3U);
    return Close(ok);
}

bool FwPort_Cancel(void)
{
    if (!Open()) { return false; }
    return Close(Erase(FLASH_SECTOR_3, 1U));
}

bool FwPort_Write(uint32_t offset, const uint8_t *data, uint32_t size)
{
    if ((offset & 3U) != 0U || size == 0U || offset >= BOOT_IMAGE_CAPACITY ||
        size > BOOT_IMAGE_CAPACITY - offset || !Open()) { return false; }
    bool ok = true;
    for (uint32_t i = 0; i < size && ok; i += 4U) {
        uint32_t word = UINT32_MAX;
        const uint32_t count = size - i < 4U ? size - i : 4U;
        memcpy(&word, data + i, count);
        ok = Word(BOOT_STAGE_ADDRESS + offset + i, word);
    }
    return Close(ok);
}

const uint8_t *FwPort_Stage(uint32_t offset, uint32_t size)
{
    if (offset > BOOT_IMAGE_CAPACITY || size > BOOT_IMAGE_CAPACITY - offset) { return NULL; }
    return (const uint8_t *)(uintptr_t)(BOOT_STAGE_ADDRESS + offset);
}

void FwPort_ReadRequest(BootImageManifest *manifest)
{
    memcpy(manifest, (const void *)(uintptr_t)BOOT_REQUEST_ADDRESS, sizeof(*manifest));
}

void FwPort_ReadInstalled(BootImageManifest *manifest)
{
    memcpy(manifest, (const void *)(uintptr_t)BOOT_INSTALLED_ADDRESS, sizeof(*manifest));
}

bool FwPort_Commit(const BootImageManifest *manifest)
{
    /* A partial previous commit must not be mistaken for a blank request. */
    for (uint32_t i = 0; i < sizeof(*manifest); i += 4U) {
        if (*(const volatile uint32_t *)(uintptr_t)(BOOT_REQUEST_ADDRESS + i) != UINT32_MAX) {
            return false;
        }
    }
    if (!Open()) { return false; }
    bool ok = true;
    for (uint32_t i = 0; i < offsetof(BootImageManifest, commit) && ok; i += 4U) {
        uint32_t word;
        memcpy(&word, (const uint8_t *)manifest + i, sizeof(word));
        ok = Word(BOOT_REQUEST_ADDRESS + i, word);
    }
    if (ok) { ok = Word(BOOT_REQUEST_ADDRESS + offsetof(BootImageManifest, commit), BOOT_IMAGE_COMMITTED); }
    /* consumed stays erased; Bootloader owns it after the next reset. */
    return Close(ok);
}
