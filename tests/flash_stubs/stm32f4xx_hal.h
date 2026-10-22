#ifndef TEST_STM32F4XX_HAL_H
#define TEST_STM32F4XX_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct {
    uint32_t TypeErase, Banks, Sector, NbSectors, VoltageRange;
} FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_SECTORS 0U
#define FLASH_SECTOR_3 3U
#define FLASH_SECTOR_8 8U
#define FLASH_VOLTAGE_RANGE_3 2U
#define FLASH_TYPEPROGRAM_WORD 2U
#define FLASH_FLAG_EOP 1U
#define FLASH_FLAG_OPERR 2U
#define FLASH_FLAG_WRPERR 16U
#define FLASH_FLAG_PGAERR 32U
#define FLASH_FLAG_PGPERR 64U
#define FLASH_FLAG_PGSERR 128U
#define __HAL_FLASH_CLEAR_FLAG(flags) ((void)(flags))
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t value);
void FLASH_FlushCaches(void);
#endif
