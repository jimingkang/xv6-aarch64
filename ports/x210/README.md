# S5PV210（FriendlyARM Tiny210 / 九鼎 X210）：不经 U-Boot 直接启动 xv6

本移植按 S5PV210 的标准 iROM 启动方式进入 xv6，但不用 U-Boot：第二阶段
由本仓库自带的 BL1（`kernel/arch/arm/mach-s5pv210/bl1`）完成。

## 代码组织（按 rpi3-lock 分支的驱动架构）

```
kernel/module.h                      module_init()/arch_initcall()… 段表（同 rpi3-lock）
kernel/arch/arm/                     ARM32 公共代码（对应 rpi3-lock 的 kernel/arch/arm64）
  vm.c vectors.S swtch.S syscall.c …   T113、AM335x、S5PV210 共用
  initcall.c                         do_initcalls()：按 (level, order) 运行 .initcall.list
  irq.c irq.h                        request_irq()/generic_handle_irq()，根中断控制器由 irqchip 驱动安装
  time.c time.h                      系统节拍与 clocksource（Cortex-A8 没有 Generic Timer）
  fdt.c                              DTB 读取
kernel/arch/arm/mach-s5pv210/        板级代码（对应 Linux arch/arm/mach-s5pv210）
  entry.S kernel.ld memlayout.h board.h atag.c
  setup.c                            启动参数、MMU、注册板级设备、do_initcalls()
  devices.c                          platform device 表（寄存器、中断号作为 resource）、LCD 面板表、I/O 映射
  bl1/                               自有 BL1
kernel/drivers/                      驱动都是 platform_driver，probe 时从 resource 取地址
  clk/clk-s5pv210.c                  core_initcall    clk_get_rate("pclk_psys"/"hclk_dsys")
  watchdog/s3c2410_wdt.c             core_initcall    关看门狗
  irqchip/irq-vic.c                  arch_initcall    4 x PL192 VIC，IRQ 0..127
  clocksource/samsung_pwm_timer.c    subsys_initcall  PWM 定时器 4：节拍 + 计数器
  tty/serial/samsung.c               module_init      控制台 UART（启动早期即可用）
  video/fbdev/s3c-fb.c + fbcon.c     module_init      FIMD + 帧缓冲文本控制台
  block/ramdisk_embed.c              内嵌根文件系统
  irqchip/irq-sun4i.c、clocksource/timer-sun4i.c、tty/serial/8250_sun4i.c   QEMU 替身用
ports/x210/                          构建脚本：Makefile、mkx210.py、test_shell.py、本说明
```

启动时串口会打印每个 initcall 和驱动 probe 的结果，例如
`initcall: irq-vic (level 3)` / `irqchip: 4 x PL192 VIC at f2000000`。
QEMU 测试走的是同一套框架（sun4i 的 irqchip/clocksource/serial 驱动），
因此 module_init 段表、platform 总线匹配、request_irq 分发都被测试覆盖。

内核和用户态与 T113、AM335x 移植共用（进程、调度、文件系统、管道、shell、
ARMv7 短描述符页表、异常向量）。

支持两块板，板级差异都在 `mach-s5pv210/board.h`，用 `S5PV210_BOARD` 选择：

| | Tiny210（默认，`S5PV210_BOARD=tiny210`） | X210（`S5PV210_BOARD=x210`） |
| --- | --- | --- |
| 控制台 | UART0（SDK 底板 COM0），ttySAC0 | UART2，ttySAC2 |
| DDR（DMC0） | 512 MiB @ 0x20000000，MEMCONFIG0 0x20E01323 | 256 MiB @ 0x30000000，MEMCONFIG0 0x30F01313 |
| 内核运行地址 | 0x20008000 | 0x30008000 |
| ATAG 地址 | 0x20000100 | 0x30000100 |
| 机器号（r1） | 3466（MINI210） | 2456（SMDKV210） |

