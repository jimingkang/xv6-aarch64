# BeagleBone Black（TI AM335x）：ARM32 xv6 串口 shell

本移植按 BeagleBone Black 的标准启动链进入 xv6，内核和用户态与
`ports/t113` 共用（进程、调度、文件系统、管道、shell、ARMv7 短描述符页表、
异常向量），板级部分独立：

```
AM335x Boot ROM ──► MLO (U-Boot SPL：时钟、PMIC、DDR3 初始化)
                ──► u-boot.img (am335x_evm)
                ──► uEnv.txt 中的 uenvcmd
                ──► load xv6-am335x.bin → 0x82000000，DTB → 0x88000000
                ──► bootz 0x82000000 - 0x88000000
                ──► xv6 _entry（ARM Linux 启动协议）
```

**验证状态：QEMU Cortex-A8 测试通过；尚未在 BeagleBone Black 实板上运行。**
QEMU 没有 AM335x 机型，测试用同为 Cortex-A8 的 `cubieboard` 机型
（全志 A10 外设）作替身，验证 Cortex-A8 指令集、bootm/Linux 交接
（r0/r1/r2）、DTB 解析和完整 shell。AM335x 的 INTC、DMTimer2、WDT1、
时钟控制寄存器只做了编译检查，依据 AM335x TRM 编写。

## 入口约定

与 Linux 在 ARM 上的约定一致（U-Boot `bootz`/`bootm` 的 `cleanup_before_linux`
负责满足）：

- SVC 模式，IRQ/FIQ 屏蔽，MMU 与 D-cache 关闭，I-cache 可开可关；
- `r0 = 0`，`r1 = 机器号`，`r2 = DTB 物理地址`。

镜像前 0x34 字节是 ARM zImage 头（0x24 处魔数 `0x016f2818`、0x28/0x2c 为
起止偏移、0x30 为小端标记），所以未压缩的 `xv6-am335x.bin` 可以直接
`bootz`。`bootz` 不重定位：镜像必须加载在链接地址 **0x82000000**，即
am335x_evm U-Boot 的默认 `${loadaddr}`。入口会检查：

| 情况 | 串口输出 |
| --- | --- |
| 加载地址不对 | `xv6: wrong load address; ...` |
| MMU 仍开着（例如直接 `go` 且没先 `dcache off`） | `xv6: MMU still on; ...` |
| r2 不是合法 DTB（例如用 `go`） | `boot: no valid DTB at r2 ...`，按默认值继续 |

启动后内核依次：关闭 WDT1（新版 U-Boot 的 `CONFIG_WATCHDOG_AUTOSTART`
会让看门狗约 60 秒后复位）→ 解析 DTB（`/model`、`/compatible`、`/memory`、
`/chosen/bootargs`，在分配器接管内存前完成）→ 开 MMU → 初始化页分配器、
INTC、DMTimer2，再进入 `/bin/sh`。

## 构建

```sh
make am335x          # 实板：build/am335x/
make am335x-check    # QEMU cubieboard 自动测试
make am335x-qemu     # QEMU 交互运行
make am335x-clean
```

产物（`build/am335x/`）：

- `xv6-am335x.bin`：带 zImage 头的裸镜像，`bootz` 用，约 4 MiB（含 4 MiB 根文件系统）。
- `xv6-am335x.uImage`：legacy uImage（IH_OS_LINUX，load/entry 0x82000000），`bootm` 用。
- `uEnv.txt`：U-Boot 自动启动脚本。
- `kernel.elf`、`kernel.map`：调试用。

**不要把 `build/am335x-qemu/` 的镜像拷到板上**，它链接在 0x42000000，外设地址也不同。
Cortex-A8 没有硬件除法和 Generic Timer：32 位除法由 `ports/t113/runtime.c` 中的
`__aeabi_*div*` 提供，`r_cntvct_el0()` 改由 DMTimer2 计数实现。

## 准备 microSD 卡

1. 卡上第 1 分区为 FAT32 并设为可启动（active），ROM 从中读取 `MLO`。
2. 编译 U-Boot（主线即可）：

   ```sh
   make am335x_evm_defconfig
   make CROSS_COMPILE=arm-none-eabi-
   ```

   得到 `MLO`、`u-boot.img`。也可沿用官方 Debian 镜像中 FAT 分区里的这两个文件。
