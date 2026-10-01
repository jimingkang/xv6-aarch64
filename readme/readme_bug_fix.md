# xv6-rpi3 问题修复记录

## SSH 远程终端逐字符回显缓慢

### 现象

SSH 已经能够完成密钥交换、密码认证并建立 PTY shell，但是远程输入时，字符经常延迟约 100～300 ms 才显示。连续输入时表现为字符断续出现，命令输出通常比本地串口慢得多。

这不是 SSH 客户端本地回显问题。SSH 终端默认由服务端 PTY 回显，因此每个按键都必须完成下面的路径：

```text
客户端按键
  -> 无线帧到达网络设备
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
net_tcp_tick();
```

当时的 Wi-Fi 接收路径依赖该轮询。一个刚到达的按键平均需要等待约50 ms，最坏
等待接近100 ms，返回的回显还可能再等待一个轮询周期。

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

10 ms 轮询解决了 SSH 的百毫秒级延迟，但最初的 IRQ 版本仍在 Generic Timer
hard IRQ 中执行 deferred poll。现在已经加入真正的 `system_wq` 和 `kworker`：
SDIO IRQ 只记录完成状态并排队，CMD53、协议解析与网络栈投递由可调度的
内核线程完成。10 ms timer 只排周期维护 work，不再直接调用设备驱动。

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
  -> schedule_work(net_deferred_work)
  -> kworker 被 wakeup
  -> brcmfmac_poll_device() bottom half
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

DWC2 host-channel IRQ 9、MT7601U EP4 DMA、SoftMAC、WPA2 与收发路径已迁移到
[readme_usb_dwc2_wifi.md](readme_usb_dwc2_wifi.md)。本文件只保留 BCM43455 SDIO
和通用 workqueue 修复过程。

### 从 Timer hard IRQ 过渡到真正的下半部

第一版 IRQ 接收是过渡实现：设备 IRQ 只设置 pending，但 CPU0 的 Generic Timer
每 10 ms 直接调用 `netdev_poll_all()`。这样虽然把耗时操作移出了 SDIO IRQ 62，
CMD53、cache maintenance、SDPCM 解析和 `net_rx_dev()` 实际仍运行在另一个 hard
IRQ 上下文，不能睡眠，也可能拉长 Timer IRQ。

现在 Timer 和设备 IRQ 都只调用 `net_deferred_schedule()`。该函数把静态 work
加入 `system_wq` 并唤醒 `kworker`。设备 `.poll()`、CMD53 和网络栈由正常内核
进程上下文执行。BCM43455 首次处理由 SDIO IRQ 立即触发；Timer 只用于 TCP
重传、设备状态机超时等周期状态推进，以及作为异常丢中断时的低成本
安全检查。

### 真机应出现的确认日志

```text
brcmfmac: SDIO DAT1 IRQ receive enabled
```

BCM43455 测试：自动关联后运行 SSH 和 `ping`，确认长时间收包没有停止。

### 当前真机验证范围

本轮真机测试只验证板载 BCM43455 的 SDIO 中断链；USB Wi-Fi 的独立测试范围
记录在 [readme_usb_dwc2_wifi.md](readme_usb_dwc2_wifi.md)。SDIO 链路为：

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

因此当前验证状态应明确记录为：

| 路径 | 代码状态 | 真机状态 |
|---|---|---|
| BCM43455 SDIO DAT1 / IRQ 62 | 已接入 | 本轮验证目标 |

USB IRQ 9 和 MT7601U 的验证矩阵见
[readme_usb_dwc2_wifi.md](readme_usb_dwc2_wifi.md)。

### IRQ 优化对应的代码改动

#### 1. 中断号和 BCM2837 legacy controller

文件：`kernel/memlayout.h`、`kernel/bcm2837.c`

新增中断号：

```c
#define SDIO_IRQ    62
```

IRQ 62 位于 legacy pending bank 2，因此实际位号为 `62 - 32 = 30`：

```c
irqwrite(ENABLE_IRQS_2, 1U << (SDIO_IRQ - 32));
```

