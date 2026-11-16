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
  input/keyboard/s5pv210-keys.c      module_init      K1--K8 GPIO/EINT -> event0
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
                         · 用 iROM CopySDMMCtoMem 从固定第 49 扇区读取 uImage
                           到 DRAM+0x7FC0（payload 位于 DRAM+0x8000）
                         · 校验 uImage 头、load/entry 和数据 CRC
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
make tiny210           # build/tiny210/；固件/FS.IMG 拷到 /Volumes/bootfs，用户程序和内核放 TFTP 目录
make tiny210-sdimg     # 生成整卡镜像，并把它写到已挂载的 SD 卡（BL1@1、uImage@49）
make tiny210-check     # 自检 + BL1 主机测试 + QEMU cubieboard 测试
make tiny210-qemu      # QEMU 交互运行内核
make tiny210-clean
```

产物（`build/tiny210/`）：

| 文件 | 用途 |
| --- | --- |
| `tiny210-sd.img` | 可启动原始镜像：BL1@1、xv6 uImage@49，第 0 扇区（MBR）留空 |
| `tiny210-bl1.bin` | 16 KiB BL1（含 iROM 头），写到第 1 扇区 |
| `xv6-tiny210.uImage` | legacy uImage；写到第 49 扇区，是 BL1 唯一的启动内核来源，也可给 U-Boot `bootm` |
| `xv6_tiny210.img` | 同一 uImage 的拷贝，仅供 `tftpclient` 下载到 FAT32 的 `/boot`；BL1 不读取 |
| `xv6-tiny210.bin` | 同一内核的裸镜像（带 zImage 头），给 U-Boot `bootz` 调试用 |
| `bl1/bl1.elf`、`bl1/bl1.map`、`kernel.elf`、`kernel.map` | 调试用 |

换板子编译前先 `make tiny210-clean`（或用不同的 `BUILD=` 目录），
否则旧的目标文件会混在一起。**不要把 `build/tiny210-qemu/` 的镜像写到卡上**。

## SD 卡布局

正常启动只使用固定扇区：BL1 不解析 MBR，也不读取 FAT32。第 49 扇区起的
uImage 就是启动内核，不是救援副本。FAT32 分区只在内核启动后用于根文件系统
（`FS.IMG`）、`/boot` 和固件文件。

| 扇区 | 内容 |
| --- | --- |
| 0 | MBR（不动） |
| 1–32 | BL1（16 KiB） |
| 33–48 | 空 |
| 49–约 469 | xv6 uImage（约 210 KiB，`make` 时会打印实际扇区数） |
| MBR 指定的 FAT32 分区 | `FS.IMG`、`mt7601u.bin` 等内核侧文件；BL1 不读取 |

只要第一个分区从 uImage 结束扇区之后开始，分区表就不受影响；当前卡从
31248 扇区开始，余量很大。

## 写卡（macOS）

卡插好、FAT32 分区已挂载（默认 `/Volumes/bootfs`）后：

```sh
make tiny210-sdimg
```

这个目标先编译 `build/tiny210/tiny210-sd.img`，再找到 `TINY210_BOOTFS`
所在的整盘、卸载它，把镜像从第 1 扇区写到卡上（`skip=1 seek=1`，第 0
扇区的 MBR 保持不动），读回第 1 和第 49 扇区校验，最后推出卡。写盘用
sudo，会提示输入密码。卡没挂载时只生成镜像并打印手动命令；`SD_WRITE=0`
只构建、不写卡；`BOOTFS_EJECT=0` 写完后不推出。

以 `/dev/disk4` 为例，第一个分区从 31248 扇区开始，前面约 15 MiB 是空闲的
原始区，xv6 只占用扇区 1–469，四个分区都不会被改动。
**先用 `diskutil list` 确认 disk4 确实是这张 SD 卡**，写错盘会毁数据。
原始区里原有的 BL1/U-Boot（例如 FriendlyARM superboot）会被覆盖。

手工等价命令：

```sh
diskutil unmountDisk /dev/disk4
sudo dd if=build/tiny210/tiny210-sd.img of=/dev/rdisk4 bs=512 skip=1 seek=1
sync
diskutil eject /dev/disk4
```

`skip=1 seek=1` 跳过第 0 扇区，分区表保持不变。也可以分两次写：

```sh
sudo dd if=build/tiny210/tiny210-bl1.bin of=/dev/rdisk4 bs=512 seek=1
sudo dd if=build/tiny210/xv6-tiny210.uImage of=/dev/rdisk4 bs=512 seek=49
```

之后只改内核时，重新执行 `make tiny210-sdimg`（或第二条 `dd`，只写第 49
扇区）。FAT32 分区里的
`xv6_tiny210.img` 不参与启动；该分区只保存根文件系统 `FS.IMG`、固件和 `/boot`。

## 上电

核心板从 NAND 启动是默认值。把 SDK 底板上的启动选择开关拨到 SD 启动一侧
（丝印一般是 `SDBOOT`，另一侧是 `NAND`），插卡上电。串口接底板的 COM0
（UART0，115200 8N1）。

BL1 会打印 iROM 从哪个 SD 通道启动（0 或 2），两种它都能处理。

Tiny210 预期串口输出：

```text
xv6 BL1 for FriendlyARM Tiny210 (Samsung S5PV210): APLL 1000 MHz, MPLL 667 MHz, PCLK_PSYS 66.7 MHz
BL1: DMC0 DDR2 512 MiB at 0x20000000 OK
BL1: SD channel 0, uImage at sector 49
BL1: "xv6 ARM32 S5PV210", 214624 bytes
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
（4 MiB、两个扫描帧，在页分配器范围之外，无缓存映射）。像素时钟取自 HCLK_DSYS
（166.75 MHz）分频。启动时先显示约一秒的彩条，然后清屏，之后内核控制台
的全部输出（包括 shell）同时显示在屏上，8x16 字体，800x480 时为 100x30 字符。
键盘输入仍然走串口；板载按键由 input 子系统发布到 `/dev/input/event0`。

