# STM32F405 APP

本工程与同级目录 `STM32f_405_boot_loader` 配套使用。启动链路为：
复位 → Bootloader 校验描述信息和固件 → APP Reset_Handler → SystemInit → main。

## 启动配置

- `STM32F405RGTX_FLASH.ld`：APP 位于 `0x08020000`，容量 384 KiB，对应 sectors 5–7。
- `SystemInit()` 从启动文件的 `g_pfnVectors` 获取实际向量表地址并写入 VTOR，
  在 HAL 启用 SysTick 之前完成设置，CMake 和 STM32CubeIDE 构建均适用。
- APP 自己初始化 HAL、8 MHz HSE / 168 MHz 系统时钟、GPIO、DMA、SPI1、USART1。
- 主循环每 500 ms 翻转 PA8 / LED0，可用于观察 APP 已进入主循环且 SysTick 正常工作。

不要将 APP 链接到暂存区，也不要给原始 `.bin` 添加文件头。
`STM32F405RGTX_RAM.ld` 仅用于 RAM 调试，不用于 Bootloader 固件打包。
CubeMX 重新生成后，需保留上述 Flash 分区、`SystemInit()` 的 VTOR 设置和主循环代码。
CMake 模板已同步打包配置，CLion 重新生成 CMakeLists 后仍可使用。

## 编译与首次装机

在 APP 目录执行（需要 ARM GCC、CMake、Ninja 和 Python 3.9 以上）：

```powershell
cmake -S . -B build/integration-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/integration-debug --target firmware_bundle
```

`firmware_bundle` 先生成 APP 及描述信息，再编译同级 Bootloader，最后生成
`build/integration-debug/firmware.factory.hex`。它包含：

| 内容 | 地址 |
| --- | --- |
| Bootloader | `0x08000000` |
| 已安装 APP 的描述信息 | `0x08008000` |
| 取消旧升级请求的 consumed 标记 | `0x0800C01C` |
| APP | `0x08020000` |

使用 STM32CubeProgrammer / SWD 对这个 HEX 按所含地址擦除、下载并校验，完成后复位。
HEX 自带地址，不需要手工指定加载偏移。首次装机时设备不得在烧录中途运行；
描述信息和 APP 完整写入并校验后再复位。已通过实板首次装机、暂存安装和复位启动验证，
详见 [2026-10-22 实板记录](docs/hardware-validation-2026-10-22.md)。

CLion 使用已有 `cmake-build-debug` 时，选择并构建 `firmware_bundle` 目标，
对应产物为 `cmake-build-debug/firmware.factory.hex`。
本工程 `flash.cfg` 使用 ST-Link/OpenOCD 烧录这个文件，烧录前先完成该目标的构建。

## 后续 APP 更新

普通构建（`cmake --build build/integration-debug`）会生成：

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
cmake -S . -B build/integration-debug -DAPP_FIRMWARE_VERSION=2
cmake --build build/integration-debug --target firmware_bundle
```

可用 `-DBOOTLOADER_DIR=绝对路径` 指定 Bootloader 工程位置。打包工具直接读取
Bootloader 的 `Core/Inc/boot_protocol.h`，共享地址和协议常量。
若使用 STM32CubeIDE 编译，则需要先导出原始 APP.bin，再手工调用 Bootloader 的
`tools/make_manifest.py` 打包，参数见 Bootloader README。

当前实现覆盖启动、首次装机和 SWD 升级验证，尚无串口/SPI/网络下载服务。
以后添加 APP 在线下载模块时，应遵守 Bootloader README 的暂存写入及 commit 最后提交协议。
