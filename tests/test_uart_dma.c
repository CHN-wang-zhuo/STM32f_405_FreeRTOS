#include "uart_transport.h"
#include "usart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static DMA_HandleTypeDef dma;
UART_HandleTypeDef huart1 = {&dma};
static uint8_t *ring;
static uint16_t ring_size, producer;
static unsigned rx_starts, tx_aborts;
static bool suppress_tx;
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *uart, uint8_t *data, uint16_t size)
{
    assert(uart == &huart1); ring = data; ring_size = size; dma.remaining = size; producer = 0; ++rx_starts; return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t size)
{
    assert(uart == &huart1 && data && size); if (!suppress_tx) { HAL_UART_TxCpltCallback(uart); } return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *uart) { assert(uart == &huart1); return HAL_OK; }
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *uart) { assert(uart == &huart1); ++tx_aborts; return HAL_OK; }
static void Feed(const uint8_t *bytes, uint16_t size, uint16_t reported)
{
    assert(size < ring_size);
    for (unsigned i = 0; i < size; ++i) { ring[producer] = bytes[i]; producer = (uint16_t)((producer + 1) % ring_size); }
    dma.remaining = ring_size - producer;
    HAL_UARTEx_RxEventCallback(&huart1, reported);
}
int main(void)
{
    assert(UartTransport_Init() && ring_size == 512);
    uint8_t source[1044], received[1044]; for (unsigned i = 0; i < sizeof(source); ++i) { source[i] = (uint8_t)i; }
    Feed(source, 100, 100);
    HAL_UARTEx_RxEventCallback(&huart1, 100); /* duplicate IDLE */
    Feed(source + 100, 200, 300);
    HAL_UARTEx_RxEventCallback(&huart1, 256); /* HT pending behind IDLE must not look like wrap */
    Feed(source + 300, 262, 512); /* live position 50, delayed TC */
    HAL_UARTEx_RxEventCallback(&huart1, 50);
    Feed(source + 562, 200, 256);
    Feed(source + 762, 200, 512);
    Feed(source + 962, 82, 20);
    assert(UartTransport_Read(received, sizeof(received), 0) == sizeof(received));
    assert(memcmp(source, received, sizeof(source)) == 0 && g_uart_rx_bytes == sizeof(source));
    assert(UartTransport_Read(received, 1, 0) == 0);
    assert(UartTransport_Send(source, 44));
    suppress_tx = true; assert(!UartTransport_Send(source, 44) && tx_aborts == 1);
    suppress_tx = false; assert(UartTransport_Send(source, 44));
    HAL_UART_ErrorCallback(&huart1); assert(UartTransport_Recover() && rx_starts == 2);
    for (unsigned i = 0; i < 12; ++i) { Feed(source, 200, 0); }
    assert(g_uart_rx_errors >= 2 && UartTransport_Recover() && rx_starts == 3);
    Feed(source, 100, 100);
    assert(UartTransport_Read(received, 100, 0) == 100 && memcmp(source, received, 100) == 0);
    puts("PASS: actual UART DMA transport, 1044-byte frame, wraps, duplicate/out-of-order events, overflow and TX/RX recovery");
    return 0;
}
