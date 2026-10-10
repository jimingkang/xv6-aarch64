# Raspberry Pi 3 USB 键盘驱动

## 驱动路径

```text
USB keyboard
  -> LAN951x high-speed hub
  -> DWC2 host controller
  -> USB bus: struct usb_device
  -> HID boot keyboard class driver
  -> input core: struct input_dev
      +-> evdev -> /dev/input/event0
      +-> console handler -> consoleintr()
          -> xv6 console/TTY line discipline
          -> /dev/ttyS0 or foreground shell read()
```

键盘输入并不直接写用户进程缓冲区。`usbkbd.c` 把 HID usage code
转换成字符后调用 `consoleintr()`，因此串口已有的回显、退格、Ctrl+C、
阻塞读和唤醒语义会被复用。

同一份 HID report 也会生成原始按键事件并进入 `/dev/input/event0`。
因此它同时具有两种用途：

```text
HID report -> HID usage转Linux KEY_* code
           -> input_report_key() + input_sync()
           -> input core
                +-> evdev队列 -> /dev/input/event0
                +-> console字符转换 -> consoleintr() -> shell/登录输入
```

xv6 现在有精简的 devtmpfs，但没有 Linux udev 的用户态规则系统。devtmpfs
预先创建 `/dev/input/event0..event3`（major 4，minor 0..3）；空槽 open 返回
`ENODEV`语义的失败，设备注册后同一节点按minor找到对应`input_dev`。事件
记录为8字节：`uint16 type`、`uint16 Linux KEY_* code`、`int value`。
`EV_KEY(type=1)`的`value=1/0`分别表示按下/释放；每份HID报告末尾还会产生
`EV_SYN/SYN_REPORT`。队列溢出时input core插入`SYN_DROPPED`，提示读取者
丢弃缓存状态。

## Input Subsystem

输入子系统位于`kernel/input.c`和`kernel/input.h`，把“硬件如何产生按键”与
“用户如何读取输入事件”分开。核心接口为：

```c
struct input_dev *input_allocate_device(void);
void input_set_capability(struct input_dev *, int type, int code);
int  input_register_device(struct input_dev *);
void input_report_key(struct input_dev *, int code, int value);
void input_sync(struct input_dev *);
void input_unregister_device(struct input_dev *);
void input_free_device(struct input_dev *);
```

USB键盘`probe()`分配`input_dev`，登记设备名称、USB VID/PID和支持的按键
能力位图，再注册为`event0`。`usbkbd.c`只保留USB/HID职责：比较新旧HID
report，把usage转换为Linux`KEY_*`编号并上报。事件去重、按键状态、环形
队列、阻塞读、`SYN_REPORT`与`SYN_DROPPED`均由input core负责。

```mermaid
flowchart LR
    A[DWC2 URB completion] --> B[usbkbd HID parser]
    B --> C[HID usage -> Linux KEY code]
    C --> D[input_report_key]
    D --> E[input_dev key state]
    E --> F[input_sync / SYN_REPORT]
    F --> G[evdev ring buffer]
    G --> H[/dev/input/event0]
    C --> I[console character handler]
    I --> J[consoleintr / TTY / shell]
```

字符设备层会把inode minor和每次open的`file.private_data`交给evdev：每个
`eventN`稳定绑定设备表中的第N槽，每个open分配独立`evdev_client`环形队列，
不同读取者不会相互窃取事件。拔出后旧fd先读完已经提交的完整事件帧，再返回
断开错误；input设备内存由引用计数延迟到最后一个fd关闭后释放。

## 枚举与绑定

`main()` 在 DWC2 枚举之前注册 `usbhid-keyboard` USB driver。DWC2 扫描
LAN951x 下游端口，读取 device/configuration/interface/endpoint 描述符。
匹配条件是：

- interface class 3：HID；
- subclass 1：Boot Interface；
- protocol 1：Keyboard；
- 至少一个 interrupt-IN endpoint。

配置完成后，USB core 按 class/subclass/protocol 将设备绑定到
`usbkbd_probe()`。probe 发送 `SET_PROTOCOL(boot)` 与 `SET_IDLE(0)`，然后
为该设备分配私有 `usbkbd_state` 和 interrupt-IN URB，再用
`usb_submit_urb()`异步 arm DWC2 channel 6。后续由 URB 的 `context` 找回
所属键盘，不再依赖全局静态键盘状态。

## Linux 风格 URB 与每设备状态

USB core 现提供一组精简的 Linux 风格接口：

```c
struct urb *usb_alloc_urb(void);
void usb_fill_int_urb(...);
int  usb_submit_urb(struct urb *urb);
void usb_hcd_giveback_urb(struct urb *urb, int status, int actual);
void usb_kill_urb(struct urb *urb);
void usb_free_urb(struct urb *urb);
```

`struct urb`保存设备、endpoint、interval、DMA buffer、请求长度、实际长度、
完成状态、完成函数和`context`。所有权规则为：

```text
USB class driver
  -> usb_submit_urb()
  -> USB core 将URB标记为SUBMITTED
  -> DWC2 HCD取得URB并启动channel 6 DMA
  -> SSPLIT/CSPLIT中间调度仍由HCD处理
  -> 最终完成或错误
  -> usb_hcd_giveback_urb(status, actual_length)
  -> URB所有权归还class driver
  -> urb->complete(urb)
  -> completion通过urb->context取得usbkbd_state
  -> queue_work(kbd->rx_work)
  -> 解析报告并重新usb_submit_urb()
```

键盘状态由`usbkbd_probe()`用`kalloc()`逐设备分配，并保存在
`udev->dev.driver_data`；`struct urb`也属于这份设备私有状态。input core为设备
分配空闲的稳定event minor；传输、完成回调、work和生命周期都不是全局键盘
单例。

拔出或HCD移除时使用同步取消顺序：

```text
usbkbd_remove()
  -> active=0, disconnected=1
  -> usb_kill_urb()
       -> DWC2停止channel 6 DMA、屏蔽HCINT/HAINT
       -> cancel_work_sync(HCD completion work)
       -> 等待正在运行的URB completion返回
  -> cancel_work_sync(kbd->rx_work)
  -> input_unregister_device()
       -> 从event minor槽摘除并唤醒阻塞read
       -> 已打开fd继续持有input_dev引用
  -> usb_free_urb()
  -> 释放driver私有状态；input_dev由最后一个引用释放
```

