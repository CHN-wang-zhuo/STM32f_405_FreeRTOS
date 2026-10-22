#ifndef TEST_STREAM_BUFFER_H
#define TEST_STREAM_BUFFER_H
#include "FreeRTOS.h"
typedef struct TestStream *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger);
size_t xStreamBufferReceive(StreamBufferHandle_t stream, void *out, size_t count, TickType_t wait);
size_t xStreamBufferSendFromISR(StreamBufferHandle_t stream, const void *data, size_t count, BaseType_t *wake);
BaseType_t xStreamBufferReset(StreamBufferHandle_t stream);
#endif
