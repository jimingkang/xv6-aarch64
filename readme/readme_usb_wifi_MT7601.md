# xv6-aarch64 Raspberry Pi 3 DWC2 USB 与 MT7601U Wi-Fi

本文专门说明 Raspberry Pi 3 上 MT7601U USB Wi-Fi 的硬件拓扑、DWC2 host
controller、USB 枚举、DMA、firmware、SoftMAC、WPA2、收发数据和 IRQ/workqueue
下半部。BCM43430/43455 板载 SDIO FullMAC 请看
[readme_sdio_wifi.md](readme_sdio_wifi.md)。通用问题修复记录请看
[readme_bug_fix.md](readme_bug_fix.md)。

## 1. 总体架构

MT7601U 不是直接挂在 CPU 总线上的网卡。RPi3 的 USB 路径是：

```mermaid
flowchart TB
    CPU["Cortex-A53 / xv6"]
    DWC2["BCM2837 DWC2 USB Host<br/>MMIO 0x3f980000"]
    HUB["LAN9514 USB Hub"]
    USB["USB device 148f:7601"]
    MCU["MT7601U MCU + MAC/BBP/RF"]
    AIR["2.4 GHz 802.11"]
    NET["net_device wlan1<br/>Ethernet/IP/TCP"]

    CPU -->|MMIO + DMA| DWC2
    DWC2 --> HUB
    HUB --> USB
    USB --> MCU
    MCU --> AIR
    USB --> NET
```

驱动分为三层：

| 层 | 代码 | 职责 |
|---|---|---|
| DWC2 HCD | `kernel/dwc2.c` | root hub、hub port、control/bulk、DMA、host channel IRQ |
| USB core | `kernel/usb.c/.h` | `usb_device`、ID 匹配、driver probe/remove |
| MT7601U | `kernel/mt7601u.c` | firmware、eFuse、MAC/BBP/RF、SoftMAC、WPA2、`wlan1` |

## 2. 为什么它既是 USB 设备又是 Wi-Fi 设备

USB 只解决主机与 MT7601U 芯片之间的传输：枚举、endpoint、Bulk IN/OUT、DMA
和完成通知。802.11 扫描、认证、关联、加密和无线帧解析属于上层 MT7601U 驱动。

```text
DWC2 识别 148f:7601
  -> usb_device
  -> usb_bus 匹配 mt7601u_driver
  -> mt7601u_probe()
  -> SoftMAC 初始化
  -> 关联完成
  -> register_netdev(wlan1)
```

它不能按 CDC-ECM 网卡处理，因为 MT7601U 暴露的是厂商专用接口，不会直接提供
标准 Ethernet USB class。

## 3. DWC2 与外置 hub 枚举

`dwc2_driver_init()` 注册 platform driver。probe 依次完成：

1. 通过 firmware property mailbox 打开 USB HCD 电源；
2. 初始化 DWC2 host mode 和 root port；
3. 读取 root device descriptor；
4. 识别 LAN9514 hub；
5. 对每个外部 port 执行状态读取、上电和 reset；
6. 为子设备分配地址并读取 descriptor/configuration；
7. 找到 `VID=0x148f, PID=0x7601`；
8. 解析两个 Bulk IN 和一个 Bulk OUT endpoint；
9. 调用 USB bus 的 driver matching。

典型日志：

```text
dwc2: root device class=9 vid=424 pid=2514 configs=1 mps=64
dwc2: hub port=3 child class=0 vid=148f pid=7601 mps=64
dwc2: MT7601U configured; bulk-in=4,5/512 bulk-out=8/512
dwc2: MT7601U handed to usb bus
```

endpoint 分工：

| Endpoint | 方向 | 用途 |
|---|---|---|
| EP4 | device → host | 802.11 RX/data/mgmt/EAPOL |
| EP5 | device → host | MCU command response |
| EP8 | host → device | firmware、MCU command、TX frame |

## 4. MT7601U firmware 为什么每次启动都下载

`MT7601U.BIN` 是芯片 MCU 的运行程序。设备主要把永久校准信息和 MAC 地址放在
eFuse/EEPROM，MCU RAM 属于易失存储，因此每次上电都要重新下载 firmware。这与
Linux `mt7601u` 驱动的基本模型一致。

驱动从 bootfs 读取：

```text
MT7601U.BIN
```

下载步骤：

