$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$testOutput = Join-Path $projectRoot 'cmake-build-debug/tests'
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    gcc -std=c11 -Wall -Wextra -Werror -IMiddlewares/UpdateProtocol/Inc `
        -IServices/FirmwareUpdate/Inc -I../STM32f_405_boot_loader/Core/Inc `
        tests/test_update.c Middlewares/UpdateProtocol/Src/update_protocol.c `
        Services/FirmwareUpdate/Src/fw_update.c ../STM32f_405_boot_loader/Core/Src/bootloader.c `
        -o "$testOutput/test_update.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
    & "$testOutput/test_update.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native update tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -Itests/flash_stubs `
        -IServices/FirmwareUpdate/Inc -I../STM32f_405_boot_loader/Core/Inc `
        tests/test_update_flash.c Drivers/Device/Src/update_flash.c -o "$testOutput/test_update_flash.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Flash driver test build failed' }
    & "$testOutput/test_update_flash.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Flash driver tests failed' }
    & 'C:\msys64\ucrt64\bin\python.exe' -B -m unittest discover -s tests -p test_uart_update.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Python client tests failed' }
} finally { Pop-Location }
