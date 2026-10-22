# APP 分层及任务设计

## 目录和职责

| 目录 | 职责 |
| --- | --- |
| `Core/` | CubeMX 启动、系统时钟、外设初始化及 IRQ 入口 |
| `Config/` | FreeRTOS 配置与 W25Q16 分区、64 字节升级记录定义 |
| `BSP/` | 板级初始化、LED、系统复位及设备 IRQ 优先级 |
| `Drivers/CMSIS/`、`Drivers/STM32F4xx_HAL_Driver/` | ST 芯片支持和 HAL |
| `Drivers/Device/uart_transport.c` | USART1 循环 DMA 接收、stream buffer 和 DMA 发送 |
| `Drivers/Device/w25q16.c` | SPI1 DMA、PA2 片选、JEDEC、页编程、4 KiB 擦除和 BUSY 等待 |
| `Drivers/Device/update_storage.c` | 外部分区边界、写后比较、记录 CRC 和最终提交 |
| `Drivers/Device/update_identity.c` | 只读现有 Bootloader 的已安装版本信息 |
| `Middlewares/Third_Party/FreeRTOS/` | 原版 V11.1.0 内核、ARM_CM4F 端口和 heap_4 |
| `Middlewares/UpdateProtocol/` | 无 HAL/RTOS 依赖的字节帧解析、编码和 CRC-32 |
| `Services/FirmwareUpdate/` | 无 HAL/RTOS 依赖的升级状态机、重传、流式校验和提交策略 |
| `Application/Tasks/` | 心跳、串口升级任务及 RTOS 故障 hooks |
| `tools/` | 串口客户端及 Windows 原生串口适配 |
| `tests/` | 真实服务/存储与 NOR 模型测试、真实 DMA 驱动与 HAL/RTOS 桩测试 |
| `cmake/` | 显式固件源文件清单及构建、打包规则 |

调用方向为任务 → 升级服务 → 存储端口 → W25Q16 驱动 → SPI1 DMA。
串口 ISR 只搬运接收数据、通知发送完成；SPI/DMA ISR 只通知完成或错误。
解析、CRC、擦写和复位在任务上下文执行。升级任务独占 UART TX 和升级存储操作。

## 启动和 RTOS

复位 → 原 Bootloader → APP Reset_Handler → SystemInit 设置 VTOR → main → BSP_Init →
创建任务 → vTaskStartScheduler。

- `heartbeat`：优先级 1，128 个 32 位字栈（512 字节），每 500 ms 翻转 PA8 并累计心跳计数。
- `fw_update`：优先级 3，512 个 32 位字栈（2048 字节），先启动 UART DMA，再探测 W25Q16、恢复外部请求。
- 内核另建 Idle 和软件定时器任务，共 4 个任务。
- heap_4 固定预留 16 KiB，容纳任务栈、控制块、stream buffer 和完成信号量、互斥锁。
  链接统计包含整个预留堆，不能再把其中任务栈重复相加。
- SPI1、USART1 及四路 DMA IRQ 均由 BSP 设为优先级 6，符合 FromISR 阈值 5。
- SVC/PendSV 由 FreeRTOS 提供；SysTick 始终更新 HAL tick，调度器启动后推进内核 tick，固定 1 kHz。
- 编译/链接使用 `-mfpu=fpv4-sp-d16 -mfloat-abi=softfp`，开启单精度硬件浮点和 CM4F 上下文保存。
- 栈溢出和 malloc 失败 hook 设置 `g_app_diagnostics.fault`；改动调用深度或浮点使用后应重新测量栈水位。

UART 接收使用 512 字节循环 DMA 缓冲和 2048 字节 stream buffer，HT/TC/IDLE 事件读取实时 NDTR。
因此同一位置的重复 IDLE、以及延迟 HT 回调不会重复复制数据。溢出或接收错误会重新启动 RX 并重置解析器。
UART TX 使用 DMA，完成信号在 UART TC 后产生，发送栈缓冲的生存期覆盖整个 DMA 操作。
SPI 使用独立互斥锁和完成信号量，传输超时后终止 DMA 并释放 /CS。
DMA 缓冲均在普通 SRAM，不放在 CCM。