LCD 引脚初始化同时关闭 GPF0--GPF3 的内部上下拉，并使用 BSP 相同的 4x
驱动强度；否则复位后残留的下拉会拖慢并行 RGB/VCLK 边沿。控制台默认对 8x16
字体做单像素受限加粗，仍保持 100x30 字符；可用 `FBCON_BOLD=0` 编译对比原字体。
运行 `/bin/fbtest sharp 30` 会依次显示单像素竖线、横线、棋盘格和窄 RGB 条：
若这些图案也有彩色重影，应继续检查排线、面板型号和 VCLK 极性，而不是继续改字体。
默认极性来自 FriendlyARM BSP；只有测试图仍有横向重影时，才用
`LCD_VCLK_INVERT=0` 编译一次作对照。

`/dev/fb0` 支持 Linux 风格的 `FBIOGET_FSCREENINFO`、
`FBIOGET_VSCREENINFO`、`FBIOPAN_DISPLAY` 和 `MAP_SHARED mmap`。窗口环境由
内核 `win32k` 合成器与用户 `win32.c` 提供；执行 `/bin/windemo` 会显示 Windows
风格桌面、任务栏、根目录资源管理器、系统信息和 PTY shell 终端，可一起测试窗口
消息、GDI 绘制、窗口 front/back surface、VFS 目录读取以及 PTY/线程路径。

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

## 板载按键（K1--K8）

K1--K4 使用 GPH2[0:3]/EINT16--19，K5--K8 使用
GPH3[0:3]/EINT24--27；按键低电平有效，EINT16--31 共用 VIC0 IRQ 16。
`s5pv210-keys` 使用双边沿中断，IRQ 中屏蔽并确认触发位，随后由 workqueue 延迟
20 ms 读取稳定电平完成软件消抖。事件通过输入核心送到 `/dev/input/event0`：

```sh
/bin/evtest
```

K1--K8 分别报告 Linux 键值 `KEY_1`--`KEY_8`；按下为 `value=1`，释放为
`value=0`，一组变化以 `EV_SYN/SYN_REPORT` 结束。它是通用 event 接口，不会直接向
串口 shell 注入字符。详细调用链见
[`doc/readme/tiny210/drivers.md`](../../doc/readme/tiny210/drivers.md)。

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

## 从 SD 卡原始扇区引导（uImage、FS.IMG、/boot）