因此`usb_kill_urb()`返回后，不会再有DWC2 completion使用这份URB；
`cancel_work_sync()`返回后，也不会再有键盘下半部访问设备私有内存。这对应
Linux `usb_kill_urb()`在`disconnect()`中的基本生命周期保证。

### 实际查找与配置顺序

键盘不是靠 VID/PID 白名单识别，而是按 USB class/interface 描述符发现。
地址 0 枚举和配置顺序如下：

```text
GET_DESCRIPTOR(Device, 8 bytes)
  -> 获得 bMaxPacketSize0（低速设备通常为 8）
GET_DESCRIPTOR(Device, 18 bytes)
  -> 获得 class、VID、PID
SET_ADDRESS
  -> 为设备分配唯一地址并保存 parent-hub/port/speed route
GET_DESCRIPTOR(Configuration, 9 bytes)
  -> 获得 wTotalLength 与 configuration value
GET_DESCRIPTOR(Configuration, wTotalLength)
  -> 遍历 interface 和 endpoint
  -> 选择 class=3, subclass=1, protocol=1 的 Boot Keyboard interface
  -> 在同一 interface 内选择 interrupt-IN endpoint
SET_CONFIGURATION
usb_device_register()
  -> USB bus 匹配 usbhid-keyboard
SET_PROTOCOL(boot)
SET_IDLE(0)
  -> 只有按键状态改变时返回报告
usb_fill_int_urb()
usb_submit_urb()
```

不能在找到 HID interface 后继续无条件扫描并使用后面的 endpoint；复合设备
可能同时包含鼠标、厂商接口等。当前枚举器用 `current_interface` 限定 endpoint
必须属于已选中的 Boot Keyboard interface。

### 枚举与配置流程图

```mermaid
flowchart TD
    A[固件给 DWC2 USB HCD 上电] --> B[复位 DWC2 并强制 Host mode]
    B --> C[开启 DMA 和全局 Host-channel IRQ]
    C --> D[复位 Root Port]
    D --> E[读取 Root Device Descriptor]
    E --> F{Device class 是否为 9 Hub}
    F -- 否 --> G[解析普通 USB device]
    F -- 是 --> H[SET_CONFIGURATION 配置第一层 Hub]
    H --> I[读取 Hub Descriptor 与端口数量]
    I --> J[给每个端口供电]
    J --> K[最多等待 500 ms PORT_CONNECTION]
    K --> L{端口状态}
    L -- 0x100 只有 POWER --> M[该端口没有检测到设备]
    L -- CONNECTION --> N[复位端口并判断 high/full/low speed]
    N --> O[为地址 0 设置临时 Hub route]
    O --> P[读取下游 Device Descriptor]
    P --> Q[分配唯一 USB address]
    Q --> R{下游设备类型}
    R -- class 9 Hub --> S[配置子 Hub 并递归扫描下游端口]
    S --> K
    R -- HID 或 class 0 --> T[读取 Configuration 与 Interface Descriptor]
    T --> U{class=3<br/>subclass=1<br/>protocol=1}
    U -- 否 --> V[不是 HID Boot Keyboard]
    U -- 是 --> W[查找 interrupt-IN endpoint]
    W --> X[SET_CONFIGURATION]
    X --> Y[usb_device_register]
    Y --> Z[USB bus 匹配 usbhid-keyboard driver]
    Z --> AA[usbkbd_probe]
    AA --> AB[SET_PROTOCOL boot 与 SET_IDLE 0]
    AB --> AC[arm channel 6 interrupt-IN DMA]

    N -. low-speed .-> LS[route.low_speed=1]
    LS --> LD[control 与 channel 6 的 HCCHAR 设置 LSPDDEV]
    LD --> O
```

本次真机日志对应的关键分支是：第一层 port 1 发现第二层 Hub；递归扫描后，
第一层 port 3 出现 `status=0x301/0x303`，因此它是低速设备，必须走图中的
`route.low_speed` 和 `HCCHAR.LSPDDEV` 分支。

## DWC2 interrupt-IN

这里的 “interrupt endpoint” 是 USB 传输类型，不表示键盘直接拉起 ARM
IRQ。主机必须向 endpoint 发起 IN token。当前实现先异步 arm DWC2 host
channel 6；传输状态变化后由 DWC2 产生 USB IRQ 9。具有 microframe 截止时间
的 SSPLIT/CSPLIT 中间状态由 DWC2 IRQ 快速路径推进；最终完整报告或错误由
DWC2 completion work 归还URB，再由URB completion进入`usbkbd_wq`。CPU不再
用Timer周期调用键盘poll函数。DMA
接收的是标准 8 字节 boot report：

```text
byte 0     modifier bitmap (Ctrl/Shift/Alt/GUI)
byte 1     reserved
byte 2..7  最多六个同时按下的 usage code
```

驱动将本次报告和上次报告比较，只为新按下的键产生字符，避免按住一个键
时把同一报告无限重复注入。当前支持字母、数字、常用符号、Enter、Tab、
Backspace、Esc、Ctrl、Shift、Caps Lock 和方向键。

### 按键报告到终端字符

`usbkbd_rx_work()`收到8字节报告后执行两次比较：

1. 本次`keys[2..7]`中存在、上次不存在的usage，产生按下事件并查`keymap`；
2. 上次存在、本次不存在的usage，通过`input_report_key(..., 0)`上报释放；
3. modifier bitmap中的Ctrl/Shift/Alt/GUI也作为独立`EV_KEY`上报；
4. 每份报告调用`input_sync()`生成`SYN_REPORT`；
5. Shift/Caps Lock选择普通或大写映射，Ctrl+字母转换为控制字符；
6. 普通字符、Backspace、Enter及方向键转义序列统一送入`consoleintr()`；
7. console/TTY行规程负责回显、行缓冲、Ctrl+C及唤醒前台读进程。

真机已经收到：

```text
usbkbd: first HID report mod=0 keys=10,0,0,0,0,0
```

日志使用十六进制输出，所以`keys=10`表示HID usage `0x10`，对应字母`m`；
`mod=0`表示没有Ctrl/Shift/Alt/GUI修饰键。这条日志证明从低速键盘、Hub TT、
DWC2 DMA/IRQ、workqueue到HID解析的整条接收路径已经成功。该日志只打印第一
份有效报告，后续每个按键不在IRQ路径逐条打印，以免再次破坏USB时序。

