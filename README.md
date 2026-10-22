# STM32F405 APP

本工程与同级目录 `STM32f_405_boot_loader` 配套使用。启动链路为：
复位 → Bootloader 校验描述信息和固件 → APP Reset_Handler → SystemInit → main。
当前 APP 已接入 FreeRTOS V11.1.0，并提供 USART1 固件更新服务：
分块接收 → 内部 Flash 暂存 → 校验 → 立即重启安装或下次复位安装。
目录职责和任务配置见 [分层架构](docs/architecture.md)，二进制格式见 [串口升级协议](docs/uart-update-protocol.md)。

## 启动配置

- `STM32F405RGTX_FLASH.ld`：APP 位于 `0x08020000`，容量 384 KiB，对应 sectors 5–7。
- `SystemInit()` 从启动文件的 `g_pfnVectors` 获取实际向量表地址并写入 VTOR，
  在 HAL 启用 SysTick 之前完成设置，CMake 和 STM32CubeIDE 构建均适用。
- APP 自己初始化 HAL、8 MHz HSE / 168 MHz 系统时钟、GPIO、DMA、SPI1、USART1。
- main 完成 BSP 初始化后创建任务并启动调度器；心跳任务每 500 ms 翻转 PA8 / LED0。

不要将 APP 链接到暂存区，也不要给原始 `.bin` 添加文件头。
`STM32F405RGTX_RAM.ld` 仅用于 RAM 调试，不用于 Bootloader 固件打包。
CubeMX 重新生成后，需保留上述 Flash 分区、`SystemInit()` 的 VTOR 设置、任务启动和 RTOS 中断入口。
CMake 构建入口直接维护 `CMakeLists.txt`，不使用生成模板；分层源文件和打包规则通过 `cmake/` 下的模块引入。
如使用 IDE 的 CubeMX 集成，请关闭其 CMakeLists 自动生成功能，避免覆盖手工维护的配置。

## 编译与首次装机

CLion 使用 `cmake-build-debug` 构建目录。首次装机选择并构建 `firmware_bundle` 目标；
后续串口更新只需编译 `APP.elf`。命令行在 APP 目录执行（需要 ARM GCC、CMake、构建工具和 Python 3.9 以上）：

```powershell
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DPython3_EXECUTABLE=C:/msys64/ucrt64/bin/python.exe
cmake --build cmake-build-debug --target firmware_bundle
```

`firmware_bundle` 先生成 APP 及描述信息，再编译同级 Bootloader，最后生成
`cmake-build-debug/firmware.factory.hex`。它包含：

| 内容 | 地址 |
| --- | --- |
| Bootloader | `0x08000000` |
| 已安装 APP 的描述信息 | `0x08008000` |
| 取消旧升级请求的 consumed 标记 | `0x0800C01C` |
| APP | `0x08020000` |

使用 STM32CubeProgrammer / SWD 对这个 HEX 按所含地址擦除、下载并校验，完成后复位。
HEX 自带地址，不需要手工指定加载偏移。首次装机时设备不得在烧录中途运行；
描述信息和 APP 完整写入并校验后再复位。已通过实板首次装机、暂存安装和复位启动验证，
基础启动记录见 [首次实板记录](docs/hardware-validation-2026-10-22.md)，
RTOS 与实际串口升级见 [FreeRTOS/USART1 联调记录](docs/freertos-uart-validation-2026-10-22.md)。

CLion 使用已有 `cmake-build-debug` 时，选择并构建 `firmware_bundle` 目标，
对应产物为 `cmake-build-debug/firmware.factory.hex`。
本工程 `flash.cfg` 使用 ST-Link/OpenOCD 烧录这个文件，烧录前先完成该目标的构建。
单独编译 `APP.elf` 目标也会在链接后生成同目录的 `APP.bin` 和 `APP.hex`。
`APP_package` 目标额外生成描述信息及 APP 安装/暂存 HEX；`firmware_bundle` 再加入 Bootloader。

## 后续 APP 更新

普通构建（`cmake --build cmake-build-debug`）会在 `cmake-build-debug/` 中生成：

