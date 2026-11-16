# USB 键盘驱动与输入子系统（Tiny210）

## 数据流

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

限制：键盘必须插在板载 Hub 的口上（根口上的全/低速设备归 OHCI，还没有驱动）；不处理热插拔（EHCI 只在启动时枚举）；中断端点固定每 1 ms 轮询，不按 bInterval。