启动链：

```
iROM → BL1（第 1 扇区）
     → iROM CopySDMMCtoMem 读第 49 扇区 uImage 到 DRAM+0x7FC0，
       校验 header/load/entry/data CRC，跳到 DRAM+0x8000
       （BL1 不解析 MBR，也不读取 FAT32）
内核 → SDHCI（iROM 启动所用的 HSMMC 通道）→ fat32init：
       FAT32 根目录 FS.IMG 作为 xv6fs 根文件系统（可写，重启后保留）
       （没有 FS.IMG：使用内核内置的 RAM 盘）
init → FAT32 分区挂到 /boot（ls /boot 可列出）
mt7601u → request_firmware("mt7601u.bin")：先读 FAT32 根目录，再用内置副本
```

- BL1 只调用 iROM 的 `CopySDMMCtoMem`，本身没有 FAT32 读取器，也没有自己的 SDHCI 代码。
- SD 驱动 `kernel/drivers/mmc/host/sdhci-s3c.c`：PIO、轮询、4 位、24 MHz，
  SCLK_MMC 取 24 MHz 晶振；通道号由 iROM 变量 0xD0037488 得出。
- 根文件系统的 FAT32/raw 分区探测完全在内核里：`kernel/fat32.c` + `kernel/vfs.c`，
  仍按“原始 xv6 分区 → FAT32 `FS.IMG` → 内嵌 RAM 盘”的顺序选择。

在 Mac 上准备卡（FAT32 分区挂在 `/Volumes/bootfs`）：

```sh
make tiny210          # 编译内核、拷 mt7601u.bin 和 FS.IMG 到 FAT32
make tiny210-sdimg    # 把 BL1 和 uImage 写到第 1、49 扇区
```

卷名不同时加 `TINY210_BOOTFS=/Volumes/卷名`；要覆盖卡上已有的 `FS.IMG`
加 `BOOTFS_FSIMG=1`。以后只改内核时重新执行 `make tiny210-sdimg`（写第
49 扇区）；FAT32 里的文件不参与启动，只在内核启动后使用。

## 根文件系统放在 SD 卡第 2 分区

内核开机时先在 MBR 里找一个非 FAT 类型、且第 1 块是 xv6 超级块的主分区作为根文件系统
（日志 `xv6fs: selected raw p2 ...`），找不到才退回 FAT32 里的 `FS.IMG`。

`make tiny210` 在 SD 卡插着（`/Volumes/bootfs` 已挂载）时，会找到这张卡，把 `fs.img`
原样写进它的第 2 分区（`/dev/rdiskNs2`，用 sudo，会问密码）：

- 只写非 FAT/Apple/EFI 类型的分区（例如 Linux 0x83），防止写错盘。
- 每次都写（4 MiB，约 1 秒），写前先卸载整张卡，写完读回超级块校验，最后推出卡。
- 分区号可用 `ROOTFS_SLICE=N` 改。

卡上还没有第 2 分区时，在 FAT32 分区后面建一个至少 4 MiB 的 Linux 分区即可
（macOS：`diskutil addPartition diskNs1 %Linux% %noformat% 64M`，或用 `fdisk -e`）。

## 不拔卡更新用户程序（tftpclient + reboot）

板子连上 Wi-Fi 以后，可以从 Mac 下载用户程序到运行中的文件系统。内核本身
不能这样更新：BL1 只从第 49 扇区启动，而 `tftpclient` 写入的是 FAT32 的
`/boot`。换内核仍需插卡重写第 49 扇区。`tftpclient` 和 `reboot` 都取自
rpi3-lock 分支。

`make tiny210` 会把用户程序和 `xv6_tiny210.img` 拷到 macOS 自带 TFTP 服务的
目录 `/private/tftpboot`（UDP 69 端口）。这个目录属于 root，第一次需要执行一次：

```sh
sudo chown $(id -un) /private/tftpboot
sudo launchctl load -F /System/Library/LaunchDaemons/tftp.plist   # tftpd 没开时才需要
```

板子上：

