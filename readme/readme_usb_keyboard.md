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
IRQ。主机必须按照 endpoint 的轮询周期发起 IN token。本实现由
`usbkbd_wq` 周期性提交 interrupt-IN 事务，DWC2 使用 host channel 6 和
DMA 接收标准 8 字节 boot report：

```text
byte 0     modifier bitmap (Ctrl/Shift/Alt/GUI)
byte 1     reserved
byte 2..7  最多六个同时按下的 usage code
```

驱动将本次报告和上次报告比较，只为新按下的键产生字符，避免按住一个键
时把同一报告无限重复注入。当前支持字母、数字、常用符号、Enter、Tab、
Backspace、Esc、Ctrl、Shift、Caps Lock 和方向键。

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
usbkbd: boot keyboard ready ep=1 mps=8 interval=10
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