```text
解析 firmware header
  -> 分离 IVB / ILM / DLM
  -> Bulk OUT 发送分块数据
  -> FCE/PDMA 把数据放入 MCU RAM
  -> 设置启动向量
  -> 等待 MT_MCU_COM_REG0 == 1
```

成功日志：

```text
mt7601u: firmware v0.1.0 ... validated
mt7601u: ILM firmware upload ...
mt7601u: MCU firmware running
```

## 5. eFuse、MAC、BBP 与 RF 初始化

MCU 启动后，`mt7601u_read_eeprom()` 通过 `MT_EFUSE_CTRL` 每16字节读取一次校准
数据，得到：

- permanent MAC；
- country/regulatory 信息；
- frequency offset；
- RSSI/LNA/temperature 参数。

随后依次执行：

```text
basic_hw_init
  -> WLAN clock/PLL
  -> USB DMA
  -> permanent MAC registers

mcu_channel_init
  -> EP8 command
  -> EP5 response

full_hw_init
  -> MAC register table
  -> BBP register table
  -> RF register table
  -> R/TXDCOC/LOFT/TXIQ/RXIQ/DPD calibration
```

MT7601U 的 MAC、BBP、RF 是不同硬件部分：MAC 处理802.11帧和队列；BBP处理数字
基带；RF控制模拟射频、信道、增益和频率校准。

## 6. SoftMAC 工作模型

BCM43455 是 FullMAC，关联控制主要通过 BCDC 命令交给 firmware。MT7601U 是
SoftMAC，ARM CPU 驱动必须处理更多802.11逻辑：

```text
扫描信道
  -> 解析 beacon/probe response
  -> 选择目标 BSSID/channel
  -> Open-System authentication
  -> association request + RSN IE
  -> WPA2 EAPOL M1/M2/M3/M4
  -> 安装 PTK/GTK/CCMP key
  -> register_netdev(wlan1)
```

当前目标网络由驱动配置为 `TP-Link_B114`。关联完成日志包括：

```text
mt7601u: Open-System authentication accepted
mt7601u: 802.11 associated ... WPA2 handshake pending
mt7601u: sent WPA2 EAPOL M2
mt7601u: sent WPA2 EAPOL M4
mt7601u: WPA2 four-way handshake complete
mt7601u: wlan1 registered mac=...
```

## 7. TX 数据路径

网络层调用 `mt7601u_net_xmit()` 后：

```text
Ethernet frame
  -> LLC/SNAP
  -> 802.11 data header
  -> TXWI
  -> USB DMA header/padding
  -> EP8 Bulk OUT
  -> DWC2 host channel
  -> MT7601U MAC/BBP/RF
  -> wireless medium
```

管理帧、EAPOL 与普通 IP 数据最终都通过 Bulk OUT，但使用不同 WCID、加密状态和
802.11 frame control。`usb_lock` 串行化 DWC2 channel/register/DMA 状态；
`mt7601u.lock` 保护 SoftMAC 状态、密钥、扫描表和 TX/RX buffer 使用。

## 8. RX 数据路径与 DMA

EP4 RX buffer 会长期预提交：

```text
mt7601u_poll()
  -> bulk_rx_arm(EP4, mt_rx_buf)
  -> DWC2 channel 4 programmed
  -> MT7601U 返回 USB packet
  -> DMA 写入 RAM
  -> host-channel IRQ
  -> bottom half bulk_rx_complete()
  -> mt7601u_rx_parse()
```

DWC2 是 bus master。CPU 使用 ARM physical address，而 DWC2 DMA 需要 VideoCore
bus alias：

```c
#define DWC2_DMA_BUS(a) (0xc0000000U | (uint32)V2P(a))
```

`0xc0000000` 不是新的 RAM，也不是 CPU 虚拟地址；它是同一片 RAM 面向
VideoCore-side bus master 的 coherent/uncached alias。DMA 前后必须执行正确的
cache clean/invalidate，避免 CPU cache 与 DMA 内存内容不一致。

`HCTSIZ` 的剩余字节数用于计算实际接收长度。短包是合法完成，DATA0/DATA1
toggle 只有在成功传输后更新。

## 9. DWC2 host-channel IRQ 9

DWC2 初始化打开：

```text
HCINTMSK(channel) = XFERCOMPL | CHHLTD | NAK | error bits
HAINTMSK          = active asynchronous channels
GINTMSK.HCHINT    = 1
GAHBCFG.DMA_EN    = 1
GAHBCFG.GLBLINTRMSK = 1
```

