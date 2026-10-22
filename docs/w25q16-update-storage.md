# W25Q16 升级存储规范 v1

本版本只修改 APP。Bootloader 工程、内部 Flash 分区及安装逻辑保持原样。
APP 通过 USART1 接收升级包，将升级描述信息和固件数据全部存入外部 W25Q16，
接收、校验、提交和取消期间均不擦写 STM32 内部 Flash。

**当前 Bootloader 不认识外部存储。`now` 或 `reboot` 会复位，但不会安装 W25Q16 中的固件；
复位后仍运行原来的 APP，查询到的外部待升级状态保持 PENDING。**
后续需另行实现 Bootloader 读取 SPI Flash，才能完成外部暂存到内部运行区的安装。
当前版本部署使用 `APP.installed.hex` 经 SWD 更新 APP 和已安装描述信息，保留 Bootloader。

## 硬件和 DMA

| 功能 | 引脚 / 配置 |
| --- | --- |
| SPI1 SCK | PA5 |
| SPI1 MISO | PA6 |
| SPI1 MOSI | PA7 |
| Flash /CS | PA2，F_CS，低有效；GPIO 初始化即拉高 |
| SPI 模式 | Mode 0，8 位，MSB first，84 MHz / 8 = 10.5 MHz |
| SPI1 RX / TX DMA | DMA2 Stream0 / Stream3，channel 3，normal |
| USART1 RX / TX | PA10 / PA9，115200，8N1 |
| USART1 RX DMA | DMA2 Stream2，channel 4，circular，512 字节环形缓冲 |
| USART1 TX DMA | DMA2 Stream7，channel 4，normal |
| 设备 IRQ 优先级 | SPI1、USART1、四路 DMA 均为 6，可调用 FreeRTOS FromISR API |

W25Q16 与 STM32 共地，使用与实际芯片后缀匹配的电源；本驱动按 W25Q16JV/BV 等标准 SPI 指令实现，
JV 为 2.7–3.6 V。使用标准 SPI 时 /WP、/HOLD 应保持高电平。
驱动先读取 JEDEC ID，要求 manufacturer 为 `0xEF` 且 density 为 `0x15`（16 Mbit），
完整 ID 可通过调试器查看 `g_w25q16_jedec_id`，常见值为 `0xEF4015`。
型号不匹配或设备无响应时不擦除外部 Flash，协议返回 FLASH_ERROR，心跳任务继续运行。

SPI 的命令、数据和状态读取均使用全双工 DMA，事务期间 /CS 保持低电平。
SPI 事务和 UART TX 各有独立完成信号量，不共用任务通知。
SPI 超时后先终止 DMA，再释放 /CS，避免局部栈或共享缓冲仍被 DMA 使用。
SPI TX/RX 和 UART RX 缓冲都位于普通 SRAM，不使用 DMA 无法访问的 CCM。

外置 Flash 擦除、编程由芯片独立执行；更新任务读取 BUSY 位，忙时 `vTaskDelay(1)`，
不会调用 `vTaskSuspendAll()`。其他任务可以继续运行，LED 不再因内部 Flash 擦除而停闪。
USART1 用 HT/TC/IDLE 事件搬运环形缓冲到 2048 字节 stream buffer，回调读取实时 DMA 位置，
避免延迟的 HT 与 IDLE 事件顺序导致误判回绕。软件缓冲溢出会丢弃当前解析状态并重新启动 RX。

## 分区表

以下均为 W25Q16 内部的**字节偏移**，不是 STM32 地址。容量为 2 MiB，擦除单位 4 KiB。
常量集中定义在 `Config/update_storage_layout.h`。

| 偏移范围（含首尾） | 容量 | 用途 |
| --- | --- | --- |
| `0x000000–0x000FFF` | 4 KiB | 升级请求；前 64 字节为描述记录，其余保持擦除态 |
| `0x001000–0x00FFFF` | 60 KiB | 保留，当前不擦写 |
| `0x010000–0x06FFFF` | 384 KiB | 候选 APP 原始 BIN，无文件头 |
| `0x070000–0x1FFFFF` | 1600 KiB | 保留，当前不擦写 |

BEGIN 先擦除请求区，然后从固件区起始偏移起擦除 `ceil(image_size / 4096)` 个扇区。
例如 23,520 字节需擦除 6 个数据扇区；加入外部驱动后应按新 BIN 的实际长度重新计算。
固件长度必须在 8–393216 字节内；未使用的数据扇区保持原内容，下次大固件会按新长度覆盖所需扇区。
数据从相对 offset 0 开始顺序接收，不能写入保留区。

## 64 字节升级描述记录

所有字段为小端 uint32。前 32 字节沿用 `BootImageManifest` 的字段定义，
后 32 字节定义外部存储格式和提交策略。保留字段均为 `0xFFFFFFFF`。