| 文件 | 用法 |
| --- | --- |
| `APP.elf` / `APP.hex` / `APP.bin` | 调试/原始固件；单独烧录不更新 Bootloader 所需描述信息 |
| `APP.manifest.bin` | 32 字节描述信息，含长度、CRC、版本和提交标志 |
| `APP.installed.hex` | SWD 直接更新运行区和描述信息，同时取消旧升级请求；保留 Bootloader |
| `APP.update.hex` | SWD 写入暂存区及升级请求，复位后由 Bootloader 校验、搬运、提交并启动 |

这两个 APP HEX 都不包含 Bootloader；对已有 Bootloader 的板卡按所含扇区擦除并写入，
不要选择整片擦除。`APP.update.hex` 仅用于调试器验证安装流程，烧录期间保持 MCU 停止，
待全部写入校验完成再复位；它不提供在线下载过程中的逐字提交顺序保证。

修改固件版本号：

```powershell
cmake -S . -B cmake-build-debug -DAPP_FIRMWARE_VERSION=2 -DPython3_EXECUTABLE=C:/msys64/ucrt64/bin/python.exe
cmake --build cmake-build-debug --target APP_package
```

可用 `-DBOOTLOADER_DIR=绝对路径` 指定 Bootloader 工程位置。打包工具直接读取
Bootloader 的 `Core/Inc/boot_protocol.h`，共享地址和协议常量。
若使用 STM32CubeIDE 编译，则需要先导出原始 APP.bin，再手工调用 Bootloader 的
`tools/make_manifest.py` 打包，参数见 Bootloader README。

## USART1 在线升级

首次通过 SWD 写入上述 FreeRTOS 框架后，后续可直接使用 COM4 更新原始 APP.bin。
先在 CLion 编译 `APP.elf`，确认生成 `cmake-build-debug/APP.bin`，再关闭占用 COM4 的串口助手。
在 PowerShell 进入 APP 目录，使用本机已验证的 Python 路径执行：

```powershell
Set-Location C:\Users\a2105\Documents\abc\STM\APP
$python = "C:\msys64\ucrt64\bin\python.exe"

# 先查询，正常会打印状态和 running_version
& $python -u -B tools/uart_update.py --port COM4 query

# 二选一：立即自动重启安装，或者下次复位安装
& $python -u -B tools/uart_update.py --port COM4 upload cmake-build-debug/APP.bin --version 2 --mode now
# & $python -u -B tools/uart_update.py --port COM4 upload cmake-build-debug/APP.bin --version 2 --mode defer

# 安装完成后再次查询，确认 running_version 为 2
& $python -u -B tools/uart_update.py --port COM4 query
```

`defer` 保持当前 APP 运行，下一次复位由 Bootloader 安装；`now` 在应答发完后主动复位安装。
UART1 使用 115200/8N1，PA9 为 TX、PA10 为 RX；升级必须逐帧等待 ACK，不能直接流式发送整个 BIN。
Windows 上位机脚本无需额外依赖。完整命令、错误码与重试约定见协议文档。
上传时会显示 `Uploaded 已上传字节/总字节`，成功应答中的 `status` 为 0。
`--version` 是串口升级包的版本标记，可依次使用 2、3、4；串口上传无需先生成 manifest 或 HEX，
也不依赖 `APP_FIRMWARE_VERSION` 的设置。其他电脑请将 `$python` 改为实际的 Python 3.9 以上解释器路径。

如果使用 `python ...` 后立即返回且完全没有输出，先检查终端实际调用的程序：

```powershell
Get-Command python -All | Format-List CommandType,Source,Definition
& "C:\msys64\ucrt64\bin\python.exe" -c "import sys; print(sys.executable)"
& "C:\msys64\ucrt64\bin\python.exe" -u -B tools/uart_update.py --port COM4 query
$LASTEXITCODE
```

无输出时应先确认解释器；串口占用或通信超时会由升级脚本打印错误。
END 超时后先查询设备，不要直接反复上传；请求可能已经提交并将在复位时安装。

主机测试：`./tests/run_tests.ps1`。默认 MSYS2 UCRT64 GCC；脚本链接实际生产版升级服务和 Bootloader 核心。
使用自定义 Python 时可通过 `-DPython3_EXECUTABLE=绝对路径` 选择可执行的解释器。
若 CMake 打包时调用了不可访问的 Python，请重新运行上述配置命令；CLion 可在 CMake options 中
加入 `-DPython3_EXECUTABLE=C:/msys64/ucrt64/bin/python.exe` 后 Reload CMake。