`gic_iar()` 虽然保留了原来的函数名，RPi3 实现读取的是 Broadcom legacy/ARM-local
寄存器，不是真正的 GIC：

```c
if(irqread(IRQ_PENDING_2) & (1U << (SDIO_IRQ - 32)))
  return SDIO_IRQ;
```

`kernel/trap.c::devintr()` 增加分发：

```c
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

详细实现、寄存器、DMA、endpoint/channel 对应关系与异步 Bulk IN 生命周期已迁移到
[readme_usb_dwc2_wifi.md](readme_usb_dwc2_wifi.md)。

#### 6. 编译和回归验证

已执行：

```sh
make -j4 kernel/kernel8.img
git diff --check
make qemu
```

QEMU 能启动到 `xv6-rpi3 login:`，没有发生未确认 legacy IRQ 导致的中断死循环。
QEMU 没有真实 BCM43455，所以只能验证代码构建、IRQ 分发和无设备启动；SDIO
IRQ 62 的最终结论必须来自当前 RPi3 真机测试。USB Wi-Fi 验证见专门文档。

## SDIO IRQ、Timer 轮询与 kworker 下半部的演进

### 1. 原始 Timer 轮询

最早没有使用 SDIO DAT1 中断。CPU0 每次 Generic Timer IRQ 都直接执行：

```text
timerintr()                         hard IRQ
  -> netdev_poll_all()
     -> brcmfmac_poll_device()
        -> CMD53 读取 BCM43455 F2
        -> SDPCM/BCDC/Ethernet 解析
        -> net_rx_dev()
```

为了改善 SSH 回显，硬件 timer 周期从 100 ms 改为 10 ms，但每十次才增加一次
逻辑 `ticks`，所以 `sleep(n)`、进程抢占和 `uptime` 仍保持原来的 100 ms 语义。
这种方式响应时间稳定，但没有数据时也会检查设备，而且完整网络接收路径运行在
Timer hard IRQ 中。

### 2. SDIO IRQ + Timer deferred poll 过渡方案

加入 BCM2837 legacy IRQ 62、Arasan `CARD_INT` 和 SDIO CCCR function interrupt
后，BCM43455 可以通过 DAT1 通知 CPU：

```text
BCM43455 F2 frame
  -> SDIO DAT1
  -> Arasan CARD_INT
  -> BCM2837 IRQ 62
  -> arasan_sdio_irq()
       mask CARD_INT
       clear host interrupt
       arasan_card_irq_pending = 1
  -> return from IRQ
```

第一版 deferred 实现仍由下一次10 ms Timer IRQ 调用 `netdev_poll_all()`。
`brcmfmac_poll_device()` 先检查 `arasan_sdio_irq_pending()`，无 pending 就立即返回；
有 pending 才执行 CMD53。这消除了无条件 SDIO 读取，却仍不是进程上下文下半部。

### 3. 当前 workqueue/kworker 下半部

现在完整调用链为：

```text
SDIO IRQ 62                         hard IRQ top half
  -> arasan_sdio_irq()
       mask/ack CARD_INT
       pending = 1
       net_deferred_schedule(0)
          -> schedule_work(&net_deferred_work)
          -> wakeup(&system_wq)
  -> return from IRQ

scheduler
  -> kworker kernel thread          process context bottom half
       -> dequeue net_deferred_work
       -> netdev_poll_all()
       -> brcmfmac_poll_device()
            -> bounded CMD53 drain
            -> SDPCM/BCDC/Ethernet
            -> net_rx_dev()
            -> drained: unmask CARD_INT
            -> not drained: schedule_work() again
       -> sleep(&system_wq)
```

Timer 路径现在只有排队操作：

```c
if(cpu == 0)
  net_deferred_schedule(logical_tick);
