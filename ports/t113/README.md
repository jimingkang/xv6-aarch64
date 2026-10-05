# 100ask T113 Pro：ARM32 xv6 串口 shell

当前分支的第一阶段移植：U-Boot 初始化 DDR 和调试 UART，加载独立的
ARMv7-A 镜像，进入 xv6 用户态 `/bin/sh`。使用原有的进程、调度、文件系统、
日志、管道和 shell 代码；根文件系统嵌入内核，读写发生在 RAM 中。

**验证状态：Cortex-A7 QEMU 测试通过；尚未接入 T113 实板验证。**
QEMU 使用 `virt` 平台的 PL011/GIC/物理定时器，T113 镜像使用 UART0、
GIC-400 和全志 timer0。QEMU 的通过证明 ARM32 内核/用户态链路，不能证明
T113 的时钟、中断路由及 U-Boot 交接已经在板上工作。

## 构建

在仓库根目录执行：

```sh
make t113
make t113-check
```

清理 T113 实板与 QEMU 的构建产物：

```sh
make t113-clean
make t113
```

也支持 `make t113 clean`，它会先清理再重新编译；即使加上 `-j`，
清理也会在编译之前完成。单独的 `make clean` 仍是原有树莓派清理入口。

产物：

- `build/t113/xv6-t113.bin`：加载到 **0x40200000** 的实板裸镜像，约 4 MiB。
- `build/t113/xv6-t113.uImage`：同一裸镜像加上 legacy U-Boot 内核头，供
  支持 legacy 格式的 `bootm` 使用，头及数据 CRC 均由打包脚本校验。
- `build/t113/kernel.elf`：带符号的 ARM ELF32/EABI5 文件。
- `build/t113/fs.img`：4 MiB xv6 原生文件系统，已经包含在裸镜像中。
- `build/t113-qemu/shell-test.log`：自动测试串口记录。

工具链默认使用 `clang --target=arm-none-eabi`，以及支持 `armelf` 的 GNU ld
和 objcopy。本机的 `aarch64-elf-binutils` 同时支持 ARM32 链接。
也可使用 ARM32 GCC 工具链：

```sh
make -f ports/t113/Makefile ARM_CC=arm-none-eabi-gcc TARGET_FLAGS= \
  ARM_LD=arm-none-eabi-ld ARM_OBJCOPY=arm-none-eabi-objcopy
```

交互式模拟：

```sh
make t113-qemu
```

两种镜像使用不同构建目录。**不要把 `build/t113-qemu` 的镜像复制到实板。**
QEMU 可通过 `QEMU_ARM` 指定路径。测试覆盖原始 BIN 加载、init/sh、fork/exec/wait、
管道、重定向、目录操作、定时器休眠、用户态访问内核内存被拒绝、异常进程退出、
动态进程创建/回收和 stressfs。

## TF 卡上已有 U-Boot，或从板载 Flash 启动 U-Boot

用户提供的厂商 U-Boot 命令清单没有 `tftpboot`、`tftp`、`go`、`dcache`
和 `icache`。对于这一版本，下面的裸镜像 `go` 流程不适用。先使用
`fatload mmc 0:1 0x42000000 /xv6-t113.uImage`，再检查 `help bootm`、
`version`、`printenv bootcmd` 和 `bdinfo`。加载地址 0x42000000 是暂存地址；
镜像头指定的最终地址仍为 0x40200000。标准 ARM Linux `bootm` 交接会执行
缓存/MMU 清理，但厂商版本是否支持 legacy 镜像、是否强制要求设备树，以及
是否会关闭调试 UART 时钟，均尚未验证。必须据实际输出确定 `bootm` 参数；
不能把 `.uImage` 文件直接当裸镜像从暂存地址执行。

保留现有启动布局。把 `xv6-t113.bin` 作为普通文件放在 U-Boot 能读取的
FAT 分区根目录即可；不是用 `dd` 把它写到整张卡。
在 macOS 上，例如 FAT 分区挂载名为 `T113BOOT`：

```sh
cp build/t113/xv6-t113.bin /Volumes/T113BOOT/xv6-t113.bin
sync
```

`T113BOOT` 是示例名称，使用实际挂载目录。弹出 TF 卡，插入开发板，
通过调试串口进入 U-Boot。波特率沿用能正常显示 U-Boot 的设置，通常是
115200、8N1。先查看设备和分区：

```text
mmc list
mmc dev 0
mmc rescan
part list mmc 0
fatls mmc 0:1 /
bdinfo
```

`0:1` 表示 MMC 设备 0 的第 1 个分区，**须按实际输出替换**。
确认 `fatls` 能看见镜像，`bdinfo` 显示 DRAM 包含 0x40000000–0x43ffffff，
U-Boot 的重定位地址、栈和保留内存不落在 xv6 使用的这段 DRAM 内。
当前内核链接在 0x40200000，BSS 到约 0x40a1e000，页分配器继续使用内存到
0x44000000。它不能与仍在工作的 DMA 缓冲区、固件或保留区重叠。