3. FAT 分区根目录放入：

   ```
   MLO
   u-boot.img
   uEnv.txt               (build/am335x/uEnv.txt)
   xv6-am335x.bin         (build/am335x/xv6-am335x.bin)
   am335x-boneblack.dtb   (Linux arch/arm/boot/dts/ti/omap/ 编译所得，或官方镜像 /boot/dtbs/ 中)
   ```

   macOS 示例（挂载名按实际修改）：

   ```sh
   cp build/am335x/{uEnv.txt,xv6-am335x.bin} /Volumes/BOOT/
   sync
   ```
4. 接 3.3 V USB 串口到 J1 调试口（115200 8N1，这是 AM335x UART0）。
5. 按住 S2（BOOT 键，靠近 microSD 卡槽）上电，让 ROM 从 microSD 启动，
   而不是从 eMMC。

am335x_evm 默认 `bootcmd` 中的 `envboot` 会从 `mmc ${mmcdev}` 的第一个 FAT
分区读取 `uEnv.txt`，导入后执行 `uenvcmd`：加载内核到 `${loadaddr}`，加载
`am335x-boneblack.dtb` 到 `${fdtaddr}`；若卡上没有 DTB 文件，则改用 U-Boot
自身的控制 DTB `${fdtcontroladdr}`，最后 `bootz`。

## 在 U-Boot 提示符下手动启动

上电后按任意键停在 `=>`，确认分区：

```text
mmc dev 0
fatls mmc 0:1 /
```

`bootz`（推荐）：

```text
load mmc 0:1 0x82000000 xv6-am335x.bin
load mmc 0:1 0x88000000 am335x-boneblack.dtb
bootz 0x82000000 - 0x88000000
```

`bootm`（uImage 先放在暂存地址，由 U-Boot 拷贝到 0x82000000）：

```text
load mmc 0:1 0x84000000 xv6-am335x.uImage
load mmc 0:1 0x88000000 am335x-boneblack.dtb
bootm 0x84000000 - 0x88000000
```

不推荐 `go`：它不传 DTB，也不保证清理缓存/MMU；若一定要用，需
`dcache off; icache off; go 0x82000000`。

预期串口输出：

```text
Kernel image @ 0x82000000 [ 0x000000 - 0x40a000 ]
## Flattened Device Tree blob at 88000000
Starting kernel ...

xv6 ARM32 / BeagleBone Black (TI AM335x)
boot: r0=0 machid=... dtb=88000000
dtb: ... model "TI AM335x BeagleBone Black"
dtb: memory 80000000 + 512 MiB
kernel 82000000-..., DRAM 80000000-88000000
single CPU, ARMv7 MMU, embedded RAM disk
init: starting sh
$
```

`machid` 取决于 U-Boot 配置，DT 启动时不使用。

## 内存与外设

- DDR：0x80000000 起。内核 0x82000000，BSS 结束约 0x82820000。缓存目前关闭，
  页分配器启动时会无缓存地填满每一页，所以只使用前 128 MiB
  （0x80000000–0x88000000，`platform.c` 的 `USE_DRAM`）；DTB 报告的容量
  更小时以 DTB 为准。DTB 在分配器接管内存前已解析完毕，可被覆盖。
- UART0 0x44E09000（继承 U-Boot 的引脚复用、时钟、波特率；关闭 UART 中断，
  每个定时器中断轮询输入）。
- INTC 0x48200000：软复位，屏蔽全部，只放开 TINT2（IRQ 68）。
- DMTimer2 0x48040000：时钟选 CLK_M_OSC（频率由 CONTROL_STATUS 的
  SYSBOOT[15:14] 得出，BBB 为 24 MHz），自动重载，10 ms 中断。
- WDT1 0x44E35000：WSPR 写 0xAAAA/0x5555 关闭。
- 单核、缓存关闭、根文件系统在 RAM 中（重启丢失）；不支持 MMC 持久存储、
  以太网、USB。

## 排查

- U-Boot 打印 `Starting kernel ...` 后无输出：确认加载地址为 0x82000000，
  串口接的是 J1（UART0）。
- 约 60 秒后自动复位：说明 WDT1 未被关闭，检查 `wdt_disable()`。
- 出现 `$` 但键盘无响应：输入依赖 DMTimer2 中断，检查 INTC/DMTimer2 时钟
  （`CM_PER_TIMER2_CLKCTRL`）。
- 出现 `ARM fault`/`panic`：保留完整串口日志，用 `kernel.elf`、`kernel.map` 定位。

参考：AM335x Technical Reference Manual（SPRUH73）第 6 章 INTC、20 章 DMTimer、
20.4 WDT、26 章 Initialization；U-Boot `arch/arm/lib/zimage.c`（zImage 头）、
`include/configs/am335x_evm.h` / `include/env/ti/mmc.env`（`loadaddr`、`fdtaddr`、`uenvcmd`）。
