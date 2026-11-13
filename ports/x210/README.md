# 九鼎 X210（Samsung S5PV210）：ARM32 xv6 串口 shell

本移植按 X210 的标准启动链进入 xv6。内核和用户态与 `ports/t113` 共用
（进程、调度、文件系统、管道、shell、ARMv7 短描述符页表、异常向量），
DTB 解析与 `ports/am335x` 共用，板级部分独立：

```
S5PV210 iROM (BL0) ──► BL1：SD 第 1 扇区，16 KiB，16 字节头 + 校验和，
                       载入 iRAM 0xD0020000，从 0xD0020010 执行（时钟、DDR 初始化）
                   ──► BL2 = U-Boot：SD 第 49 扇区，由 BL1 拷到 DDR 运行
                   ──► movi read kernel 30008000（SD 第 1073 扇区）
                   ──► bootm 30008000（uImage，ATAGs 在 0x30000100）
                   ──► xv6 _entry（ARM Linux 启动协议）
```

**验证状态：QEMU Cortex-A8 测试通过；尚未在 X210 实板上运行。**
QEMU 没有 S5PV210 机型，测试用同为 Cortex-A8 的 `cubieboard`
（全志 A10 外设）作替身：镜像同样链接在 DRAM+0x8000，QEMU 以 bootm
相同的方式交接（MMU 关、r0=0、r1=机器号、r2=DRAM+0x100 处的 ATAG 链表），
验证了 Cortex-A8 指令集、ATAG 解析和完整 shell。S5PV210 的 UART2、VIC、
PWM 定时器、看门狗、时钟寄存器只做了编译检查，依据 S5PV210 用户手册编写。
BL1 头与 SD 布局由 `mkx210.py selftest` 自检，没有在 iROM 上验证过。

## 入口约定

与 Linux 在 ARM 上的约定一致（U-Boot `do_bootm_linux` 的
`cleanup_before_linux` 负责满足）：

- SVC 模式，IRQ/FIQ 屏蔽，MMU 与 D-cache 关闭，I-cache 可开可关；
- `r0 = 0`，`r1 = 机器号`（X210 BSP 用 SMDKV210 = 2456），
  `r2 = ATAG 链表`（X210 U-Boot 放在 0x30000100）或 DTB 地址。

入口会检查：

| 情况 | 串口输出 |
| --- | --- |
| 加载地址不对 | `xv6: wrong load address; ...` |
| MMU 仍开着（X210 BSP U-Boot 自己开着 MMU，`go` 会遇到） | `xv6: MMU still on; ...` |
| r2 既不是 ATAG 也不是 DTB，0x30000100 处也没有 ATAG | `boot: no ATAG list or DTB at r2 ...`，按默认值继续 |

镜像前 0x34 字节是 ARM zImage 头（0x24 处魔数 `0x016f2818`），
所以裸镜像 `xv6-x210.bin` 也能走三星 U-Boot `bootm` 的 “Boot with zImage”
分支或 `bootz`。两者都不重定位：镜像必须在链接地址 **0x30008000**。

启动后内核依次：关闭看门狗（WTCON=0）→ 解析 ATAG（`ATAG_MEM`、
`ATAG_CMDLINE`、`ATAG_INITRD2` 等，在分配器接管内存前完成）→ 开 MMU →
初始化页分配器、VIC、PWM 定时器 4，再进入 `/bin/sh`。

## 构建

```sh
make x210                              # 实板：build/x210/
make x210-check                        # BL1/SD 布局自检 + QEMU cubieboard 自动测试
make x210-qemu                         # QEMU 交互运行
make x210-bl1 UBOOT=/path/u-boot.bin   # 只生成 BL1（等同 BSP 的 mkx210）
make x210-sdimg UBOOT=/path/u-boot.bin # 整卡原始镜像
make x210-clean
```

产物（`build/x210/`）：

- `xv6-x210.uImage`：legacy uImage（IH_OS_LINUX，load/entry 0x30008000），`bootm` 用。
- `xv6-x210.bin`：带 zImage 头的裸镜像，约 3 MiB（含 3 MiB 根文件系统）。
- `x210-bl1.bin`：16 KiB BL1（u-boot.bin 前 16 KiB−16 字节 + 头）。
- `x210-sd.img`：BL1@1、U-Boot@49、xv6 uImage@1073 的原始卡镜像（第 0 扇区留空）。
- `kernel.elf`、`kernel.map`：调试用。

