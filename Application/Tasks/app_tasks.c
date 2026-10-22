#include "app_tasks.h"
#include "bsp_board.h"
#include "uart_transport.h"
#include "fw_update.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

AppDiagnostics g_app_diagnostics;
static UpdateDecoder decoder;
static FwUpdate update;

static void HeartbeatTask(void *argument)
{
    (void)argument;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        BSP_LedToggle();
        ++g_app_diagnostics.heartbeat;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(500));
    }
}

static void UpdateTask(void *argument)
{
    (void)argument;
    uint8_t rx[128];
    uint8_t tx[UPDATE_HEADER_SIZE + UPDATE_RESPONSE_SIZE];
    FwUpdate_Init(&update);
    UpdateDecoder_Reset(&decoder);
    configASSERT(UartTransport_Init());
    TickType_t last_byte = xTaskGetTickCount();
    TickType_t last_frame = last_byte;
    for (;;) {
        const size_t count = UartTransport_Read(rx, sizeof(rx), pdMS_TO_TICKS(50));
        const TickType_t now = xTaskGetTickCount();
        if (UartTransport_Recover() || (TickType_t)(now - last_byte) > pdMS_TO_TICKS(1000)) {
            UpdateDecoder_Reset(&decoder);
        }
        if ((TickType_t)(now - last_frame) > pdMS_TO_TICKS(60000)) { FwUpdate_Expire(&update); }
        if (count != 0U) { last_byte = now; }
        for (size_t i = 0; i < count; ++i) {
            UpdateFrame frame;
            const UpdateDecodeResult result = UpdateDecoder_Push(&decoder, rx[i], &frame);
            if (result == UPDATE_FRAME_REJECTED) { ++g_app_diagnostics.rejected; }
            if (result != UPDATE_FRAME_READY) { continue; }
            ++g_app_diagnostics.frames;
            FwUpdateReply reply;
            FwUpdate_Process(&update, &frame, &reply);
            last_frame = last_byte = xTaskGetTickCount();
            g_app_diagnostics.received = update.received;
            g_app_diagnostics.state = update.state;
            const size_t size = UpdateProtocol_Encode(tx, sizeof(tx), frame.command | 0x80U,
                                                      frame.sequence, reply.bytes, sizeof(reply.bytes));
            const bool sent = UartTransport_Send(tx, size);
            if (reply.reset_after_ack && sent) {
                vTaskDelay(pdMS_TO_TICKS(50));
                BSP_Reset();
            }
        }
    }
}

void AppTasks_Create(void)
{
    configASSERT(xTaskCreate(HeartbeatTask, "heartbeat", 128U, NULL, 1U, NULL) == pdPASS);
    configASSERT(xTaskCreate(UpdateTask, "fw_update", 512U, NULL, 3U, NULL) == pdPASS);
}

void vApplicationMallocFailedHook(void)
{
    g_app_diagnostics.fault = 1U;
    Error_Handler();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    g_app_diagnostics.fault = 2U;
    Error_Handler();
}
