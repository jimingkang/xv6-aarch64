# FriendlyARM Tiny210（Samsung S5PV210）：不经 U-Boot 直接启动 xv6

本移植按 S5PV210 的标准 iROM 启动方式进入 xv6，但不用 U-Boot：第二阶段
由本仓库自带的 BL1（`kernel/arch/arm/mach-s5pv210/bl1`）完成。

面向 Tiny210 驱动开发者的分层说明见
[`doc/readme/tiny210/README.md`](../../doc/readme/tiny210/README.md)，其中分别说明
启动与内存、中断、LCD/FIMD、当前内嵌文件系统以及 platform 驱动模型。本文件继续
作为构建、SD 布局、烧录和上电操作手册。

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
ports/tiny210/                          构建脚本：Makefile、mktiny210.py、test_shell.py、本说明
```

启动时串口会打印每个 initcall 和驱动 probe 的结果，例如
`initcall: irq-vic (level 3)` / `irqchip: 4 x PL192 VIC at f2000000`。
QEMU 测试走的是同一套框架（sun4i 的 irqchip/clocksource/serial 驱动），
因此 module_init 段表、platform 总线匹配、request_irq 分发都被测试覆盖。

内核和用户态与 T113、AM335x 移植共用（进程、调度、文件系统、管道、shell、
ARMv7 短描述符页表、异常向量）。

板级常量在 `mach-s5pv210/board.h`：

| 项目 | Tiny210 |
| --- | --- |
| 控制台 | UART0（SDK 底板 COM0），ttySAC0 |
| DDR（DMC0） | 512 MiB @ 0x20000000，MEMCONFIG0 0x20E01323 |
| 内核运行地址 | 0x20008000 |
| ATAG 地址 | 0x20000100 |
| 机器号（r1） | 3466（MINI210） |

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
所以同一个 `xv6-tiny210.uImage` 也能用 U-Boot 的 `bootm` 启动。

**验证状态：尚未在实板上运行。**
- QEMU 没有 S5PV210 机型，内核用同为 Cortex-A8 的 `cubieboard` 作替身测试：
  镜像链接在 DRAM+0x8000，QEMU 以 bootm 方式交接（ATAG 在 DRAM+0x100），
  完整 shell 测试通过。
- BL1 中与硬件无关的部分（CRC-32、uImage 头检查、ATAG 生成）在主机上编译，
  用两块板各自的 `xv6-tiny210.uImage` 和内核自己的 ATAG 解析器对照测试。
- BL1 的时钟、DDR 时序、UART 寄存器值取自 Samsung S5PV210 U-Boot 的
  `lowlevel_init.S`/`cpu_init.S`（FriendlyARM 和九鼎的 BSP U-Boot 都基于它）；
  Tiny210 的 DDR 容量/行数按 512 MiB、14 位行地址配置，iROM 函数地址依据
  S5PV210 iROM Application Note。这些都只做了编译检查。若板上颗粒其实是
  13 位行地址，BL1 会检测到上半区与下半区重叠，自动只用一半并打印警告。

## 构建

```sh
make tiny210           # build/tiny210/，并把内核/固件拷到 /Volumes/bootfs
make tiny210-check     # 自检 + BL1 主机测试 + QEMU cubieboard 测试
make tiny210-qemu      # QEMU 交互运行内核
make tiny210-clean
```

产物（`build/tiny210/`）：

| 文件 | 用途 |
| --- | --- |
| `tiny210-sd.img` | 整卡原始镜像：BL1@1、xv6@49，第 0 扇区（MBR）留空 |
| `tiny210-bl1.bin` | 16 KiB BL1（含 iROM 头），写到第 1 扇区 |
| `xv6-tiny210.uImage` | xv6 内核 + 4 MiB 根文件系统，legacy uImage，写到第 49 扇区 |
| `xv6-tiny210.bin` | 同一内核的裸镜像（带 zImage 头），给 U-Boot `bootz` 调试用 |
| `bl1/bl1.elf`、`bl1/bl1.map`、`kernel.elf`、`kernel.map` | 调试用 |

换板子编译前先 `make tiny210-clean`（或用不同的 `BUILD=` 目录），
否则旧的目标文件会混在一起。**不要把 `build/tiny210-qemu/` 的镜像写到卡上**。

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
make tiny210
```