BCM2837 legacy IRQ 9 到达后，`dwc2_irq()` 只执行顶半部：

```c
channels = rd(HAINT) & rd(HAINTMSK);
wr(HAINTMSK, rd(HAINTMSK) & ~channels);
dwc2_irq_pending |= channels;
if(channels & (1U << 4))
    mt7601u_rx_irq();
```

它不清除每个 channel 的 `HCINT`，因为下半部还要读取 `HCINT/HCTSIZ`、区分
XFERCOMPL/NAK/error并计算 DMA 实际长度。临时关闭对应 `HAINTMSK` 可避免 level
interrupt 在完成项尚未消费时重复进入。

当前构建不考虑 CDC USB 有线网卡，因此 DWC2 IRQ 不再投递公共
`net_deferred_work`。channel 4完成只唤醒MT7601U私有`rx_work`。

当前 channel 分工：

| Channel | 用途 |
|---|---|
| channel 2 | CDC RX（如果存在 CDC Ethernet） |
| channel 4 | MT7601U EP4 异步 Bulk IN |
| 其他同步 channel | control、枚举、MCU response、Bulk OUT |

## 10. workqueue 下半部

旧实现每2 ms同步轮询 `HCINT`，后来改为 IRQ pending + 10 ms Timer poll。当前实现
为MT7601U建立专用`mt7601u_wq`：

```mermaid
sequenceDiagram
    participant HW as DWC2 HW
    participant IRQ as IRQ 9 top half
    participant WQ as mt7601u_wq
    participant KW as kworker
    participant MT as mt7601u
    participant NET as network stack

    HW->>IRQ: HCHINT channel 4
    IRQ->>IRQ: mask HAINT channel + set pending
    IRQ->>WQ: queue_work(rx_work)
    IRQ-->>HW: EOI / return
    WQ->>KW: wakeup
    KW->>MT: rx_work
    MT->>HW: bulk_rx_complete()
    MT->>MT: RXWI + 802.11 + EAPOL/data
    MT->>NET: net_rx_dev(wlan1)
    MT->>HW: re-arm EP4 channel 4
```

因此耗时的 cache maintenance、RXWI解析、802.11状态机和网络栈不在 hard IRQ
中执行。扫描dwell、认证/关联/EAPOL watchdog使用`state_work`的`delayed_work`。
状态worker不再每10 ms自重排，而是预约下一个真实期限：扫描信道500 ms、认证
或关联响应1 s、WPA2 M1/EAPOL watchdog 5 s；RX事件改变状态时通过
`mod_delayed_work()`提前或延后期限。握手完成后不再提交状态work。

相关通用实现位于：

- `kernel/workqueue.c/.h`：命名队列、FIFO、按期限排序的delayed队列和`kworker`；
- `kernel/proc.c/.h`：`kthread_create()`；
- `kernel/net.c`：专用`net_wq`和TCP重传`delayed_work`；
- `kernel/timer.c`：只推进10 ms workqueue时钟并把到期work入队。

### MT7601U 路径涉及的队列清单

当前内核共有 `system_wq`、`net_wq`、`brcmf0_wq`、`brcmf1_wq` 和
`mt7601u_wq`。MT7601U 数据路径直接使用设备专用的 `mt7601u_wq` 和共享网络层
的 `net_wq`；不会把 RX 投递到 `system_wq`，也不会进入 BCM43455 的队列。

| 队列/工作项 | 类型 | 触发源 | 功能 |
|---|---|---|---|
| `mt7601u_wq` | 专用 workqueue，1个worker | 驱动初始化 | 串行执行同一USB Wi-Fi设备的RX和状态机 |
| `mt7601u.rx_work` | 普通 work | DWC2 channel 4 IRQ | 消费DMA completion、解析RXWI/802.11并重新arm EP4 |
| `mt7601u.state_work` | delayed work | 扫描/认证/关联/EAPOL状态变化 | 仅在下一个真实Wi-Fi期限运行 |
| `net_wq` | 网络层专用 workqueue，1个worker | `netinit()` | 执行与具体网卡无关的TCP定时任务 |
| `tcp_retransmit_work` | delayed work | TCP存在未确认段 | 在所有连接中最早的`tx_deadline`到期时重传 |
| `system_wq` | 通用队列，4个worker | `schedule_work()` | 通用fallback；当前MT7601U路径不使用 |