| 偏移 | 字段 | 值 / 说明 |
| --- | --- | --- |
| 0 | magic | `0x424C4657` |
| 4 | format | 1，原固件描述格式 |
| 8 | image_size | 原始 BIN 长度，不含任何对齐字节 |
| 12 | image_crc | BIN 的 CRC-32/ISO-HDLC，与 zlib.crc32 一致 |
| 16 | version | 上传时指定的版本标记，不限制递增 |
| 20 | header_crc | 记录 bytes 0–19 的 CRC-32 |
| 24 | commit | `0xB007C0DE`，最后单独写入 |
| 28 | consumed | 当前 APP 保持 `0xFFFFFFFF`；未来安装端使用 |
| 32 | storage_magic | `0x31505557`，小端 ASCII `WUP1` |
| 36 | layout_version | 1 |
| 40 | image_offset | `0x010000` |
| 44 | image_capacity | `0x060000` |
| 48 | apply_mode | 0：延期复位；1：ACK 发送后主动复位 |
| 52 | reserved | `0xFFFFFFFF` |
| 56 | metadata_crc | 拼接 bytes 0–23 和 bytes 28–55 后计算 CRC-32；不含 commit 和本字段 |
| 60 | reserved_tail | `0xFFFFFFFF` |

APP 当前不生成“外部已安装版本”记录，因为没有安装端可以确认安装结果。
QUERY 的 `running_version` 仍只读内部 `0x08008000` 的已安装描述信息；
这是现有 Bootloader 提供的运行身份，不是新写入的升级请求，也不会因收到外部包而变更。

## 写入与恢复顺序

1. BEGIN 校验长度，先擦除请求扇区并检查读回为全 FF，撤销旧请求。
2. 只擦除新固件所需的 4 KiB 数据扇区，每个扇区擦后读回检查。
3. DATA 每次接收不超过 1024 字节，驱动自动按 256 字节页边界拆分 Page Program，
   每页前 Write Enable，完成后读回比较。最后一块可非 4 字节长度，不额外写入填充。
4. END 分块读回整个固件并增量计算 CRC，再检查 MSP 和 Thumb 复位入口；不将整个固件读入 RAM。
5. 确认记录区为空，先写 bytes 0–23，再写 bytes 28–63；每次写后比较读回。
6. 最后单独写 bytes 24–27 的 commit，读回成功后才回复 END 成功。
7. mode=0 保持运行；mode=1 等 USART1 DMA 发送完成且 TC 触发，再延迟 50 ms 并系统复位。

APP 启动时只读外部存储，不擦除请求或固件。仅当存储布局、两层元数据 CRC、commit、
固件 CRC 和向量表均有效时恢复 PENDING。不完整接收不支持断点续传，重新 BEGIN 从头接收。
ABORT 只擦除外部请求扇区，不擦固件数据；未提交的接收会话 60 秒无有效帧后结束。
复位后不会因 apply_mode=1 自动再次复位，避免在现有 Bootloader 未接入时形成复位循环。

擦除请求区之前发生故障，原待升级包可能仍然有效；一旦请求区已擦除，后续失败不会恢复旧请求。
若 commit 已写入但 ACK 丢失，设备可能已是 PENDING，主机应先 QUERY，不要直接重新 BEGIN。
此方案只保留一个候选包，替换候选包时不承诺保留上一个候选包；当前运行 APP 始终不被擦写。

## 验证与边界

`tests/run_tests.ps1` 链接实际服务和 `update_storage.c`，通过 NOR 模型检查分区、4 KiB 边界、
最大 384 KiB、大小固件替换、重复数据、读取失败、缺失设备、元数据损坏及 133 个擦除/逐字节编程故障点。
同时链接原 Bootloader 核心，确认外部待升级包不会触发旧 Bootloader 安装或内部 Flash 改写。
W25Q16 驱动测试覆盖 JEDEC、跨页写入、BUSY 让出调度、超时和 DMA 错误；UART 测试覆盖环形回绕、
HT/IDLE 乱序、1044 字节完整帧、溢出及 TX/RX 恢复。

当前已通过上述主机测试及 Debug 构建；BIN 为 28,592 字节，链接 RAM 占用 21,648 字节，CCM 为 0。
编译器栈分析已检查主要调用路径，不能替代实际运行时的高水位测量。
实板尝试能识别 ST-Link，但普通连接及复位下连接都无法读取 MCU 内核，
尚未验证真实芯片 JEDEC、DMA 波形、LED 周期、任务栈水位及长时间通信稳定性。
COM4 查询仍能返回板上原运行版本 2；此次未烧录新的外部存储版本或执行实板 W25Q16 擦写。
此前内部 Flash 升级的硬件记录属于历史验证，不代表本外部存储版本已通过实板验证。

参考：[Winbond W25Q16JV 数据手册](https://www.mouser.com/catalog/specsheets/Winbond%20Electronics%20Corporation_08-25-2025_W25Q16JV.pdf)。
