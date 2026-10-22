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

- `heartbeat`：优先级 1、256 个 32 位字的栈，每 500 ms 翻转 PA8，并更新诊断计数。
- `fw_update`：优先级 3、1024 个 32 位字的栈，初始化 USART1 stream buffer，解析并处理更新帧。
- FreeRTOS 额外创建 Idle 和软件定时器服务任务，总计 4 个任务。
- heap_4 配置为 64 KiB，存放 RTOS 动态对象和任务栈；它位于普通 SRAM，不使用 CCM。
- 开启栈溢出检查和 malloc 失败 hook，可通过 `g_app_diagnostics.fault` 查看故障。
- 编译/链接均使用 `-mfpu=fpv4-sp-d16 -mfloat-abi=softfp`，不混用不同 APP ABI。
- SVC、PendSV 由 FreeRTOS 直接提供。SysTick 保留独立入口：始终更新 HAL tick，
  调度器启动后再调用 xPortSysTickHandler。编译检查固定 RTOS 节拍为 1 kHz。
- BSP 将 USART1、SPI1 和相关 DMA IRQ 优先级统一设为 6，符合阈值 5 的 FromISR 要求。
- UART1 为 115200、8N1、PA9/TX、PA10/RX，当前接收实现使用逐字节中断和 2048 字节 stream buffer。
  DMA 初始化保留给后续外设业务，但本升级传输不使用 DMA，也不使用 CCM 缓冲区。

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

## 构建和生成工具

CMake 使用显式源文件清单，第三方示例、其他端口及主机测试不会混入固件。
CMake 模板已同步；STM32CubeIDE 的 Debug/Release include paths 和 source entries 也已补充，
其中 CubeIDE 沿用工程已有的 hard-float 选项，不与 CMake 产物混合链接。
本次已实际验证 CMake 构建，未运行 CubeIDE 的 managed build。

CubeMX 重新生成可能重新加入裸机的 SVC/PendSV 和 main 初始化调用，需要保留当前 RTOS 入口修改。
`SystemInit()` 的 VTOR 设置、384 KiB Flash 链接分区也必须保留。

## 主机验证

在 APP 根目录执行 `./tests/run_tests.ps1`。脚本编译生产版协议、升级服务和同级 Bootloader 核心，
使用 NOR Flash 模型覆盖立即/延期安装、重试、乱序、取消、超时、非法入口和擦写失败，
再运行 Python 客户端帧格式及超时重试测试。默认主机 C 编译器为 MSYS2 UCRT64 GCC。
这组模型测试不操作串口，也不烧录硬件。
