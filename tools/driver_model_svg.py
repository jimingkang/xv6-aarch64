#!/usr/bin/env python3
"""xv6 Tiny210 driver model (kernel/device.c) diagrams.

  driver-model-overview.svg  boot order, the four "buses" (platform, usb,
                             input handlers, net/chrdev), which device binds
                             to which driver, and what bind() does
  driver-model-flow.svg      sequence: bus init -> driver/device registration
                             -> match -> probe -> enumeration -> URB data
                             path, each step with its real trace line

All names and trace lines come from a QEMU run with
  make -f ports/tiny210/Makefile BUILD=build/q QEMU_TEST=1 QEMU_USB=1 \\
       DRIVER_CORE_TRACE=2 USB_CORE_TRACE=1 USB_ENUM_TRACE=1 USBHID_TRACE=2
  qemu-system-arm -M cubieboard,usb=on ... -device usb-kbd,bus=usb-bus.0
kept in tools/manim/qemu-driver-model-trace.log.

Regenerate:
  python3 tools/driver_model_svg.py doc/readme/tiny210/images
  rsvg-convert -z 1.5 doc/readme/tiny210/images/driver-model-overview.svg -o doc/readme/tiny210/images/driver-model-overview.png
  rsvg-convert -z 1.5 doc/readme/tiny210/images/driver-model-flow.svg -o doc/readme/tiny210/images/driver-model-flow.png
"""
import sys
from pathlib import Path
from xml.sax.saxutils import escape as esc

FONT = "'PingFang SC','Noto Sans CJK SC','WenQuanYi Zen Hei','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono','DejaVu Sans Mono',Consolas,monospace"

# fill, stroke
C = {
    "core":     ("#fff3cd", "#b7950b"),
    "platform": ("#dbe8fb", "#1f5fae"),
    "usb":      ("#d9f2df", "#1e8449"),
    "input":    ("#fde9cf", "#d35400"),
    "net":      ("#e3d7f5", "#6c3fb5"),
    "chr":      ("#f6e0f0", "#a93d8c"),
    "hw":       ("#fde2e1", "#c0392b"),
    "grey":     ("#f2f2f2", "#8a8a8a"),
    "init":     ("#eef2f7", "#5d6d7e"),
}


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.out = w, h, []

    def add(self, s):
        self.out.append(s)

    def rect(self, x, y, w, h, kind="grey", rx=8, sw=1.6, dash=None, fill=None):
        f, s = C[kind]
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" '
                 f'fill="{fill or f}" stroke="{s}" stroke-width="{sw}"{d}/>')

    def text(self, x, y, s, size=14, anchor="start", weight="normal",
             color="#1d2733", mono=False, italic=False, halo=False):
        fam = MONO if mono else FONT
        st = ' font-style="italic"' if italic else ""
        if halo:
            st += ' stroke="#ffffff" stroke-width="4" paint-order="stroke" stroke-linejoin="round"'
        self.add(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" '
                 f'text-anchor="{anchor}" font-weight="{weight}" fill="{color}"{st}>'
                 f'{esc(s)}</text>')

    def lines(self, x, y, items, size=13, gap=None, **kw):
        gap = gap or size + 5
        for i, s in enumerate(items):
            self.text(x, y + i * gap, s, size=size, **kw)

    def arrow(self, x1, y1, x2, y2, color="#34495e", sw=1.8, dash=None, head=True):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        m = f' marker-end="url(#ah{color[1:]})"' if head else ""
        self.markers.add(color)
        self.add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
                 f'stroke-width="{sw}"{d}{m}/>')

    def path(self, d, color="#34495e", sw=1.8, dash=None, head=True):
        da = f' stroke-dasharray="{dash}"' if dash else ""
        m = f' marker-end="url(#ah{color[1:]})"' if head else ""
        self.markers.add(color)
        self.add(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}"{da}{m}/>')

    markers = set()

    def badge(self, x, y, n, color="#c0392b"):
        self.add(f'<circle cx="{x}" cy="{y}" r="12" fill="{color}"/>')
        self.text(x, y + 5, str(n), size=13, anchor="middle", weight="bold", color="#fff")

    def save(self, path):
        defs = "".join(
            f'<marker id="ah{c[1:]}" markerWidth="10" markerHeight="8" refX="9" refY="4" '
            f'orient="auto"><path d="M0,0 L10,4 L0,8 z" fill="{c}"/></marker>'
            for c in sorted(self.markers))
        body = "\n".join(self.out)
        Path(path).write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}">\n<defs>{defs}</defs>\n'
            f'<rect width="100%" height="100%" fill="#ffffff"/>\n{body}\n</svg>\n',
            encoding="utf-8")