## 改造前：CPU 定时轮询方式

早期版本虽然 endpoint 类型叫作 `interrupt-IN`，CPU 处理方式仍然是
轮询，并不是由 BCM2837 legacy USB IRQ 9 驱动。调用链如下：

```text
ARM Generic Timer 定时中断
  -> workqueue_timer_tick()
  -> keyboard.poll_work 到期
  -> poll_work 加入专用 usbkbd_wq
  -> usbkbd_wq 内核线程调用 usbkbd_poll()
  -> udev->ops->interrupt()
  -> dwc2_usb_interrupt()
  -> channel_xfer(channel 6)
  -> CPU 循环读取 HCINT(6)
  -> 收到 HID report、NAK、错误或超时
  -> usbkbd_poll() 解析按键
  -> queue_delayed_work(..., 1) 安排下一轮
```

因此旧实现有两层 CPU 软件轮询：

1. `queue_delayed_work()` 每隔一个 xv6 tick 唤醒 `usbkbd_poll()`，由 CPU
   定期启动一次 interrupt-IN 事务；
2. `channel_xfer()` 启动 host channel 6 后忙等读取 `HCINT(6)`，直到本次
   事务完成、NAK、出错或超时。

```text
第一层：Timer -> delayed_work -> usbkbd_poll()
第二层：channel_xfer() -> while -> rd(HCINT(6))
```

这里仍需区分 USB 总线协议和 CPU 执行方式：USB 键盘不能主动向主机发送
报文，主机必须产生 IN token；但是这些 IN token 可以由 DWC2 硬件调度，
完成后再使用 IRQ 通知 CPU。旧实现没有使用后面这种异步完成方式。

## 当前代码：DWC2 IRQ 驱动方式

当前版本已经改为：

```text
usbkbd_probe()
  -> 首次 arm 长期 interrupt-IN DMA 请求
  -> 返回，不等待传输

DWC2 硬件调度 IN token
  -> 低速键盘经 LAN951x TT 执行 SSPLIT/CSPLIT
  -> 键盘有数据时 DMA 写入 HID report
  -> HCINT(6).XFERCOMPL
  -> BCM2837 legacy USB IRQ 9

dwc2_irq()                         hard IRQ / HCD 快速路径
  -> SSPLIT ACK: 记录 IRQ 时 HFNUM
  -> 到 issue+2 microframes 时提交 CSPLIT
  -> 窗口内 NYET: 下一 microframe 重试 CSPLIT
  -> XFERCOMPL/超出窗口/错误: 屏蔽 channel 6
  -> schedule_work(DWC2 URB completion work)

DWC2 completion work               HCD可调度上下文
  -> 读取HCINT/HCTSIZ和DMA实际长度
  -> NAK/本轮无数据时按bInterval重新调度，URB保持submitted
  -> 最终完成时usb_hcd_giveback_urb()

usbkbd_irq_complete()              URB completion
  -> 通过urb->context取得usbkbd_state
  -> queue_work(kbd->rx_work)

usbkbd_wq                          进程上下文下半部
  -> 读取URB结果、解析HID report、consoleintr()
  -> 重新usb_submit_urb()，arm新的SSPLIT/interrupt-IN请求
```

具体代码变化如下：

- 删除 `keyboard.poll_work`、`usbkbd_poll()` 及周期性
  `queue_delayed_work()`；
- 增加每设备`kbd->rx_work`，只有对应URB completion才会调度；
- 增加通用URB core以及host ops `submit_urb()`和`kill_urb()`；
- DWC2 `submit_urb()`配置DMA、`HCTSIZ(6)`、`HCCHAR(6)`后立即返回，
  不再循环读取 `HCINT(6)`；
- `HCINTMSK(6)` 打开 XFERCOMPL、CHHLTD、NAK、ACK、NYET 和错误事件，
  `HAINTMSK` 打开 bit 6；
- `dwc2_irq()` 对有 microframe 截止时间的 SSPLIT ACK 和窗口内 NYET 直接推进
  channel 6，避免普通 kworker 的毫秒级调度延迟；
- 最终完成、超出split窗口或错误才屏蔽channel 6，并调度DWC2自己的URB
  completion work；
- HCD完成处理调用`usb_hcd_giveback_urb()`；键盘的URB completion仅排入
  `rx_work`；
- `rx_work`读取`urb->status/actual_length`，解析成功报告并重新提交URB。

若出现 `IRQ-driven boot keyboard ready` 但按键无响应，检查后续诊断：
`ch6 armed` 表示 DMA/channel 已启动；`ch6 final` 与
`usbkbd: channel 6 IRQ received` 表示host-channel请求已由HCD完成并通过URB
completion排入键盘下半部；
`first HID report` 表示 DMA report 已收到。若只有 armed，没有 final/report，问题在
DWC2 channel/IRQ 路径；若有 irq 但没有 report，则看 `HCINT`/`HCTSIZ` 及
下半部的重试/错误输出。

full/low-speed 键盘在 LAN951x Hub 后需要 start-split/complete-split。
start-split ACK 和有效调度窗口内的 complete-split NYET 由 HCD IRQ 快速路径
处理，不会被当成完整 HID report，也不会先经过普通 workqueue。

workqueue 自己的链表、`pending/running/rerun` 已由 `wq->lock` 保护；
`usbkbd_state.lock` 保护设备的 `active` 和生命周期状态。IRQ 上半部不解析
HID、不调用 console、不访问文件系统；为了满足 split microframe 时序，它会
在HCD内最多短暂等待500 us并重启channel。DMA cache invalidate和
`bInterval`等待在DWC2 completion work中完成；HID解析、`consoleintr()`与
新一轮URB提交在`usbkbd_wq`中完成。

### IRQ 接收时序图

