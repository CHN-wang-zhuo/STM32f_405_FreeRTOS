#ifndef APP_TASKS_H
#define APP_TASKS_H
#include <stdint.h>
void AppTasks_Create(void);
typedef struct {
    volatile uint32_t heartbeat;
    volatile uint32_t frames;
    volatile uint32_t rejected;
    volatile uint32_t received;
    volatile uint32_t state;
    volatile uint32_t fault;
} AppDiagnostics;
extern AppDiagnostics g_app_diagnostics;
#endif
