# Tiny210 上板调试记录：BL1、SD、USB 与串口

本文记录在 FriendlyARM Tiny210（S5PV210）实板上调通 BL1 → xv6 启动链、
SD 卡 FAT32、EHCI/MT7601U 时遇到的问题、原因和解决办法。每一条都对应
一次实际的串口现象，便于以后遇到同类问题时对照。

相关代码：`kernel/arch/arm/mach-s5pv210/bl1/`（BL1）、
`kernel/drivers/mmc/host/`（SD）、`kernel/drivers/usb/host/ehci-hcd.c`、
`ports/tiny210/`（构建与烧录）。

## 1. 启动链与各阶段运行在哪里

```
上电 → iROM（BL0，片内 ROM 0xD0000000）
     → 从 SD 第 1 扇区读 16 KiB 到 iRAM 0xD0020000，校验 16 字节头
     → 跳到 0xD0020010：BL1（在 iRAM 中运行）
         时钟 / UART / DDR 初始化 → 读 MBR → FAT32 → xv6_tiny210.img
         → 读到 DDR 0x20007FC0（uImage 数据正好落在 0x20008000）
     → 跳到 0x20008000：xv6 内核（在 DDR 中运行）
         SDHCI → fat32init（FS.IMG 作根文件系统）→ init 挂载 /boot
```

- iROM 跳到 BL1 时 **DDR 还没有初始化**，能用的只有 iROM（只读）和
  iRAM（0xD0020000–0xD0037FFF，96 KiB 片内 SRAM）。所以 BL1 只能在 iRAM
  里运行，最大 16 KiB（含 16 字节头）。
- BL1 的地址分配（`bl1/bl1.ld`、`bl1/start.S`）：

  | 内容 | 地址 |
  | --- | --- |
  | 代码入口 `_start` | 0xD0020010（iROM 固定跳转地址） |
  | 代码、常量、数据、BSS | 紧随其后，约 13 KiB |
  | 异常向量表（VBAR） | 单独的 `.vectors` 段，32 字节对齐 |
  | 栈 | 0xD0036000 向下 |
  | iROM 自用数据 | 0xD0037xxx（启动通道变量 0xD0037488、拷贝函数指针 0xD0037F98） |

- DDR 初始化好以后，大块数据都放在 DDR：内核 0x20007FC0、扇区缓冲
  0x21000000、ATAG 0x20000100。

## 2. 串口：板子正常，终端显示乱码