先加载，再关闭缓存并跳转：

```text
fatload mmc 0:1 0x40200000 /xv6-t113.bin
dcache off
icache off
go 0x40200000
```

**只有 `fatload` 成功后才执行后面三条。** 不在这里更改 `bootcmd` 或保存
环境变量，第一次测试手动加载即可。普通 `go` 不保证自动清理 CPU 缓存和 MMU，
因此需要上述关闭步骤。入口要求 MMU 已关闭、镜像已写回 DRAM，使用 ARM 状态；
可从 SVC 或 HYP 进入，HYP 路径会降到 SVC。内核不返回 U-Boot。
若入口仍检测到 MMU 开启，会停住；需要检查该 BSP 的 `dcache off` 实现。
若 BSP 缺少 `go` 或缓存命令，需启用 `CONFIG_CMD_GO`、`CONFIG_CMD_CACHE`
及实际 ARMv7 缓存清理支持，再构建该板 U-Boot。

若已有分区是 ext4，且 U-Boot 支持 ext4，不要重新格式化该分区，可在 Linux
主机上复制文件后改用：

```text
ext4ls mmc 0:1 /
ext4load mmc 0:1 0x40200000 /xv6-t113.bin
```

分区号和文件路径按实际布局调整。其余缓存关闭、跳转步骤相同。

预期串口输出：

```text
xv6 ARM32 / 100ask T113 Pro
single CPU, ARMv7 MMU, embedded RAM disk
init: starting sh
$ echo hello
hello
$ echo pipe | cat
pipe
$ armcheck
...
armcheck: OK
```

`armcheck` 故意让子进程触发两次用户态异常，因此会打印 ARM fault；正常结果是
父进程检查通过并返回 shell。它也检查定时器和 fork 后的内存隔离。

## TF 卡为空，需要从 TF 卡启动 U-Boot

仅复制 xv6 文件不能让全志 Boot ROM 启动：TF 卡还需要该板的 SPL/boot0、
DDR 初始化和 U-Boot。先按 [官方 TF 卡烧录说明](https://github.com/DongshanPI/buildroot_100ask_t113-pro/blob/master/docs/03-2_FlashSystemTFCard.md)
写入 **100ask T113 Pro** 官方 TF 卡镜像，确认可以进入 U-Boot，然后按上面流程
把 xv6 文件放入一个可读分区。官方固件布局不一定是 FAT，先检查分区，不要
把已烧好的启动卡重新格式化。如果板载 Flash 已经能启动 U-Boot，也可以使用
另一张普通 FAT32 TF 卡仅存放 xv6 文件。

## 实板边界和排查

- 单核；只使用 DRAM 起始的 64 MiB。页表根池有 256 个槽位（含内核根和 exec
  的临时根），不能据此假定支持无限进程。
- 继承 U-Boot 已配置好的 UART0（0x02500000）引脚、时钟、波特率；不初始化
  DDR，不解析 DTB。请确认 BSP 调试控制台确实是 UART0。
- timer0 基址 0x02050000，24 MHz HOSC，10 ms 周期；每次定时器 IRQ 轮询 UART
  输入。逻辑 uptime/sleep 的 tick 是 100 ms。实板仍需验证 HOSC 和 IRQ 路由。
- T113 GIC 分发器 0x03021000，CPU 接口 0x03022000；timer0 IRQ 为 SPI59+32=91。
  输入依赖 timer0；若显示 `$` 但键盘无响应，优先检查 timer0/GIC。
- CPU 缓存保持关闭，使用 normal non-cacheable DRAM 映射；内核栈尚无 guard page，
  ELF 段尚未分别执行只读/XN 权限收紧。
- 根盘在 RAM：新增或修改的文件重启后丢失；无需另烧 `fs.img`。
- 本阶段不支持 SD/MMC 持久存储、双核、USB、网络、Wi-Fi、挂载和线程系统调用。
  未实现的系统调用返回 -1。
- 入口之后完全无输出：确认用了实板镜像、加载地址正确、完整加载、缓存/MMU
  交接正确、UART0 是当前调试 UART；检查串口波特率。
- 出现 kernel ARM fault/panic：保留完整串口日志，对照 `kernel.elf` 和
  `kernel.map` 定位，不代表实板移植已验证。

硬件地址依据：[Linux T113 CPU/GIC 描述](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/allwinner/sun8i-t113s.dtsi)、
[共享 UART/timer 描述](https://github.com/torvalds/linux/blob/v6.6/arch/riscv/boot/dts/allwinner/sunxi-d1s-t113.dtsi)。
U-Boot `go` 交接依据：[U-Boot cmd/boot.c](https://github.com/u-boot/u-boot/blob/v2024.01/cmd/boot.c)。