```mermaid
sequenceDiagram
    participant K as usbkbd_probe / rx_work
    participant H as DWC2 channel 6
    participant TT as LAN951x Transaction Translator
    participant KB as Low-speed USB Keyboard
    participant IRQ as BCM2837 USB IRQ 9
    participant HCW as DWC2 URB completion work
    participant URB as USB core / URB completion
    participant WQ as usbkbd_wq
    participant C as console / input event

    K->>URB: usb_submit_urb(buffer, 8)
    URB->>H: HCD submit_urb
    Note over H: 配置 HCDMA/HCTSIZ/HCCHAR<br/>HCINTMSK(6) 与 HAINTMSK bit 6
    H-->>K: 立即返回，不忙等 HCINT

    H->>TT: start-split IN
    TT-->>H: ACK
    H->>IRQ: channel 6 interrupt
    Note over IRQ: 记录真实 HFNUM<br/>等待到 issue + 2 microframes
    IRQ->>H: fast path 提交 complete-split
    H->>TT: complete-split IN

    alt 键盘暂时没有新报告
        TT->>KB: IN token
        KB-->>TT: NAK
        TT-->>H: NYET / 本轮无可取结果
        H->>IRQ: channel 6 interrupt
        alt 仍在当前 split frame
            IRQ->>H: 下一 microframe 重试 CSPLIT
        else 已超过 CSPLIT 窗口
            IRQ->>HCW: schedule HCD completion work
            HCW->>HCW: 等待 bInterval
            HCW->>H: 从新的 SSPLIT 重新调度，URB保持submitted
        end
    else Transaction Translator 尚未完成
        TT-->>H: NYET
        H->>IRQ: channel 6 interrupt
        IRQ->>H: 窗口内直接重试 complete-split
    else 键盘返回按键
        TT->>KB: IN token
        KB-->>TT: 8-byte HID report
        TT-->>H: DATA / XFERCOMPL
        H->>H: DMA 写入 report buffer
        H->>IRQ: BCM2837 legacy USB IRQ 9
        IRQ->>HCW: schedule HCD completion work
        HCW->>URB: usb_hcd_giveback_urb(status, actual)
        URB->>WQ: urb completion queue_work(rx_work)
        WQ->>WQ: 读取status/actual并解析HID report
        WQ->>C: consoleintr(character)
        WQ->>C: enqueue /dev/input/event0
        WQ->>URB: usb_submit_urb下一次interrupt-IN
    end
```

## Hub split transaction

树莓派 3 的外置 USB 口在 LAN951x 高速 Hub 后面，而普通键盘通常是
full-speed/low-speed。DWC2 不能直接向这种设备发送普通事务，必须执行：

```text
start-split -> LAN951x transaction translator
            -> full/low-speed keyboard transaction
complete-split <- transaction result
```

`dwc2.c` 保存 USB address 到 hub-address/port 的路由，并通过 `HCSPLT`
设置 `SPLTENA`、`HUBADDR`、`PRTADDR`、`COMPSPLT`。Hub 返回 NYET 时只重试
complete-split，不重复发送 start-split 数据；但只允许在所属 USB frame 的
有效周期窗口内重试。超过窗口后必须结束本轮，等待 `bInterval`，再以新的
start-split 开始下一次键盘轮询。

路由还记录端口速度。对于 `status` 含 `HUB_PORT_LOW_SPEED` 的键盘，所有
枚举 control channel 和后续 interrupt-IN channel 都必须在 `HCCHAR` 设置
`LSPDDEV`；否则 LAN951x 会在低速设备的 SETUP start-split 上返回 STALL，
典型日志是 `intr=0x0a`、`control setup failed req=6 addr=0`。

split-IN 的 complete-split 如果返回 NAK，表示下游设备已经结束这一次 USB
事务但暂时没有数据。不能只反复提交 COMPSPLT；同步 control 路径必须把
状态切回 start-split，在下一帧开始一笔新事务。否则会在读取低速设备
descriptor 时耗尽等待期限，表现为 `control data failed ... result=-2`。

低速设备的 EP0 最大包长为 8 字节。读取 18 字节 Device Descriptor 时，
数据阶段需要多个 split-IN 事务；每个包都要单独完成 SSPLIT/CSPLIT，并在
完整收到一个包后切换 DATA PID。`channel_xfer()` 因此将多包 split 请求按
`mps` 分段提交，并在调试输出中报告每次 CSPLIT 实际收到的字节数。若只
收到描述符前 8 字节，后续 VID/PID 字段仍为零，不能据此认为枚举成功。

复合 HID 设备可能有多个 interface。枚举器先查找 Boot Keyboard interface，
之后只从该 interface 收集 class/subclass/protocol 和 interrupt-IN endpoint；
不会再被同一配置中的鼠标或其他 HID interface 覆盖。
键盘完成 USB bus 注册和驱动 probe 后，DWC2 初始化会直接返回，不会再把
键盘交给 CDC-ECM 网络层读取配置描述符。

Pi 3/LAN951x 的实际日志还可能呈现两层 Hub。枚举器会在发现 class 9
设备后配置子 Hub、读取 Hub descriptor、给下游端口供电和复位，并继续
搜索 HID Boot Keyboard。递归深度限制为两层，所有下游设备使用独立 USB
address；full/low-speed 子设备的 address 同时记录父 Hub address 和 port，
供后续 control 与 interrupt-IN split transaction 使用。

```text
DWC2
  -> root LAN951x hub
      +-> nested hub -> USB Ethernet
      +-> low-speed HID boot keyboard
```

对应诊断日志为 `scanning nested hub`、`nested hub N port M status` 和
`nested ... child class/vid/pid/address`。

端口上电后不会只读取一次状态。第一层和嵌套 Hub 都会在复位枚举前等待
最多 500 ms 的 `PORT_CONNECTION`/去抖，以兼容冷启动较慢的 Hub、无线
键盘接收器和低速键盘。如果等待后仍为 `status=0x100`，含义是控制器只
看到 `PORT_POWER`，没有检测到设备的电气连接；此时 HID 和 channel 6
IRQ 尚未参与。

## 本次真机排查记录

这次排查使用的设备拓扑为：

```text
DWC2 root port
  -> LAN951x hub (VID:PID 0424:2514)
      +-> port 1: nested hub, USB address 2
      |    -> USB Ethernet function (0424:7800)
      +-> port 3: low-speed HID keyboard receiver (2a7a:8a47)
```

过程记录如下。每一步只把串口日志实际证明的内容当作结论；`ACK` 不等于
下游设备数据已成功返回，`ready` 也不等于已经收到按键报告。

