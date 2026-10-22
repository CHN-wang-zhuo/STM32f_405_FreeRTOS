#ifndef TEST_HAL_H
#define TEST_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { uint32_t remaining; } DMA_HandleTypeDef;
typedef struct { DMA_HandleTypeDef *hdmarx; } UART_HandleTypeDef;
#define GPIO_PIN_RESET 0
#define GPIO_PIN_SET 1
#define __HAL_DMA_GET_COUNTER(dma) ((dma)->remaining)
#define __HAL_UART_CLEAR_OREFLAG(uart) ((void)(uart))
void HAL_GPIO_WritePin(void *port, uint16_t pin, int state);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *spi, const uint8_t *tx, uint8_t *rx, uint16_t size);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *spi);
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *spi);
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *uart, uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *uart);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *uart);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart);
#endif
