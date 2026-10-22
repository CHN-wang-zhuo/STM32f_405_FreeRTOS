#include "bsp_board.h"
#include "main.h"
#include "dma.h"
#include "gpio.h"
#include "spi.h"
#include "usart.h"
#include "FreeRTOS.h"

extern void SystemClock_Config(void);

void BSP_Init(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    /* All enabled device IRQs may safely use FreeRTOS FromISR APIs. */
    const IRQn_Type irqs[] = {USART1_IRQn, SPI1_IRQn, DMA2_Stream0_IRQn,
                             DMA2_Stream2_IRQn, DMA2_Stream3_IRQn, DMA2_Stream7_IRQn};
    for (unsigned i = 0; i < sizeof(irqs) / sizeof(irqs[0]); ++i) {
        HAL_NVIC_SetPriority(irqs[i], configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1U, 0);
    }
}

void BSP_LedToggle(void)
{
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
}

_Noreturn void BSP_Reset(void)
{
    __DSB();
    NVIC_SystemReset();
    for (;;) {}
}