```sh
diskutil unmountDisk /dev/disk4
```

```sh
sudo dd if=build/tiny210/tiny210-sd.img of=/dev/rdisk4 bs=512 skip=1 seek=1
```

```sh
sync
diskutil eject /dev/disk4
```

`skip=1 seek=1` 跳过第 0 扇区，分区表保持不变。也可以分两次写：

```sh
sudo dd if=build/tiny210/tiny210-bl1.bin of=/dev/rdisk4 bs=512 seek=1
sudo dd if=build/tiny210/xv6-tiny210.uImage of=/dev/rdisk4 bs=512 seek=49
```

之后只改了内核时，只需重写第 49 扇区那一条。

## 上电

核心板从 NAND 启动是默认值。把 SDK 底板上的启动选择开关拨到 SD 启动一侧
（丝印一般是 `SDBOOT`，另一侧是 `NAND`），插卡上电。串口接底板的 COM0
（UART0，115200 8N1）。

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

`make -f ports/tiny210/Makefile uarttest` 生成 `tiny210-uarttest.bin`（只有 BL1，
写到第 1 扇区）：它按 Tiny210 U-Boot 的时钟和 UART 参数，每半秒输出一行
`xv6 UART test ... line N`，LED 轮流亮，用来单独检查串口链路。

## LCD

`kernel/drivers/video/fbdev/s3c-fb.c` 驱动 S5PV210 的 FIMD（面板时序和引脚设置作为 platform data 由 `mach-s5pv210/devices.c` 提供）：GPF0–GPF3 设为 LCD 功能，DISPLAY_CONTROL
选 FIMD，Window 0 为 32 bpp（XRGB8888），帧缓冲放在 DRAM_BASE+128 MiB
（2 MiB，在页分配器范围之外，无缓存映射）。像素时钟取自 HCLK_DSYS
（166.75 MHz）分频。启动时先显示约一秒的彩条，然后清屏，之后内核控制台
的全部输出（包括 shell）同时显示在屏上，8x16 字体，800x480 时为 100x30 字符。
键盘输入仍然走串口。

面板型号在编译时选择，换型号前先 `make tiny210-clean`：

```sh
make -f ports/tiny210/Makefile S5PV210_LCD=s70   # 7" 800x480（默认）
make -f ports/tiny210/Makefile S5PV210_LCD=h43   # 4.3" 480x272
make -f ports/tiny210/Makefile S5PV210_LCD=w50   # 5" 800x480
make -f ports/tiny210/Makefile S5PV210_LCD=a70   # 7" 800x480
```

时序取自 FriendlyARM mini210/tiny210 BSP 的面板表（凭记录整理，未在实板
核对）；背光由 LCD 模块自身控制，本驱动不调节。

## /dev/fb0（ioctl + mmap）

`file_operations` 增加了 Linux 风格的 `ioctl(f, cmd, arg)` 和 `mmap(f, vma)`，
对应新系统调用 `ioctl(fd, cmd, arg)`、`mmap(addr, len, prot, flags, fd, off)`、
`munmap(addr, len)`（`kernel/sysfile.c`，常量在 `kernel/mman.h`）。

- 只支持字符设备的 `MAP_SHARED` 映射，地址由内核在 0x10000000–0x18000000 窗口里挑选
  （`addr` 参数只作提示，被忽略）。驱动在 `mmap` 回调里调用 `remap_pfn_range()`。
- ARM32 实现（`kernel/arch/arm/vm.c`）用 1 MiB 段描述符映射：物理地址和长度按
  1 MiB 对齐，用户可读写、XN、Normal 非缓存。fork 时子进程继承同一段映射（共享帧），
  exec/exit 随页表根一起丢弃，munmap 清除并刷 TLB。
- mmap 区域不能作为 read/write 等系统调用的缓冲区（copyin/copyout 只认普通页）。

帧缓冲核心 `kernel/drivers/video/fbdev/fbmem.c`：驱动填好 `struct fb_info` 后调用
`register_framebuffer()`，得到 `/dev/fb0`（主设备号 5，init 会 mknod）：

