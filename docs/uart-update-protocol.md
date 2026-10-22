# USART1 固件更新协议 FWUP v1

## 串口与交互

USART1：PA9/TX、PA10/RX、115200 baud、8N1，无硬件流控。
UART1 专用于二进制协议，不输出文本日志。电脑 TX 接 PA10、RX 接 PA9，并共地。

所有多字节整数均为小端。上位机严格采用 stop-and-wait：发送一帧，等相同序号和对应命令的 ACK，
收到后再发送下一帧。固件为按 `0x08020000` 链接的原始 APP.bin，无额外固件文件头，最大 393216 字节。
帧 CRC 和固件 CRC 均为 CRC-32/ISO-HDLC，与 Python zlib.crc32 相同。

## 帧结构

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | 魔数 ASCII `FWUP`，十六进制 `46 57 55 50` |
| 4 | 1 | 协议版本，固定 1 |
| 5 | 1 | 命令；应答命令为请求命令 OR 0x80 |
| 6 | 2 | payload 字节数，范围 0–1028 |
| 8 | 4 | sequence，上位机生成的帧标识，ACK 原样返回 |
| 12 | 4 | CRC-32，覆盖本帧偏移 4–11 的 8 字节，随后接整个 payload；不含魔数和 CRC 字段 |
| 16 | N | payload |

设备按字节解析，支持任意 UART 分片。错误魔数、协议版本、超长帧或帧 CRC 错误不会执行命令；
错误帧静默丢弃，由上位机超时重试。半帧超过 1 秒无新字节时清空解析状态。
接收会话 60 秒无完整有效帧则终止，未提交固件不会安装；已提交的延期请求不因超时取消。

## 命令

| 值 | 命令 | 请求 payload | 行为 |
| --- | --- | --- | --- |
| 0x01 | QUERY | 空 | 查询运行版本、会话状态及接收进度 |
| 0x02 | BEGIN | size:u32、image_crc:u32、version:u32 | 校验长度后先撤销旧请求，再擦除暂存区，开始新会话 |
| 0x03 | DATA | offset:u32、data:1–1024 字节 | 顺序写暂存区，成功后 ACK 下一偏移 |
| 0x04 | END | mode:u8 | 完整校验并提交请求；mode=0 延期，mode=1 ACK 发完后自动复位 |
| 0x05 | ABORT | 空 | 擦除请求区，取消会话或延期升级；暂存内容保留，但不再有有效请求 |
| 0x06 | REBOOT | 空 | 仅在有已提交延期请求时有效；ACK 发完后复位并由 Bootloader 安装 |

DATA 的 offset 从 0 开始且必须 4 字节对齐。除最后一块外，data 长度必须是 4 的倍数。
最后一块允许不足 4 字节，Flash 编程按 0xFF 补齐，但固件 CRC 不包括对齐填充。
超过声明长度、跳过偏移、重传内容不一致均拒绝。重复已写入的数据块在内容一致时只回复 ACK，不重复编程。

相邻完全相同的 command/sequence/payload 请求返回缓存 ACK，不重复擦除、写入或提交。
相同 sequence 携带不同请求内容返回 BAD_SEQUENCE。sequence 是重试标识，不用于身份认证，
每个新请求应递增（可按 u32 回绕）；重试必须保持完全相同的帧。
客户端默认每帧等待 20 秒，超时原帧重发，最多 3 次。BEGIN 擦除耗时较长，切勿缩短到普通串口字节超时。

## 应答 payload（固定 28 字节）

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 1 | 原请求命令 |
| 1 | 1 | status，0 表示成功 |
| 2 | 1 | state：0 IDLE、1 RECEIVING、2 PENDING |
| 3 | 1 | reset：1 表示本次 ACK 发完后将主动复位 |
| 4 | 4 | received，已确认暂存字节数 |
| 8 | 4 | size，当前升级声明的长度 |
| 12 | 4 | version，当前升级包版本 |
| 16 | 4 | image_crc，当前升级包 CRC |
| 20 | 4 | chunk，最大 DATA 数据长度，当前为 1024 |
| 24 | 4 | running_version，启动时已安装描述信息中的版本 |

IDLE 状态不应把升级字段当作有效待安装固件；判断当前程序版本应读取 running_version。
END 成功只代表完整固件及安装请求已持久化，安装发生在随后复位后的 Bootloader 中。

| status | 名称 | 处理建议 |
| --- | --- | --- |
| 0 | OK | 发送下一帧 |
| 1 | BAD_COMMAND | 使用支持的命令 |
| 2 | BAD_LENGTH | 检查 payload、固件长度和 END 模式 |
| 3 | BAD_STATE | QUERY 后重新 BEGIN，或检查是否已提交 |
| 4 | BAD_OFFSET | 检查 received、对齐和重传内容 |
| 5 | BAD_CRC | 暂存读回 CRC 不匹配，重新 BEGIN 完整下载 |
| 6 | FLASH_ERROR | 擦写/校验失败，QUERY 确认状态后重新 BEGIN；检查供电 |
| 7 | BAD_IMAGE | MSP、Thumb 复位入口或运行区链接地址非法 |
| 8 | BAD_SEQUENCE | 同序号被用于不同请求，换新序号 |

## 提交与重启策略

1. BEGIN 先擦除 sector 3，撤销此前请求；再擦除 sectors 8–10。
2. DATA 仅写 `0x08080000` 开始的暂存区，每块写后读回检查。
3. END 校验完整读回 CRC、栈指针及复位入口，生成与 Bootloader 相同的 32 字节描述信息。
4. 先写请求偏移 0–23，最后写 offset 24 的 commit=`0xB007C0DE`；consumed 保持 `0xFFFFFFFF`。
5. mode=0：ACK 后继续运行旧 APP，下次任何系统复位都会安装；mode=1：UART TC 完成并等待 50 ms 后调用系统复位。
6. Bootloader 安装、校验、更新已安装信息，然后将 consumed 置为 0，再启动 APP。

安装包的版本只作为识别字段，不强制递增，也没有自动回滚策略。CRC 检测传输和存储损坏，
本协议未实现签名或发送端身份认证。
未完整提交时断电，旧 APP 不受影响；commit 已成功写入后即使 ACK 丢失，下一次复位仍可能安装。
因此 END 超时后应先 QUERY，避免直接发送新的 BEGIN 抹掉已提交的延期升级。

单 Bank Flash 擦写会阻塞 CPU，暂停正常实时任务；接收期间必须等待 ACK。
32 位 Flash 编程要求板卡供电 2.7–3.6 V。本次实板供电约 3.20 V。

## 上位机示例

Windows 可以直接使用系统 Python，脚本自带 Win32 串口适配，无需安装额外包。
其他系统安装 pyserial 后可使用相同 CLI。关闭其他占用 COM4 的串口终端。

```powershell
# 查询
python -B tools/uart_update.py --port COM4 query

# 接收完成后继续运行当前 APP，下次复位安装
python -B tools/uart_update.py --port COM4 upload build/freertos/APP.bin --version 2 --mode defer

# 接收完成后自动重启安装
python -B tools/uart_update.py --port COM4 upload build/freertos/APP.bin --version 3 --mode now

# 取消延期升级
python -B tools/uart_update.py --port COM4 abort

# 主动触发已经延期提交的升级
python -B tools/uart_update.py --port COM4 reboot
```

立即模式之后给 Bootloader 留出 Flash 安装时间，再 QUERY 确认 running_version。
默认 QUERY 的重试超时足以覆盖本工程的安装时长。
