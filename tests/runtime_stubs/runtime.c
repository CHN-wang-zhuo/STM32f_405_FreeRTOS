#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static TickType_t tick;
unsigned test_delay_count;
struct TestSemaphore { unsigned value; };
struct TestStream { uint8_t bytes[2048]; size_t size, used; };
TickType_t xTaskGetTickCount(void) { return tick; }
void vTaskDelay(TickType_t delay) { tick += delay; ++test_delay_count; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct TestSemaphore)); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { SemaphoreHandle_t s = xSemaphoreCreateBinary(); assert(s); s->value = 1; return s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    assert(s);
    if (s->value) { s->value = 0; return pdTRUE; }
    tick += wait; return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s); s->value = 1; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *wake) { *wake = pdTRUE; return xSemaphoreGive(s); }
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger)
{
    (void)trigger; assert(size <= 2048); StreamBufferHandle_t s = calloc(1, sizeof(*s)); assert(s); s->size = size; return s;
}
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *out, size_t count, TickType_t wait)
{
    (void)wait; if (count > s->used) { count = s->used; }
    memcpy(out, s->bytes, count); memmove(s->bytes, s->bytes + count, s->used - count); s->used -= count; return count;
}
size_t xStreamBufferSendFromISR(StreamBufferHandle_t s, const void *data, size_t count, BaseType_t *wake)
{
    if (count > s->size - s->used) { count = s->size - s->used; }
    memcpy(s->bytes + s->used, data, count); s->used += count; *wake = pdTRUE; return count;
}
BaseType_t xStreamBufferReset(StreamBufferHandle_t s) { s->used = 0; return pdTRUE; }