- `FBIOGET_VSCREENINFO` / `FBIOGET_FSCREENINFO`（`kernel/fb.h`，与 Linux 相同的
  编号和字段顺序）：分辨率、32 bpp、XRGB8888 的通道偏移（红 16、绿 8、蓝 0）、行长、
  缓冲区物理地址和长度；
- `mmap(0, smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)`：直接得到帧缓冲；
- 只要有进程打开着 /dev/fb0，文本控制台（fbcon）就暂停绘制，最后一个关闭时清屏恢复。

实板上由 s3c-fb 注册（`s3c-fb`），QEMU 替身由 `simplefb` 注册一块普通内存，
因此 `make tiny210-check` 会自动运行 `fbtest`：取得屏幕信息、mmap、画彩条和渐变并回读
校验、fork 后子进程写入父进程能看到、munmap 后再访问会触发缺页并被终止。

在板子上看效果：`fbtest 10`（画完保留 10 秒再退出，屏幕随后回到文本控制台）。

## USB 与 MT7601U 无线网卡

完整的硬件连接、initcall、USB 枚举、固件下载、SoftMAC、WPA2、收发路径及故障定位，
参见 [`doc/readme/tiny210/wifi.md`](../../doc/readme/tiny210/wifi.md)。

- `kernel/drivers/usb/host/ehci-hcd.c`：S5PV210 USB Host（底板 USB-A 口，EHCI
  0xEC200000，IRQ 55）。异步调度环上每个端点一个 QH，qTD 用 dummy-qTD 方式追加；
  提供 `usb_host_ops`（control/bulk/bulk_rx_arm/bulk_rx_complete），支持高速 hub，
  hub 后的全速/低速设备走 split；根口上的全速/低速设备交给 OHCI（尚未驱动）。
  枚举在 `ehci-enum` 内核线程里进行（需要中断和定时器已运行）。
- Host PHY1 上电在 `mach-s5pv210/devices.c` 的 `host_phy_init()`（按 Linux
  phy-s5pv210-usb2.c）。
- `kernel/drivers/usb/core/usb.c`：USB 总线、URB；`kernel/drivers/net/wireless/mt7601u.c`
  + `wpa_crypto.c`：从树莓派分支移植，固件改用 `request_firmware("mt7601u.bin")`，
  固件 `firmware/mt7601u.bin` 链接进内核（`kernel/drivers/base/firmware.c`）。
- 网络栈 `kernel/net.c`、`workqueue.c`、`epoll.c` 已编进 Tiny210 内核；
  `wifi_connect` 系统调用以及 `wifi`、`dhcp`、`ping` 等用户程序也已接入。
  驱动完成 WPA2 握手后注册的接口名是 `wlan0`。
- OTG 口（DWC2 主机模式，`kernel/drivers/usb/dwc2/hcd.c`）也已移植，
  `make -f ports/tiny210/Makefile S5PV210_USB_OTG=1` 启用。
- 时钟源改为 PWM 定时器 3 自由计数（PCLK/16，32 位），关中断忙等时 udelay 也准确；
  定时器 4 仍是 100 Hz 节拍。

QEMU 的 cubieboard 没有 USB 主机模型，这部分只能在实板上验证。预期串口输出：

```text
initcall: ehci-s5p (level 6)
ehci: s5p-ehci at ec200000, enumeration deferred to ehci-enum
ehci: 1 port(s), running, async schedule on
ehci: port 0: high-speed device connected
ehci: high-speed device 148f:7601 class 0 at address 1 (hub 0 port 0)
mt7601u: firmware v... validated
mt7601u: transport and MCU ready; ...
```

（若底板 USB-A 口经过 hub，会先出现 `ehci: hub 1: N ports`。）

## 从 SD 卡 FAT32 分区引导（xv6_tiny210.img、FS.IMG、/boot）

启动链（找不到文件时自动退回旧方式）：