```
S5PV210 iROM (BL0) ──► xv6 BL1：SD 第 1 扇区，16 KiB，16 字节头 + 校验和，
                       iROM 载入 iRAM 0xD0020000，从 0xD0020010 执行
                         · PS_HOLD 供电锁存、关看门狗
                         · APLL 1000 MHz / MPLL 667 MHz，PCLK_PSYS 66.7 MHz
                         · 控制台 UART 115200 8N1，打印 BL1 进度
                         · DMC0 DDR2 初始化、容量探测与读写自检
                         · iROM CopySDMMCtoMem：把 SD 第 49 扇区起的 xv6 uImage
                           读到 DRAM+0x7FC0（数据正好落在 DRAM+0x8000），校验头/数据 CRC
                         · 在 DRAM+0x100 生成 ATAG（CORE、MEM、CMDLINE）
                   ──► xv6 _entry @ DRAM+0x8000（r0=0，r1=机器号，r2=ATAG）
```

BL1 交接给内核的方式与 U-Boot `bootm` 完全一样（ARM Linux 启动协议），
所以同一个 `xv6-x210.uImage` 也能用 U-Boot 的 `bootm` 启动。

**验证状态：尚未在实板上运行。**
- QEMU 没有 S5PV210 机型，内核用同为 Cortex-A8 的 `cubieboard` 作替身测试：
  镜像链接在 DRAM+0x8000，QEMU 以 bootm 方式交接（ATAG 在 DRAM+0x100），
  完整 shell 测试通过。
- BL1 中与硬件无关的部分（CRC-32、uImage 头检查、ATAG 生成）在主机上编译，
  用两块板各自的 `xv6-x210.uImage` 和内核自己的 ATAG 解析器对照测试。
- BL1 的时钟、DDR 时序、UART 寄存器值取自 Samsung S5PV210 U-Boot 的
  `lowlevel_init.S`/`cpu_init.S`（FriendlyARM 和九鼎的 BSP U-Boot 都基于它）；
  Tiny210 的 DDR 容量/行数按 512 MiB、14 位行地址配置，iROM 函数地址依据
  S5PV210 iROM Application Note。这些都只做了编译检查。若板上颗粒其实是
  13 位行地址，BL1 会检测到上半区与下半区重叠，自动只用一半并打印警告。

## 构建

```sh
make x210                                   # Tiny210：build/x210/
make -f ports/x210/Makefile S5PV210_BOARD=x210   # 九鼎 X210
make x210-check     # 自检 + BL1 主机测试（两块板）+ QEMU cubieboard 测试
make x210-qemu      # QEMU 交互运行内核
make x210-clean
```

产物（`build/x210/`）：

| 文件 | 用途 |
| --- | --- |
| `x210-sd.img` | 整卡原始镜像：BL1@1、xv6@49，第 0 扇区（MBR）留空 |
| `x210-bl1.bin` | 16 KiB BL1（含 iROM 头），写到第 1 扇区 |
| `xv6-x210.uImage` | xv6 内核 + 4 MiB 根文件系统，legacy uImage，写到第 49 扇区 |
| `xv6-x210.bin` | 同一内核的裸镜像（带 zImage 头），给 U-Boot `bootz` 调试用 |
| `bl1/bl1.elf`、`bl1/bl1.map`、`kernel.elf`、`kernel.map` | 调试用 |

换板子编译前先 `make x210-clean`（或用不同的 `BUILD=` 目录），
否则旧的目标文件会混在一起。**不要把 `build/x210-qemu/` 的镜像写到卡上**。

## SD 卡布局

| 扇区 | 内容 |
| --- | --- |
| 0 | MBR（不动） |
| 1–32 | BL1（16 KiB） |
| 33–48 | 空 |
| 49–约 8322 | xv6 uImage（约 4.1 MiB，`make` 时会打印实际扇区数） |

整个布局在第 8400 扇区以内，第一个分区只要从它之后开始就不受影响。

## 写卡（macOS）

以 `/dev/disk4` 为例，第一个分区从 31248 扇区开始，前面约 15 MiB 是空闲的
原始区，xv6 只占用前约 4.3 MiB，四个分区都不会被改动。
**先用 `diskutil list` 确认 disk4 确实是这张 SD 卡**，写错盘会毁数据。
原始区里原有的 BL1/U-Boot（例如 FriendlyARM superboot）会被覆盖。

```sh
make x210
```

```sh
diskutil unmountDisk /dev/disk4
```

```sh
sudo dd if=build/x210/x210-sd.img of=/dev/rdisk4 bs=512 skip=1 seek=1
```

```sh
sync
diskutil eject /dev/disk4
```

`skip=1 seek=1` 跳过第 0 扇区，分区表保持不变。也可以分两次写：

