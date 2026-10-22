#ifndef FW_UPDATE_PORT_H
#define FW_UPDATE_PORT_H
#include "boot_protocol.h"
#include <stdbool.h>

/* Implemented by the device driver; the service itself has no HAL/RTOS dependency. */
/* Invalidate old request FIRST, then erase only staging sectors covering size. */
bool FwPort_Begin(uint32_t image_size);
bool FwPort_Cancel(void);
bool FwPort_Write(uint32_t offset, const uint8_t *data, uint32_t size);
const uint8_t *FwPort_Stage(uint32_t offset, uint32_t size);
void FwPort_ReadRequest(BootImageManifest *manifest);
void FwPort_ReadInstalled(BootImageManifest *manifest);
bool FwPort_Commit(const BootImageManifest *manifest); /* commit word LAST */
#endif