可调参数：`X210_DRAM_BASE`（默认 0x30000000；若 U-Boot 把 DRAM 配在
0x20000000，则 `make x210 X210_DRAM_BASE=0x20000000`，加载地址随之变为
0x20008000）、`X210_FSBLOCKS`（根文件系统块数，默认 3072，使镜像小于 4 MiB
的内核分区）。

**不要把 `build/x210-qemu/` 的镜像拷到板上**，它链接在 0x40008000，外设地址也不同。
Cortex-A8 没有硬件除法和 Generic Timer：32 位除法由 `ports/t113/runtime.c`
中的 `__aeabi_*div*` 提供，`r_cntvct_el0()` 改由 PWM 定时器 4 的计数实现。

## BL1 格式

S5PV210 iROM 从 SD 第 1 扇区读 16 KiB 到 iRAM 0xD0020000，校验 16 字节头后
从 0xD0020010 执行：

| 偏移 | 内容 |
| --- | --- |
| 0x0 | BL1 大小 0x4000 |
| 0x4 | 保留 0 |
| 0x8 | 校验和：第 16..16383 字节逐字节相加 |
| 0xC | 保留 0 |

BSP 自带的 `mkx210` 在大小字段写的是 `"S5PC"` 字符，校验和算法相同；
`mkx210.py check-bl1` 两种都接受。BL1 本身就是 U-Boot 的前 16 KiB
（`start.S` → `lowlevel_init`：关看门狗、时钟、DDR、UART，然后从第 49 扇区把
整个 u-boot.bin 拷到 DDR）。本仓库不自带 U-Boot，用九鼎 BSP 的
`uboot_jiuding`（`make x210_sd_config && make`）或其他 S5PV210 U-Boot 均可；
芯片未烧写安全启动熔丝，iROM 只校验和不验签。

## SD 卡布局（X210 BSP U-Boot 约定）

| 扇区 | 内容 |
| --- | --- |
| 0 | MBR（不动） |
| 1–32 | BL1（16 KiB） |
| 33–48 | U-Boot 环境变量区（BL1 与 BL2 之间，具体位置以 `movi.h` 为准） |
| 49–1072 | BL2 = u-boot.bin（≤512 KiB） |
| 1073– | 内核（`movi read kernel` 读取，≤4 MiB） |

第一个 FAT 分区要从 1073+8192 扇区之后开始（一般 ≥ 第 10240 扇区）。
扇区号来自 BSP U-Boot 的 `include/movi.h`；如果你的 U-Boot 不同，
用 `movi` 命令的帮助输出或源码确认后再写卡。

macOS 写卡（`diskN` 先用 `diskutil list` 确认，**写错盘会毁数据**）：

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=build/x210/x210-sd.img of=/dev/rdiskN bs=512 skip=1 seek=1 conv=sync
sync
```

`skip=1 seek=1` 保留卡上原有分区表。也可以像 BSP 的 `sd_fusing.sh` 分开写：

```sh
sudo dd if=build/x210/x210-bl1.bin of=/dev/rdiskN bs=512 seek=1
sudo dd if=u-boot.bin of=/dev/rdiskN bs=512 seek=49
sudo dd if=build/x210/xv6-x210.uImage of=/dev/rdiskN bs=512 seek=1073 conv=sync
```

按开发板手册选择启动设备：X210 默认先从核心板 iNand（SD0）启动，iNand 中没有
有效 BL1 时 iROM 才尝试 SD2 卡槽。串口接 UART2（底板调试串口，115200 8N1）。

## 在 U-Boot 提示符下启动

上电后按任意键停在 `x210 #`：

```text
movi read kernel 30008000
bootm 30008000
```

BSP 默认 `bootcmd` 是 `movi read kernel 30008000; movi read rootfs 30B00000 300000; bootm 30008000 30B00000`。
xv6 不需要 initrd，而第二个参数不是 uImage 时旧版 bootm 会报 `Bad Magic Number`
并复位，所以改成：

