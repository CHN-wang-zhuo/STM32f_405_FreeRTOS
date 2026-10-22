# APP 分层及任务设计

## 目录和职责

| 目录 | 职责 |
| --- | --- |
| `Core/` | CubeMX 生成的启动、系统时钟、外设初始化与 IRQ 入口；main 只负责启动框架 |
| `Config/` | 项目自己的 FreeRTOS 配置，避免与第三方内核混放 |
| `BSP/` | 板级初始化、LED、系统复位；集中设置可调用 RTOS API 的设备 IRQ 优先级 |
| `Drivers/CMSIS/`、`Drivers/STM32F4xx_HAL_Driver/` | ST 原始芯片支持和 HAL |
| `Drivers/Device/` | USART1 传输适配、内部 Flash 暂存/请求区擦写适配 |
| `Middlewares/Third_Party/FreeRTOS/` | 从原 Drivers/FreeRtos 迁入的 V11.1.0 内核、ARM_CM4F 端口及 heap_4；内核文件未改动 |
| `Middlewares/UpdateProtocol/` | 无 HAL/RTOS 依赖的字节帧解析、编码、小端读写、CRC-32 |
| `Services/FirmwareUpdate/` | 无 HAL/RTOS 依赖的升级状态机、重试、校验、提交策略；通过 fw_update_port.h 访问存储 |
| `Application/Tasks/` | 心跳任务、USART1 升级任务、任务创建和 RTOS 故障 hooks |
| `tools/` | 串口升级客户端及 Windows 原生串口适配 |
| `tests/` | 主机协议/状态机/Bootloader 联调测试，以及显式调用的硬件测试 |
| `cmake/` | 显式固件源文件清单、第三方配置及联合打包规则 |

调用方向为任务 → 升级服务 → 协议中间件及存储端口；端口由设备驱动实现。
USART 接收 ISR 只将字节投递到 stream buffer，不进行解析、CRC、Flash 擦写或复位。
升级业务由单个任务串行处理，UART1 TX 和升级 Flash 操作均只有这个任务拥有。

## 启动与 RTOS

复位 → Bootloader 校验/安装 → APP Reset_Handler → SystemInit 设置 APP VTOR → main →
BSP_Init → 创建任务 → vTaskStartScheduler。

- `heartbeat`：优先级 1、128 个 32 位字的栈（512 字节），每 500 ms 翻转 PA8，并更新诊断计数。
- `fw_update`：优先级 3、512 个 32 位字的栈（2048 字节），初始化 USART1 stream buffer，解析并处理更新帧。
- FreeRTOS 额外创建 Idle 和软件定时器服务任务，总计 4 个任务。
- heap_4 配置为 16 KiB，存放 RTOS 动态对象和任务栈；它位于普通 SRAM，不使用 CCM。
  链接统计包含整个预留堆，任务栈已在其中，不能重复相加；只缩小任务栈不会降低链接统计的 RAM 占用。
- 开启栈溢出检查和 malloc 失败 hook，可通过 `g_app_diagnostics.fault` 查看故障。
- 编译/链接均使用 `-mfpu=fpv4-sp-d16 -mfloat-abi=softfp`，不混用不同 APP ABI。
  `softfp` 允许硬件浮点运算并采用通用寄存器传参；FPU 支持单精度 `float`，FreeRTOS 端口保存浮点上下文。
  使用浮点或增加任务调用深度后，应重新检查任务栈水位和最低剩余堆。
- SVC、PendSV 由 FreeRTOS 直接提供。SysTick 保留独立入口：始终更新 HAL tick，
  调度器启动后再调用 xPortSysTickHandler。编译检查固定 RTOS 节拍为 1 kHz。
- BSP 将 USART1、SPI1 和相关 DMA IRQ 优先级统一设为 6，符合阈值 5 的 FromISR 要求。
- UART1 为 115200、8N1、PA9/TX、PA10/RX，当前接收实现使用逐字节中断和 2048 字节 stream buffer。
  DMA 初始化保留给后续外设业务，但本升级传输不使用 DMA，也不使用 CCM 缓冲区。

早期实板升级记录使用 64 KiB 堆和 256/1024 字的两个应用任务栈。
当前 16 KiB 堆和 128/512 字栈配置已通过构建，完整升级流程下的栈水位及最低剩余堆仍需重新实测。

## Flash 和复位边界

运行区仍是 `0x08020000` 的 384 KiB，暂存区仍是 `0x08080000` 的 384 KiB，
升级请求仍在 `0x0800C000`。地址、描述信息结构及 commit 常量直接引用同级 Bootloader 头文件。
不改变 Bootloader 固件或其协议。

