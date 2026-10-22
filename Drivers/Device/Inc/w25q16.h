#ifndef W25Q16_H
#define W25Q16_H
#include <stdbool.h>
#include <stdint.h>

#define W25Q16_CAPACITY UINT32_C(0x200000)
#define W25Q16_SECTOR_SIZE UINT32_C(4096)
#define W25Q16_PAGE_SIZE UINT32_C(256)

/* Task-context only. Serialized SPI1 DMA; BUSY polling yields to FreeRTOS. */
bool W25Q16_Init(void);
bool W25Q16_Read(uint32_t address, void *data, uint32_t size);
bool W25Q16_Program(uint32_t address, const void *data, uint32_t size);
bool W25Q16_EraseSector(uint32_t address);
extern volatile uint32_t g_w25q16_jedec_id;
extern volatile uint32_t g_w25q16_errors;
#endif
