# FreeRTOS / USART1 在线升级实板验证

日期：2026-10-22。工程目标 STM32F405RGT6，实板 Device ID 0x413，供电约 3.20 V。
通过 ST-Link/SWD 烧录初始框架；实际升级数据通过用户确认的 COM4 / USART1 发送，
不是由调试器直接写入暂存区。未执行整片擦除。

## 构建和框架

- FreeRTOS V11.1.0，GCC ARM_CM4F，heap_4 64 KiB。
- CMake Debug 和 Release 均构建通过；Debug 的 Ninja 及现有 CLion/MinGW Makefiles 构建均通过。
- CubeIDE 的源码层级和 include paths 已更新，但未执行 CubeIDE managed build。
- Debug APP.bin 为 23504 字节，CRC32 `0x68C90B70`，RAM 链接占用 69768 字节。
- Release APP.bin 为 26060 字节，仅完成构建，未进行 Release 实板烧录。
- Debug SHA-256：`430850464E72A7D1E9B582B88E9DDB255BA2165AB1697A80AD26F397D7C489A0`。
- `cmake-build-debug/APP.bin` 与 `build/freertos/APP.bin` 完全相同。

在板上确认 xSchedulerRunning=1、uxCurrentNumberOfTasks=4。
心跳任务持续运行；约 1.1 秒观察窗内 RTOS tick 增加 1109–1113，心跳增加 2–3。
VTOR=`0x08020000`，SVC/PendSV 向量指向 FreeRTOS 端口，SysTick 指向 HAL/RTOS 共用入口。
各检查点的 malloc/栈溢出诊断值、UART 接收错误计数、CFSR、HFSR 均为 0。

## 延期升级（真实串口）

1. 从运行版本 1 开始，使用工具向 COM4 发送 APP.bin，声明版本 2，END mode=0。
2. 应答：state=2、reset=0、received=23504、version=2、running_version=1。
3. 再次 QUERY 及 SWD 检查：旧版仍运行、调度器和心跳正常。
   已安装描述信息版本仍为 1，请求版本为 2、commit 有效、consumed=`0xFFFFFFFF`。
4. 使用外部硬件复位，等待 Bootloader 安装后 QUERY：running_version=2、state=0。

## 立即升级及异常帧（真实串口）

使用 `tests/hardware_uart_update.py` 从运行版本 2 上传版本 3：

- 注入噪声和 CRC 错误帧，随后正常 QUERY 成功，原运行状态未改变。
- 重复完全相同的 BEGIN，返回同样的 ACK。
- 第一块 DATA 分别按原 sequence 和新 sequence 重发，received 不增加。
- 上传末尾补入三个 0xFF 的 APP（23507 字节），覆盖不足 4 字节的最后一次 Flash 编程。
- 先发送 END mode=2，收到 BAD_LENGTH；再发送 mode=1，收到 reset=1 的成功 ACK。
- 上位机没有发复位命令，APP 在应答发送完成后自行系统复位。
- 安装后 QUERY 返回 running_version=3，RTOS 四个任务和心跳均正常。

## Flash 读回与保护区

升级后通过 SWD 读回全片 Flash，确认：

- 运行区和暂存区的 23507 字节均逐字节匹配真实 UART 上传数据。
- CRC32=`0xAA4234E7`；已安装及请求描述信息的长度、CRC、版本 3、头 CRC 和 commit 均一致。
- consumed=0，运行区剩余空间为 0xFF。
- Bootloader 内容匹配本次构建，保留 sectors 1、4、11 与本次升级前备份一致。

## 主机测试

`tests/run_tests.ps1` 全部通过。生产版协议/服务与生产版 Bootloader 核心共同参与测试，覆盖：
帧分片、噪声、最大帧、错误 CRC、超长帧、重复 BEGIN/DATA、序号冲突、非法偏移、
不完整固件、超时、取消、非法向量、28 个接收侧擦写失败点、完整 384 KiB 下载/安装和读回 CRC 拒绝。
Python 客户端另验证线格式、帧重组和 ACK 丢失后的原帧重试。

## 最终板卡与产物

已重新烧录当前构建的版本 1 `APP.installed.hex`，并读回校验。
最终 QUERY：state=0、running_version=1；调度器、4 个任务、心跳、UART 和故障状态检查通过，
APP 留在运行状态。暂存区保留测试版本 3，但其请求已取消，不会自动再次安装。

首次装机文件：`cmake-build-debug/firmware.factory.hex`。
原始固件：`cmake-build-debug/APP.bin`。
使用方式与帧结构见 [串口升级协议](uart-update-protocol.md)。

本机日志、断点脚本和 Flash 备份保存在被 Git 忽略的 `build/freertos/hardware/`：
`freertos-start.log`、`uart-defer.log`、`defer-still-old.log`、`uart-immediate.log`、
`immediate-running.log`、`readback-validation.txt`、`restore-final-v1.log` 和 `final-running-v1.log`。
未进行实际断电中断擦写、长时间负载稳定性或其他业务外设通信测试。