```
$ tftpclient windemo /bin  # 默认服务器：从 /etc/tftp.conf 读取
$ tftpclient               # 下载 xv6_tiny210.img 到 /boot；BL1 不用它启动
$ reboot                   # S5PV210 软件复位（SWRESET），重新从 SD 卡启动；reboot -h 关机（拉低 PS_HOLD）
```

- `/etc/tftp.conf`（`server=IP`）由 `make tiny210` 用本机 en0–en3 的地址生成，也可以用
  `TFTP_SERVER=a.b.c.d` 指定。命令行写法：`tftpclient [IP[:端口] [文件名]]`。
- 文件先下载到 `/boot/TFTP.TMP`，完整收到后才复制成 `/boot/<文件名>`。传输中断时，
  旧文件不会被破坏。
- 更新单个程序：`tftpclient windemo /bin`。`make tiny210` 会把所有用户程序拷到
  `/private/tftpboot/tiny210/`（去掉 `_` 前缀），tftpclient 取 `tiny210/windemo`，
  先存成 `/bin/.tftp.tmp`，收完整再替换 `/bin/windemo`。目录可以是根文件系统里的任意目录。
- 下载的 `xv6_tiny210.img` 只是 FAT32 `/boot` 里的一个文件；它不会成为下一次启动的内核。
- 内核不再内嵌 fs.img，根文件系统只能来自 SD 卡上的 `FS.IMG`。卡上没有
  FS.IMG 时，内核会打印提示后 panic。QEMU 替身没有 SD 卡，所以那一版仍然内嵌。
- 不能用 tftpclient 更新 `FS.IMG`，因为它是正在挂载的根文件系统。`tftpclient`、`reboot` 这两个
  命令本身也在 FS.IMG 里，所以第一次要插卡执行一次 `make tiny210 BOOTFS_FSIMG=1`。
- `tiny210-sd.img` 是整张卡的原始镜像：BL1 在扇区 1、uImage 在扇区 49。它经过的是
  原始扇区，放进 /boot 不起作用；换了 BL1 或内核都要插卡用 `dd` 写入。

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
芯片没有烧写安全启动熔丝，iROM 只校验和、不验签。BL1 代码约 6 KiB，
链接脚本保证它不超过 16 KiB−16 字节。

BL1 调用的 iROM 服务：

- `0xD0037488`：iROM 启动所用的 SD 控制器基址（0xEB000000 = 通道 0，
  0xEB200000 = 通道 2）；
- `0xD0037F98`：`CopySDMMCtoMem(ch, start_block, u16 count, dst, init)`
  的函数指针，`init=0` 沿用 iROM 已初始化好的卡。BL1 用它把第 49 扇区起的
  uImage 读到 DRAM+0x7FC0，不包含任何 FAT32 或 SDHCI 代码。

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
| `SD read of the uImage header failed` | 第 49 扇区没有 uImage，或扇区写入失败 |
| `no uImage magic` / `bad header CRC` | 第 49 扇区的 uImage 损坏或不完整，重新生成并写入 |
| `uImage load/entry address does not match this board` | 镜像不是为 Tiny210 编的（应为 0x20008000） |
| `kernel data CRC mismatch` | SD 读取出错或镜像写入不完整 |
| 内核报 FAT32/FS.IMG 或 raw 分区错误 | 根文件系统探测在内核侧，与 BL1 无关；检查 FAT32 分区、`FS.IMG`、第 2 分区 |
| BL1 正常但 xv6 无输出 | 把完整串口日志和 `kernel.map` 一起发回来 |
| 出现 `$` 但键盘无响应 | 检查 `timer:` 一行的频率和 VIC0 第 25 位 |

参考：S5PV210 RISC Microprocessor User's Manual（iROM 启动、时钟控制器、
DMC、VIC、PWM Timer、Watchdog、UART 章节）、S5PV210 iROM Application Note
（BL1 头、校验和、设备拷贝函数）、Samsung S5PV210 U-Boot
（`cpu/s5pc11x/s5pc110/cpu_init.S`、`board/samsung/smdkc110/lowlevel_init.S`）、
FriendlyARM Tiny210/Mini210 BSP，九鼎 X210 BSP 的 `sd_fusing/mkx210.c`（BL1 头格式参考），
Linux `Documentation/arch/arm/booting.rst`（ATAG 与入口寄存器）。