```
iROM → BL1（第 1 扇区）→ MBR → 第一个 FAT32 分区（类型 0x0b/0x0c）
     → 根目录 xv6_tiny210.img（内核 uImage，长文件名，大小写不敏感）读到 DRAM+0x7FC0，校验 CRC，跳转
       （没有该文件：读第 49 扇区的原始镜像）
内核 → SDHCI（iROM 启动所用的 HSMMC 通道）→ fat32init：
       FAT32 根目录 FS.IMG 作为 xv6fs 根文件系统（可写，重启后保留）
       （没有 FS.IMG：使用内核内置的 RAM 盘）
init → FAT32 分区挂到 /boot（ls /boot 可列出）
mt7601u → request_firmware("mt7601u.bin")：先读 FAT32 根目录，再用内置副本
```

- BL1 的 FAT32 读取在 `bl1/bl1fat.c`（只读根目录、支持 VFAT 长文件名和 8.3 名、512 字节扇区），
  主机测试：`make -f ports/tiny210/Makefile fattest CARD=raw.img`。
- SD 驱动 `kernel/drivers/mmc/host/sdhci-s3c.c`：PIO、轮询、4 位、24 MHz，
  SCLK_MMC 取 24 MHz 晶振；通道号由 iROM 变量 0xD0037488 得出。
- FAT32/FS.IMG/raw 分区逻辑沿用 `kernel/fat32.c`，挂载用 `kernel/vfs.c`。

在 Mac 上准备卡（FAT32 分区挂在 `/Volumes/卷名`）：

```sh
make tiny210
cp build/tiny210/xv6_tiny210.img /Volumes/卷名/xv6_tiny210.img
cp build/tiny210/fs.img          /Volumes/卷名/FS.IMG
cp firmware/mt7601u.bin       /Volumes/卷名/mt7601u.bin
diskutil unmountDisk /dev/diskN
sudo dd if=build/tiny210/tiny210-bl1.bin of=/dev/rdiskN bs=512 seek=1   # 只需在 BL1 变化时
```

以后只改内核时，只需替换 FAT32 分区里的 xv6_tiny210.img。

## BL1 格式

S5PV210 iROM 从 SD 第 1 扇区读 16 KiB 到 iRAM 0xD0020000，校验 16 字节头后
从 0xD0020010 执行：

| 偏移 | 内容 |
| --- | --- |
| 0x0 | BL1 大小 0x4000 |
| 0x4 | 保留 0 |
| 0x8 | 校验和：第 16..16383 字节逐字节相加 |
| 0xC | 保留 0 |

`mktiny210.py check-bl1` 也接受九鼎 BSP `mkx210` 的格式（大小字段为 `"S5PC"`）。
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
| 串口完全没输出 | 启动开关没拨到 SD、串口接错口、第 1 扇区没写对（用 `mktiny210.py check-bl1` 检查） |
| `DMC0 DLL did not lock` 或 `DDR mismatch` | DDR 参数与板上颗粒不符，把串口日志发回来 |
| `aliases the lower half` 警告 | 颗粒行地址比配置少一位，已自动只用一半，不影响 xv6 |
| `not booted from SD/MMC` | iROM 不是从 SD 启动的 |
| `no uImage magic` / `bad header CRC` | 第 49 扇区没写对 |
| `uImage load/entry address does not match this board` | 镜像不是为 Tiny210 编的（应为 0x20008000） |
| `kernel data CRC mismatch` | SD 读取出错或镜像写入不完整 |
| BL1 正常但 xv6 无输出 | 把完整串口日志和 `kernel.map` 一起发回来 |
| 出现 `$` 但键盘无响应 | 检查 `timer:` 一行的频率和 VIC0 第 25 位 |

参考：S5PV210 RISC Microprocessor User's Manual（iROM 启动、时钟控制器、
DMC、VIC、PWM Timer、Watchdog、UART 章节）、S5PV210 iROM Application Note
（BL1 头、校验和、设备拷贝函数）、Samsung S5PV210 U-Boot
（`cpu/s5pc11x/s5pc110/cpu_init.S`、`board/samsung/smdkc110/lowlevel_init.S`）、
FriendlyARM Tiny210/Mini210 BSP，九鼎 X210 BSP 的 `sd_fusing/mkx210.c`（BL1 头格式参考），
Linux `Documentation/arch/arm/booting.rst`（ATAG 与入口寄存器）。