APP 的设备驱动只允许擦除 request sector 3 和 staging sectors 8–10，
只允许写入暂存区和请求描述信息的前 28 字节。运行区、已安装描述信息和 consumed 由 Bootloader 管理。
接收时不在 APP 运行区执行覆盖操作。

BEGIN 先擦除旧请求，再擦除暂存区；END 完整校验暂存读回的内容后写请求头，commit 最后写。
任意未完成传输均不会发布有效升级请求，旧 APP 的运行区和描述信息保持完整。
已提交的延期请求会在下一次任意系统复位时安装；该策略不只针对上位机主动请求的复位。

STM32F405 是单 Bank Flash。擦写期间会阻塞从 Flash 取指，设备驱动同时暂停任务调度，
因此升级擦写阶段不保证心跳或实时任务的周期，HAL/RTOS tick 也不代表完整的墙钟耗时。
上位机必须等 ACK 才发送下一帧，不在擦写时连续流式发送。BEGIN 默认等待上限 20 秒。

### 擦写期间的任务暂停

`Drivers/Device/Src/update_flash.c` 的 `Open()` 调用 `vTaskSuspendAll()`，
`Close()` 调用 `xTaskResumeAll()`；当前整个 `FwPort_Begin()` 擦除过程位于这一调度暂停区间内。
BEGIN 从协议获得固件长度，通过 `FwPort_Begin(image_size)` 传给设备驱动；
驱动先擦除请求 sector 3，再从 sector 8 起擦除 `ceil(image_size / 128 KiB)` 个暂存扇区。
固件大小不超过 128 KiB 时只擦 sector 8，超过 128 KiB 加擦 sector 9，超过 256 KiB 再加擦 sector 10。
扇区边界采用向上取整，恰好 128/256 KiB 时不会多擦一个扇区；非法长度在任何擦除之前拒绝。
因此在主机开始显示 DATA 上传进度前，LED 心跳会明显暂停；擦除结束、恢复调度并回复 ACK 后才开始传输。
DATA 每块编程和 END 请求提交也会短暂暂停任务调度。

调度暂停不会直接关闭中断，但单 Bank Flash 擦写会阻塞对 Flash 的读取；
当前代码和中断入口位于 Flash，单纯移除 `vTaskSuspendAll()` 或调整任务优先级无法保证实时性。
按固件长度擦除已实现，可缩短小固件 BEGIN 的停顿，但仍会擦除整个所需扇区，不能按字节擦除。
未使用扇区中的旧内容不参与当前描述信息中长度范围之外的 CRC 和安装；下次固件变大时会按新长度擦除。
Bootloader 的运行区擦除逻辑不在本次修改范围，协议帧和上位机用法也不变。
完整 RAM 执行路径或外部存储暂存可作为持续运行方案，这两项尚未实现。
完整现象解释及主机处理方式见 [升级时 LED 暂停](uart-update-protocol.md#升级时-led-暂停)。

## 构建和生成工具

CMake 使用显式源文件清单，第三方示例、其他端口及主机测试不会混入固件。
项目直接维护 `CMakeLists.txt`，不使用生成模板，IDE 的 CMakeLists 自动生成应关闭。
STM32CubeIDE 的 Debug/Release include paths 和 source entries 也已补充，
其中 CubeIDE 沿用工程已有的 hard-float 选项，不与 CMake 产物混合链接。
本次已实际验证 CMake 构建，未运行 CubeIDE 的 managed build。

CubeMX 重新生成可能重新加入裸机的 SVC/PendSV 和 main 初始化调用，需要保留当前 RTOS 入口修改。
`SystemInit()` 的 VTOR 设置、384 KiB Flash 链接分区也必须保留。

## 主机验证

在 APP 根目录执行 `./tests/run_tests.ps1`。脚本编译生产版协议、升级服务和同级 Bootloader 核心，
使用 NOR Flash 模型覆盖立即/延期安装、重试、乱序、取消、超时、非法入口和擦写失败，
再运行 Python 客户端帧格式及超时重试测试。默认主机 C 编译器为 MSYS2 UCRT64 GCC。
另将实际 `update_flash.c` 与 HAL/RTOS 桩链接，检查 128/256/384 KiB 边界的扇区选择、
请求区先擦除、非法长度无擦写和失败后恢复调度；服务联调测试保留未使用暂存扇区中的旧数据，验证仍能安装。
测试产物放在 `cmake-build-debug/tests/`，不再创建独立的 `build/` 目录。
这组模型测试不操作串口，也不烧录硬件。
