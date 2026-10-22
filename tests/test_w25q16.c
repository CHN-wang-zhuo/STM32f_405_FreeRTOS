/* Real SPI/DMA W25Q16 driver; the mock chip enforces write-enable and page wrap rules. */
#include "w25q16.h"
#include "spi.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
SPI_HandleTypeDef hspi1;
static uint8_t flash[W25Q16_CAPACITY];
static bool selected, write_enabled, suppress_completion, dma_error, stuck_busy, protected;
static unsigned busy, programs, erases, aborts, transfers;
static uint8_t density = 0x15;
void HAL_GPIO_WritePin(void *port, uint16_t pin, int state) { (void)port; assert(pin == 4); selected = state == GPIO_PIN_RESET; }
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *spi) { assert(spi == &hspi1); ++aborts; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *spi, const uint8_t *tx, uint8_t *rx, uint16_t size)
{
    assert(spi == &hspi1 && selected && size >= 1 && size <= 260); ++transfers;
    memset(rx, 0xFF, size);
    const uint32_t address = size >= 4 ? ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3] : 0;
    switch (tx[0]) {
    case 0xAB: assert(size == 1); break;
    case 0x9F: assert(size == 4); rx[1] = 0xEF; rx[2] = 0x40; rx[3] = density; break;
    case 0x05:
        assert(size == 2); rx[1] = (busy || stuck_busy) ? 1 : (write_enabled ? 2 : 0);
        if (busy) { --busy; } break;
    case 0x06: assert(!busy && !stuck_busy); write_enabled = true; break;
    case 0x03:
        assert(!busy && !stuck_busy && address + size - 4 <= sizeof(flash));
        memcpy(rx + 4, flash + address, size - 4); break;
    case 0x02:
        assert(write_enabled && size > 4 && !busy);
        assert((address / 256) == ((address + size - 5) / 256));
        for (unsigned i = 4; !protected && i < size; ++i) { flash[address + i - 4] &= tx[i]; }
        ++programs; busy = 2; write_enabled = false; break;
    case 0x20:
        assert(write_enabled && size == 4 && !busy && (address % 4096) == 0);
        if (!protected) { memset(flash + address, 0xFF, 4096); }
        ++erases; busy = 3; write_enabled = false; break;
    default: assert(!"unexpected SPI opcode");
    }
    if (dma_error) { HAL_SPI_ErrorCallback(spi); }
    else if (!suppress_completion) { HAL_SPI_TxRxCpltCallback(spi); }
    return HAL_OK;
}
int main(void)
{
    memset(flash, 0xA5, sizeof(flash));
    density = 0x16; assert(!W25Q16_Init() && !selected && erases == 0);
    density = 0x15; assert(W25Q16_Init() && g_w25q16_jedec_id == 0xEF4015);
    assert(W25Q16_EraseSector(0x1000) && erases == 1 && test_delay_count >= 3);
    assert(flash[0xFFF] == 0xA5 && flash[0x2000] == 0xA5);
    uint8_t data[600], readback[600]; for (unsigned i = 0; i < sizeof(data); ++i) { data[i] = (uint8_t)i; }
    assert(W25Q16_Program(0x10F0, data, sizeof(data)) && programs == 4);
    assert(W25Q16_Read(0x10F0, readback, sizeof(readback)) && memcmp(data, readback, sizeof(data)) == 0);
    unsigned before = transfers;
    assert(!W25Q16_EraseSector(1));
    assert(!W25Q16_Program(W25Q16_CAPACITY - 1, data, 2));
    assert(!W25Q16_Read(UINT32_MAX, data, 1) && transfers == before);
    protected = true; assert(!W25Q16_EraseSector(0x3000)); protected = false;
    suppress_completion = true; assert(!W25Q16_Read(0, data, 1) && !selected && aborts > 0);
    suppress_completion = false; assert(W25Q16_Read(0, data, 1));
    dma_error = true; assert(!W25Q16_Read(0, data, 1) && !selected); dma_error = false;
    stuck_busy = true; assert(!W25Q16_Read(0, data, 1)); stuck_busy = false;
    assert(W25Q16_Read(0, data, 1));
    puts("PASS: actual W25Q16 DMA driver, JEDEC gate, page splits, 4 KiB erase/readback, cooperative BUSY, DMA/timeout recovery");
    return 0;
}
