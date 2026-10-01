# xv6-rpi3 问题修复记录

## SSH 远程终端逐字符回显缓慢

### 现象

SSH 已经能够完成密钥交换、密码认证并建立 PTY shell，但是远程输入时，字符经常延迟约 100～300 ms 才显示。连续输入时表现为字符断续出现，命令输出通常比本地串口慢得多。

这不是 SSH 客户端本地回显问题。SSH 终端默认由服务端 PTY 回显，因此每个按键都必须完成下面的路径：

```text
客户端按键
  -> 无线帧到达 BCM43455/MT7601U
  -> xv6 CPU 轮询网卡
  -> TCP 接收并唤醒 sshd
  -> SSH 解密 CHANNEL_DATA
  -> 写入 PTY master
  -> PTY 行规程产生字符回显
  -> sshd 读取 PTY master
  -> SSH 加密 CHANNEL_DATA
  -> TCP 发送
  -> 客户端显示
```

### 根因一：网络设备只有每 100 ms 才被轮询

原来的 ARM Generic Timer 周期为 100 ms，CPU0 在每次 timer IRQ 中调用：

```c
netdev_poll_all();
mt7601u_poll();
net_tcp_tick();
```

BCM43455 和当前尚未完全 IRQ 化的 USB Wi-Fi 接收路径依赖该轮询。一个刚到达的按键平均需要等待约 50 ms，最坏等待接近 100 ms，返回的回显还可能再等待一个轮询周期。

### 根因二：TCP `write()` 同步等待 ACK

原来的 `net_tcp_write()` 只有一个未确认发送槽。调用者发送一个 TCP 段后，会执行：

```c
while(c->tx_unacked && !c->tx_failed)
  sleep(c, &c->lock);
```

因此 `write()` 不是“数据复制到 socket 发送缓冲后返回”，而是“对端确认后才返回”。PTY 的单字符回显会形成很小的 SSH 包，每个小包都可能支付一次完整网络往返时间。

加密 SSH 包还包含密文和 32 字节 HMAC。即使连续调用两次 `write()`，第二次也会被前一个未确认段阻塞。

## 修复

### 1. 10 ms 设备轮询，保留 100 ms xv6 tick

修改 `kernel/timer.c`：

- ARM Generic Timer 的硬件周期从 100 ms 改为 10 ms；
- 每次 10 ms 中断在 CPU0 执行网络设备轮询；
- 每累计 10 次硬件中断才形成一个 xv6 `ticks`；
- `net_tcp_tick()` 仍每 100 ms 调用一次，因此原来的 500 ms TCP 重传超时没有被意外缩短；
- 只有每第十次 timer IRQ 返回调度时钟事件，保持原有 100 ms 抢占节奏。

```text
10 ms hardware timer IRQ
  +-> netdev_poll_all()          每次调用
  +-> mt7601u_poll()             每次调用（注册 wlan1 前）
  +-> divider == 10 ?
        +-> ticks++              每 100 ms
        +-> net_tcp_tick()       每 100 ms
        +-> scheduler preempt    每 100 ms
```

所以 `sleep(n)`、`uptime()` 和 TCP 重传计时仍采用原来的 100 ms 单位，不会因为降低网络轮询延迟而快十倍。

### 2. 添加 TCP 异步发送队列

修改 `kernel/net.c`，每条 TCP 连接增加 8 KiB `txq`：

```text
user write()
  -> copyin 到 txq
  -> 如果当前没有未确认段，立即启动一个 MSS 段
  -> write() 在数据成功入队后返回

TCP ACK
  -> 释放当前重传槽
  -> 从 txq 启动下一个 MSS 段
  -> 唤醒等待发送队列空间的 writer
```

仍然只允许一个 TCP 段处于未确认状态。这保留了当前简化 TCP 的 stop-and-wait 重传模型，但用户进程不再为每个小包同步等待 ACK。

队列相关行为：