运行关系：

```mermaid
flowchart TD
    IRQ["DWC2 channel 4 IRQ"] --> RX["mt7601u.rx_work"]
    RX --> MWQ["mt7601u_wq kworker"]
    MWQ --> PARSE["DMA completion / RXWI / 802.11 / net_rx_dev"]

    EVENT["scan/auth/assoc/EAPOL状态变化"] --> MOD["mod_delayed_work(next Wi-Fi deadline)"]
    MOD --> SORT["全局expires有序delayed链表"]
    SORT -->|到期| STATE["mt7601u.state_work"]
    STATE --> MWQ

    TCP["最早TCP tx_deadline"] --> SORT
    SORT -->|到期| RETX["tcp_retransmit_work"]
    RETX --> NWQ["net_wq kworker"]
```

`mt7601u.state_work` 的期限为：扫描信道500 ms、认证/关联响应1 s、WPA2
watchdog 5 s；需要立即发送下一阶段请求时使用下一个10 ms jiffy。`rx_work` 与
`state_work` 是不同 work，同一专用队列保证它们不会同时修改该设备状态；DWC2
IRQ只负责投递，不运行状态机。

所有 delayed work 共用 `kernel/workqueue.c` 的 `delayed_head`，该单链表按
`expires`从早到晚排列。CPU0每10 ms调用`workqueue_timer_tick()`，只取出已经
到期的表头项目，再按照各自的`target`投递到`mt7601u_wq`或`net_wq`。因此
“delayed队列”负责时间排序，“workqueue”负责实际执行，两者不是同一种队列。

## 11. NAK、错误与重新调度

Bulk IN 没有数据时，USB 设备可返回 NAK。NAK不是设备故障，也不应在 hard IRQ
中长时间循环。下半部消费完成状态后，在后续 USB frame 重新 arm buffer。

常见错误位：

```text
STALL / XACTERR / BBLERR / FRMOVRUN / DATATGLERR
```

错误恢复需要保持以下状态一致：

- host channel 已 halt；
- `HCINT` 已清除；
- `HAINTMSK` 恢复；
- DATA PID toggle 没有被错误推进；
- DMA buffer cache 状态正确；
- EP4 buffer 再次 arm。

早期出现的 `intr=0x400`、TX result `-2` 和 DHCP无 offer，通常与 channel halt、
toggle、ZLP、DMA长度或错误恢复不完整有关，而不是 DHCP协议本身。

## 12. 设备与驱动生命周期

```text
platform device bcm2837-dwc2
  -> dwc2 platform driver
  -> usb_bus
  -> usb_device 148f:7601
  -> mt7601u_driver.probe
  -> net_device wlan1
```

remove/unplug 的正确顺序应是：

```text
阻止新的 work
  -> mask DWC2 IRQ/channel
  -> cancel_work_sync / flush_work
  -> halt DMA
  -> unregister_netdev(wlan1)
  -> usb driver remove
  -> 释放设备私有状态
```

当前硬件热拔生命周期仍需要强化。活跃 DMA 时直接拔出设备可能使 worker访问失效的
`usb_device`，因此真机测试阶段应先停止接口，再拔设备。

## 13. 锁与并发

| 锁 | 类型 | 保护内容 |
|---|---|---|
| `usb_lock` | spinlock | DWC2 register、host channel、DMA、toggle |
| `mt7601u.lock` | spinlock | SoftMAC link state、scan、密钥和设备私有状态 |
| `system_wq.lock` | spinlock | work FIFO、pending/running |
| `delayed_lock` | spinlock | 保护按到期时间排序的全局delayed队列 |
| `p->lock` | spinlock | kworker 的 RUNNABLE/SLEEPING/RUNNING 状态 |

IRQ 顶半部不能获取可能被下半部长期持有的设备锁。它只写 controller pending 并
排 work。进入网络栈前还应释放 `usb_lock`/设备锁，因为 ARP/TCP ACK 可能同步走
TX 路径重新进入同一驱动。

## 14. 与 Linux 的关系

共同点：

- DWC2 HCD 与 USB device driver 分层；
- VID/PID 匹配 `probe/remove`；
- firmware 每次上电下载；
- eFuse/EEPROM提供校准与 MAC；
- SoftMAC由host处理扫描、关联和加密；
- RX buffer预提交，IRQ只报告完成；
- 下半部有 budget/重新调度思想。

