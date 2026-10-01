# Raspberry Pi 3 USB 键盘驱动

## 驱动路径

```text
USB keyboard
  -> LAN951x high-speed hub
  -> DWC2 host controller
  -> USB bus: struct usb_device
  -> HID boot keyboard class driver
  -> consoleintr()
  -> xv6 console/TTY line discipline
  -> /dev/ttyS0 or foreground shell read()
```

键盘输入并不直接写用户进程缓冲区。`usbkbd.c` 把 HID usage code
转换成字符后调用 `consoleintr()`，因此串口已有的回显、退格、Ctrl+C、
阻塞读和唤醒语义会被复用。

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
启动该设备的 `usbkbd_wq` delayed work。

## DWC2 interrupt-IN

这里的 “interrupt endpoint” 是 USB 传输类型，不表示键盘直接拉起 ARM
IRQ。主机必须向 endpoint 发起 IN token。当前实现先异步 arm DWC2 host
channel 6；传输状态变化后由 DWC2 产生 USB IRQ 9，再由 `usbkbd_wq` 下半部
推进 split/NAK 状态机或接收完成的 DMA 数据。CPU 不再用 Timer 周期调用
键盘 poll 函数。DMA 接收的是标准 8 字节 boot report：

```text
byte 0     modifier bitmap (Ctrl/Shift/Alt/GUI)
byte 1     reserved
byte 2..7  最多六个同时按下的 usage code
```

驱动将本次报告和上次报告比较，只为新按下的键产生字符，避免按住一个键
时把同一报告无限重复注入。当前支持字母、数字、常用符号、Enter、Tab、
Backspace、Esc、Ctrl、Shift、Caps Lock 和方向键。

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
  -> 键盘无数据时收到 NAK，并在之后的 USB frame 再调度
  -> 键盘有数据时 DMA 写入 HID report
  -> HCINT(6).XFERCOMPL
  -> BCM2837 legacy USB IRQ 9

dwc2_irq()                         hard IRQ 上半部
  -> 确认并屏蔽 channel 6 completion
  -> queue_work(usbkbd.rx_work)
  -> 退出 IRQ

usbkbd_wq                          进程上下文下半部
  -> cache invalidate
  -> 读取实际传输长度
  -> 解析 HID report
  -> consoleintr()
  -> 重新 arm interrupt-IN 请求
```

具体代码变化如下：

- 删除 `keyboard.poll_work`、`usbkbd_poll()` 及周期性
  `queue_delayed_work()`；
- 增加普通 `keyboard.rx_work`，只有 channel 6 IRQ 才会调度；
- 增加 host ops `interrupt_rx_arm()` 和 `interrupt_rx_complete()`；
- `interrupt_rx_arm()` 配置 DMA、`HCTSIZ(6)`、`HCCHAR(6)` 后立即返回，
  不再循环读取 `HCINT(6)`；
- `HCINTMSK(6)` 打开 XFERCOMPL、CHHLTD、NAK、ACK、NYET 和错误事件，
  `HAINTMSK` 打开 bit 6；
- `dwc2_irq()` 看到 channel 6 后先屏蔽该 channel，再调用
  `usbkbd_rx_irq()` 将 `rx_work` 放入 `usbkbd_wq`；
- `rx_work` 调用 `interrupt_rx_complete()`，解析成功报告并重新 arm。

full/low-speed 键盘在 LAN951x Hub 后需要 start-split/complete-split：
start-split ACK、complete-split NYET 和键盘 NAK 都会先产生 DWC2 IRQ，随后
由同一个 `rx_work` 状态机提交下一阶段；它们不会被当成完整 HID report。

workqueue 自己的链表、`pending/running/rerun` 已由 `wq->lock` 保护；
`usbkbd_state.lock` 只保护设备的 `active` 和生命周期状态。IRQ 上半部不
解析 HID、不调用 console，也不等待 USB，只执行确认、屏蔽和
`queue_work()`。HID 解析、cache invalidate、`consoleintr()` 与重新 arm
都在可调度的 kworker 进程上下文执行。

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
complete-split，不重复发送 start-split 数据。

## 真机测试

构建并同步 TFTP 镜像：

```sh
make
```

启动时预期看到类似：

```text
dwc2: hub port=3 child class=0 vid=xxxx pid=xxxx mps=8
usbkbd: IRQ-driven boot keyboard ready ep=1 mps=8 interval=10
```

进入登录或 shell 后测试字母、Shift、Caps Lock、Backspace、Enter 和
Ctrl+C。若出现 `child descriptor failed`，优先记录对应端口的 speed、
HCINT、HCTSIZ 和 HCSPLT；这通常表示 split transaction 时序仍需针对该
Hub/键盘组合调整。

## 当前边界

当前 DWC2 枚举器仍以“选择一个受支持的外置 USB function”为主，USB
键盘与 MT7601U 同时插入时尚未完整发布为两个独立 `usb_device`。下一步
应把 `usb_child` 改成按 hub port 分配的设备数组，并为每个设备独立保存
地址、toggle、host channel/completion 与拔出生命周期。