```text
setenv bootcmd 'movi read kernel 30008000; bootm 30008000'
saveenv
```

也可以用网络加载（与九鼎教程一致）：

```text
tftp 30008000 xv6-x210.uImage
bootm 30008000
```

uImage 的 load 地址是 0x30008000，头部 64 字节之后的数据会被 bootm 下移
64 字节到 0x30008000；也可直接 `tftp 30008000 xv6-x210.bin; bootm 30008000`
走 zImage 分支。不推荐 `go`：BSP U-Boot 开着 MMU，入口会报错停下。

预期串口输出：

```text
Starting kernel ...

xv6 ARM32 / Jiuding X210 (Samsung S5PV210)
boot: r0=0 machid=2456 (SMDKV210) r2=30000100
atags: at 30000100
atags: memory 30000000 + 256 MiB
atags: memory 40000000 + 256 MiB
atags: cmdline "console=ttySAC2,115200 ..."
timer: PWM4 from PCLK_PSYS 66700000 Hz
kernel 30008000-..., DRAM 30000000-38000000
single CPU, ARMv7 MMU, embedded RAM disk
init: starting sh
$
```

## 内存与外设

- DDR：0x30000000（DMC0）与 0x40000000（DMC1）各 256 MiB。内核 0x30008000，
  BSS 结束约 0x30726000（其中 4 MiB 是一级页表池）。缓存关闭，页分配器启动时
  会无缓存地填满每一页，所以只使用前 128 MiB（0x30000000–0x38000000，
  `platform.c` 的 `USE_DRAM`）；ATAG_MEM 更小时以它为准。ATAG 在
  0x30000100，位于内核之下，不会被分配。用户空间为 [0, 0x20000000)。
- UART2 0xE2900800：沿用 U-Boot 的 ULCON/UCON/UBRDIV，屏蔽 UART 中断
  （UINTM），每个定时器中断轮询输入；FIFO 开着时用 UFSTAT 判断收发。
- VIC0–3 0xF2000000–0xF2300000（PL192）：全部禁用并选为 IRQ，只开 VIC0
  第 25 位（PWM 定时器 4）；SCTLR.VE 清零，IRQ 走 0x18 向量；中断结束写
  VICADDRESS。
- PWM 定时器 0xE2500000：定时器 4，预分频 1 与分频器都设为 1，时钟为
  PCLK_PSYS（由 CLK_SRC0/CLK_DIV0/MPLL_CON 算出，异常时取 66.7 MHz），
  自动重载，10 ms 中断；TINT_CSTAT 写 1 清除时保留使能位。
- 看门狗 0xE2700000：WTCON 写 0。
- 单核、缓存关闭、根文件系统在 RAM 中（重启丢失）；不支持 SD 持久存储、
  DM9000 网卡、LCD、USB。

## 排查

- `Starting kernel ...` 后无输出：确认加载地址是 0x30008000，串口接的是 UART2。
  若 `bootm` 报 `Bad Magic Number`，检查 bootcmd 是否还带着 rootfs 参数。
- `xv6: MMU still on`：用了 `go`，改用 `bootm`。
- 出现 `$` 但键盘无响应：输入依赖 PWM 定时器 4 中断，检查 `timer:` 一行的
  频率和 VIC0 第 25 位。
- 卡启动失败、iROM 回退：用 `mkx210.py check-bl1` 检查第 1 扇区；
  确认分区没有覆盖 1–10240 扇区。
- 出现 `ARM fault`/`panic`：保留完整串口日志，用 `kernel.elf`、`kernel.map` 定位。

参考：S5PV210 RISC Microprocessor User's Manual（第 2.4 节 iROM 启动、
VIC、PWM Timer、Watchdog、UART、Clock Controller 章节）、S5PV210 iROM
Application Note（BL1 头与校验和）、九鼎 X210 BSP U-Boot（`sd_fusing/mkx210.c`、
`include/movi.h`、`include/configs/x210_sd.h`），Linux
`Documentation/arch/arm/booting.rst`（ATAG 与入口寄存器）。