```sh
sudo dd if=build/x210/x210-bl1.bin of=/dev/rdisk4 bs=512 seek=1
sudo dd if=build/x210/xv6-x210.uImage of=/dev/rdisk4 bs=512 seek=49
```

之后只改了内核时，只需重写第 49 扇区那一条。

## 上电

- **Tiny210**：核心板从 NAND 启动是默认值。把 SDK 底板上的启动选择开关
  拨到 SD 启动一侧（丝印一般是 `SDBOOT`，另一侧是 `NAND`），插卡上电。
  串口接底板的 COM0（UART0，115200 8N1）。
- **X210**：默认先从核心板 iNand 启动，iNand 中没有有效 BL1 时才尝试 SD2 卡槽。
  串口接 UART2。

BL1 会打印 iROM 从哪个 SD 通道启动（0 或 2），两种它都能处理。

Tiny210 预期串口输出：

```text
xv6 BL1 for FriendlyARM Tiny210 (Samsung S5PV210): APLL 1000 MHz, MPLL 667 MHz, PCLK_PSYS 66.7 MHz
BL1: DMC0 DDR2 512 MiB at 0x20000000 OK
BL1: SD channel 0, xv6 at sector 49
BL1: "xv6 ARM32 S5PV210", 4235264 bytes, 8273 sectors
BL1: data CRC OK
BL1: ATAGs at 0x20000100 (72 bytes), starting xv6 at 0x20008000

xv6 ARM32 / FriendlyARM Tiny210 (Samsung S5PV210)
boot: r0=0 machid=3466 (MINI210) r2=20000100
atags: at 20000100
atags: memory 20000000 + 512 MiB
atags: cmdline "console=ttySAC0,115200 xv6 bl1"
timer: PWM4 from PCLK_PSYS 66700000 Hz
kernel 20008000-..., DRAM 20000000-28000000
single CPU, ARMv7 MMU, embedded RAM disk
init: starting sh
$
```

## 串口终端