- 阻塞 socket：仅在 8 KiB 队列装满时睡眠；
- 非阻塞 socket：写入当前可用空间后立即返回，队列满时返回 `-1`；
- `EPOLLOUT`：发送队列存在空间时就绪，不再要求网络上完全没有未确认段；
- `close()`：等待队列与当前未确认段全部排空，再发送 FIN；
- 重传最终失败：清空发送队列、设置 `tx_failed` 并唤醒等待者；
- 对端已经发送 FIN 的 `TCP_CLOSE_WAIT` 状态仍允许排空此前入队的数据。

### 修复后的交互延迟

理论上的设备发现延迟由 `0～100 ms` 降为 `0～10 ms`。小型 SSH 回显包入队后立即返回，不再把一次 TCP ACK 往返串行加入每个字符的处理路径。

这不是完整的 Linux TCP 实现。目前仍有以下限制：

- 同一连接最多只有一个在途 TCP 段；
- 没有拥塞窗口、滑动发送窗口和 RTT 自适应 RTO；
- IRQ 完成后的协议处理仍由 10 ms timer IRQ 调度，尚无可睡眠内核 worker；
- 大量输出填满 8 KiB 队列后仍会产生背压，这是预期行为。

下一步应增加可调度内核 worker，并把 TCP 扩展为滑动窗口发送。

## 验证

代码必须先通过：

```sh
make kernel/kernel8.img
```

安装到 BOOTFS 并启动后：

```sh
ssh root@192.168.0.201
```

检查项目：

1. 单个字符应基本立即回显，不再呈现固定约 100 ms 的阶梯延迟；
2. 快速输入一整行不应丢字、乱序或重复；
3. `ls -l /bin` 等多行输出不应导致连接挂死；
4. 上下箭头历史、退格和回车的 PTY 行规程仍正常；
5. 空闲超过 TCP 重传周期后连接仍可继续交互；
6. 退出 shell 后 SSH channel、TCP FIN 和进程回收正常。

## Wi-Fi 接收改为硬件 IRQ 驱动

10 ms 轮询解决了 SSH 的百毫秒级延迟，但 CPU 仍会周期性读取 SDIO/DWC2
状态寄存器。现在接收完成检测已改为硬件中断；10 ms timer 仅充当简化的
deferred worker 调度点，不再主动探测设备是否完成。

### BCM43455：SDIO DAT1 → Arasan IRQ 62

接收路径：

```text
BCM43455/F2 有 SDPCM 帧
  -> SDIO DAT1 拉起
  -> Arasan EMMC_INTERRUPT.CARD_INT
  -> BCM2837 legacy pending-2 bit 30（IRQ 62）
  -> arasan_sdio_irq() 顶半部
       屏蔽 CARD_INT signal
       清 host CARD_INT 状态
       设置 arasan_card_irq_pending
  -> 10 ms deferred poll
  -> brcmfmac_poll_device()
       CMD53 读取 F2 FIFO
       解析 SDPCM/BCDC/EAPOL/Ethernet
       net_rx_dev()
       队列排空后重新打开 CARD_INT
```

同时增加了 SDIO CCCR `INT_ENABLE` 管理：

- `sdio_claim_irq()` 打开 CCCR master interrupt 和 function 1 interrupt；
- `sdio_release_irq()` 在 remove/unregister 时关闭 function interrupt；
- BCM43455 的 `SDIO_CORE_HOSTINTMASK` 仍负责芯片内部 mailbox/frame 中断掩码；
- Arasan `EMMC_IRPT_EN` 负责把 card interrupt 输出到 ARM legacy controller。

顶半部不执行 CMD52/CMD53，不获取 `brcmf_bus.lock`，也不进入网络栈。这样
避免在 hard IRQ 中长时间等待 SDIO command/data inhibit 或递归获取驱动锁。

### DWC2：host-channel IRQ 9

DWC2 初始化现在配置：

