# Explicit lists keep vendor examples, tests and alternate ports out of firmware.
set(BOOTLOADER_DIR "${CMAKE_SOURCE_DIR}/../STM32f_405_boot_loader" CACHE PATH
    "Bootloader repository containing the shared protocol and package tool")
set(FREERTOS_ROOT "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS")
set(SOURCES
    Core/Startup/startup_stm32f405rgtx.s
    Core/Src/main.c Core/Src/system_stm32f4xx.c Core/Src/stm32f4xx_it.c
    Core/Src/stm32f4xx_hal_msp.c Core/Src/dma.c Core/Src/gpio.c
    Core/Src/spi.c Core/Src/usart.c Core/Src/syscalls.c Core/Src/sysmem.c
    BSP/Src/bsp_board.c
    Drivers/Device/Src/uart_transport.c Drivers/Device/Src/w25q16.c
    Drivers/Device/Src/update_storage.c Drivers/Device/Src/update_identity.c
    Middlewares/UpdateProtocol/Src/update_protocol.c
    Services/FirmwareUpdate/Src/fw_update.c Application/Tasks/app_tasks.c)
foreach(module hal hal_cortex hal_dma hal_dma_ex hal_flash hal_flash_ex hal_gpio
               hal_pwr hal_pwr_ex hal_rcc hal_rcc_ex hal_spi hal_uart hal_exti)
    list(APPEND SOURCES "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_${module}.c")
endforeach()
# ST shares these Flash APIs across single/dual-bank devices. On F405 the
# Banks argument is unused; keep this exception confined to the vendor file.
set_source_files_properties(
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
foreach(module tasks queue list timers event_groups stream_buffer)
    list(APPEND SOURCES "${FREERTOS_ROOT}/${module}.c")
endforeach()
list(APPEND SOURCES "${FREERTOS_ROOT}/portable/GCC/ARM_CM4F/port.c"
                    "${FREERTOS_ROOT}/portable/MemMang/heap_4.c")
include_directories(
    Config BSP/Inc Drivers/Device/Inc Application/Inc
    Middlewares/UpdateProtocol/Inc Services/FirmwareUpdate/Inc
    "${BOOTLOADER_DIR}/Core/Inc" "${FREERTOS_ROOT}/include"
    "${FREERTOS_ROOT}/portable/GCC/ARM_CM4F")
# softfp enables the CM4F context-save instructions while preserving the
# existing C calling ABI. Use the same options for every APP object and link.
add_compile_options(-mfpu=fpv4-sp-d16 -mfloat-abi=softfp -Wall -Wextra)
add_link_options(-mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