不同点：Linux通常使用成熟的 USB URB、completion、tasklet/workqueue，以及
mac80211/cfg80211 网络子系统。当前 xv6 把这些功能压缩在 `dwc2.c`、
`mt7601u.c` 和一个全局 `system_wq` 中，还没有 URB对象、每endpoint请求队列、
mac80211 rate control、完整 regulatory和可靠热拔。

## 15. 当前状态与验证

已经在真机观察到：

- USB hub 和 `148f:7601` 枚举；
- firmware下载、MCU启动；
- eFuse/MAC读取；
- MAC/BBP/RF初始化和校准；
- AP扫描；
- Open-System authentication；
- association；
- WPA2四次握手；
- `wlan1`注册。

仍需重点验证 DWC2 IRQ 9 模式下的数据面稳定性：

```sh
/bin/dhcp wlan1
ping 192.168.0.1
ping google.com
```

执行前确认默认路由已经指向 `wlan1`；当前 xv6 `ping` 不实现 Linux 的 `-I`
接口选择参数。

预期启动日志：

```text
workqueue: system_wq worker=0 pid=1
workqueue: system_wq worker=1 pid=2
workqueue: system_wq worker=2 pid=3
workqueue: system_wq worker=3 pid=4
dwc2: host-channel IRQ enabled
dwc2: MT7601U configured; bulk-in=4,5/512 bulk-out=8/512
mt7601u: EP4 receive uses DWC2 IRQ with 4 buffers
hart 1 starting
hart 2 starting
hart 3 starting
```

QEMU `raspi3b` 没有真实 LAN9514/MT7601U，只能验证无设备启动和 IRQ代码构建，
不能代替真实 Bulk IN completion、DHCP、ping与持续流量测试。

## 16. 后续改进

前四项已经实现：

1. MT7601U 使用私有 `rx_work` 和 `state_work`。DWC2 channel 4 IRQ只调度
   `rx_work`，状态机由专用队列中的`delayed_work`调度；不再由全局网络poll重复执行；
2. DWC2异步Bulk IN建立4槽`QUEUED/ACTIVE/DONE/FREE` request/completion环；
3. EP0 control固定channel 0，MCU/普通Bulk IN使用channel 3，数据RX使用channel 4，
   Bulk OUT/TX使用channel 5；
4. MT7601U预提交4个独立DMA buffer。完成一个buffer后，DWC2立即启动下一个，
   worker解析完成项并把原buffer重新放回队列，缩短re-arm空窗。

同时已开启SMP：CPU0建立页表后以release/acquire屏障和`SEV/WFE`释放已进入
`_entry`的副核，并写入Raspberry Pi标准spin-table `0xe0..0xf0`兼容固件armstub。
最终内核页表只额外映射物理第0页以访问spin-table。4个CPU均进入scheduler，
`system_wq`创建4个kworker，因此独立work可以在不同CPU上并发执行；设备私有锁
和DWC2锁仍负责串行化同一硬件状态。同一`work_struct`运行期间的新事件只设置
`rerun`，回调结束后最多重排队一次，避免多个worker同时重入同一个设备回调。

第5项也已经实现：workqueue核心维护按`expires`排序的delayed队列；CPU0的10 ms
Generic Timer只推进jiffies并把到期项放入目标队列。MT7601U根据扫描、认证、
关联或WPA2阶段选择真正的下一deadline并进入`mt7601u_wq`；TCP重传进入
`net_wq`，并按照所有连接中最早的`tx_deadline`重新预约。空闲时两者都不会
周期唤醒。

仍待改进：

6. 完整实现 unplug、`cancel_work_sync()`、DMA halt和资源回收；
7. 增加 RX/TX统计、IRQ/NAK/error计数和 `/proc/net/dev`；
8. 抽象 mac80211式 SoftMAC层，避免加密、扫描和802.11解析绑定单一芯片；
9. 增加 rate control、重传、power save和 regulatory domain；
10. 对长时间 DHCP、ping、SSH与双网卡路由进行压力测试。

## 17. 代码导航

