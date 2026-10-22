# Keep packaging in the default build so a bare APP is never the only output.
set(BOOTLOADER_DIR "${CMAKE_SOURCE_DIR}/../STM32f_405_boot_loader" CACHE PATH
    "Bootloader repository containing the shared manifest tool")
set(APP_FIRMWARE_VERSION "1" CACHE STRING "APP uint32 firmware version (decimal or 0x-prefixed)")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(APP_PACKAGE_TOOL "${BOOTLOADER_DIR}/tools/make_manifest.py")
if(NOT EXISTS "${APP_PACKAGE_TOOL}")
    message(FATAL_ERROR "Missing ${APP_PACKAGE_TOOL}; set BOOTLOADER_DIR to the Bootloader repository")
endif()

# An ALL target also refreshes metadata when APP_FIRMWARE_VERSION or the tool changes
# without requiring a new ELF link. It is available in CLion as APP_package.
add_custom_target(APP_package ALL
    COMMAND "${Python3_EXECUTABLE}" -B "${APP_PACKAGE_TOOL}" "${BIN_FILE}"
        --version "${APP_FIRMWARE_VERSION}"
        --output "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.manifest.bin"
        --installed-hex "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.installed.hex"
        --update-hex "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.update.hex"
    DEPENDS ${PROJECT_NAME}.elf "${BIN_FILE}" "${HEX_FILE}"
    BYPRODUCTS "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.manifest.bin"
               "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.installed.hex"
               "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.update.hex"
    COMMENT "Packaging APP for Bootloader validation and installation"
    VERBATIM)

# Explicit joint build for first installation: rebuild both repositories before
# combining them, rather than accidentally packaging an old Bootloader binary.
set(BOOTLOADER_BUILD_DIR "${PROJECT_BINARY_DIR}/bootloader")
add_custom_target(firmware_bundle
    COMMAND "${CMAKE_COMMAND}" -S "${BOOTLOADER_DIR}" -B "${BOOTLOADER_BUILD_DIR}"
        -G "${CMAKE_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    COMMAND "${CMAKE_COMMAND}" --build "${BOOTLOADER_BUILD_DIR}" --config "$<CONFIG>"
    COMMAND "${Python3_EXECUTABLE}" -B "${APP_PACKAGE_TOOL}" "${BIN_FILE}"
        --version "${APP_FIRMWARE_VERSION}"
        --output "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.manifest.bin"
        --bootloader "${BOOTLOADER_BUILD_DIR}/stm32f405_boot_loader.bin"
        --factory-hex "${PROJECT_BINARY_DIR}/firmware.factory.hex"
    DEPENDS APP_package
    BYPRODUCTS "${PROJECT_BINARY_DIR}/firmware.factory.hex"
    COMMENT "Building Bootloader and combining it with the validated APP"
    VERBATIM)
