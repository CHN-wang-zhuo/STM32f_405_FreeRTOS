#include "uart_transport.h"
#include "usart.h"
#include "stream_buffer.h"
#include "semphr.h"
#include "task.h"

static StreamBufferHandle_t rx_stream;
static SemaphoreHandle_t tx_done;
static uint8_t rx_dma[512]; /* SRAM, not CCM. HT/TC/IDLE report ring positions. */
static uint16_t rx_position;
static volatile bool rx_fault, tx_active, tx_ok;
volatile uint32_t g_uart_rx_errors;
volatile uint32_t g_uart_rx_bytes;

bool UartTransport_Init(void)
{
    rx_stream = xStreamBufferCreate(2048U, 1U);
    tx_done = xSemaphoreCreateBinary();
    rx_position = 0;
    return rx_stream != NULL && tx_done != NULL &&
           HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma, sizeof(rx_dma)) == HAL_OK;
}

size_t UartTransport_Read(uint8_t *data, size_t capacity, TickType_t timeout)
{
    return xStreamBufferReceive(rx_stream, data, capacity, timeout);
}

bool UartTransport_Send(const uint8_t *data, size_t size)
{
    if (size == 0U || size > UINT16_MAX) { return false; }
    /* Only the update task owns TX; its buffer lives until DMA + UART TC. */
    (void)xSemaphoreTake(tx_done, 0);
    tx_ok = false; tx_active = true;
    const bool started = HAL_UART_Transmit_DMA(&huart1, data, (uint16_t)size) == HAL_OK;
    const bool completed = started && xSemaphoreTake(tx_done, pdMS_TO_TICKS(1000)) == pdTRUE;
    tx_active = false;
    if (!completed || !tx_ok) { (void)HAL_UART_AbortTransmit(&huart1); return false; }
    return true;
}

bool UartTransport_Recover(void)
{
    if (!rx_fault) { return false; }
    /* Stop this producer before resetting the stream/ring. TX is independent. */
    (void)HAL_UART_AbortReceive(&huart1);
    taskENTER_CRITICAL();
    (void)xStreamBufferReset(rx_stream);
    rx_position = 0;
    rx_fault = false;
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma, sizeof(rx_dma)) != HAL_OK) { rx_fault = true; }
    taskEXIT_CRITICAL();
    return true;
}

static void Deliver(uint16_t offset, uint16_t size, BaseType_t *wake)
{
    if (size == 0U) { return; }
    g_uart_rx_bytes += size;
    if (xStreamBufferSendFromISR(rx_stream, rx_dma + offset, size, wake) != size) {
        ++g_uart_rx_errors; rx_fault = true;
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t reported_position)
{
    (void)reported_position;
    if (uart != &huart1 || rx_stream == NULL || rx_fault) { return; }
    /* HT/TC and IDLE can be pending together. Sample the LIVE DMA producer
     * instead of mistaking a delayed HT position for a new ring wrap. */
    const uint16_t position = (uint16_t)(sizeof(rx_dma) - __HAL_DMA_GET_COUNTER(uart->hdmarx));
    if (position > sizeof(rx_dma)) { ++g_uart_rx_errors; rx_fault = true; return; }
    BaseType_t wake = pdFALSE;
    /* USART1 and its RX DMA IRQ have the same preemption priority. */
    if (position > rx_position) { Deliver(rx_position, position - rx_position, &wake); }
    else if (position < rx_position) {
        Deliver(rx_position, (uint16_t)(sizeof(rx_dma) - rx_position), &wake);
        Deliver(0, position, &wake);
    }
    rx_position = position;
    portYIELD_FROM_ISR(wake);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart != &huart1 || !tx_active || tx_done == NULL) { return; }
    BaseType_t wake = pdFALSE;
    tx_ok = true;
    (void)xSemaphoreGiveFromISR(tx_done, &wake);
    portYIELD_FROM_ISR(wake);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart != &huart1) { return; }
    ++g_uart_rx_errors;
    rx_fault = true;
    if (tx_active && tx_done != NULL) {
        BaseType_t wake = pdFALSE;
        tx_ok = false;
        (void)xSemaphoreGiveFromISR(tx_done, &wake);
        portYIELD_FROM_ISR(wake);
    }
}
