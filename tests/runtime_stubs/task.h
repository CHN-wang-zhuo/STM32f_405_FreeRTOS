#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
#endif