```

因此 Timer hard IRQ 不再调用 `netdev_poll_all()` 或 `net_tcp_tick()`。
`logical_tick` 被累积到 `net_deferred_ticks`，随后由 kworker
调用 `net_tcp_tick()`。10 ms 周期仍用于设备状态机超时，但实际工作在进程上下文。

### 4. 新增的通用 workqueue 对象

新增文件：`kernel/workqueue.h`、`kernel/workqueue.c`。

work 对象保存回调和队列状态：

```c
struct work_struct {
  void (*func)(struct work_struct *work);
  struct work_struct *next;
  int pending;
  int running;
};
```

`system_wq` 是带自旋锁的 FIFO：

```c
struct workqueue {
  struct spinlock lock;
  struct work_struct *head;
  struct work_struct *tail;
};
```

公开操作包括：

| 接口 | 作用 |
|---|---|
| `init_work()` | 初始化静态 work 和回调 |
| `schedule_work()` | IRQ-safe 入队；相同 pending work 自动合并 |
| `flush_work()` | 等待 queued/running work 完成 |
| `cancel_work_sync()` | 从队列删除并等待正在执行的回调退出 |

`schedule_work()` 可以在 hard IRQ 中调用，因为它只获取短时间自旋锁、链接静态
对象并执行 `wakeup()`；不分配内存、不执行设备传输。

### 5. 内核线程支持

`kernel/proc.h` 给 `struct proc` 增加：

```c
void (*kthread_fn)(void *);
void *kthread_arg;
```

`kernel/proc.c` 新增 `kthread_create()` 和 `kthread_entry()`。内核线程仍使用 xv6
进程槽、独立内核栈和调度上下文，但不返回 EL0；第一次被 scheduler 选中时释放
`p->lock`，直接运行内核函数。`workqueue_init()` 创建：

```text
workqueue: system_wq kworker pid=1
```

`kworker()` 无任务时调用：

```c
while(wq->head == 0)
  sleep(wq, &wq->lock);
```

IRQ 入队后的 `wakeup(&system_wq)` 将它置为 `RUNNABLE`。回调运行时不持有队列锁，
因此能够等待其他锁，也允许将同一个 work 重新排队。

### 6. 网络层代码变化

`kernel/net.c` 新增一个静态 `net_deferred_work`、`net_deferred_lock` 和
`net_deferred_ticks`。`netinit()` 初始化它们；`net_deferred_worker()` 负责：

1. 调用所有已注册 `net_device` 的 `.poll()`；
2. 推进需要周期维护的网络设备状态机；
3. 在进程上下文处理累积的 TCP timer tick。

`kernel/arasan_sdio.c` 顶半部在设置 controller pending 后直接调用
`net_deferred_schedule(0)`。`kernel/brcmfmac.c` 每轮最多处理16个 SDPCM
项目；如果尚未排空，就再次 `schedule_work()`，形成类似 NAPI budget 的有界连续
处理，而不等待下一次 Timer。

### 7. 上下文与锁的变化

| 阶段 | 执行上下文 | 可以做的工作 |
|---|---|---|
| SDIO top half | hard IRQ | ack/mask、记录 pending、排 work |
| `system_wq` 队列操作 | hard IRQ 或进程 | 短自旋锁、合并重复 work、唤醒 worker |
| `kworker` bottom half | 内核进程 | CMD53/DMA completion、解析帧、进入网络栈 |
| Generic Timer | hard IRQ | 重装 timer、累计逻辑 tick、排周期 work |

当前只有一个 `system_wq` worker，因此网络下半部不会彼此并发；这简化了现有
共享网络栈的锁模型。USB Wi-Fi 如何复用该 worker，见专门的 DWC2 文档。

### 8. 构建与启动验证

已验证：

```sh
make -j4 kernel/kernel8.img
make qemu
```

QEMU 成功启动到 `xv6-rpi3 login:`，说明 kworker 能够进入睡眠，Timer IRQ 能够
唤醒它，且没有破坏 `fsinit()`、init 和本地登录。真实 BCM43455 还需要验证：

```text
workqueue: system_wq kworker pid=1
brcmfmac: SDIO DAT1 IRQ receive enabled
wifi: associated
```

随后连续执行 `ping` 和 SSH 交互，确认 IRQ 触发后能立即收包、持续流量不会因为
CARD_INT 被屏蔽后未重新打开而停止。