## 外部升级存储与实时性

W25Q16 的请求区位于偏移 `0x000000`，占一个 4 KiB 扇区；候选固件区为
`0x010000–0x06FFFF`，384 KiB。其余空间保留；地址均为 SPI Flash 字节偏移。
完整布局及记录格式见 [W25Q16 存储规范](w25q16-update-storage.md)。

BEGIN 先撤销旧请求，再只擦除固件长度覆盖的 4 KiB 扇区；DATA 按 256 字节页拆分写入并比较读回；
END 分块读取固件计算 CRC、校验向量表，最后单独写 commit。服务不再依赖内存映射的 `FwPort_Stage()`，
使用可失败的 `FwPort_Read()`，不会一次分配整个固件大小的 RAM。
启动恢复也会校验外部完整固件，只恢复有效 PENDING 请求，ABORT 只清除外部请求区。

历史内部 Flash 实现通过 `vTaskSuspendAll()` 包围擦写，而且 F405 单 Bank Flash 擦写会阻塞取指，
造成 BEGIN 阶段 LED 停闪。本版接收、校验、提交和取消都不擦写内部 Flash，也不暂停全局调度。
外置芯片 BUSY 时更新任务调用 `vTaskDelay(1)`，SPI/UART DMA 完成则通过信号量唤醒，心跳可继续运行。
这消除了内部 Flash 擦除导致的取指阻塞，不代表所有业务均已完成最坏实时性验证。

## 与现有 Bootloader 的边界

本次不修改 Bootloader 工程或配置。APP 运行地址仍为 `0x08020000`，最大 384 KiB。
`running_version` 只读内部 `0x08008000` 的已安装描述；所有新的升级信息和固件只写 W25Q16。
APP 不清除内部旧升级请求，也不写内部 `0x08080000` 暂存区；部署时通过 `APP.installed.hex` 取消旧内部请求。

现有 Bootloader 不读取 W25Q16。因此 `now` 和 `reboot` 虽然会复位，却不会安装外部候选包；
原 APP 重启后仍报告原 `running_version`，外部请求仍为 PENDING。
本版本不在启动时根据保存的 now 模式自动再次复位，避免循环重启。
未来外部安装需要单独扩展 Bootloader；当前请用 SWD 安装本版 APP。

## 构建和验证

直接维护 `CMakeLists.txt`，不保留生成模板；IDE 的 CMakeLists 自动生成应关闭。
`APP.elf` 链接后生成 BIN/HEX，`APP_package` 生成当前 Bootloader 兼容的 SWD 安装包。
`APP.update.hex` 仍是原内部暂存调试包，不是外置 Flash 镜像。此打包配置未随本次迁移修改。
CubeIDE 保留原 hard-float 配置，其对象不能与 CMake softfp 对象混合；本次只验证 CMake 构建。
CubeMX 重新生成时须保留 RTOS 中断入口、VTOR、PA2 高电平、SPI 分频和 USART1 RX circular DMA 配置。

在 APP 根目录执行 `./tests/run_tests.ps1`，产物位于 `cmake-build-debug/tests/`。
测试链接真实协议、服务、外部存储后端和原 Bootloader 核心，检查 NOR 限制、CRC、大小包替换、
133 个擦除/逐字节编程故障点、请求恢复及内部 Flash 不改写。
另用 HAL/RTOS 桩链接真实 W25Q16 和 UART DMA 驱动，覆盖页边界、超时、BUSY 等待、
HT/IDLE 乱序、DMA 回绕及错误恢复，再运行 Python 客户端测试。
这些是主机测试。当前 ST-Link 无法连接 MCU 内核，W25Q16/DMA 实板验证及当前栈水位仍待完成。