# --------------------------------------------------------------------------
def overview(path):
    W, H = 1760, 1350
    s = Svg(W, H)
    s.markers = set()
    s.text(W / 2, 40, "xv6 Tiny210 驱动模型：总线 · 设备 · 驱动 · match · probe",
           size=26, anchor="middle", weight="bold")
    s.text(W / 2, 66, "kernel/device.c 的 device_core 是唯一的注册表；platform / usb / net 是 bus_type，"
           "input 用同样思路的 handler->match()；QEMU cubieboard 实测（DRIVER_CORE_TRACE=2）",
           size=14, anchor="middle", color="#5d6d7e")

    # ---- 1. boot timeline ------------------------------------------------
    y0 = 92
    s.text(30, y0 + 16, "① 启动顺序（setup.c main → do_initcalls 按 level, order 稳定排序）", size=17, weight="bold")
    steps = [
        ("device_init()", "bus_register(platform)", "core"),
        ("consoleinit()", "console 设备+驱动", "platform"),
        ("netinit()", "bus_register(net)", "net"),
        ("s5pv210_register_\ndevices()", "board_devices[] →\nplatform_device_register", "platform"),
        ("L1 core / L3 arch", "clk · wdt · irq-vic\n(QEMU: irq-sun4i)", "init"),
        ("L4 subsys", "input · timer ·\nusbcore→bus_register(usb)", "init"),
        ("L5 fs", "kbd handler\ninput_register_handler", "input"),
        ("L6 device o0/o1", "serial · fb · usbkbd\nusbmouse · mt7601u", "init"),
        ("L6 device o2", "ehci-s5p / dwc2\n(主机控制器最后)", "usb"),
        ("scheduler()", "ehci-enum kthread\n枚举 → usb 设备", "usb"),
    ]
    bw, gap, bx = 160, 13, 30
    for i, (a, b, k) in enumerate(steps):
        x = bx + i * (bw + gap)
        s.rect(x, y0 + 30, bw, 84, k)
        for j, ln in enumerate(a.split("\n")):
            s.text(x + bw / 2, y0 + 52 + j * 17, ln, size=14, anchor="middle", weight="bold", mono=True)
        off = 52 + 17 * len(a.split("\n"))
        for j, ln in enumerate(b.split("\n")):
            s.text(x + bw / 2, y0 + off + 2 + j * 16, ln, size=12, anchor="middle", color="#34495e")
        if i:
            s.arrow(x - gap, y0 + 72, x - 1, y0 + 72, "#5d6d7e", sw=1.5)

    # ---- 2. device_core --------------------------------------------------
    cy = 240
    s.rect(30, cy, W - 60, 62, "core")
    s.text(48, cy + 26, "device_core (kernel/device.c)", size=17, weight="bold", mono=True)
    s.text(48, cy + 49, "buses[]: platform, net, usb   devices[]: 全部 struct device (任一总线)   "
           "drivers[]: 全部 struct device_driver   chrdevs[NDEV]: major → file_operations",
           size=14, mono=True, color="#5a4a00")
    s.text(W - 50, cy + 26, "device_register() / driver_register() 都会立刻扫描另一侧 → bind()",
           size=14, anchor="end", color="#7d6608", weight="bold")

    # ---- 3. bus panels ---------------------------------------------------
    py, ph = 330, 560
    pw, pg = 410, 20
    panels = [
        ("platform", "platform_bus", "match: strcmp(dev->name, drv->name)",
         ["console", "sun4i-intc", "sun4i-timer", "sun4i-uart", "simple-framebuffer", "s5p-ehci"],
         ["console", "sun4i-intc  L3", "sun4i-timer  L4", "sun4i-uart  L6", "simple-framebuffer  L6/1",
          "s5p-ehci  L6/2"],
         [(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5)],
         ["板级 devices.c 提供 name + resource[] (MEM/IRQ)",
          "probe(dev): 取 resource[0].start 映射寄存器，",
          "request_irq(resource[1])，register_chrdev，",
          "或者像 ehci_probe 那样启动子总线 (kthread)",
          "Tiny210 实板: s5pv210-clock/-vic/-pwm/-uart,",
          "s3c2410-wdt, s5pv210-keys, s3c-sdhci, s3c-fb"]),
        ("usb", "usb_bus", "match: id_table (VID:PID 或 class/sub/proto)",
         ["usb-device 627:1  3/1/1", "usb-device (鼠标)  3/1/2", "usb-device 148f:7601 (板)"],
         ["usbkbd  {3,1,1}", "usbmouse  {3,1,2}", "mt7601u  {148f:7601}"],
         [(0, 0), (1, 1), (2, 2)],
         ["设备不是静态表：由 ehci-enum 线程枚举出来",
          "(GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIG)",
          "后 usb_device_register(u) 注册；驱动更早注册",
          "并在 drivers[] 里等待 (order 1 < ehci order 2)",
          "usb_probe(): container_of → usb_driver.probe",
          "probe 内: SET_PROTOCOL, input_register_device,",
          "usb_fill_int_urb + usb_submit_urb"]),
        ("input", "input core (类总线)", "match: handler->match(input_dev)",
         ["USB HID Boot Keyboard  event0", "s5pv210-keys  (实板 K1-K8)", "USB 鼠标 eventN"],
         ["kbd  (有 KEY_A 和 ENTER)", "evdev  /dev/input/eventN", "win32k 钩子 (弱符号)"],
         [(0, 0), (0, 1), (1, 1), (2, 1), (0, 2), (2, 2)],
         ["不是 bus_type，但同一模式：",
          "input_register_device() 扫 handlers[]，",
          "input_register_handler() 扫 devices[]，",
          "match 成功 → input_connect_locked()",
          "之后 input_report_key → 每个 handler->event",
          "+ 每个打开 eventN 的 evdev client 缓冲"]),
        ("net", "net_bus + chrdev 表", "net_bus 没有 match()：只登记，不绑定",
         ["wlan0 (mt7601u)", "", "", ""],
         ["chrdev 1 console", "chrdev 2 tty · 3 ttyS0", "chrdev 4 input/event", "chrdev 5 fb0"],
         [],
         ["register_netdev(): dev.bus=&net_bus →",
          "device_register()，日志: \"class device, never bound\"",
          "open(\"/dev/xxx\") → inode major → chrdev_open",
          "read/write/ioctl/mmap → fops，用户态从这里",
          "拿到驱动的数据 (console, evdev, fb0)",
          "/proc/devices 由 device_format() 生成"]),
    ]
    for i, (k, name, rule, devs, drvs, links, notes) in enumerate(panels):
        x = 30 + i * (pw + pg)
        s.rect(x, py, pw, ph, k, rx=12, sw=2, fill="#ffffff")
        s.rect(x, py, pw, 62, k, rx=12, sw=2)
        s.text(x + 16, py + 26, name, size=18, weight="bold", mono=True)
        s.text(x + 16, py + 50, rule, size=13, color="#2c3e50", mono=True)
        s.text(x + 18, py + 90, "devices[] (bus 上)", size=13, weight="bold", color=C[k][1])
        s.text(x + pw - 18, py + 90, "drivers[] / handlers", size=13, weight="bold",
               color=C[k][1], anchor="end")
        rowh = 40 if len(devs) > 3 else 52
        dy = py + 104
        for j, d in enumerate(devs):
            if d:
                s.rect(x + 12, dy + j * rowh, 176, rowh - 10, k, rx=6, sw=1.2)
                s.text(x + 20, dy + j * rowh + rowh / 2 - 1, d, size=11.5, mono=True)
        for j, d in enumerate(drvs):
            s.rect(x + pw - 188, dy + j * rowh, 176, rowh - 10, k, rx=6, sw=1.2)
            s.text(x + pw - 180, dy + j * rowh + rowh / 2 - 1, d, size=11.5, mono=True)
        for a, b in links:
            s.arrow(x + 188, dy + a * rowh + (rowh - 10) / 2, x + pw - 190,
                    dy + b * rowh + (rowh - 10) / 2, "#1e8449", sw=1.6)
        ny = dy + max(len(devs), len(drvs)) * rowh + 22
        s.add(f'<line x1="{x+12}" y1="{ny-12}" x2="{x+pw-12}" y2="{ny-12}" stroke="#d0d7de"/>')
        s.text(x + 16, ny + 6, "match 之后 / 说明", size=13, weight="bold", color=C[k][1])
        s.lines(x + 16, ny + 28, notes, size=12.5, gap=21)
    # cross-bus links: ehci probe spawns usb devices; usbkbd probe makes input_dev
    x_p, x_u, x_i = 30, 30 + pw + pg, 30 + 2 * (pw + pg)
    s.path(f"M{x_p+pw-100},{py+104+5*40+30} C{x_p+pw+40},{py+104+5*40+80} {x_u-40},{py+104+52} {x_u+12},{py+104+20}",
           "#c0392b", sw=2.2, dash="6 4")
    s.path(f"M{x_u+pw-12},{py+104+20} C{x_u+pw+30},{py+104+20} {x_i-30},{py+104+20} {x_i+12},{py+104+20}",
           "#d35400", sw=2.2, dash="6 4")
    ly = py + ph + 22
    s.add(f'<line x1="40" y1="{ly-5}" x2="80" y2="{ly-5}" stroke="#c0392b" stroke-width="2.2" stroke-dasharray="6 4"/>')
    s.text(88, ly, "跨总线：s5p-ehci 的 probe 启动 ehci-enum 线程，它枚举出的设备用 usb_device_register() 注册到 usb_bus（平台总线 → USB 总线）",
           size=13, color="#c0392b", weight="bold")
    s.add(f'<line x1="40" y1="{ly+17}" x2="80" y2="{ly+17}" stroke="#d35400" stroke-width="2.2" stroke-dasharray="6 4"/>')
    s.text(88, ly + 22, "跨子系统：usbkbd_probe() 里 input_register_device() 把键盘交给 input core（USB 总线 → input handler）",
           size=13, color="#d35400", weight="bold")

    # ---- 4. bind() flow --------------------------------------------------
    by = 985
    s.text(30, by, "② bind(dev, drv, why)：两种注册路径共用的匹配 / probe 逻辑", size=17, weight="bold")
    flow = [
        ("device_register(dev)\n或 driver_register(drv)", "core", "加入 devices[]/drivers[]\n然后扫描另一侧"),
        ("dev->driver 已有?\ndrv->bus != dev->bus?", "grey", "是 → 跳过 (不同总线\n的组合不尝试)"),
        ("bus->match(dev, drv)", "platform", "platform: 比名字\nusb: id_table\nDM_LOG2: no match"),
        ("dev->driver = drv", "usb", "DM_LOG: match ...\n(from device_/\ndriver_register)"),
        ("bus->probe(dev)", "usb", "platform: bus_probe\nusb: usb_probe\n→ container_of"),
        ("drv->probe(dev)", "input", "拿 resource、request_irq\nregister_chrdev / 注册\n子设备 / 提交 URB"),
        ("r < 0 ?", "hw", "失败: dev->driver = 0\n(总是打印)\n成功: -> bound"),
    ]
    fw, fg = 218, 26
    for i, (a, k, b) in enumerate(flow):
        x = 30 + i * (fw + fg)
        s.rect(x, by + 20, fw, 66, k)
        for j, ln in enumerate(a.split("\n")):
            s.text(x + fw / 2, by + 46 + j * 19 - (9 if len(a.split("\n")) > 1 else 0) + 6, ln,
                   size=13.5, anchor="middle", weight="bold", mono=True)
        for j, ln in enumerate(b.split("\n")):
            s.text(x + fw / 2, by + 110 + j * 18, ln, size=12.5, anchor="middle", color="#34495e")
        if i:
            s.arrow(x - fg, by + 53, x - 2, by + 53, "#34495e")
    # key facts
    ky = 1175
    s.rect(30, ky, W - 60, 150, "init", rx=10)
    facts = [
        "• 先后顺序无关：设备先到 → driver_register 时补 match（platform 设备都是这样）；驱动先到 → device_register 时补 match（usbkbd 等 USB 设备是这样）。",
        "• 一个设备只绑定一个驱动（device_register 里 bind 成功就 break）；一个驱动可以绑定多个设备（driver_register 扫完所有设备）。",
        "• probe 返回 <0 → 解绑，设备留在 devices[] 里等下一个驱动；device_unregister / driver_unregister → bus->remove → drv->remove。",
        "• 打开日志：make -f ports/tiny210/Makefile DRIVER_CORE_TRACE=1（注册/match/probe/chrdev）或 =2（再加每次 no match 和 chrdev open）；",
        "  USB 侧再加 USB_CORE_TRACE=1 USB_ENUM_TRACE=1，数据路径加 USBHID_TRACE=2。改开关会自动重编受影响的 .o（.usb-wifi-trace）。",
    ]
    s.lines(48, ky + 30, facts, size=14, gap=27)
    s.save(path)


