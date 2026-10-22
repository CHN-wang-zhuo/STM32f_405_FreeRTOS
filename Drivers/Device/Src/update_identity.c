#include "fw_update_port.h"
#include <string.h>

/* Read-only identity supplied by the existing Bootloader. All NEW upgrade
 * metadata and firmware are stored exclusively in external W25Q16. */
void FwPort_ReadInstalled(BootImageManifest *manifest)
{
    memcpy(manifest, (const void *)(uintptr_t)BOOT_INSTALLED_ADDRESS, sizeof(*manifest));
}
