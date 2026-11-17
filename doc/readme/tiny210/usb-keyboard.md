# USB 键盘驱动与输入子系统（Tiny210）

## 数据流

完整的加载与通信图（进程/中断上下文、0–11 号消息、锁、按位展开的数据格式）：[images/usbkbd-comm-steps.svg](images/usbkbd-comm-steps.svg)

![usbkbd 通信图](images/usbkbd-comm-steps.png)

```
USB 键盘 ──(低/全速, split)── USB2514 Hub ── S5PV210 EHCI
   │  周期调度：frame list(1024) → anchor QH → 中断 QH，每 1 ms 轮询一次
   ▼
ehci_irq → ehci_intr_reap → usb_hcd_giveback_urb → usbkbd_irq (中断上下文)
   ▼ queue_work(usbkbd_wq)
usbkbd_rx_work: 比较 8 字节 boot 报告 → input_report_key()/input_sync() → 重新提交 URB
   ▼
input 核心 (kernel/input.c)
   ├─ evdev：/dev/input/eventN（N = 注册顺序：K1-K8 按键是 event0，USB 键盘是 event1…）
   ├─ input_handler "kbd"（kernel/drivers/tty/vt/keyboard.c）→ consoleintr()，和串口同一条路径
   └─ win32k 原始事件队列（窗口管理器存在时）
```

## 改动

| 文件 | 内容 |
| --- | --- |
| `kernel/drivers/usb/host/ehci-hcd.c` | 新增周期调度（`frame_list`、`anchor_qh`、`qh_create_intr`），实现 `submit_urb`/`kill_urb`；全/低速设备用 S-mask 0x01、C-mask 0x1c 的 split；IRQ 里回收完成的中断 qTD；QH halt 后在下一次提交时修复 |
| `kernel/drivers/hid/usbhid/usbkbd.c` | USB HID boot 键盘驱动（class 3/1/1）：SET_PROTOCOL(boot)、SET_IDLE(0)、Caps Lock 灯（SET_REPORT），HID usage → KEY_*，忽略 ErrorRollOver 报告，连续 32 次传输错误后停止 |
| `kernel/input.c` / `kernel/input.h` | 每个设备一个 minor（event0..3），不再只有 event0；新增 `input_register_handler()`、`input_has_key()`；拔出时先释放仍按下的键 |
| `kernel/drivers/tty/vt/keyboard.c` | 控制台键盘 handler：只连接有字母键的设备（K1-K8 不会进控制台）；Shift/Ctrl/Caps Lock，方向键输出 `ESC [ A..D`；win32k 有窗口时不往控制台送字符 |
| `include/linux/input.h` | 增加 F1-F12、Home/End/PageUp/PageDown/Insert/Delete、`BUS_USB` |
| `kernel/arch/arm/init.c`, `user/init.c` | 创建 `/dev/input/event1..3` |
| `ports/tiny210/Makefile` | 编译 `tty/vt/keyboard`、`hid/usbhid/usbkbd` |

## 上板验证

启动日志应看到：

```
input: Tiny210 GPIO Keys registered as /dev/input/event0
input: handler kbd registered
ehci: ... periodic on
ehci: low-speed device 413c:2107 class 0 at address 3 (hub 1 port 2)
input: USB HID Boot Keyboard registered as /dev/input/event1
usbkbd: 413c:2107 keyboard on ep1 in, mps 8, interval 10
```

然后直接在 USB 键盘上打字就是控制台输入；`evtest /dev/input/event1` 看原始事件。

调试打印：一个开关 `USBHID_TRACE` 同时控制 usbkbd、usbmouse 和 kbd handler（旧名 `USBKBD_TRACE` 仍可用），改值后直接 `make`，相关目标会自动重编：

| 值 | 打印 |
| --- | --- |
| 0（默认） | 只有 probe 结果、第一个报告和错误 |
| 1 | + probe 每一步、每个报告、按键/按钮按下松开、LED |
| 2 | + 每个 URB 完成和重新提交、kbd 送往控制台的字符 |

无线键鼠接收器（一个 USB 设备上接口 0 是键盘、接口 1 是鼠标）：EHCI 枚举给每个 HID 接口建一个 usb_device（共用地址和 EP0），所以 usbkbd 和 usbmouse 分别绑定，日志里多一行 `ehci:   interface 1 class 3/1/2 intr in 2 ...`。如果这一行是 `class 3/0/0`，说明鼠标接口不支持 boot 协议，usbmouse 不会绑定。

限制：键盘必须插在板载 Hub 的口上（根口上的全/低速设备归 OHCI，还没有驱动）；不处理热插拔（EHCI 只在启动时枚举）；中断端点固定每 1 ms 轮询，不按 bInterval。

## USB 鼠标（usbmouse）

加载与数据通信图（进程/中断上下文、0–12 号消息、锁、报告和消息格式按位展开）：[images/usbmouse-comm-steps.svg](images/usbmouse-comm-steps.svg)

![usbmouse 通信图](images/usbmouse-comm-steps.png)

`kernel/drivers/hid/usbhid/usbmouse.c` 绑定 HID 3/1/2（boot 鼠标），传输路径与键盘完全相同（EHCI 周期调度的中断 URB → IRQ 55 → `usbmouse_wq` → input 核心 → 重新提交）。

| 报告字节 | 含义 | 上报 |
| --- | --- | --- |
| byte0 | b0 左 / b1 右 / b2 中 / b3 侧 / b4 额外 | `EV_KEY BTN_LEFT..BTN_EXTRA`（0x110..0x114） |
| byte1 | X，有符号 8 位，向右为正 | `EV_REL REL_X` |
| byte2 | Y，有符号 8 位，向下为正 | `EV_REL REL_Y` |
| byte3（报告 ≥ 4 字节时） | 滚轮，有符号 | `EV_REL REL_WHEEL` |

input 核心为此增加了 `EV_REL`、`input_report_rel()`，`KEY_MAX` 扩到 0x2ff 以容纳 `BTN_*`。鼠标是下一个 `/dev/input/eventN`（键盘之后通常是 event2），`evtest /dev/input/event2` 可看到 `REL_X/REL_Y`。kbd handler 不连接鼠标（没有字母键）。

### win32k 光标

有窗口时（运行 `windemo`），win32k 处理鼠标：

* 光标是 12×19 的箭头，不进 shadow；移动时只用 shadow 恢复旧位置、在新位置重画，不重组整个屏幕。
* 左键按在带标题栏窗口上：置顶并激活（两个窗口都重绘标题栏颜色）；按在标题栏上拖动窗口，按在红色 x 上发 `WM_CLOSE`；按在客户区发 `WM_LBUTTONDOWN`。
* 无标题栏窗口（windemo 的桌面、任务栏）只收消息，不会被置顶盖住其他窗口。
* 用户态消息：`WM_MOUSEMOVE / WM_[LRM]BUTTONDOWN/UP / WM_MOUSEWHEEL`，`lParam` 为客户区坐标（`GET_X_LPARAM/GET_Y_LPARAM`），按下的窗口获得捕获，松开消息发给它。
* 一批事件只合成一次、只发一个 `WM_MOUSEMOVE`；没有窗口时鼠标事件被忽略（控制台不受影响）。

限制：只看设备的第一个接口，所以键鼠一体的无线接收器只会识别成键盘；其余同键盘（插板载 Hub、无热插拔、每 1 ms 轮询）。