| 阶段 | 观察到的现象 | 分析与处理 | 结果 |
| --- | --- | --- | --- |
| 1. 找到键盘端口 | 外部 hub port 3 为 `status=0x301`，reset 后 `status=0x303 speed=low` | 识别为低速设备，控制传输必须记录 parent hub/port/速度并走 split transaction | 低速键盘端口及路由已识别 |
| 2. 低速设备 SETUP 失败 | 早期 `control setup failed req=6 addr=0`，曾出现 `intr=0x0a` | 低速路由还必须设置 `HCCHAR.LSPDDEV`；同步 `channel_xfer()` 根据 USB 地址路由设置该位 | SETUP 后续可以继续，旧 STALL 不再是主要阻塞点 |
| 3. split-IN 超时 | GET_DESCRIPTOR 的数据阶段返回 `-2` | 对照 Linux DWC2，split NAK 后应清除 `complete_split` 并重新发起 SSPLIT；NYET 则继续当前 CSPLIT | 日志出现 SSPLIT ACK 和 CSPLIT 完成，排查进入数据长度阶段 |
| 4. 设备描述符字段为零 | 原先枚举日志出现 `class=0 vid=0 pid=0` | 低速 EP0 MPS 为 8，18 字节描述符不能作为单个跨多个 split packet 的请求处理；按 MPS 分包，每个完整数据包切换 DATA PID，并检查 descriptor 长度/type/VID | 日志成功读到 `vid=2a7a pid=8a47 mps=8` |
| 5. HID interface 被拒 | 曾报告 `USB HID device is not a boot keyboard` | 先查找 Boot Keyboard interface，再只从这个 interface 收集 class/subclass/protocol 和其 interrupt-IN endpoint，避免复合 HID 的其他 interface 覆盖结果 | 后续日志成功到达键盘 probe |
| 6. 绑定后额外 STALL | 键盘 ready 后仍出现 `req=6 addr=4` 和 `intr=0x0a` | HID 绑定后错误地继续走 CDC-ECM 网络探测，导致向键盘发起不相关的配置描述符请求 | HID 初始化后直接返回；后续日志显示 `HID boot keyboard handed to usb bus` |
| 7. channel 6 重复 NYET | 按键无响应；IRQ 日志为 `phase=1 hcint=0x42`，`hcsplt` 带 `COMPSPLT`，`hctsiz=0x80008` | `0x42` 是 `CHHLTD|NYET`。旧状态机把每个 NYET 都当成“TT 仍在工作”，因此永久重发 CSPLIT；但 interrupt split 的 complete 阶段只能在本次周期调度窗口内重试 | 记录 SSPLIT ACK 的 `HFNUM.FRNUM`；同一 USB frame 内最多重试 3 次 CSPLIT。跨 frame 仍为 NYET 时结束本轮、清除 `complete_split`，等待 endpoint `bInterval` 后重新发 SSPLIT |
| 8. kworker 错过 CSPLIT 窗口 | 新日志中 SSPLIT IRQ 的 `age=0/1`，但随后保存的 `ssack` 比 `issue` 晚几十个 microframe | IRQ 上半部只屏蔽 channel 6，然后依赖普通 kworker 处理 ACK；启动及多核调度期间 worker 可能晚数毫秒运行，而 CSPLIT 必须在同一 USB frame 内提交 | SSPLIT ACK 和同一窗口内的 CSPLIT NYET 重试改由 DWC2 IRQ 快速状态机处理；只把完整 HID report 或本轮无数据结果交给 `usbkbd_wq` |
| 9. IRQ 诊断自身破坏时序 | 快速状态机启用后仍出现跨 frame；日志中 `issue/ssack` 一度正确，但处理仍迟到 | `printf("dwc2 ch6 ...")` 位于快速状态机之前；115200 波特率输出一行需要数毫秒，远超过 125 us microframe，是典型的 timing-sensitive Heisenbug | ACK/窗口内 NYET 不再打印；先执行 IRQ 快速状态机。只有最终完成、无数据或错误退出窗口后才限量打印 `dwc2 ch6 final ...` |

关键寄存器样本可以解码为：

| 样本 | 含义 |
| --- | --- |
| `HCCHAR=0x010e8808` | device address 4、endpoint 1、Interrupt-IN、low-speed、MPS 8 |
| `HCSPLT=0x8001c083` | SPLITENA、COMPSPLT、XACTPOS=ALL、Hub address 1、port 3 |
| `HCINT=0x22` | `CHHLTD | ACK`，SSPLIT已被TT接受 |
| `HCINT=0x42` | `CHHLTD | NYET`，CSPLIT暂时没有完成结果 |
| `HCINT=0x03` | `CHHLTD | XFERCOMPL`，DMA中已有完整传输结果 |

### 当前中断/报告诊断步骤

近期在 `dwc2.c` 加入了限量日志。日志需要来自包含这些改动的镜像；仅有
`ready` / `handed to usb bus` 的旧输出不能判定 IRQ 到达情况。

```mermaid
flowchart TD
    A[usbkbd ready / HID handed to bus] --> B{出现 ch6 armed?}
    B -- 否 --> B1[驱动未成功 arm 或运行的镜像不含诊断]
    B -- 是 --> C{按键时出现 ch6 final 或 HID report?}
    C -- 否 --> C1[检查 HCCHAR HCSPLT HCTSIZ<br/>以及 USB IRQ 9 / HAINT / GINTSTS]
    C -- 是 --> D{出现 usbkbd channel 6 IRQ received?}
    D -- 否 --> D1[检查 IRQ 上半部到 workqueue 的通知路径]
    D -- 是 --> E{出现 first HID report?}
    E -- 否 --> E1[检查 HCINT 状态、split 重试和 DMA residual]
    E -- 是 --> F{报告 keys 字段非零?}
    F -- 否 --> F1[检查键盘实际报告、report 格式和 endpoint]
    F -- 是 --> G[检查 usage-to-character 映射与 consoleintr]
```

日志标志含义：

- `dwc2: ch6 armed ...`：channel 6 的 HCSPLT、HCCHAR、HCTSIZ、HCINTMSK、
  HAINTMSK 和 GINTMSK 已配置并启动；不是“键盘数据已到”。
- `dwc2: ch6 final ...`：只有不再需要满足当前CSPLIT实时期限时才打印；`p=1`
  表示当前是complete-split，`int`是HCINT，`uf`是IRQ采样的microframe，
  `issue`是本次channel enable时的counter，`age`是发出到IRQ的microframe
  距离，`ssack`是最近一次SSPLIT ACK的counter。中间ACK/NYET不打印。