| 文件 | 入口/职责 |
|---|---|
| `kernel/dwc2.c` | `dwc2_driver_init()`、枚举、Bulk、DMA、`dwc2_irq()` |
| `kernel/usb.c/.h` | USB bus、device/driver、host ops |
| `kernel/mt7601u.c` | `mt7601u_probe()`、firmware、SoftMAC、RX/TX |
| `kernel/workqueue.c/.h` | `system_wq`、`schedule_work()`、kworker |
| `kernel/net.c` | `net_wq`、TCP deadline和`wlan1`网络投递 |
| `kernel/bcm2837.c` | legacy USB IRQ 9 enable/pending |
| `kernel/trap.c` | IRQ 9分发到 `dwc2_irq()` |
| `kernel/memlayout.h` | DWC2 MMIO与 IRQ号 |
| `Makefile` | kernel对象、firmware与bootfs安装 |

## 18. EP0 Control 与 Bulk 事务细节

标准和 vendor control request 都通过 EP0 的三个阶段：

```mermaid
sequenceDiagram
    participant CPU
    participant DWC2
    participant DEV as MT7601U EP0
    CPU->>DWC2: SETUP packet / PID SETUP
    DWC2->>DEV: SETUP stage
    alt request 有数据
        DEV-->>DWC2: DATA IN
    else host 写数据
        DWC2->>DEV: DATA OUT
    end
    DWC2->>DEV: 反方向 zero-length STATUS
    DEV-->>DWC2: ACK
```

EP0用于USB `GET_DESCRIPTOR`、`SET_ADDRESS`、`SET_CONFIGURATION`，也用于
MT7601U vendor request读取ASIC/MAC revision、访问MAC/BBP/RF/FCE寄存器和提交
firmware IVB。Bulk没有固定延迟保证，但提供CRC、ACK与USB重传，适合firmware
与帧数据：

```text
EP8 OUT：ARM RAM -> DWC2 DMA -> USB -> MT7601U FIFO
EP4 IN ：MT7601U RX FIFO -> USB -> DWC2 DMA -> ARM RAM
EP5 IN ：MT7601U MCU response -> USB -> ARM RAM
```

### DATA0/DATA1、短包和ZLP

EP4、EP5和EP8必须各自维护DATA toggle。NAK表示设备暂时没有数据，不得推进
toggle；DTERR表示host与endpoint对下一PID认识不一致，payload没有被接受。

同步Bulk IN成功后按实际packet数推进toggle。若请求长度大于返回长度、返回长度
又正好是max-packet整数倍，则传输由额外的zero-length packet结束，ZLP也必须计入
packet数。否则下一次请求可能使用错误PID并出现：

```text
HCINT=0x400
dwc2: endpoint 4 IN toggle resync -> DATA0/1
```

当前HCD检测DTERR后只翻转发生错误的IN endpoint并重试一次，不污染EP5或EP8。
同一个host channel会复用多个endpoint，因此不能简单把channel结束时的
`HCTSIZ.DPID`当作所有endpoint的软件toggle。

## 19. 两级DMA不能混为一谈

firmware下载同时使用两级完全不同的DMA：

```mermaid
flowchart LR
    FAT["bootfs MT7601U.BIN"]
    RAM["ARM staging buffer"]
    HCD["DWC2 HCDMA<br/>0xc0000000 | PA"]
    USB["USB Bulk OUT EP8"]
    FCE["MT7601U FCE/PDMA"]
    ILM["MCU ILM"]
    DLM["MCU DLM"]
    FAT --> RAM --> HCD --> USB --> FCE
    FCE --> ILM
    FCE --> DLM
```

第一层DWC2 DMA的地址属于BCM2837 bus address，指向ARM RAM；第二层FCE DMA的
destination属于MT7601U内部地址，指向MCU ILM/DLM或MAC queue。驱动为每个
firmware chunk增加TXINFO，通过vendor request设置`MT_FCE_DMA_ADDR`、
`MT_FCE_DMA_LEN`和descriptor index，再由EP8送入芯片内部PDMA。两种地址空间
不能相互替代。

## 20. 信道扫描状态机

MT7601U只支持2.4 GHz。驱动按Linux MT7601U frequency plan写RF bank 0寄存器
17--20、BBP gain并触发VCO calibration：

```mermaid
stateDiagram-v2
    [*] --> Channel1
    Channel1 --> Channel2: dwell 500 ms
    Channel2 --> More
    More --> Channel11
    Channel11 --> Channel1: 未找到目标
    More --> Locked: SSID匹配
    Locked --> Auth
    Auth --> Assoc
    Assoc --> WPA2
    WPA2 --> Wlan1
```

