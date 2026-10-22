#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H
#include "FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool UartTransport_Init(void);
size_t UartTransport_Read(uint8_t *data, size_t capacity, TickType_t timeout);
bool UartTransport_Send(const uint8_t *data, size_t size);
bool UartTransport_Recover(void);
extern volatile uint32_t g_uart_rx_errors;
extern volatile uint32_t g_uart_rx_bytes;
#endif