# --------------------------------------------------------------------------
def flow(path):
    lanes = [
        ("setup.c\ninitcall", "init"),
        ("device core\ndevice.c", "core"),
        ("EHCI 主机驱动\n(platform drv)", "platform"),
        ("ehci-enum\nkthread", "platform"),
        ("usb core\nusb.c", "usb"),
        ("usbkbd\n类驱动", "usb"),
        ("input core\n+ kbd handler", "input"),
        ("用户进程\nsh / evtest", "chr"),
    ]
    LX0, LW = 120, 190
    W = LX0 + LW * len(lanes) + 30
    lx = [LX0 + LW * i + LW / 2 for i in range(len(lanes))]
    # (phase) or (from, to, label, log)
    seq = [
        ("A  总线初始化",),
        (0, 1, "device_init(): bus_register(&platform_bus)", "dm: bus=platform registered by device_init() (match = name)"),
        (0, 1, "netinit(): bus_register(&net_bus)", "dm: register bus=net"),
        (0, 1, "s5pv210_register_devices(): platform_device_register ×N", "dm: register device=s5p-ehci.0 bus=platform nres=2; scan 1 driver(s)"),
        (1, 1, "暂无同名驱动 → 设备挂在 devices[] 等待", "dm:   device=s5p-ehci waits unbound for its driver"),
        (0, 4, "L4 subsys_initcall(usbcore): usb_bus_init → bus_register(&usb_bus)", "dm: register bus=usb"),
        (0, 6, "L4 input_init: register_chrdev(4)；L5 kbd: input_register_handler", "input: handler kbd registered"),
        ("B  驱动注册 → match → probe",),
        (0, 5, "L6 order1: usbkbd_driver_init → usb_register_driver", "dm: register driver=usbkbd bus=usb; scan 6 device(s)"),
        (5, 1, "usb 总线上还没有设备 → 驱动在 drivers[] 等待", "dm:   driver=usbkbd: no device yet, waits for device_register()"),
        (0, 1, "L6 order2: platform_driver_register(&ehci_driver)", "dm: match bus=platform dev=s5p-ehci.0 drv=s5p-ehci (from driver_register)"),
        (1, 2, "bus_probe → ehci_probe(dev): MMIO=resource[0], IRQ=resource[1]", "usb-enum: platform probe mmio=1c14000 irq=39"),
        (2, 3, "request_irq(39, ehci_irq)；kthread_create(ehci-enum)", "dm: probe end drv=s5p-ehci dev=s5p-ehci result=0 -> bound"),
        ("C  scheduler 之后：USB 设备枚举 → 第二次 match",),
        (3, 3, "ehci_start + root_ports: 复位端口，enumerate()", "usb-enum: interface=0 class=3/1/1 endpoints=1"),
        (3, 4, "usb_device_register(u)  (dev.bus = &usb_bus)", "usb-core: register device=627:1 addr=1 class=3/1/1"),
        (4, 1, "device_register → 扫 drivers[] → usb_match(id_table)", "dm: match bus=usb dev=usb-device.0 drv=usbkbd (from device_register)"),
        (1, 5, "bus->probe = usb_probe → usbkbd_probe(udev)", "usbkbd: SET_PROTOCOL(boot) ok"),
        (5, 6, "input_register_device → kbd_match 成功 → connect", "dm: input: connect device=USB HID Boot Keyboard -> handler=kbd"),
        (5, 2, "usb_fill_int_urb + usb_submit_urb → ehci_submit_urb (周期 QH)", "usbkbd: 627:1 keyboard on ep1 in, mps 8, interval 7"),
        ("D  数据传输：一次按键 'l'",),
        (2, 2, "EHCI 轮询中断端点 → IOC → IRQ 39 → ehci_irq → ehci_intr_reap", "(USB_XFER_TRACE=1: usb-xfer: intr-complete addr=1 ep=1 len=8)"),
        (2, 4, "usb_hcd_giveback_urb(urb, 0, 8)", "URB 所有权交回类驱动"),
        (4, 5, "urb->complete = usbkbd_irq (中断上下文) → queue_work", "usbkbd: urb done status=0 len=8"),
        (5, 6, "usbkbd_rx_work: 比较报告 → input_report_key(KEY_L, 1)", "usbkbd:   usage 0xf -> KEY 38 down"),
        (6, 7, "kbd_event → console 输入 'l'；evdev client 缓冲 → read(event0)", "kbd: KEY 38 -> console 6c mods=0 caps=0"),
        (5, 2, "usb_submit_urb(urb) 再次提交，等待下一个报告", "usbkbd: urb resubmitted ep1 len 8"),
    ]
    top, step, phase_h = 150, 66, 44
    H = top + sum(phase_h if len(e) == 1 else step for e in seq) + 50
    s = Svg(W, H)
    s.markers = set()
    s.text(W / 2, 40, "从总线初始化到一次按键：xv6 Tiny210 驱动模型时序", size=26, anchor="middle", weight="bold")
    s.text(W / 2, 66, "每一步下方绿色等宽字是 QEMU 实测日志 (tools/manim/qemu-driver-model-trace.log)",
           size=14, anchor="middle", color="#5d6d7e")
    for i, (name, k) in enumerate(lanes):
        s.rect(lx[i] - 82, 86, 164, 52, k)
        for j, ln in enumerate(name.split("\n")):
            s.text(lx[i], 107 + j * 19, ln, size=14, anchor="middle", weight="bold")
        s.add(f'<line x1="{lx[i]}" y1="138" x2="{lx[i]}" y2="{H-30}" stroke="{C[k][1]}" '
              f'stroke-width="1.3" stroke-dasharray="5 5" opacity="0.6"/>')
    y = top
    n = 0
    for e in seq:
        if len(e) == 1:
            s.add(f'<rect x="20" y="{y+6}" width="{W-40}" height="30" rx="6" fill="#2c3e50" opacity="0.9"/>')
            s.text(36, y + 27, e[0], size=16, weight="bold", color="#ffffff")
            y += phase_h
            continue
        n += 1
        a, b, label, log = e
        yy = y + 22
        s.badge(40, yy, n)
        if a == b:
            x = lx[a]
            s.path(f"M{x},{yy-6} h40 v16 h-38", "#34495e")
            tx, anchor = x + 50, "start"
        else:
            col = "#1e8449" if "match" in log else "#34495e"
            s.arrow(lx[a], yy, lx[b] + (-6 if b > a else 6), yy, col, sw=2)
            tx, anchor = (min(lx[a], lx[b]) + 10), "start"
            if min(a, b) >= 5:
                tx, anchor = max(lx[a], lx[b]) - 12, "end"
        if a == b and a >= 5:
            tx, anchor = lx[a] - 12, "end"
        s.text(tx, yy - 8, label, size=14, anchor=anchor, weight="bold", halo=True)
        s.text(tx, yy + 22, log, size=12.5, anchor=anchor, mono=True, color="#1e8449", halo=True)
        y += step
    s.save(path)


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    overview(out / "driver-model-overview.svg")
    flow(out / "driver-model-flow.svg")
    print("wrote", out / "driver-model-overview.svg", out / "driver-model-flow.svg")
