$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$testOutput = Join-Path $projectRoot 'cmake-build-debug/tests'
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    gcc -std=c11 -Wall -Wextra -Werror -IConfig -IDrivers/Device/Inc -IMiddlewares/UpdateProtocol/Inc `
        -IServices/FirmwareUpdate/Inc -I../STM32f_405_boot_loader/Core/Inc `
        tests/test_update.c Middlewares/UpdateProtocol/Src/update_protocol.c `
        Services/FirmwareUpdate/Src/fw_update.c Drivers/Device/Src/update_storage.c `
        ../STM32f_405_boot_loader/Core/Src/bootloader.c `
        -o "$testOutput/test_update.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
    & "$testOutput/test_update.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native update tests failed' }
    foreach ($driver in @('w25q16', 'uart_dma')) {
        $driverSource = if ($driver -eq 'w25q16') { 'w25q16' } else { 'uart_transport' }
        gcc -std=c11 -Wall -Wextra -Werror -Itests/runtime_stubs -IDrivers/Device/Inc `
            "tests/test_$driver.c" tests/runtime_stubs/runtime.c "Drivers/Device/Src/$driverSource.c" `
            -o "$testOutput/test_$driver.exe"
        if ($LASTEXITCODE -ne 0) { throw "$driver test build failed" }
        & "$testOutput/test_$driver.exe"
        if ($LASTEXITCODE -ne 0) { throw "$driver tests failed" }
    }
    & 'C:\msys64\ucrt64\bin\python.exe' -B -m unittest discover -s tests -p test_uart_update.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Python client tests failed' }
} finally { Pop-Location }
