# Bootloader / APP 实板验证记录

验证日期：2026-10-22。通过 ST-Link / SWD 操作实际连接板卡，未执行整片擦除。
工程目标为 STM32F405RGT6；工具识别 Device ID `0x413`、Rev Y、1 MiB Flash、Cortex-M4，
目标电压约 3.20 V。使用 STM32CubeProgrammer 2.23.0 和 OpenOCD 0.12.0。

## 固件

| 项目 | 本次验证值 |
| --- | --- |
| Bootloader 长度 | 3956 字节 |
| APP 长度 | 8848 字节 |
| APP CRC-32/ISO-HDLC | `0x221E4953` |
| 首次装机/最终恢复版本 | 1 |
| 暂存升级测试版本 | 2（相同 APP 内容，不同描述信息版本，强制执行安装） |
| 合并烧录文件 | `cmake-build-debug/firmware.factory.hex` |

烧录前已备份全片 Flash：`build/hardware-validation/flash-before.bin`（1048576 字节）。
SHA-256：`FB87711DAD1A474BD35D545867A003C772238F5B1F49C555C3F5F79E2FB1061A`。

## 验证结果

1. **首次装机通过。** 合并 HEX 经按扇区擦除、下载、读回校验成功。
   硬件复位后，在 Bootloader `BootPort_JumpApplication` 的 `0x08000824` 命中硬件断点，
   此时 `boot_status=BOOT_READY`；继续运行，在 APP `main` 的 `0x08020670` 命中断点。
2. **启动状态通过。** APP main 入口 `MSP=0x20020000`，`VTOR=0x08020000`。
   初始化后 `SystemCoreClock=168000000`、SysTick CTRL 低三位为 7；
   `uwTick` 持续增加，PA8 ODR 位 8 的高、低电平均被采到。
   每次明确复位并进入 APP 后，采样的 `CFSR` 和 `HFSR` 均为 0。
3. **暂存安装通过。** 下载版本 2 的暂存 HEX，仅编程请求区 sector 3 和暂存区 sector 8。
   复位后在 `BootPort_EraseInstalled` 命中断点，确认实际进入安装路径；
   运行至跳转前，已安装版本变为 2，请求 consumed 从 `0xFFFFFFFF` 变为 0。
   随后成功进入 APP main，并继续运行心跳。
4. **Flash 读回通过。** 全片读回至 `build/hardware-validation/flash-after-update.bin`。
   运行区和暂存区的实际固件均逐字节匹配 APP.bin；已安装描述信息匹配版本 2 的生成结果，
   请求描述信息前 28 字节匹配、consumed 为 0。APP 区剩余空间全为 `0xFF`，符合完整擦除后安装的结果。
5. **复位不重复安装。** 同时在安装擦除函数和 APP 跳转函数设置断点，复位后先命中跳转函数，
   未进入擦除函数；版本仍为 2，consumed 仍为 0，APP 正常启动。
6. **保护区通过。** 升级后 Bootloader 内容逐字节匹配本次构建；
   sectors 1、4、11 及本次未编程的暂存 sectors 9–10 均与烧录前备份一致。
7. **最终状态。** 已重新烧录并校验当前构建的版本 1 合并固件，再次验证完整启动链路、
   时钟、SysTick、PA8 输出和故障寄存器。已移除测试硬件断点并恢复运行，板卡留在 APP 中。
   版本 2 测试暂存数据仍在 sector 8，但请求已取消，不会自动再次安装。

## 日志与边界

原始日志和断点脚本保存在本机 `build/hardware-validation/`，该目录由 Git 忽略：

- `startup-factory.log`：首次装机后的复位启动和运行状态。
- `program-update-v2.log` / `staging-install.log`：暂存下载、实际安装和 APP 启动。
- `repeat-reset.log`：已处理请求不重复安装。
- `restore-factory-v1.log` / `final-v1.log`：恢复当前版本、最终启动和留板状态。

暂存包下载命令没有附带 reset/run，下载后首次调试接管曾见到 HardFault 上下文；
本次验收以脚本明确复位后的启动为准，此后 APP 的故障寄存器检查均通过。
烧录后应显式复位，不从烧录过程残留的 CPU 上下文直接继续执行应用。

PA8 验证来自 GPIO 输出寄存器，未使用光学测量观察 LED。
本次未执行实际切断供电、在线下载、SPI/串口通信或长时间稳定性测试。
既有主机断电模型测试不能替代实际断电实验。