- `usbkbd: channel 6 IRQ received`：HCD已经归还URB，键盘completion已把工作
  提交给`usbkbd_wq`。
- `usbkbd: first HID report ...`：workqueue 已处理完成的传输并读取 DMA
  report；`keys` 六个字段是 HID usage code。
- `consoleintr()`：只有 report 中出现新的、受支持的按键 usage 后才会调用；
  modifier-only 或全零报告不应产生字符。

测试时应在开机完成后按下并松开一个字母键，保留从 `ch6 armed` 开始的
完整输出。若只有 `armed` 而没有 `ch6 final`/report，先调查 host-channel 中断产生/
路由；若有 IRQ 但无 report，依据 `HCINT`、`HCSPLT`、`HCTSIZ` 检查 split
状态机或 DMA；若 `first HID report` 有非零 usage 但终端不回显，再调查
键码映射、workqueue 和 console 输入路径。

补充：`HCINT=0x42` 的重复日志代表 channel halted 并收到 NYET，不是完整
HID report。`HCSPLT.COMPSPLT=1` 表示当前尝试的是 complete-split。仅增加
固定 125/250 us 延迟并不能解决问题，因为它没有给 CSPLIT 设置调度窗口
终点。修复后的规则与 Linux DWC2 周期 split 处理一致：SSPLIT ACK 后只在
所属 frame 内继续 CSPLIT；超过窗口就将本次无数据轮询结束，按 `bInterval`
开始下一次 SSPLIT。等待期间会释放 `usb_lock`，不会让空闲键盘阻塞其他 USB
channel。排错阶段曾使用 `dwc2 ch6 p=... int=... uf=... issue=... age=...
ssack=...` 比较 channel enable、SSPLIT ACK 和 NYET 的counter；最终版本已把
这类中间日志移出实时路径，只保留不会影响CSPLIT期限的 `ch6 final`。

进一步采样出现了 `p=0 int=0x22` 与 `p=1 int=0x42` 交替，但同时暴露出
worker 延迟：SSPLIT 从 issue 到 IRQ 只经过 0--1 microframe，而 worker 写入
`ssack` 时已过去数十个 microframe。为此，中间 split 调度不再经过进程调度器：

```mermaid
flowchart TD
    I[DWC2 channel 6 IRQ] --> S{HCINT 状态}
    S -->|SSPLIT ACK 0x22| A[IRQ 内记录真实 HFNUM]
    A --> B[等待到 issue + 2 microframes]
    B --> C[提交 CSPLIT]
    S -->|CSPLIT NYET 0x42<br/>仍在本 frame| D[IRQ 内等待下一 microframe]
    D --> C
    S -->|CSPLIT 超出窗口| W[屏蔽 channel 6<br/>schedule HCD URB work]
    W --> P[HCD等待 bInterval]
    P --> R[重新提交 SSPLIT]
    S -->|XFERCOMPL| X[schedule HCD URB work]
    X --> G[cache invalidate + giveback URB]
    G --> H[URB completion queue kbd rx_work<br/>HID report解析]
```

当前快速路径最多忙等 500 us，并只负责 DWC2 split 的硬件时序，不解析 HID、
不进入 console，也不执行文件系统操作。更完整的 HCD 可再启用 SOF/microframe
调度器，以定时事件替代 IRQ 内的短忙等；在现有 xv6 HCD 中，快速状态机避免了
普通 100 ms tick 和 kworker 调度精度不足的问题。

调试这条路径时必须避免在 SSPLIT ACK 和 CSPLIT NYET 的实时分支中直接输出
串口。115200 8N1 每字符约 86.8 us，一条几十字符的日志会消耗多个 USB frame。
因此最终版本只输出 `ch6 armed` 一次，并在 split 调度已经结束后输出限量的
`ch6 final`；中间事务如需跟踪，应写入内存 trace ring，稍后由进程上下文打印。

## 真机测试

构建并同步 TFTP 镜像：

```sh
make
```

启动时预期看到类似：

```text
dwc2: hub port=3 child class=0 vid=xxxx pid=xxxx mps=8
usbkbd: IRQ-driven boot keyboard ready ep=1 mps=8 interval=10
dwc2: HID boot keyboard handed to usb bus
```

进入登录或 shell 后测试字母、Shift、Caps Lock、Backspace、Enter 和
Ctrl+C。若只看到上面三行但按键无反应，按“当前中断/报告诊断步骤”收集
channel 6 IRQ 与 report 日志。若出现 `child descriptor failed`，记录对应
端口 speed、HCINT、HCTSIZ 和 HCSPLT；这通常表示枚举阶段 split transaction
仍需针对该 Hub/键盘组合调整。

设备节点检查：

```sh
ls /dev/input
cat /proc/devices
```

应看到 `event0`，以及字符设备 `4 input/event0`。注意 `ls /dev` 只显示
`input` 子目录，不会直接把子目录内的 `event0` 展开显示。

## 当前边界

当前 DWC2 枚举器仍以“选择一个受支持的外置 USB function”为主，USB
键盘与 MT7601U 同时插入时尚未完整发布为两个独立 `usb_device`。下一步
应把 `usb_child` 改成按 hub port 分配的设备数组，并为每个设备独立保存
地址、toggle、host channel/completion 与拔出生命周期。

## Linux 风格 input/evdev 与热插拔（2026-10）

输入路径现在分为四层，USB class driver 不再承担用户设备节点策略：

```mermaid
flowchart LR
    H[LAN951x hub port] --> D[DWC2 HCD<br/>control + interrupt-IN URB]
    D --> C[usbhid keyboard/mouse<br/>解析 boot report]
    C --> I[input core<br/>input_dev + capability/state]
    I --> E[evdev clients<br/>每个 open 独立 ring]
    E --> N[/dev/input/event0..event3]
```

实现与 Linux 对齐的关键语义：

- `input_register_device()`从设备表分配稳定的event minor；`open(eventN)`按
  inode minor绑定对应`input_dev`，不再让所有设备共享或“晋升”为event0。
- 每个open创建独立`evdev_client`和ring；慢读者溢出时收到`SYN_DROPPED`，
  只有到`SYN_REPORT`的完整帧才对read可见。
