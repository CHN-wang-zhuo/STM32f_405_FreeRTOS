/* Compile the actual device driver against HAL/RTOS call-recording stubs. */
#include "fw_update_port.h"
#include "stm32f4xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FLASH_EraseInitTypeDef calls[2];
static unsigned call_count, suspended, resumes, unlocks, locks, fail_call;
static bool fail_unlock, report_sector_error;

void vTaskSuspendAll(void) { assert(suspended == 0); ++suspended; }
int xTaskResumeAll(void) { assert(suspended == 1); --suspended; ++resumes; return 0; }
HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    assert(suspended == 1); ++unlocks;
    return fail_unlock ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Lock(void) { assert(suspended == 1); ++locks; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error)
{
    assert(suspended == 1 && call_count < 2);
    assert(erase->TypeErase == FLASH_TYPEERASE_SECTORS);
    assert(erase->VoltageRange == FLASH_VOLTAGE_RANGE_3);
    calls[call_count++] = *erase;
    *error = report_sector_error ? erase->Sector : UINT32_MAX;
    return call_count == fail_call ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t value)
{
    (void)type; (void)address; (void)value;
    assert(!"BEGIN must never program Flash"); return HAL_ERROR;
}
void FLASH_FlushCaches(void) { assert(!"BEGIN must never program Flash"); }

static void Reset(void)
{
    memset(calls, 0, sizeof(calls));
    call_count = suspended = resumes = unlocks = locks = fail_call = 0;
    fail_unlock = report_sector_error = false;
}

static void TestSectorSelection(void)
{
    const struct { uint32_t size, count; } cases[] = {
        {8U, 1U}, {23504U, 1U}, {131071U, 1U}, {131072U, 1U},
        {131073U, 2U}, {262143U, 2U}, {262144U, 2U},
        {262145U, 3U}, {393215U, 3U}, {393216U, 3U}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Reset();
        assert(FwPort_Begin(cases[i].size));
        assert(call_count == 2 && unlocks == 1 && locks == 1);
        assert(suspended == 0 && resumes == 1);
        /* Request cancellation must finish before touching any staged image. */
        assert(calls[0].Sector == 3 && calls[0].NbSectors == 1);
        assert(calls[1].Sector == 8 && calls[1].NbSectors == cases[i].count);
        assert(calls[1].Sector + calls[1].NbSectors <= 11);
    }
    puts("PASS: actual Flash driver selects only required sectors at every 128 KiB boundary");
}

static void TestInvalidSizesAndFailures(void)
{
    const uint32_t invalid[] = {0U, 1U, 7U, 393217U, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        Reset();
        assert(!FwPort_Begin(invalid[i]));
        assert(call_count == 0 && unlocks == 0 && resumes == 0);
    }
    Reset(); fail_unlock = true;
    assert(!FwPort_Begin(23504));
    assert(call_count == 0 && locks == 0 && suspended == 0 && resumes == 1);
    for (unsigned failure = 1; failure <= 2; ++failure) {
        Reset(); fail_call = failure;
        assert(!FwPort_Begin(393216));
        assert(call_count == failure && suspended == 0 && resumes == 1 && locks == 1);
    }
    Reset(); report_sector_error = true;
    assert(!FwPort_Begin(23504));
    assert(call_count == 1 && suspended == 0 && resumes == 1 && locks == 1);
    puts("PASS: invalid lengths touch no Flash; failures stop erasure and restore scheduling");
}

int main(void)
{
    TestSectorSelection();
    TestInvalidSizesAndFailures();
    return 0;
}