```text
HCINTMSK(channel) = XFERCOMPL | CHHLTD | NAK | error bits
HAINTMSK          = 对应异步 RX channel
GINTMSK           = HCHINT
GAHBCFG           = DMA_EN | GLBL_INTR_EN
```

BCM2837 legacy controller 的 USB IRQ 9 被加入分发，`dwc2_irq()` 顶半部只做：

1. 读取 `GINTSTS.HCHINT` 和 `HAINT & HAINTMSK`；
2. 暂时屏蔽已完成 channel 的 `HAINTMSK` 位；
3. 把 channel 位记录到 `dwc2_irq_pending`；
4. 保留 `HCINT` 和 `HCTSIZ`，交给 deferred handler 计算实际长度。

CDC 网络 RX 使用 channel 2。MT7601U 的数据端点 EP4 使用独立 channel 4，
长期预提交 DMA buffer：

```text
arm EP4 channel 4 DMA
  -> USB packet / short packet / NAK
  -> DWC2 HCHINT -> IRQ 9
  -> dwc2_irq() 标记 channel 4 completion
  -> mt7601u_poll() deferred handler
       读取 HCINT/HCTSIZ
       cache invalidate
       更新 DATA0/DATA1 toggle
       解析 RXWI 和 802.11 frame
       投递 Ethernet frame
       重新 arm channel 4
```

NAK 不再在 hard IRQ 中循环重试。deferred handler 清除/停止 channel 后，在
后续 USB frame 上重新提交相同 buffer。控制传输、MCU response endpoint 和
TX 仍使用原来的同步 channel，数据 RX 不再执行 2 ms `HCINT` 忙轮询。

### 保留 10 ms deferred worker 的原因

当前 xv6 没有 Linux 风格 softirq、tasklet、NAPI 或通用 workqueue。顶半部直接
调用 CMD53、cache maintenance、RXWI/SDPCM 解析和 `net_rx_dev()` 会让 hard
IRQ 时间过长。因此 timer 仍每 10 ms 调用设备 `.poll()`，但这些函数首先检查
IRQ pending bit，没有硬件完成事件就立即返回。

后续可以增加真正的内核 worker：IRQ 顶半部 `wakeup(worker)`，由可调度内核
线程立即执行 bottom half，即可去掉最多 10 ms 的 deferred 调度延迟。

### 真机应出现的确认日志

```text
dwc2: host-channel IRQ enabled
brcmfmac: SDIO DAT1 IRQ receive enabled
mt7601u: EP4 receive uses DWC2 host-channel IRQ
```

BCM43455 测试：自动关联后运行 SSH 和 `ping`，确认长时间收包没有停止。MT7601U
测试：完成关联与 DHCP 后执行 `/bin/ping`，同时确认不再出现 channel 3 的
2 ms NAK timeout 日志。拔除 USB 设备前必须走 remove/unregister；当前硬件没有
可靠热拔检测时，不应在活跃 DMA 期间直接拔出。

### 当前真机验证范围

当前 Raspberry Pi 3 只使用板载 BCM43455，MT7601U USB 网卡没有插入。因此本轮
真机测试只验证下面的 SDIO 中断链：

```text
BCM43455 收到无线帧
  -> SDIO DAT1
  -> Arasan EMMC/SDIO controller
  -> BCM2837 legacy pending-2 bit 30（IRQ 62）
  -> arasan_sdio_irq()
  -> brcmfmac deferred RX
  -> CMD53 / SDPCM / net_rx_dev()
```

启动日志必须出现：

```text
brcmfmac: SDIO DAT1 IRQ receive enabled
```

建议依次验证：

```sh
/bin/wifi
ping 192.168.0.1
ping google.com
```

然后从 Mac 验证交互接收、TCP 和 PTY 回显：

```sh
ssh root@192.168.0.201
```

