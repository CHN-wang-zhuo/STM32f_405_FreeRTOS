#ifndef FW_UPDATE_PORT_H
#define FW_UPDATE_PORT_H
#include "boot_protocol.h"
#include <stdbool.h>

/* Implemented by the device driver; the service itself has no HAL/RTOS dependency. */
bool FwPort_Init(void);
/* Invalidate external request FIRST, then erase only sectors covering size. */
bool FwPort_Begin(uint32_t image_size);
bool FwPort_Cancel(void);
bool FwPort_Write(uint32_t offset, const uint8_t *data, uint32_t size);
bool FwPort_Read(uint32_t offset, void *data, uint32_t size);
bool FwPort_ReadRequest(BootImageManifest *manifest);
void FwPort_ReadInstalled(BootImageManifest *manifest);
bool FwPort_Commit(const BootImageManifest *manifest, uint8_t mode); /* commit word LAST */
#endif