**现象**：`minicom` 里每行只有 7 个左右固定的乱码，如 `` `J&?C?? ``。

**原因**：

1. minicom 实际按 9600 接收，而板子按 115200 发送，速率差 12 倍，正好
   对应“一行约 90 个字符只收到 7 个”。
2. 后台残留了好几个脱离终端的 minicom（甚至有 9600 的），同时抢读
   同一个串口。新开的 minicom 报 `Failed to lock`，screen 报
   `could not find a pty`。

**验证方法**：关掉所有 minicom 后，直接用 Python termios 在 115200 读
`/dev/cu.usbserial-1110`，能读到完整文字；改用 9600 读，得到的正是同样的乱码。

**做法**：

```sh
screen /dev/cu.usbserial-1110 115200      # 退出：Ctrl-A K y
screen -ls ; screen -r <id>                # 接回已有会话
```

`make -f ports/tiny210/Makefile uarttest` 生成一个只打印、不加载内核的
BL1（`tiny210-uarttest.bin`），用来单独检查串口链路。

## 3. LED 判断 BL1 走到哪一步

Tiny210 核心板的 LED1–4 接在 GPJ2[0:3]，低电平点亮。BL1 每完成一阶段就
多点亮一个：1 = BL1 在运行，2 = PLL 设好，3 = DDR 自检通过，
4 = 内核已读入、即将跳转。出错停下时，当前阶段的 LED 会闪烁。

注意：复位后 GPJ2 是带下拉的输入，四个灯默认就是全亮的，“全亮”本身
不能说明 BL1 在运行。要看开头是否先闪 3 下。

## 4. EHCI 上第一个 bulk OUT 永远发不完

**现象**：控制传输正常，MT7601U 固件上传的第一个 14 KiB bulk OUT 超时。
qTD 状态是 `0xa0` = Active + **Data Buffer Error**。

**原因**：主机控制器发 OUT 包时，从 DRAM 取数据跟不上（欠载），于是不停
重试。当时 CPU 完全不开 cache，每条指令都要去 DRAM 取，总线被占满。

**解决**（f9bfbc6）：

- Synopsys EHCI 的 `INSNREG01`（cap+0x94）把 OUT/IN 包缓冲阈值设成整包
  512 字节（`0x00800080`），整包到齐才开始发；
- 打开 I-cache 和分支预测（D-cache 仍关，DMA 不用维护缓存；exec 后会
  清 I-cache）。

**顺带修的隐患**：控制器若支持 64 位地址（HCCPARAMS bit0），会读写
qTD/QH 末尾的 `buf_hi[5]`。软件字段必须放在这 5 个字之后，和 Linux 一样。
Tiny210 实测 `hccparams a010`，不支持 64 位，但布局已按规范留出位置。

## 5. iROM 拷贝函数读第 0 扇区卡死

**现象**：BL1 打印 `SD channel 0` 后不再有输出。

**原因**：iROM 的 `CopySDMMCtoMem(0xD0037F98)` 读第 49 扇区（原始内核）
一直正常，但读第 0 扇区（MBR）会卡死。最初还把目标放在了 iRAM 的静态
数组里，改到 DDR 后仍卡，说明问题在第 0 扇区本身。

**解决**（49da8ac）：不再依赖 iROM 读 FAT32。把内核 SD 驱动的协议部分拆成
`kernel/drivers/mmc/host/sdcore.c`（无内核依赖、无除法），BL1 和内核共用：

- 时间基准用 PWM 定时器 3 自由计数（PCLK_PSYS/16）；
- 引脚 GPG0、HSMMC0 时钟门控、SCLK_MMC 取 24 MHz 晶振，和内核一致；
- 卡识别 375 kHz，数据 24 MHz、4 位、单块 PIO；
- 自己的初始化失败时，退回 iROM 函数（最后一个参数 `init=1` 让它重新
  初始化卡），读第 49 扇区的后备内核。

## 6. 初始化“无声失败”：R1b 的 busy 结束判断

**现象**：`own SD init failed`，但没有任何具体原因。

**原因**：CMD2/3/7 失败时原来不打日志。CMD7（SELECT_CARD）是带 busy 的
R1b 响应，原来只等 Transfer Complete 中断，这个控制器在没有数据阶段时
不一定会产生它。

**解决**（0cec709）：每一步都打日志；R1b 的 busy 期以 Transfer Complete
**或** PRNSTS bit20（DAT0 电平）变高为结束。之后初始化成功：
`SD 2.0 card, ocr 0xc0ff8000`、`SDHC/SDXC card, 4-bit 24 MHz, rca 0x21e1`。

## 7. 没有异常向量表时，出错只会“卡死”

正常执行不需要向量表，CPU 只是顺序取指令；只有发生异常（data abort、
未定义指令、中断）时才会跳到“向量基址 + 偏移”。BL1 原来没有设置 VBAR，
异常会进入 iROM 自己的处理程序，表现为无输出卡死或重新启动，看不出原因。

**解决**（eb41bd3）：BL1 启动时设置 VBAR 指向自己的向量表，异常时打印：

```
BL1: CPU exception: data abort lr=... dfar=... dfsr=... ifsr=...
```

内核另有自己的向量表（`kernel/arch/arm/vectors.S`），因为它依赖定时器
中断和 SVC 系统调用。

## 8. 一行 `.balign 32` 让所有地址错开 16 字节

**现象**：加上向量表后，从第一行起字符串就成了乱码，并走进各种错误分支，
最后触发异常、复位。

**原因**：VBAR 要求 32 字节对齐，向量表写在 `.text.start` 里并用了
`.balign 32`。链接器因此把整个 `.text` 对齐到 32 字节，起始地址变成
0xD0020020。但 iROM 总是把镜像放到 0xD0020010 再跳过去，于是所有绝对
地址（字符串、常量池、跳转表）都偏了 16 字节。

**解决**（fcdbbce）：向量表放进单独的 `.bl1vectors` 段，排在 `.text` 之后；
`bl1.ld` 增加断言 `ADDR(.text) == 0xD0020010`，入口地址不对时编译直接报错。

检查办法：

```sh
arm-none-eabi-objdump -h build/tiny210/bl1/bl1.elf   # .text 必须从 d0020010 开始
xxd -s 16 -l 16 build/tiny210/tiny210-bl1.bin        # 第一条指令 cpsid if = c0 00 0c f1
```

## 9. 解析 MBR 时的对齐错误

**现象**：

```
BL1: sd: CMD17, lba 0 ... transfer complete
BL1: CPU exception: data abort lr=0xd00217fc dfar=0x210001c6 dfsr=0x00000001
```

**分析**：`dfsr=0x1` 是 alignment fault；`dfar=0x210001c6` = 扇区缓冲
0x21000000 + 0x1C6，正是 MBR 第一个分区表项的起始 LBA（446+8），
不是 4 字节对齐的地址。代码是逐字节读取的，但 clang 针对 Cortex-A8 把
它合并成了一条非对齐的 `ldr`；而 iROM 留下的 SCTLR.A=1（开着对齐检查）。

**解决**（6653ddc）：BL1 用 `-mno-unaligned-access` 编译，并在启动时清
SCTLR.A。内核的 `entry.S` 本来就清了 A 位。

## 10. SD 卡布局与文件

| 位置 | 内容 |
| --- | --- |
| 第 0 扇区 | MBR（不动） |
| 第 1–32 扇区 | BL1（16 KiB） |
| 第 49 扇区起 | 后备内核（原始 uImage，约 8.6k 扇区） |
| 第 31248 扇区起 | FAT32（`/Volumes/bootfs`） |
| FAT32 根目录 | `xv6_tiny210.img`（内核）、`FS.IMG`（根文件系统，可写）、`mt7601u.bin` |

- `xv6_tiny210.img` 是 VFAT 长文件名，BL1 不区分大小写匹配。
- 内核镜像里还内嵌了一份 4 MiB 的 fs.img 作为后备根文件系统，
  卡上没有 `FS.IMG` 时使用（只在内存中，修改不保存）。
- `make tiny210` 会把内核和固件拷到 `/Volumes/bootfs`；`FS.IMG` 只在卡上
  没有时才写（`BOOTFS_FSIMG=1` 强制覆盖）。BL1 变化时需要手动：

```sh
sudo dd if=build/tiny210/tiny210-bl1.bin of=/dev/rdisk4 bs=512 seek=1
```

## 11. 固件与文件的加载时机

- 根文件系统挂载之前就要用的东西只能随内核一起带进来：`mt7601u.bin`
  的内置副本用 `.incbin` 链接进内核（`request_firmware()` 的后备）。
- 根文件系统之后：`fat32init()` 在 initcall 之后运行，FAT32 卷在内核里
  直接可读；init 把它挂到 `/boot`。`request_firmware()` 先读 FAT32 根目录，
  失败再用内置副本。
- MT7601U 在 `ehci-enum` 内核线程里枚举，此时调度器、中断和定时器都已
  运行，FAT32 也已就绪。

## 12. 内核启动前的长时间停顿

**现象**：BL1 打印 `starting xv6 at 0x20008000` 之后，要停好几秒内核才有输出。

**原因**：初始化页分配器时，xv6 把 128 MiB 空闲内存每一页都填“垃圾值”
（`kfree` 里的 `memset(va, 1, PGSIZE)`，用来发现悬空指针）。内核的
`memset` 是逐字节循环，D-cache 又是关的，相当于对 DRAM 做了约 1.3 亿次
未缓存的单字节写。

**解决**（e1263cd）：`memset`/`memmove` 改成按对齐的 32 位字操作；
`freerange()` 在启动时把从未用过的页交给分配器时跳过填充，之后正常
`kfree` 仍然填充。

## 13. Wi-Fi 扫描永远停在信道 1：延时任务从来不执行

**现象**：扫描列出的 AP 全都是 `channel=1`；`wifi` 打印 `joining TP-Link_B114`
后 30 秒超时，始终没有 `target ... found`。

**原因**：workqueue 的延时任务（`queue_delayed_work`）靠
`workqueue_timer_tick()` 推进 `work_jiffies`。树莓派的定时器中断会调用它，
ARM32 移植时漏掉了，`work_jiffies` 一直是 0。于是 MT7601U 的 `state_work`
（定时换信道、认证/关联超时重试）和 `wifi_connect` 安排的“立即开始连接”
都永远不会触发。

为什么系统其他部分完全正常：进程调度、`sleep()`、`uptime` 用的是另一套
计时（`ticks` 和 `sched_tick()`，在 `arm32_timer_tick()` 里推进）；立即执行的
`queue_work()`（例如 mt7601u 的收包 NAPI）也不依赖 `work_jiffies`。只有延时
任务受影响，所以问题很隐蔽。

**解决**（0bd029e）：`arm32_timer_tick()` 每个节拍（10 ms，与树莓派相同）
调用一次 `workqueue_timer_tick()`。之后扫描覆盖 1–11 信道，在信道 9 找到
TP-Link_B114，认证、关联成功，有一次 WPA2 四次握手完整完成。

## 14. 超时后清理 QH 把数据 toggle 复位了

**现象**：某个 bulk OUT 超时后重新连接，AP 反复发 M1，驱动反复回 M2
（replay 计数递增），AP 始终收不到。

**原因**：EHCI 驱动在超时后用 `qh_flush()` 清理 QH，把 overlay 的 token 整个
清零，数据 toggle 也就回到了 DATA0。但设备端的 toggle 没变，于是之后每个
包都被设备当成重发包，ACK 之后又悄悄丢弃。主机这边看来全部“发送成功”。

**解决**（f649f28）：清理时只清除 Active/Halted/Ping，保留 DT 位；
bulk OUT 的超时从 1 秒放宽到 3 秒，给 PING/NAK 中的设备更多时间。

## 15. MT7601U 发送端点一直 NAK（排查中）

**现象**：认证、关联、EAPOL 等前若干帧都能发出去，之后 EP8 的 bulk OUT
一直不完成。qTD 状态 `0x80`（只有 Active），QH token 带 Ping 位，说明设备对
PING 一直回 NAK，即设备侧发送缓冲没有腾出来。

**尝试 1：换端点**（da79cff，已撤回）。MT7601U 有 6 个 bulk OUT
（0x08、0x04、0x05、0x06、0x07、0x09）。参照 Linux mt7601u，0x08 是 MCU 带内命令
端点，管理队列（QSEL_MGMT）对应第 6 个端点 0x09，于是把 802.11 帧改到 EP9。
结果 EP9 从第一个认证帧就 NAK，比 EP8 更糟，说明在本驱动当前的初始化
状态下这个推测不成立。USB core 现在会记录全部 bulk OUT 端点
（`usb_device.bulk_out_eps[]`），方便后续继续试验。

**尝试 2：读空 TX 状态 FIFO**（5ac8c59，待上板验证）。驱动从未读取
`TX_STAT_FIFO_EXT`（0x1798）/`TX_STAT_FIFO`（0x1718），而 Linux 每次发送后都会读。
推测 FIFO 满后 MAC 不再接收新帧，于是端点一直 NAK，与“前几帧正常、之后
永远 NAK”的现象吻合。现在每发一帧就读空 FIFO，并打印前 8 条状态
（`acked` / `not acked`）用于确认。

## 16. 经验小结


1. 先确认观察手段：串口终端设置、后台进程、LED 默认状态，都可能造成误判。
2. 早期代码一定要有异常向量和逐步日志，否则所有问题看起来都是“卡死”。
3. 位置相关的代码（BL1）要在链接脚本里断言入口地址。
4. iROM / Boot ROM 提供的函数只在它们被验证过的用法下可靠（这里：读
   第 49 扇区到 DDR），超出这个范围就自己实现。
5. iROM 留下的 CPU 状态（SCTLR.A、V 位、cache）要显式设成自己需要的值。
6. 主机测试只能验证逻辑（FAT32 解析、CRC、ATAG），验证不了内存类型、
   对齐、时序这些硬件相关的问题。
7. 从另一个平台移植代码时，要核对它依赖的“周期性调用”（定时器节拍里的
   `workqueue_timer_tick()` 这类）是否都接上了，缺了往往不会崩溃，只会
   让某些功能静静地失效。
8. 出错恢复路径（超时、flush）本身也要正确：错误恢复时丢掉的协议状态
   （USB 数据 toggle）会让之后所有“成功”的传输变成无声丢包。
9. 不熟悉的硬件行为（端点用途、状态 FIFO）先对照 Linux 驱动，但改动要能
   上板验证并且可以撤回，像第 15 节那样记录每一次尝试的结果。