`dwc2: host-channel IRQ enabled` 只表示 DWC2 的 IRQ 寄存器和 BCM2837 legacy
IRQ 9 已配置，不表示 USB RX 已经在真实 MT7601U 上通过测试。没有插入 MT7601U
时，下面这条日志不会出现，这是正常情况：

```text
mt7601u: EP4 receive uses DWC2 host-channel IRQ
```

因此当前验证状态应明确记录为：

| 路径 | 代码状态 | 真机状态 |
|---|---|---|
| BCM43455 SDIO DAT1 / IRQ 62 | 已接入 | 本轮验证目标 |
| DWC2 host-channel / IRQ 9 | 已接入，编译及 QEMU 启动通过 | 未验证 |
| MT7601U EP4 / channel 4 DMA RX | 已接入 | 未插设备，待以后验证 |

在 MT7601U 真机验证完成前，不能把 USB IRQ 9 标记为“实机通过”；出现 USB 初始化
日志也不能替代实际的 Bulk IN completion、DHCP、ping 和持续收包测试。

### IRQ 优化对应的代码改动

#### 1. 中断号和 BCM2837 legacy controller

文件：`kernel/memlayout.h`、`kernel/bcm2837.c`

新增中断号：

```c
#define DWC2_IRQ    9
#define SDIO_IRQ    62
```

IRQ 9 位于 legacy pending bank 1；IRQ 62 位于 pending bank 2，因此实际位号为
`62 - 32 = 30`：

```c
irqwrite(ENABLE_IRQS_1, (1U << UART0_IRQ) | (1U << DWC2_IRQ));
irqwrite(ENABLE_IRQS_2, 1U << (SDIO_IRQ - 32));
```

`gic_iar()` 虽然保留了原来的函数名，RPi3 实现读取的是 Broadcom legacy/ARM-local
寄存器，不是真正的 GIC：

```c
if(irqread(IRQ_PENDING_1) & (1U << DWC2_IRQ))
  return DWC2_IRQ;
if(irqread(IRQ_PENDING_2) & (1U << (SDIO_IRQ - 32)))
  return SDIO_IRQ;
```

`kernel/trap.c::devintr()` 增加分发：

```c
} else if(irq == DWC2_IRQ){
  dwc2_irq();
  dev = 1;
} else if(irq == SDIO_IRQ){
  arasan_sdio_irq();
  dev = 1;
}
```

#### 2. SDIO CCCR function interrupt

文件：`kernel/sdio.c`、`kernel/sdio.h`

`sdio_claim_irq()` 通过 CMD52 修改 CCCR `INT_ENABLE(0x04)`：

```c
enable |= 1U | (1U << func->function); // master + function 1
```

`sdio_release_irq()` 清除 function 位；如果已经没有 function interrupt，则同时
清除 master 位。这对应 Linux MMC/SDIO core 中 claim/release IRQ 的职责，而不是
由 brcmfmac 直接操作 CCCR。

#### 3. Arasan SDIO IRQ 顶半部

文件：`kernel/arasan_sdio.c`

新增状态：

```c
static volatile int arasan_card_irq_pending;
```

启用时只允许 `INT_CARD_INT` 输出到 CPU：

```c
wr(EMMC_INTERRUPT, INT_CARD_INT);
wr(EMMC_IRPT_EN, INT_CARD_INT);
```

顶半部屏蔽 level-triggered source、清 host status、记录 pending：

```c
void arasan_sdio_irq(void)
{
  uint32 status = rd(EMMC_INTERRUPT);
  if(status & INT_CARD_INT){
    wr(EMMC_IRPT_EN, 0);
    wr(EMMC_INTERRUPT, INT_CARD_INT);
    arasan_card_irq_pending = 1;
  }
}
```

只有 F2/SDPCM 队列排空后，`arasan_sdio_irq_complete()` 才重新写入
`EMMC_IRPT_EN`。这样可避免 DAT1 保持有效时重复进入 IRQ。

platform device 的资源表也增加：