- 键盘发布`EV_KEY`；Boot Mouse发布`BTN_LEFT/RIGHT/MIDDLE`以及
  `EV_REL/REL_X/REL_Y/REL_WHEEL`，然后统一`input_sync()`。
- driver持有`input_dev`初始引用；已打开event fd持有额外引用。拔出后设备
  从注册表删除并唤醒reader，但内存一直保留到最后一个fd关闭。
- EP0最大包长现在按USB address保存；hub、低速键盘和鼠标不再共享一个全局
  `ep0_mps`，热插拔状态查询始终使用hub地址1自己的控制端点参数。
- `/dev/input/event0..event3`预先由devtmpfs创建；空槽open失败。这样避免当前
  简化devtmpfs缺少动态unlink时，把已经复用的旧节点误指向其他设备。

直接连接在LAN951x根hub外部端口上的输入设备支持运行时热插拔。HCD读取
Hub configuration descriptor中的interrupt-IN endpoint，为地址1的Hub建立
长期Hub URB，并用独立DWC2 channel 7接收change bitmap。空闲时Hub返回NAK；
只有bitmap非零才由`dwc2_hub_wq`读取发生变化端口的status并处理拓扑：

```mermaid
sequenceDiagram
    participant P as DWC2 channel 7
    participant Q as Hub interrupt URB
    participant W as dwc2_hub_wq
    participant U as USB bus/device core
    participant H as usbhid driver
    participant I as input core/evdev
    participant D as DWC2 HCD
    P->>Q: interrupt-IN完成，DMA得到change bitmap
    Q->>W: URB completion排入hub_event_work
    W->>W: 只对bitmap标记端口执行GET_STATUS/CLEAR_FEATURE
    W->>U: usb_device_unregister(old child)
    U->>H: remove()
    H->>D: usb_kill_urb()
    H->>H: cancel_work_sync()
    H->>I: input_unregister_device()
    I-->>I: wake blocked readers, preserve open-file refs
    W->>D: halt/mask channels, clear pending IRQ
    W->>D: reset host/root port and enumerate
    D->>U: usb_device_register(new child)
    U->>H: match + probe
    H->>I: input_register_device()
```

启动及插入鼠标时预期日志：

```text
input: USB HID Boot Mouse registered as event0
usbmouse: IRQ-driven boot mouse ready ep=... mps=... interval=...
dwc2: HID boot mouse handed to usb bus
```

启动以及拔出/重新插入时应出现：

```text
dwc2: hub interrupt-IN ep=1 mps=1 interval=12 ports=4
dwc2: event-driven hub hotplug armed on channel 7
dwc2: hub IRQ change=... connected=...->...; reprobe
```

当前热插拔边界仍需明确：HCD只有一个发布用`usb_child`和一个HID数据
interrupt-IN channel，因此支持“键盘或鼠标”的拔出/替换，但还不能让键盘和鼠标同时
工作；嵌套hub内部端口变化也不会改变LAN951x根端口连接位图。当前检测已经
使用Hub interrupt endpoint，但拓扑改变后仍整棵重枚举。完整Linux式多设备
支持下一步应把`usb_child`、interrupt request和host channel按端口/接口数组化，
并为EP0控制请求增加统一队列，做到只增删发生变化的port/device。

## 热插键盘真机日志与完整通信流程（2a7a:8a47）

本节对应运行时在LAN951x外部port 3插入低速键盘的真实日志。结论是：热插拔
检测、枚举、HID驱动绑定、input注册和interrupt-IN URB提交均已成功；日志若
停在`handed to usb bus`，只说明接收请求已经arm，还需要实际按键产生首份报告
才能证明数据通路完成。

### 连接位图与物理拓扑

```text
dwc2: hub topology changed connected=2->a; reprobe
```

上面是旧轮询版本留下的真机记录；新版本同一事件输出为
`dwc2: hub IRQ change=8 connected=2->a; reprobe`，位图含义不变。

连接位图的bit编号就是root-hub port编号：`0x2=bit1`表示原先只有port 1；
`0xa=bit1|bit3`表示插入后port 1和port 3同时连接。因此新增设备来自port 3。

```mermaid
flowchart TD
    CPU[BCM2837 CPU<br/>legacy USB IRQ 9] --> HCD[DWC2 Host Controller]
    HCD --> RH[LAN951x high-speed root hub<br/>addr 1, 0424:2514]
    RH -->|port 1| IH[内部hub addr 2]
    IH --> IF[板载vendor function<br/>0424:7800 addr 8]
    RH -->|port 3| TT[Hub Transaction Translator]
    TT --> K[low-speed HID boot keyboard<br/>2a7a:8a47 addr 4]
```

热插拔发现现在由Hub interrupt URB驱动：DWC2按Hub endpoint的`bInterval`在
channel 7发起interrupt-IN；NAK仅表示没有变化，并按期限重新arm，不访问EP0。
Hub返回非零change bitmap后，USB IRQ 9将完成项交给`dwc2_hub_wq`，worker才
通过EP0读取并清除相应port的change feature。连接位图变化后先
`usb_device_unregister()`旧设备，同步kill class URB/work，再停止host channel
并重新枚举。键盘报告则继续独立使用channel 6；channel 7负责拓扑事件，
channel 6负责HID数据，不能混为一谈。

### 端口状态、地址0与地址4

```text
dwc2: hub port 3 status=301 change=1
dwc2: hub port 3 after-reset status=303 speed=low
dwc2: hub port=3 child class=0 vid=2a7a pid=8a47 mps=8
```

- `0x301`表示connected、powered、low-speed；reset后的`0x303`又增加enable。
- 新设备首先只能响应默认地址0。HCD读取Device Descriptor前8字节，从
  `bMaxPacketSize0`得到EP0 MPS为8。
- 当前简单地址分配策略为该port分配地址4，之后控制请求都使用`addr=4`。
- Device Descriptor的`class=0`表示类别由interface声明，并不表示设备不是
  HID。Configuration中的interface最终给出`class=3, subclass=1,
  protocol=1`，即HID Boot Keyboard。

枚举控制传输顺序如下：

