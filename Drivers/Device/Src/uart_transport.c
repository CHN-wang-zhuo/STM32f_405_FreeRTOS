#include "uart_transport.h"
#include "usart.h"
#include "stream_buffer.h"
#include "task.h"

static StreamBufferHandle_t rx_stream;
static uint8_t rx_byte;
static volatile bool rx_fault;
volatile uint32_t g_uart_rx_errors;
volatile uint32_t g_uart_rx_bytes;

bool UartTransport_Init(void)
{
    rx_stream = xStreamBufferCreate(2048U, 1U);
    return rx_stream != NULL && HAL_UART_Receive_IT(&huart1, &rx_byte, 1U) == HAL_OK;
}

size_t UartTransport_Read(uint8_t *data, size_t capacity, TickType_t timeout)
{
    return xStreamBufferReceive(rx_stream, data, capacity, timeout);
}

bool UartTransport_Send(const uint8_t *data, size_t size)
{
    /* Only the update task owns TX. HAL waits for TC before returning. */
    return size <= UINT16_MAX &&
           HAL_UART_Transmit(&huart1, data, (uint16_t)size, 1000U) == HAL_OK;
}

bool UartTransport_Recover(void)
{
    bool recovered = false;
    taskENTER_CRITICAL();
    if (rx_fault) {
        /* This consumer is not blocked; USART IRQ is masked during the reset. */
        (void)xStreamBufferReset(rx_stream);
        rx_fault = false;
        recovered = true;
        __HAL_UART_CLEAR_OREFLAG(&huart1);
        (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
    }
    taskEXIT_CRITICAL();
    return recovered;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart != &huart1 || rx_stream == NULL) { return; }
    BaseType_t wake = pdFALSE;
    ++g_uart_rx_bytes;
    if (xStreamBufferSendFromISR(rx_stream, &rx_byte, 1U, &wake) != 1U) {
        ++g_uart_rx_errors;
        rx_fault = true;
    }
    if (HAL_UART_Receive_IT(uart, &rx_byte, 1U) != HAL_OK) {
        ++g_uart_rx_errors;
        rx_fault = true;
    }
    portYIELD_FROM_ISR(wake);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart1) {
        ++g_uart_rx_errors;
        rx_fault = true;
        /* ORE ends the HAL receive operation; nonblocking errors leave it armed. */
        if (uart->RxState == HAL_UART_STATE_READY) {
            (void)HAL_UART_Receive_IT(uart, &rx_byte, 1U);
        }
    }
}
