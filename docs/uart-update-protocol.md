# USART1 固件接收协议 FWUP v1

当前 APP 把固件和升级信息写入外部 W25Q16，帧格式兼容原 FWUP v1。
配套启动程序由独立的 [STM32f_405_boot_loader 项目](https://github.com/CHN-wang-zhuo/STM32f_405_boot_loader) 维护。
**现有 Bootloader 未修改，不能安装外部固件。END 成功只表示外部候选包已完整提交，
`now` 或 `reboot` 会复位但仍启动旧 APP。** 分区与元数据格式见 [W25Q16 存储规范](w25q16-update-storage.md)。

## 串口与交互

USART1：PA9/TX、PA10/RX、115200 baud、8N1、无流控；电脑 TX 接 PA10，RX 接 PA9，并共地。
UART1 专用于二进制协议，无文本日志；使用 circular RX DMA + HT/TC/IDLE 和 normal TX DMA。
所有多字节整数均为小端。发送一帧后必须等待相同序号、对应命令的 ACK 再发下一帧。
BIN 按 `0x08020000` 链接，无额外文件头，最大 393216 字节。
帧与固件 CRC 均为 CRC-32/ISO-HDLC，与 Python zlib.crc32 一致。

## 帧结构

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | 魔数 ASCII `FWUP`（46 57 55 50） |
| 4 | 1 | 协议版本 1 |
| 5 | 1 | 命令，应答为请求命令 OR 0x80 |
| 6 | 2 | payload 长度，0–1028 |
| 8 | 4 | sequence，ACK 原样返回 |
| 12 | 4 | CRC，覆盖 bytes 4–11 后接 payload，不含魔数和 CRC 字段 |
| 16 | N | payload |

支持任意 UART 分片。错误魔数、版本、长度或帧 CRC 的帧静默丢弃，由主机超时重试。
半帧 1 秒无新字节则丢弃；接收会话 60 秒无有效帧则结束。已提交 PENDING 请求不随会话超时取消。

## 命令

| 值 | 命令 | 请求 payload | 行为 |
| --- | --- | --- | --- |
| 0x01 | QUERY | 空 | 查询当前运行版本及外部候选包状态 |
| 0x02 | BEGIN | size:u32、image_crc:u32、version:u32 | 先撤销外部旧请求，擦除固件所需的 4 KiB 扇区 |
| 0x03 | DATA | offset:u32、data:1–1024 字节 | 顺序写 W25Q16、读回比较后 ACK |
| 0x04 | END | mode:u8 | 全量读回 CRC、向量检查并提交外部记录；0 保持运行，1 应答后复位 |
| 0x05 | ABORT | 空 | 擦除外部请求扇区，保留固件数据 |
| 0x06 | REBOOT | 空 | 仅 PENDING 时有效，应答后系统复位；现有 Bootloader 不安装外部包 |

DATA offset 从 0 开始且须 4 字节对齐；除最后一块外，data 长度须为 4 的倍数。
最后一块可不足 4 字节，外置 Flash 按真实长度写入，不额外填充。驱动自动拆分 256 字节页，不能跨页直接编程。
超出声明长度、跳过偏移、重复数据不一致均拒绝；重复已写入块读回一致时只 ACK，不重新编程。

相邻相同 command/sequence/payload 返回缓存 ACK，不重复擦写；相同 sequence 但内容不同返回 BAD_SEQUENCE。
新请求 sequence 递增并可 u32 回绕，重试必须使用完全相同的帧。客户端每帧默认等待 20 秒，最多尝试 3 次。
BEGIN 需擦除多个外部扇区，不能把等待时间缩成串口字符间隔；W25Q16 忙时其他任务仍可调度。

## 应答 payload（固定 28 字节）

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 1 | 原命令 |
| 1 | 1 | status，0 表示成功 |
| 2 | 1 | state：0 IDLE、1 RECEIVING、2 PENDING |
| 3 | 1 | reset：本次 ACK 后是否主动复位 |
| 4 | 4 | received，已确认外部存储字节数 |
| 8 | 4 | size，候选包长度 |
| 12 | 4 | version，候选包版本 |
| 16 | 4 | image_crc，候选包 CRC |
| 20 | 4 | chunk，当前最大 DATA 数据 1024 字节 |
| 24 | 4 | running_version，只读现有 Bootloader 的内部已安装版本 |

IDLE 时不要将候选包字段当作有效固件。上传版本 2 后，`state=2, version=2` 表示外部请求已提交；
`running_version` 不会随上传变成 2，必须由实际安装端完成安装后才有变化。
缺失芯片或 SPI 初始化失败时 QUERY 也返回 FLASH_ERROR，更新任务和心跳不会因此断言停止，后续请求会重试探测。

| status | 名称 | 处理建议 |
| --- | --- | --- |
| 0 | OK | 发送下一帧 |
| 1 | BAD_COMMAND | 检查命令 |
| 2 | BAD_LENGTH | 检查长度或 END mode |
| 3 | BAD_STATE | QUERY 后检查接收状态或重新 BEGIN |
| 4 | BAD_OFFSET | 检查 offset、对齐和重复内容 |
| 5 | BAD_CRC | 候选固件 CRC 不匹配，重新 BEGIN 完整下载 |
| 6 | FLASH_ERROR | 检查 W25Q16 接线、型号、保护状态、供电、DMA 和读回 |
| 7 | BAD_IMAGE | MSP、Thumb 入口或链接地址非法 |
| 8 | BAD_SEQUENCE | 同序号用于不同请求，换新序号 |

## 提交、复位与 LED

BEGIN 先擦除 W25Q16 偏移 0 的请求扇区，再从 `0x010000` 起擦除 `ceil(size/4096)` 个扇区。
END 分块读回固件并校验 CRC、MSP 和复位入口，先写元数据，最后单独写 commit。
全部成功才回复 PENDING；启动后只有完整记录及固件都通过校验才恢复该状态。
版本只作标记，不强制递增；CRC 检查损坏，本协议未实现签名、身份认证或自动回滚。

`defer` 保存 apply_mode=0 并继续运行；`now` 保存 apply_mode=1，在 UART DMA 完成和 TC 后再等 50 ms 复位。
APP 启动时不自动执行保存的 now 策略，因此现有 Bootloader 未接入也不会循环复位。
Bootloader 仍只支持原内部暂存区，当前外部请求即使复位也不会被安装或标为已消费。

历史内部暂存版 LED 停闪的原因是全局暂停调度及 STM32F405 单 Bank 内部 Flash 的取指阻塞。
本版所有接收擦写改为外置芯片，SPI DMA 等待阻塞当前任务，BUSY 时用 `vTaskDelay(1)` 让出 CPU，
不暂停全局调度，心跳可以继续执行。主机可能仍等待擦除 ACK，但这不代表 CPU 被 Flash 卡住。
旧版本硬件验证不适用于本版；真实 LED 周期及 DMA 稳定性仍需上板测量。

## 上位机示例

先在 CLion 编译 `APP.elf`。部署本版接收程序使用 SWD 烧录 `APP.installed.hex`，保留 Bootloader。
关闭其他占用 COM4 的终端，进入 APP 根目录执行；其他电脑请替换 Python 路径。

```powershell
$python = "C:\msys64\ucrt64\bin\python.exe"
& $python -u -B tools/uart_update.py --port COM4 query

# 保存候选包，当前版本推荐使用 defer
& $python -u -B tools/uart_update.py --port COM4 upload cmake-build-debug/APP.bin --version 2 --mode defer

# 提交后复位；当前 Bootloader 不会安装外部包
# & $python -u -B tools/uart_update.py --port COM4 upload cmake-build-debug/APP.bin --version 2 --mode now

& $python -u -B tools/uart_update.py --port COM4 query
# 如需复位已有 PENDING 请求
# & $python -u -B tools/uart_update.py --port COM4 reboot
# 如需取消候选包
# & $python -u -B tools/uart_update.py --port COM4 abort
```

上传显示 `Uploaded 已上传字节/总字节`，成功结果 `status=0, state=2, version=2`；
`running_version` 仍为原版本。END 超时后先 QUERY，不要重复 BEGIN 覆盖已提交请求。
Windows 脚本自带 Win32 串口适配；其他系统安装 pyserial。

## Python 执行后无输出

正常查询会打印状态，串口失败会打印错误。如果命令立即无输出返回，检查解释器别名和 PATH：

```powershell
Get-Command python -All | Format-List CommandType,Source,Definition
& "C:\msys64\ucrt64\bin\python.exe" -c "import sys; print(sys.executable)"
& "C:\msys64\ucrt64\bin\python.exe" -u -B tools/uart_update.py --port COM4 query
$LASTEXITCODE
```

`-u` 禁用输出缓冲，`-B` 禁止字节码缓存。优先执行到 WindowsApps 下的 python.exe 时，请指定真实解释器。