```c
{ SDIO_IRQ, SDIO_IRQ, IORESOURCE_IRQ, "Arasan SDIO IRQ" }
```

#### 4. brcmfmac 从无条件 poll 改为 IRQ pending 驱动

文件：`kernel/brcmfmac.c`

控制面初始化完成并注册 `wlan0` 后才打开中断：

```c
if(sdio_claim_irq(func) < 0)
  ...;
arasan_sdio_irq_enable();
```

`brcmfmac_poll_device()` 首先检查：

```c
if(!arasan_sdio_irq_pending())
  return;
```

有 pending 时最多按 budget 读取 16 个 SDPCM frame。网络帧通过
`net_rx_dev()` 投递；读到“当前无帧”后调用：

```c
arasan_sdio_irq_complete();
```

remove 路径新增 `sdio_release_irq(func)`，防止设备状态释放后仍进入 handler。

#### 5. DWC2 host-channel interrupt

文件：`kernel/dwc2.c`

新增寄存器定义：

```c
#define HAINT       0x414
#define HAINTMSK    0x418
#define HCINTMSK(c) (0x50c + 0x20*(c))
```

控制器初始化打开：

```c
wr(GINTMSK, GINTSTS_HCHINT);
wr(GAHBCFG, GAHBCFG_DMA_EN | GAHBCFG_GLBL_INTR_EN);
```

顶半部不会清除 `HCINT`，因为 bottom half 还需要 `HCTSIZ` 计算实际长度：

```c
channels = rd(HAINT) & rd(HAINTMSK);
wr(HAINTMSK, rd(HAINTMSK) & ~channels);
dwc2_irq_pending |= channels;
```

CDC RX 使用 channel 2；MT7601U 异步 EP4 RX 使用 channel 4。两个 channel 都只在
完成/NAK/error 后产生 pending，不再由 CPU 循环读取 `HCINT` 判断是否完成。

#### 6. MT7601U 异步 Bulk IN 接口

文件：`kernel/usb.h`、`kernel/dwc2.c`、`kernel/mt7601u.c`

`usb_host_ops` 新增：

```c
int (*bulk_rx_arm)(struct usb_device *, int endpoint, void *, int length);
int (*bulk_rx_complete)(struct usb_device *, int endpoint);
```

`dwc2_usb_bulk_rx_arm()` 为 channel 4 设置 DMA bus address、长度、DATA PID、
`HCINTMSK` 和 `HAINTMSK`。`dwc2_usb_bulk_rx_complete()` 仅在 IRQ pending 后：

- 读取 `HCINT/HCTSIZ`；
- 处理 NAK/error；
- invalidate DMA buffer cache；
- 更新 DATA0/DATA1 toggle；
- 返回实际接收长度。

`mt7601u_poll()` 不再调用同步 2 ms Bulk IN；它检查 completion，解析完成数据，
然后重新 arm：

```c
got = dev->udev->ops->bulk_rx_complete(dev->udev,
                                        dev->udev->bulk_in_ep);
if(got > 0)
  mt7601u_rx_parse(dev, mt_rx_buf, got);
dev->udev->ops->bulk_rx_arm(...);
```

因为 deferred 调度周期从原来的约 100 ms 变成 10 ms，MT7601U 的认证、关联、
EAPOL watchdog 和信道 dwell 计数同步扩大十倍，保持原来的实际超时时间不变。

#### 7. 编译和回归验证

已执行：

```sh
make -j4 kernel/kernel8.img
git diff --check
make qemu
```

QEMU 能启动到 `xv6-rpi3 login:`，没有发生未确认 legacy IRQ 导致的中断死循环。
QEMU 没有真实 BCM43455/MT7601U，所以只能验证代码构建、IRQ 分发和无设备启动；
SDIO IRQ 62 的最终结论必须来自当前 RPi3 真机测试，USB IRQ 9 则等 MT7601U 插入后
再验证。
