$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path build/tests | Out-Null
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    gcc -std=c11 -Wall -Wextra -Werror -IMiddlewares/UpdateProtocol/Inc `
        -IServices/FirmwareUpdate/Inc -I../STM32f_405_boot_loader/Core/Inc `
        tests/test_update.c Middlewares/UpdateProtocol/Src/update_protocol.c `
        Services/FirmwareUpdate/Src/fw_update.c ../STM32f_405_boot_loader/Core/Src/bootloader.c `
        -o build/tests/test_update.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
    ./build/tests/test_update.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native update tests failed' }
    python -B -m unittest discover -s tests -p test_uart_update.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Python client tests failed' }
} finally { Pop-Location }
