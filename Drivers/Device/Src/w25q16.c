#include "w25q16.h"
#include "spi.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <string.h>

static SemaphoreHandle_t bus_lock, transfer_done;
/* DMA cannot access CCM. Ordinary .bss places these buffers in SRAM. */
static uint8_t tx[4U + W25Q16_PAGE_SIZE], rx[4U + W25Q16_PAGE_SIZE];
static volatile bool transfer_active, transfer_ok;
static bool ready;
volatile uint32_t g_w25q16_jedec_id;
volatile uint32_t g_w25q16_errors;

static bool Range(uint32_t address, uint32_t size)
{
    return address < W25Q16_CAPACITY && size <= W25Q16_CAPACITY - address;
}

static bool Transfer(uint16_t size)
{
    (void)xSemaphoreTake(transfer_done, 0);
    transfer_ok = false;
    transfer_active = true;
    HAL_GPIO_WritePin(F_CS_GPIO_Port, F_CS_Pin, GPIO_PIN_RESET);
    const bool started = HAL_SPI_TransmitReceive_DMA(&hspi1, tx, rx, size) == HAL_OK;
    const bool completed = started && xSemaphoreTake(transfer_done, pdMS_TO_TICKS(100)) == pdTRUE;
    transfer_active = false;
    if (!completed || !transfer_ok) { (void)HAL_SPI_Abort(&hspi1); }
    HAL_GPIO_WritePin(F_CS_GPIO_Port, F_CS_Pin, GPIO_PIN_SET);
    if (!completed || !transfer_ok) { ++g_w25q16_errors; return false; }
    return true;
}

static void Address(uint8_t command, uint32_t address)
{
    tx[0] = command;
    tx[1] = (uint8_t)(address >> 16);
    tx[2] = (uint8_t)(address >> 8);
    tx[3] = (uint8_t)address;
}

static bool Status(uint8_t *value)
{
    tx[0] = 0x05; tx[1] = 0xFF;
    if (!Transfer(2)) { return false; }
    *value = rx[1];
    return true;
}

static bool WaitReady(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        uint8_t status;
        if (!Status(&status)) { return false; }
        if ((status & 1U) == 0U) { return true; }
        if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            ++g_w25q16_errors; return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool WriteEnable(void)
{
    uint8_t status;
    if (!WaitReady(2000)) { return false; }
    tx[0] = 0x06;
    return Transfer(1) && Status(&status) && (status & 3U) == 2U;
}

static bool Read(uint32_t address, uint8_t *data, uint32_t size)
{
    while (size != 0U) {
        const uint32_t count = size > W25Q16_PAGE_SIZE ? W25Q16_PAGE_SIZE : size;
        Address(0x03, address);
        memset(tx + 4, 0xFF, count);
        if (!Transfer((uint16_t)(count + 4U))) { return false; }
        memcpy(data, rx + 4, count);
        address += count; data += count; size -= count;
    }
    return true;
}

bool W25Q16_Init(void)
{
    if (bus_lock == NULL) { bus_lock = xSemaphoreCreateMutex(); }
    if (transfer_done == NULL) { transfer_done = xSemaphoreCreateBinary(); }
    if (bus_lock == NULL || transfer_done == NULL ||
        xSemaphoreTake(bus_lock, pdMS_TO_TICKS(3000)) != pdTRUE) { return false; }
    ready = false;
    HAL_GPIO_WritePin(F_CS_GPIO_Port, F_CS_Pin, GPIO_PIN_SET);
    tx[0] = 0xAB; /* Release power-down; do not clear protection or reset busy writes. */
    bool ok = Transfer(1);
    vTaskDelay(pdMS_TO_TICKS(1));
    if (ok) { ok = WaitReady(2000); }
    if (ok) {
        tx[0] = 0x9F; memset(tx + 1, 0xFF, 3);
        ok = Transfer(4);
        if (ok) {
            g_w25q16_jedec_id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
            /* Winbond manufacturer and 16-Mbit density; report the complete type. */
            ok = rx[1] == 0xEF && rx[3] == 0x15;
        }
    }
    ready = ok;
    (void)xSemaphoreGive(bus_lock);
    return ok;
}

bool W25Q16_Read(uint32_t address, void *data, uint32_t size)
{
    if (!ready || data == NULL || !Range(address, size) ||
        xSemaphoreTake(bus_lock, pdMS_TO_TICKS(3000)) != pdTRUE) { return false; }
    const bool ok = WaitReady(2000) && Read(address, data, size);
    (void)xSemaphoreGive(bus_lock);
    return ok;
}

bool W25Q16_Program(uint32_t address, const void *data, uint32_t size)
{
    if (!ready || data == NULL || size == 0U || !Range(address, size) ||
        xSemaphoreTake(bus_lock, pdMS_TO_TICKS(3000)) != pdTRUE) { return false; }
    const uint8_t *source = data;
    bool ok = true;
    while (size != 0U && ok) {
        uint32_t count = W25Q16_PAGE_SIZE - (address % W25Q16_PAGE_SIZE);
        if (count > size) { count = size; }
        ok = WriteEnable();
        if (ok) {
            Address(0x02, address); memcpy(tx + 4, source, count);
            ok = Transfer((uint16_t)(count + 4U)) && WaitReady(20);
        }
        address += count; source += count; size -= count;
    }
    (void)xSemaphoreGive(bus_lock);
    return ok;
}

bool W25Q16_EraseSector(uint32_t address)
{
    if (!ready || (address % W25Q16_SECTOR_SIZE) != 0U ||
        !Range(address, W25Q16_SECTOR_SIZE) ||
        xSemaphoreTake(bus_lock, pdMS_TO_TICKS(3000)) != pdTRUE) { return false; }
    bool ok = WriteEnable();
    if (ok) { Address(0x20, address); ok = Transfer(4) && WaitReady(2000); }
    /* Readback also catches protected sectors silently ignoring an erase. */
    for (uint32_t offset = 0; ok && offset < W25Q16_SECTOR_SIZE; offset += W25Q16_PAGE_SIZE) {
        Address(0x03, address + offset); memset(tx + 4, 0xFF, W25Q16_PAGE_SIZE);
        ok = Transfer(sizeof(tx));
        for (uint32_t i = 4; ok && i < sizeof(rx); ++i) { ok = rx[i] == 0xFF; }
    }
    (void)xSemaphoreGive(bus_lock);
    return ok;
}

static void Complete(SPI_HandleTypeDef *spi, bool ok)
{
    if (spi != &hspi1 || !transfer_active || transfer_done == NULL) { return; }
    BaseType_t wake = pdFALSE;
    transfer_ok = ok;
    (void)xSemaphoreGiveFromISR(transfer_done, &wake);
    portYIELD_FROM_ISR(wake);
}
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *spi) { Complete(spi, true); }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *spi) { Complete(spi, false); }