macOS 上推荐直接用 `screen /dev/cu.usbserial-XXXX 115200`。用 minicom 时要
确认它确实是 115200：Tiny210 上实测，minicom 实际按 9600 接收时，每行只收到
约 7 个固定样式的乱码字符（例如 `` `J&?C?? ``）。也不要让多个 minicom/screen
同时打开同一个串口，否则数据会被分抢，同样显示为乱码。

`make -f ports/x210/Makefile uarttest` 生成 `x210-uarttest.bin`（只有 BL1，
写到第 1 扇区）：它按 Tiny210 U-Boot 的时钟和 UART 参数，每半秒输出一行
`xv6 UART test ... line N`，LED 轮流亮，用来单独检查串口链路。

## LCD

`kernel/drivers/video/fbdev/s3c-fb.c` 驱动 S5PV210 的 FIMD（面板时序和引脚设置作为 platform data 由 `mach-s5pv210/devices.c` 提供）：GPF0–GPF3 设为 LCD 功能，DISPLAY_CONTROL
选 FIMD，Window 0 为 32 bpp（XRGB8888），帧缓冲放在 DRAM_BASE+128 MiB
（2 MiB，在页分配器范围之外，无缓存映射）。像素时钟取自 HCLK_DSYS
（166.75 MHz）分频。启动时先显示约一秒的彩条，然后清屏，之后内核控制台
的全部输出（包括 shell）同时显示在屏上，8x16 字体，800x480 时为 100x30 字符。
键盘输入仍然走串口。

面板型号在编译时选择，换型号前先 `make x210-clean`：

```sh
make -f ports/x210/Makefile S5PV210_LCD=s70   # 7" 800x480（默认）
make -f ports/x210/Makefile S5PV210_LCD=h43   # 4.3" 480x272
make -f ports/x210/Makefile S5PV210_LCD=w50   # 5" 800x480
make -f ports/x210/Makefile S5PV210_LCD=a70   # 7" 800x480
```

时序取自 FriendlyARM mini210/tiny210 BSP 的面板表（凭记录整理，未在实板
核对）；背光由 LCD 模块自身控制，本驱动不调节。

## BL1 格式

S5PV210 iROM 从 SD 第 1 扇区读 16 KiB 到 iRAM 0xD0020000，校验 16 字节头后
从 0xD0020010 执行：

| 偏移 | 内容 |
| --- | --- |
| 0x0 | BL1 大小 0x4000 |
| 0x4 | 保留 0 |
| 0x8 | 校验和：第 16..16383 字节逐字节相加 |
| 0xC | 保留 0 |

`mkx210.py check-bl1` 也接受九鼎 BSP `mkx210` 的格式（大小字段为 `"S5PC"`）。
芯片没有烧写安全启动熔丝，iROM 只校验和、不验签。BL1 代码约 3.7 KiB，
链接脚本保证它不超过 16 KiB−16 字节。

BL1 调用的 iROM 服务：

- `0xD0037488`：iROM 启动所用的 SD 控制器基址（0xEB000000 = 通道 0，
  0xEB200000 = 通道 2）；
- `0xD0037F98`：`CopySDMMCtoMem(ch, start_block, u16 count, dst, init)`
  的函数指针，`init=0` 沿用 iROM 已初始化好的卡。

## 内核侧

入口约定与 Linux 在 ARM 上一致：SVC 模式，IRQ/FIQ 屏蔽，MMU 与 D-cache 关，
`r0 = 0`，`r1 = 机器号`，`r2 = ATAG` 或 DTB。入口检查加载地址、CPU 模式和
MMU 状态，出错时直接从控制台 UART 打印原因后停住。

启动后内核依次：关看门狗 → 解析 ATAG（在分配器接管内存前完成）→ 开 MMU →
初始化页分配器、VIC、PWM 定时器 4，再进入 `/bin/sh`。

- DDR：BL1 只初始化 DMC0，DMC1 不动。缓存关闭，页分配器会无缓存地填满
  每一页，所以只使用前 128 MiB（`memlayout.h` 的 `S5PV210_USE_DRAM`）。ATAG 在
  DRAM+0x100，位于内核之下，不会被分配。用户空间为 [0, 0x20000000)。
- 控制台 UART：沿用 BL1 的设置，屏蔽 UART 中断，每个定时器中断轮询输入。
- VIC0–3 0xF2000000–0xF2300000（PL192）：全部禁用并选为 IRQ，只开 VIC0
  第 25 位（PWM 定时器 4）；SCTLR.VE 清零，IRQ 走 0x18 向量。
- PWM 定时器 4（0xE2500000）：时钟为 PCLK_PSYS（由时钟寄存器算出，
  异常时取 66.7 MHz），自动重载，10 ms 中断；也作 `r_cntvct_el0()` 的计数源
  （Cortex-A8 没有 Generic Timer，也没有硬件除法）。
- 单核、缓存关闭、根文件系统在 RAM 中（重启丢失）；不支持 SD 持久存储、
  网卡、LCD、USB。

## 排查

| 现象 | 可能原因 |
| --- | --- |
| 串口完全没输出 | 启动开关没拨到 SD、串口接错口、第 1 扇区没写对（用 `mkx210.py check-bl1` 检查） |
| `DMC0 DLL did not lock` 或 `DDR mismatch` | DDR 参数与板上颗粒不符，把串口日志发回来 |
| `aliases the lower half` 警告 | 颗粒行地址比配置少一位，已自动只用一半，不影响 xv6 |
| `not booted from SD/MMC` | iROM 不是从 SD 启动的 |
| `no uImage magic` / `bad header CRC` | 第 49 扇区没写对 |
| `uImage load/entry address does not match this board` | 用错了板子的镜像（Tiny210 是 0x20008000，X210 是 0x30008000） |
| `kernel data CRC mismatch` | SD 读取出错或镜像写入不完整 |
| BL1 正常但 xv6 无输出 | 把完整串口日志和 `kernel.map` 一起发回来 |
| 出现 `$` 但键盘无响应 | 检查 `timer:` 一行的频率和 VIC0 第 25 位 |

参考：S5PV210 RISC Microprocessor User's Manual（iROM 启动、时钟控制器、
DMC、VIC、PWM Timer、Watchdog、UART 章节）、S5PV210 iROM Application Note
（BL1 头、校验和、设备拷贝函数）、Samsung S5PV210 U-Boot
（`cpu/s5pc11x/s5pc110/cpu_init.S`、`board/samsung/smdkc110/lowlevel_init.S`）、
FriendlyARM Tiny210/Mini210 BSP、九鼎 X210 BSP（`sd_fusing/mkx210.c`），
Linux `Documentation/arch/arm/booting.rst`（ATAG 与入口寄存器）。