```text
port power/reset
  -> GET_DESCRIPTOR(Device, first 8 bytes), address 0
  -> 获得 EP0 MPS=8
  -> SET_ADDRESS(4)
  -> GET_DESCRIPTOR(Device), address 4
  -> GET_DESCRIPTOR(Configuration, 9-byte header)
  -> GET_DESCRIPTOR(Configuration, wTotalLength)
  -> 找到 Boot Keyboard interface及EP1 interrupt-IN
  -> SET_CONFIGURATION
  -> usb_device_register()
```

每个USB control request都由SETUP、可选DATA和STATUS三个阶段组成。SETUP固定
使用DATA0；DATA阶段按包切换DATA0/DATA1；STATUS阶段方向与DATA相反并使用
零长度包。枚举使用DWC2 host channel 0，后续按键使用channel 6。

### 为什么日志反复出现SSPLIT和CSPLIT

低速键盘位于高速hub之后，DWC2不能把低速token直接放到高速链路上。它先发送
Start Split，要求hub的Transaction Translator代为执行低速事务，再发送
Complete Split取回结果：

```text
DWC2 --SSPLIT--> LAN951x TT --low-speed SETUP/IN/OUT--> keyboard
DWC2 <--CSPLIT-- LAN951x TT <--结果暂存---------------- keyboard
```

典型日志：

```text
dwc2: ch0 SSPLIT ACK addr=0 in=1 hcint=22 ...
dwc2: ch0 CSPLIT complete addr=0 bytes=8/8 hcint=23 ...
```

字段解释：

| 字段 | 含义 |
| --- | --- |
| `ch0` | 枚举及class request使用的控制host channel |
| `addr=0/4` | SET_ADDRESS前后的USB设备地址 |
| `in=1/0` | 当前control阶段的数据方向 |
| `HCINT=0x22` | `ACK|CHHLTD`，TT接受了SSPLIT |
| `HCINT=0x23` | `ACK|CHHLTD|XFERCOMPL`，CSPLIT返回完整结果 |
| `HCTSIZ` | 请求剩余字节、包数和DATA PID的组合字段 |
| `HFNUM` | USB frame/microframe时序诊断值 |

`HCSPLT`同时编码split enable、hub address 1和port 3，所以控制channel和按键
channel都能沿正确TT路由到低速设备。

### USB core绑定、HID初始化和input注册

Configuration解析得到EP1 IN、MPS 8、interval 10 ms后，DWC2先执行
`SET_CONFIGURATION`，再把`struct usb_device`发布到USB bus。USB core用
class/subclass/protocol匹配`usbhid-keyboard`并同步调用`usbkbd_probe()`：

```text
usb_device_register()
  -> USB bus match (3/1/1)
  -> usbkbd_probe()
       -> SET_PROTOCOL(BOOT)       固定为标准8字节报告
       -> SET_IDLE(0)              状态不变时不重复报告
       -> input_allocate_device()
       -> input_register_device()  分配event0
       -> usb_alloc_urb()
       -> usb_fill_int_urb(EP1 IN, 8 bytes, 10 ms)
       -> usb_submit_urb()
```

因此日志的正常顺序是：

```text
input: USB HID Boot Keyboard registered as event0
usbkbd: IRQ-driven boot keyboard ready ep=1 mps=8 interval=10
dwc2: HID boot keyboard handed to usb bus
```

`usb_device_register()`内部会同步完成match和probe，所以input/keyboard日志先于
DWC2的`handed to usb bus`总结行，这是正常调用顺序。

### 按键报告、DMA、CPU IRQ和下半部

USB的“interrupt transfer”不表示键盘可以直接拉起ARM中断。USB总线只能由host
发起事务；`bInterval=10`表示DWC2应至少每10 ms给EP1安排一次IN机会。没有状态
变化时键盘NAK，有变化时返回8字节Boot Report；DWC2 host-channel事件才产生
BCM2837 legacy USB IRQ 9。

```mermaid
sequenceDiagram
    participant K as USB keyboard
    participant TT as LAN951x TT
    participant D as DWC2 channel 6/DMA
    participant IRQ as IRQ 9 / dwc2_irq
    participant U as USB core/URB
    participant W as usbkbd_wq
    participant I as input/TTY
    D->>TT: SSPLIT, EP1 IN
    TT->>K: low-speed IN token
    K-->>TT: NAK或8-byte HID report
    D->>TT: CSPLIT
    TT-->>D: result; DMA写入report buffer
    D->>IRQ: host-channel interrupt
    IRQ->>IRQ: 实时推进ACK/NYET split状态
    IRQ->>U: final completion schedule_work
    U->>U: cache invalidate + usb_hcd_giveback_urb
    U->>W: complete callback queue rx_work
    W->>W: 比较previous/current report
    W->>I: input_report_key + input_sync
    W->>I: 字符/方向键送consoleintr
    W->>D: usb_submit_urb重新arm
```

8字节Boot Keyboard报告格式：

```text
byte 0     Ctrl/Shift/Alt/Meta modifier bitmap
byte 1     reserved
byte 2..7  最多六个同时按下的HID usage code
```

真实按键后应继续看到：

```text
usbkbd: channel 6 IRQ received
usbkbd: first HID report mod=0 keys=...
```

同一份报告有两条消费者路径：Linux兼容键码进入`/dev/input/eventN`的evdev
独立队列；可显示字符和方向键同时进入`consoleintr()`，复用TTY的回显、退格、
Ctrl+C及命令历史。可用`/bin/evtest /dev/input/event0`验证按下、`SYN_REPORT`
和释放事件。

### 与Linux USB/HID仍有差距的部分

- 与Linux相同，Hub interrupt endpoint用长期URB唤醒hub event worker；空闲
  NAK只按`bInterval`重排，不再每250 ms扫描所有port的EP0状态。
- Linux只增删发生变化的port/device；当前拓扑变化会重新初始化整个DWC2。
- Linux为每个设备/endpoint动态调度多个host channel；当前channel 7固定给Hub
  URB、channel 6固定给唯一HID数据URB，所以键盘和鼠标仍不能同时长期接收。
- 当前空闲NAK路径会在HCD worker中`udelay(bInterval)`；功能正确但占用worker。
  后续应改成基于USB frame期限的delayed work/periodic scheduler。
- 当前为满足低速split的125 us microframe期限，在IRQ快速路径内短暂等待并推进
  CSPLIT；Linux DWC2使用更完整的SOF/periodic schedule状态机，可进一步减少
  hard IRQ中的忙等。