每个信道约停留500 ms，由`scan_deadline = now + 50 jiffies`一次性预约。Beacon/Probe Response
解析BSSID、SSID IE、DS channel、privacy capability、RSN IE和vendor WPA IE；
扫描表按BSSID去重，隐藏SSID后来被probe response揭示时更新原条目。

RX聚合记录的基本布局为：

```text
4-byte RXINFO/DMA prefix
28-byte RXWI
802.11 MPDU
trailing info/alignment
```

CRC、ICV或MIC错误帧被丢弃。management frame进入扫描/认证/关联状态机；data
frame处理普通或QoS header和`L2PAD`。EtherType `0x888e`进入EAPOL状态机，其余
LLC/SNAP数据重建Ethernet header后交给`net_rx_dev(wlan1)`。

## 21. 启动顺序与日志映射

MT7601U初始化分为“先枚举USB设备、后注册需要bootfs的驱动”两个阶段：

```text
device_init
  -> usb_bus_init
  -> dwc2_driver_init
       -> usb_device_register(148f:7601)
  -> fat32init
  -> mt7601u_driver_init
       -> usb_register_driver
       -> VID/PID match
       -> mt7601u_probe
```

DWC2必须先发现设备，但`MT7601U.BIN`要等`fat32init()`后才能读取。USB core会在
driver注册时遍历已经存在的device并执行匹配。

| 日志 | 已完成阶段 |
|---|---|
| `root device class=9 vid=424 pid=2514` | LAN951x根hub枚举 |
| `child ... vid=148f pid=7601` | 外部port发现MT7601U |
| `bulk-in=4,5/512 bulk-out=8/512` | configuration与endpoint解析 |
| `ASIC=76010001 MAC=...` | VID/PID probe与vendor register访问 |
| `firmware ... validated` | header、ILM/DLM长度校验 |
| `MCU firmware running` | 两级DMA、IVB与MCU启动完成 |
| `EEPROM ... permanent mac=...` | eFuse读取完成 |
| `MAC/BBP/RF ... calibration complete` | radio数据通路初始化完成 |
| `AP ssid=...` | EP4 RX、RXWI和Beacon解析工作 |
| `associated aid=...` | management TX/RX工作 |
| `four-way handshake complete` | EAPOL与CCMP密钥安装完成 |
| `wlan1 registered` | 接口可交给DHCP和网络层 |

firmware日志里的文件总长度与ILM payload长度不同是正常现象：文件总长度还包含
header/IVB。当前分块约14 KiB，每批都要等待芯片FCE完成内部DMA。

## 22. 历史故障：完整初始化重复

正常单次启动不应反复打印从`USB device 148f:7601`到`transport and MCU ready`的
完整序列：

- `main()`只调用一次`mt7601u_driver_init()`；
- device已有`dev->driver`时不会再次bind；
- probe成功后`mt7601u.used=1`；
- timer/workqueue不会再次调用probe。

如果同一次启动出现多次完整firmware load，优先检查串口日志是否拼接了多次启动、
SD卡kernel是否与源码一致、是否发生remove/register，以及内存破坏是否清零
`usb_child.dev.driver`或`mt7601u.used`。可在probe/remove打印计数和device指针；
firmware chunk进度多行只是一次probe内部循环，不代表重复probe。

## 23. 历史故障：关联后停在WPA2 pending

曾经出现：

```text
Open-System authentication accepted
association request sent
802.11 associated ... WPA2 handshake pending
```

这通常不是CPU死机，而是首批EAPOL M1因短包、toggle resync或USB错误丢失，旧状态
机又没有`ASSOCIATED`阶段的watchdog。当前逻辑在每个有效EAPOL-Key frame到达时
把`state_deadline`刷新到当前时间后5秒；到期仍未推进握手，便清理临时
ANonce/SNonce/PTK并回到关联请求：

```text
mt7601u: WPA2 M1 timeout; reassociating
mt7601u: association request sent rsn=22 bytes
```

这里不重新扫描和认证，因为BSSID、信道及Open-System Authentication仍然有效。
新的Association response会促使AP重新发送M1。

## 24. 文档范围

本文统一覆盖驱动分层、USB事务、两级 DMA、firmware、eFuse、MAC/BBP/RF、
扫描、SoftMAC、WPA2、IRQ、锁、日志映射和历史故障，是 MT7601U/DWC2 实现
的唯一主文档。
