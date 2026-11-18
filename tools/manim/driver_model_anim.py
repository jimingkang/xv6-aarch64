"""xv6 Tiny210 driver model animation, driven by a real DRIVER_CORE_TRACE=2 log.

Scenes
  BusInit        device_init()/netinit()/consoleinit() and the board's
                 platform devices: buses[], devices[], drivers[] fill up,
                 devices wait unbound
  PlatformMatch  do_initcalls(): each platform_driver_register() scans
                 devices[], bus->match() compares names (red = no match,
                 green = match), bind() -> probe(); usbkbd/usbmouse wait on
                 the empty usb bus; ehci_probe takes MMIO/IRQ resources and
                 starts the ehci-enum kthread
  UsbMatch       ehci-enum enumerates the keyboard (control transfers),
                 usb_device_register() -> usb_match() against the id_table
                 field by field -> usb_probe -> usbkbd_probe: SET_PROTOCOL,
                 input_register_device (kbd handler connects), submit URB
  DataFlow       one key press: periodic QH -> IRQ 39 -> giveback ->
                 usbkbd_irq -> work -> input_report_key -> kbd handler /
                 evdev -> sh, and the URB is resubmitted

Every register/match/probe step and the bottom "console" line is taken from
the log, in log order.  Render (from the repository root):
  tools/manim/render_driver_model.sh [l|m|h]

The log comes from the QEMU stand-in with a USB keyboard and three key
presses ("ls\\n") sent from the QEMU monitor:
  make -f ports/tiny210/Makefile BUILD=build/q QEMU_TEST=1 QEMU_USB=1 \\
       DRIVER_CORE_TRACE=2 USB_CORE_TRACE=1 USB_ENUM_TRACE=1 USBHID_TRACE=2 MMU_TRACE=0
  qemu-system-arm -M cubieboard,usb=on -m 512M -display none \\
       -serial file:trace.log -kernel build/q/xv6-tiny210.uImage \\
       -device usb-kbd,bus=usb-bus.0      (monitor: sendkey l / sendkey s / sendkey ret)
and is kept in tools/manim/qemu-driver-model-trace.log (DRIVER_ANIM_LOG overrides).
"""
import os
import re
from pathlib import Path

from manim import (
    BLUE, DOWN, GREEN, GREY, GREY_B, GREY_D, LEFT, ORANGE, PURPLE, RED,
    RIGHT, UP, WHITE, YELLOW, Arrow, Create, Cross, Dot, FadeIn, FadeOut,
    GrowArrow, Indicate, Line, ReplacementTransform, RoundedRectangle,
    Scene, SurroundingRectangle, Text, Transform, VGroup, Write, config,
    LaggedStart, MoveAlongPath, CurvedArrow, Circumscribe,
)

CJK = os.environ.get("ANIM_CJK", "Hiragino Sans GB")
MONO = os.environ.get("ANIM_MONO", "Menlo")
config.background_color = "#14171c"

LOG = Path(os.environ.get("DRIVER_ANIM_LOG",
                          Path(__file__).with_name("qemu-driver-model-trace.log")))
LINES = [l.rstrip() for l in LOG.read_text(errors="replace").splitlines()]

BUSCOL = {"platform": BLUE, "usb": GREEN, "net": PURPLE, "input": ORANGE}


def T(s, size=24, color=WHITE, mono=False, **kw):
    return Text(s, font=MONO if mono else CJK, font_size=size, color=color, **kw)


def fit(m, w):
    if m.width > w:
        m.scale_to_fit_width(w)
    return m


def find(pattern, default=None):
    rx = re.compile(pattern)
    for line in LINES:
        m = rx.search(line)
        if m:
            return m
    if default is None:
        raise SystemExit(f"driver_model_anim: '{pattern}' not in {LOG}")
    return default


# ---------------------------------------------------------------- log events
EV = [
    ("bus", re.compile(r"dm: bus=(\w+) registered by device_init")),
    ("bus", re.compile(r"dm: register bus=(\w+)")),
    ("dev", re.compile(r"dm: register device=([\w-]+)\.(\d+) bus=(\w+) nres=(\d+)")),
    ("drv", re.compile(r"dm: register driver=([\w-]+) bus=(\w+)")),
    ("wait_dev", re.compile(r"dm:   device=([\w-]+) waits unbound")),
    ("wait_drv", re.compile(r"dm:   driver=([\w-]+): no device yet")),
    ("nomatch", re.compile(r"dm:   try bus=(\w+) dev=([\w-]+) drv=([\w-]+): no match")),
    ("match", re.compile(r"dm: match bus=(\w+) dev=([\w-]+)\.\d+ drv=([\w-]+) \(from (\w+)\)")),
    ("bound", re.compile(r"dm: probe end drv=([\w-]+) dev=([\w-]+) result=(-?\d+)")),
    ("chrdev", re.compile(r"dm: register chrdev major=(\d+) name=(\S+)")),
    ("initcall", re.compile(r"initcall: (\S+) \(level (\d)\)")),
    ("connect", re.compile(r"dm: input: connect device=(.+) -> handler=(\w+)")),
]


def events(start=None, stop=None):
    """Driver-model events between the first line matching start (inclusive)
    and the first line matching stop (exclusive)."""
    out, on = [], start is None
    for line in LINES:
        if not on and start and re.search(start, line):
            on = True
        if on and stop and re.search(stop, line):
            break
        if not on:
            continue
        for kind, rx in EV:
            m = rx.search(line)
            if m:
                out.append((kind, m.groups(), line))
                break
    return out


FIRST_INITCALL = r"^initcall: "
ENUM_START = r"usb-enum: ehci-enum kthread entered"
FIRST_KEY = r"usbkbd: urb done"


# ---------------------------------------------------------------- helpers
def title(scene, s, sub=None):
    t = fit(T(s, 32).to_edge(UP, buff=0.18), 13.4)
    grp = VGroup(t)
    if sub:
        grp.add(fit(T(sub, 16, GREY_B).next_to(t, DOWN, buff=0.08), 13.4))
    scene.play(FadeIn(grp, shift=DOWN * 0.2), run_time=0.7)
    return grp


class Ticker:
    """Bottom line: the trace line the current step printed."""

    def __init__(self, scene):
        self.scene, self.m = scene, None
        self.frame = RoundedRectangle(corner_radius=0.08, width=13.8, height=0.46,
                                      stroke_color=GREY_D, fill_color="#0b0d10",
                                      fill_opacity=1).to_edge(DOWN, buff=0.12)
        self.tag = T("console$", 14, GREY, mono=True).move_to(self.frame).align_to(self.frame, LEFT).shift(RIGHT * 0.12)
        scene.add(self.frame, self.tag)

    def show(self, line, color=GREEN, rt=0.25):
        # Console echo can glue a typed character to the front of a trace line.
        m = re.search(r"(dm:|usbkbd:|kbd:|usb-\w+:|input:|initcall:|ehci:)", line)
        if m:
            line = line[m.start():]
        new = fit(T(line.strip() or " ", 15, color, mono=True), 12.3)
        new.next_to(self.tag, RIGHT, buff=0.15)
        if self.m is None:
            self.scene.play(FadeIn(new), run_time=rt)
        else:
            self.scene.play(ReplacementTransform(self.m, new), run_time=rt)
        self.m = new


def note(scene, old, s, color=YELLOW, pos=None):
    new = fit(T(s, 19, color), 13.4)
    new.move_to(pos if pos is not None else [0, -2.95, 0])
    if old is None:
        scene.play(FadeIn(new), run_time=0.4)
    else:
        scene.play(ReplacementTransform(old, new), run_time=0.4)
    return new


def slot(label, color, w=3.2, h=0.38, size=15, dim=False):
    r = RoundedRectangle(corner_radius=0.07, width=w, height=h, stroke_color=color,
                         stroke_width=2, fill_color=color,
                         fill_opacity=0.04 if dim else 0.45,
                         stroke_opacity=0.45 if dim else 1)
    t = fit(T(label, size, WHITE, mono=True), w - 0.2).move_to(r)
    return VGroup(r, t)


class Registry:
    """buses[] / devices[] / drivers[] columns and chrdev row of device_core."""

    XB, XD, XR = -5.6, -1.55, 3.6
    TOP = 2.15
    STEP = 0.44

    def __init__(self, scene):
        self.s = scene
        self.buses, self.devs, self.drvs, self.chr = {}, {}, {}, {}
        self.dev_bus, self.drv_bus = {}, {}
        self.links = {}
        hdr = VGroup(
            T("buses[]", 18, YELLOW, mono=True).move_to([self.XB, self.TOP + 0.45, 0]),
            T("devices[]", 18, YELLOW, mono=True).move_to([self.XD, self.TOP + 0.45, 0]),
            T("drivers[]", 18, YELLOW, mono=True).move_to([self.XR, self.TOP + 0.45, 0]),
        )
        box = RoundedRectangle(corner_radius=0.15, width=13.6, height=5.25,
                               stroke_color=YELLOW, stroke_width=1.5).move_to([0, 0.18, 0])
        lab = T("device_core  (kernel/device.c)", 14, YELLOW, mono=True)
        lab.move_to(box.get_corner(DOWN + RIGHT) + LEFT * (lab.width / 2 + 0.15) + UP * 0.2)
        self.chr_lab = T("chrdevs[]", 15, YELLOW, mono=True).move_to([self.XB, -2.15, 0])
        self.frame = VGroup(box, lab, hdr, self.chr_lab)

    def pos(self, col, i):
        return [col, self.TOP - i * self.STEP, 0]

    def draw(self, animate=True):
        if animate:
            self.s.play(Create(self.frame[0]), FadeIn(self.frame[1:]), run_time=0.8)
        else:
            self.s.add(self.frame)

    # Each apply_* returns animations (or adds instantly when fast).
    def bus(self, name, fast=False):
        m = slot(name, BUSCOL.get(name, GREY), w=2.2).move_to(self.pos(self.XB, len(self.buses)))
        self.buses[name] = m
        return self._show(m, fast)

    def dev(self, name, bus, fast=False):
        m = slot(name, BUSCOL.get(bus, GREY), dim=True).move_to(self.pos(self.XD, len(self.devs)))
        self.devs[name], self.dev_bus[name] = m, bus
        return self._show(m, fast)

    def drv(self, name, bus, fast=False):
        m = slot(name, BUSCOL.get(bus, GREY), dim=True).move_to(self.pos(self.XR, len(self.drvs)))
        self.drvs[name], self.drv_bus[name] = m, bus
        return self._show(m, fast)

    def chrdev(self, major, name, fast=False):
        x = -4.1 + len(self.chr) * 1.85
        m = slot(f"{major} {name}", PURPLE, w=1.75, h=0.34, size=13).move_to([x, -2.15, 0])
        self.chr[major] = m
        return self._show(m, fast)

    def _show(self, m, fast):
        if fast:
            self.s.add(m)
            return None
        self.s.play(FadeIn(m, shift=RIGHT * 0.2), run_time=0.35)
        return m

    def try_pair(self, dev, drv, ok, rt=0.25):
        a, b = self.devs[dev], self.drvs[drv]
        ln = Line(a.get_right(), b.get_left(), stroke_width=3,
                  color=GREEN if ok else RED)
        if not ok:
            x = Cross(scale_factor=0.12, stroke_color=RED).move_to(ln.get_center())
            self.s.play(Create(ln), FadeIn(x), run_time=rt)
            self.s.play(FadeOut(ln), FadeOut(x), run_time=rt * 0.6)
            return None
        self.s.play(Create(ln), run_time=0.4)
        self.links[dev] = ln
        return ln

    def bind_fast(self, dev, drv):
        a, b = self.devs[dev], self.drvs[drv]
        ln = Line(a.get_right(), b.get_left(), stroke_width=3, color=GREEN)
        self.s.add(ln)
        self.links[dev] = ln
        for m in (a, b):
            m[0].set_fill(opacity=0.45).set_stroke(opacity=1)

    def light(self, name_dev, name_drv):
        a, b = self.devs[name_dev], self.drvs[name_drv]
        self.s.play(a[0].animate.set_fill(opacity=0.45).set_stroke(opacity=1),
                    b[0].animate.set_fill(opacity=0.45).set_stroke(opacity=1), run_time=0.3)


def replay(reg, evs, scene, ticker=None, fast=False, nomatch_rt=0.2, hook=None):
    """Apply driver-model events to the registry (animated unless fast)."""
    for kind, g, line in evs:
        if hook and hook(kind, g, line) == "skip":
            continue
        if kind == "bus":
            reg.bus(g[0], fast)
        elif kind == "dev":
            reg.dev(g[0], g[2], fast)
        elif kind == "drv":
            reg.drv(g[0], g[1], fast)
        elif kind == "chrdev":
            reg.chrdev(g[0], g[1], fast)
        elif kind == "match":
            if fast:
                reg.bind_fast(g[1], g[2])
            else:
                reg.try_pair(g[1], g[2], True)
        elif kind == "nomatch" and not fast:
            reg.try_pair(g[1], g[2], False, rt=nomatch_rt)
        elif kind == "bound" and not fast:
            reg.light(g[1], g[0])
        if ticker and not fast and kind not in ("nomatch",):
            ticker.show(line, rt=0.2)
        elif ticker and not fast and kind == "nomatch":
            ticker.show(line, color=RED, rt=0.1)


# ---------------------------------------------------------------- scene 1
class BusInit(Scene):
    def construct(self):
        tt = title(self, "① 总线初始化：device_core、platform / net 总线与板级设备",
                   "setup.c main(): device_init() → consoleinit() → netinit() → s5pv210_register_devices()（QEMU 实测日志）")
        reg = Registry(self)
        reg.draw()
        tk = Ticker(self)
        evs = events(stop=FIRST_INITCALL)
        n = None
        step_notes = {
            "platform": "device_init()：清空静态 device_core，bus_register(&platform_bus)，match = 比较名字",
            "console": "consoleinit()：console 设备和驱动都挂在 platform 总线上，后注册的一方触发 bind()",
            "net": "netinit()：bus_register(&net_bus)，它没有 match()，网卡只登记不绑定",
            "sun4i-intc": "s5pv210_register_devices()：board_devices[] 每项一个 platform 设备（name + MEM/IRQ resource）",
        }
        for ev in evs:
            kind, g, line = ev
            key = g[0] if kind in ("bus", "dev") else None
            if key in step_notes:
                n = note(self, n, step_notes.pop(key))
            replay(reg, [ev], self, tk)
        n = note(self, n, "此时驱动大多还没注册：设备在 devices[] 里以“未绑定”（半透明）状态等待", GREEN)
        self.wait(1.5)


# ---------------------------------------------------------------- scene 2
class PlatformMatch(Scene):
    def construct(self):
        title(self, "② do_initcalls()：驱动注册 → 扫描设备 → match → probe",
              "按 (level, order) 排序；红线 = bus->match() 失败，绿线 = 匹配成功并 probe")
        reg = Registry(self)
        reg.draw(animate=False)
        replay(reg, events(stop=FIRST_INITCALL), self, fast=True)
        tk = Ticker(self)
        ic = None
        n = None
        evs = events(start=FIRST_INITCALL, stop=ENUM_START)

        def hook(kind, g, line):
            nonlocal ic, n
            if kind == "initcall":
                new = fit(T(f"initcall: {g[0]}  level {g[1]}", 17, ORANGE, mono=True), 5)
                new.move_to([4.6, -2.62, 0])
                self.play(FadeIn(new) if ic is None else ReplacementTransform(ic, new), run_time=0.3)
                ic = new
                tk.show(line, ORANGE, rt=0.15)
                return "skip"
            if kind == "drv" and g[1] == "usb":
                n = note(self, n, f"{g[0]} 注册到 usb 总线：usb 上还没有设备 → 驱动先在 drivers[] 等待", GREEN)
            if kind == "wait_drv":
                tk.show(line, YELLOW, rt=0.2)
                return "skip"
            if kind == "match" and g[2] == "s5p-ehci":
                n = note(self, n, "s5p-ehci 名字匹配 → bus_probe() → ehci_probe(dev)", GREEN)
            if kind == "bound" and g[0] == "s5p-ehci":
                self.ehci_probe(reg, tk)
            return None

        replay(reg, evs, self, tk, hook=hook)
        n = note(self, n, "platform 设备全部绑定；usbkbd/usbmouse 留在 drivers[] 等 USB 设备出现", GREEN)
        self.wait(1.5)

    def ehci_probe(self, reg, tk):
        mmio = find(r"usb-enum: platform probe mmio=(\w+) irq=(\d+)")
        d = reg.devs["s5p-ehci"]
        res = VGroup(
            T("struct device s5p-ehci", 16, BLUE, mono=True),
            T(f"resource[0] MEM 0x{mmio.group(1)}", 15, WHITE, mono=True),
            T(f"resource[1] IRQ {mmio.group(2)}", 15, WHITE, mono=True),
            T("driver_data = ehci_platdata", 15, GREY_B, mono=True),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        act = VGroup(
            T("ehci_probe(dev):", 16, GREEN, mono=True),
            T("cap = resource[0].start", 15, WHITE, mono=True),
            T(f"request_irq({mmio.group(2)}, ehci_irq)", 15, WHITE, mono=True),
            T("kthread_create(ehci_enum_thread)", 15, YELLOW, mono=True),
            T("return 0 → bound", 15, GREEN, mono=True),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        card = VGroup(res, act).arrange(RIGHT, buff=0.5, aligned_edge=UP)
        bg = SurroundingRectangle(card, buff=0.2, color=BLUE, fill_color="#14171c", fill_opacity=0.95,
                                  corner_radius=0.1)
        grp = VGroup(bg, card).move_to([0.6, -0.4, 0])
        self.play(FadeIn(grp, shift=UP * 0.2), Indicate(d), run_time=0.6)
        tk.show(f"usb-enum: platform probe mmio={mmio.group(1)} irq={mmio.group(2)}", rt=0.2)
        self.play(LaggedStart(*[Indicate(l) for l in act[1:]], lag_ratio=0.5), run_time=2)
        tk.show("ehci: s5p-ehci at 1c14000, enumeration deferred to ehci-enum", rt=0.2)
        self.wait(0.8)
        self.play(FadeOut(grp), run_time=0.4)


# ---------------------------------------------------------------- scene 3
class UsbMatch(Scene):
    def construct(self):
        title(self, "③ USB 总线：枚举出设备 → usb_match(id_table) → usbkbd_probe",
              "scheduler() 之后 ehci-enum 线程运行；这次是“驱动先到、设备后到”，由 device_register() 触发 bind()")
        tk = Ticker(self)
        # left: enumeration control transfers
        hc = slot("ehci-enum kthread", BLUE, w=3.4, h=0.5, size=17).move_to([-5.0, 2.3, 0])
        self.play(FadeIn(hc), run_time=0.4)
        enum_lines = [l for l in LINES if l.startswith("usb-enum:") and
                      any(k in l for k in ("enumerate begin", "assigning address", "vid:pid",
                                           "configuration", "interface=", "endpoint addr"))]
        reqs = [
            ("GET_DESCRIPTOR(DEVICE, 8)", 0), ("SET_ADDRESS(1)", 1),
            ("GET_DESCRIPTOR(DEVICE, 18)", 2), ("GET_DESCRIPTOR(CONFIG, 9)", 3),
            ("GET_DESCRIPTOR(CONFIG, 34)", 4), ("SET_CONFIGURATION(1)", 5),
        ]
        ctrl = VGroup()
        for i, (r, _) in enumerate(reqs):
            m = T(r, 15, WHITE, mono=True).move_to([-5.0, 1.75 - i * 0.38, 0])
            ctrl.add(m)
        n = note(self, None, "enumerate()：在端点 0 上做控制传输，读出描述符并分配地址", YELLOW)
        for i, m in enumerate(ctrl):
            self.play(FadeIn(m, shift=RIGHT * 0.2), run_time=0.3)
            if i < len(enum_lines):
                tk.show(enum_lines[i], rt=0.15)
        vp = find(r"usb-enum: device addr=(\d+) vid:pid=(\w+):(\w+)")
        cls = find(r"usb-enum: interface=\d+ class=(\d+)/(\d+)/(\d+)")
        ep = find(r"usb-enum: endpoint addr=(\w+) ep=(\d+) dir=IN type=3 mps=(\d+) interval=(\d+)")
        fields = [
            ("vendor:product", f"{vp.group(2)}:{vp.group(3)}"),
            ("address", vp.group(1)),
            ("class", cls.group(1)), ("subclass", cls.group(2)), ("protocol", cls.group(3)),
            ("interrupt_in_ep", f"{ep.group(2)}  mps {ep.group(3)}"),
            ("ops", "&ehci_usb_ops"),
        ]
        rows = VGroup(*[VGroup(T(k, 15, GREY_B, mono=True), T(v, 15, WHITE, mono=True))
                        .arrange(RIGHT, buff=0.25) for k, v in fields]).arrange(DOWN, aligned_edge=LEFT, buff=0.07)
        hdr = T("struct usb_device", 17, GREEN, mono=True)
        card = VGroup(hdr, rows).arrange(DOWN, aligned_edge=LEFT, buff=0.12)
        cbg = SurroundingRectangle(card, buff=0.15, color=GREEN, corner_radius=0.1)
        ucard = VGroup(cbg, card).move_to([-0.6, 0.85, 0])
        self.play(FadeIn(ucard, shift=RIGHT * 0.3), run_time=0.7)
        reg_line = next(l for l in LINES if l.startswith("usb-core: register device="))
        n = note(self, n, "usb_device_register(u)：dev.bus = &usb_bus → device_register() 扫描 drivers[]", GREEN)
        tk.show(reg_line, rt=0.2)
        # right: drivers on the usb bus with id tables
        kbd = VGroup(T("usb_driver usbkbd", 16, GREEN, mono=True),
                     T("id_table:", 14, GREY_B, mono=True),
                     T("class    = 3  (HID)", 14, WHITE, mono=True),
                     T("subclass = 1  (boot)", 14, WHITE, mono=True),
                     T("protocol = 1  (keyboard)", 14, WHITE, mono=True),
                     T("flags = CLASS|SUBCLASS|PROTOCOL", 13, GREY_B, mono=True)
                     ).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        kbg = SurroundingRectangle(kbd, buff=0.15, color=GREEN, corner_radius=0.1)
        kcard = VGroup(kbg, kbd).move_to([4.6, 1.3, 0])
        mouse = VGroup(T("usb_driver usbmouse", 15, GREY_B, mono=True),
                       T("{3, 1, 2}  (未尝试：已绑定就 break)", 13, GREY, ))
        mouse.arrange(DOWN, aligned_edge=LEFT, buff=0.06).next_to(kcard, DOWN, buff=0.3).align_to(kcard, LEFT)
        self.play(FadeIn(kcard), FadeIn(mouse), run_time=0.5)
        # field-by-field compare
        pairs = [(rows[2], kbd[2]), (rows[3], kbd[3]), (rows[4], kbd[4])]
        for a, b in pairs:
            ln = Line(a.get_right(), b.get_left(), color=GREEN, stroke_width=2.5)
            ok = T("✓", 22, GREEN).move_to(ln.get_center() + UP * 0.15)
            self.play(Create(ln), FadeIn(ok), run_time=0.35)
            self.play(FadeOut(ln), run_time=0.15)
            self.remove(ok)
        for l in LINES:
            if l.startswith("usb-core: match") or l.startswith("dm: match bus=usb"):
                tk.show(l, rt=0.25)
        self.play(Circumscribe(kcard, color=GREEN), run_time=0.8)
        # probe steps
        self.play(FadeOut(ctrl), FadeOut(hc), FadeOut(mouse), run_time=0.4)
        n = note(self, n, "bind()：dev->driver = usbkbd → usb_probe() → container_of → usbkbd_probe(udev)", GREEN)
        steps = [
            ("SET_PROTOCOL(boot)", "usbkbd: SET_PROTOCOL(boot) ok"),
            ("SET_IDLE(0)", "usbkbd: SET_IDLE(0) ok"),
            ("SET_REPORT(LED=0)", "usbkbd: SET_REPORT(output) leds=0"),
            ("input_register_device()", "dm: input: connect device="),
            ("→ /dev/input/event0, kbd handler 已连接", "input: USB HID Boot Keyboard registered"),
            ("usb_fill_int_urb + usb_submit_urb", "usbkbd: 627:1 keyboard on ep1"),
            ("→ ehci_submit_urb：周期调度 QH (ep1)", "usb-core: probe end"),
            ("probe 返回 0 → bound", "dm: probe end drv=usbkbd"),
        ]
        col = VGroup(*[T(s, 16, WHITE) for s, _ in steps]).arrange(DOWN, aligned_edge=LEFT, buff=0.12)
        col.move_to([-4.0, 0.2, 0]).align_to([-6.7, 0, 0], LEFT)
        for (s, key), m in zip(steps, col):
            color = ORANGE if "input" in s or "kbd" in s else (YELLOW if "urb" in s or "QH" in s else WHITE)
            m.set_color(color)
            self.play(FadeIn(m, shift=RIGHT * 0.2), run_time=0.35)
            line = next((l for l in LINES if key in l), None)
            if line:
                tk.show(line, rt=0.15)
        n = note(self, n, "键盘驱动已经把一个中断 URB 交给主机控制器：之后的数据靠完成回调推动", YELLOW)
        self.wait(1.5)


# ---------------------------------------------------------------- scene 4
class DataFlow(Scene):
    def construct(self):
        title(self, "④ 数据传输：一次按键从 USB 中断端点到 sh",
              "URB 是驱动和主机控制器之间的“请求单”：提交 → 硬件完成 → giveback → 回调 → 再提交")
        tk = Ticker(self)
        nodes = [
            ("USB 键盘\nep1 IN 8 字节", RED, [-5.6, 2.0]),
            ("EHCI 周期调度\nQH(addr1,ep1)", BLUE, [-2.2, 2.0]),
            ("IRQ 39\nehci_irq", BLUE, [1.2, 2.0]),
            ("ehci_intr_reap\nusb_hcd_giveback_urb", GREEN, [4.8, 2.0]),
            ("usbkbd_irq\n(中断上下文)", GREEN, [4.8, 0.45]),
            ("queue_work\nusbkbd_wq", GREEN, [1.2, 0.45]),
            ("usbkbd_rx_work\n解析报告", GREEN, [-2.2, 0.45]),
            ("input_report_key\ninput core", ORANGE, [-5.6, 0.45]),
            ("kbd handler\n→ console", ORANGE, [-5.6, -1.15]),
            ("evdev client\n/dev/input/event0", ORANGE, [1.2, -1.15]),
            ("sh: read(0)\n得到 'l'", PURPLE, [-2.2, -1.15]),
        ]
        boxes = []
        for lab, col, (x, y) in nodes:
            r = RoundedRectangle(corner_radius=0.1, width=3.0, height=0.95, stroke_color=col,
                                 fill_color=col, fill_opacity=0.18).move_to([x, y, 0])
            t = VGroup(*[T(s, 15, WHITE, mono=False) for s in lab.split("\n")]).arrange(DOWN, buff=0.05).move_to(r)
            boxes.append(VGroup(r, t))
        self.play(LaggedStart(*[FadeIn(b) for b in boxes], lag_ratio=0.08), run_time=1.5)
        order = [(0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 8), (8, 10), (7, 9)]
        arrows = {}
        for a, b in order:
            A, B = boxes[a][0], boxes[b][0]
            ax, ay = A.get_center()[:2]
            bx, by = B.get_center()[:2]
            if abs(ay - by) < 0.01:
                p, q = (A.get_right(), B.get_left()) if bx > ax else (A.get_left(), B.get_right())
            else:
                p, q = A.get_bottom(), B.get_top()
            arr = Arrow(p, q, buff=0.05, stroke_width=3, color=GREY_B,
                        max_tip_length_to_length_ratio=0.25)
            arrows[(a, b)] = arr
        self.play(*[GrowArrow(a) for a in arrows.values()], run_time=0.8)
        resub = CurvedArrow(boxes[6].get_top() + UP * 0.02, boxes[1].get_bottom() + DOWN * 0.02,
                            angle=-1.2, color=YELLOW, stroke_width=3)
        rl = T("usb_submit_urb() 再次提交", 14, YELLOW).move_to([-0.35, 1.22, 0])
        # report bytes
        rep = find(r"usbkbd: first report mod=(\w+) keys=([\w,]+)")
        keys = rep.group(2).split(",")
        bytes_ = [rep.group(1), "0"] + keys
        pkt = VGroup(*[T(f"{int(b, 16):02x}", 15, WHITE, mono=True) for b in bytes_]).arrange(RIGHT, buff=0.1)
        pbg = SurroundingRectangle(pkt, buff=0.08, color=RED, fill_color="#3a1515", fill_opacity=1, corner_radius=0.05)
        packet = VGroup(pbg, pkt).scale(0.9).move_to(boxes[0].get_center() + DOWN * 0.75)
        n = note(self, None, "QEMU monitor 里 sendkey l：键盘在下一次轮询时回 8 字节 boot report", YELLOW, pos=[0, -2.4, 0])
        self.play(FadeIn(packet), run_time=0.4)
        self.play(Indicate(pkt[2], color=YELLOW), run_time=0.6)
        hop_logs = {
            1: "EHCI 每帧读 frame list → 锚点 QH → 键盘 QH；qTD 完成后置 IOC",
            2: "USBSTS.USBINT → ehci_irq() 写 1 清状态，调用 ehci_intr_reap()",
            3: "qTD 不再 ACTIVE：len = 8 - TOK_LEFT → usb_hcd_giveback_urb(urb, 0, 8)",
            4: "urb 状态回到 IDLE，调用 urb->complete = usbkbd_irq",
            5: "中断里不做重活：queue_work(&usbkbd_wq, &kbd->rx_work)",
            6: "工作线程 usbkbd_rx_work()：和上一份报告比较，找出按下/松开的键",
            7: "input_report_key(KEY_L, 1) + input_sync()",
            8: "kbd_event()：KEY 38 → 字符 0x6c 'l' 送进 console 输入缓冲",
            10: "consoleread() 唤醒 sh，sh 得到 'l'",
        }
        log_for = {
            3: next((l for l in LINES if "intr-complete" in l), "usb-xfer: intr-complete addr=1 ep=1 len=8 (USB_XFER_TRACE=1)"),
            4: next(l for l in LINES if "urb done status=0 len=8" in l),
            6: next(l for l in LINES if "first report" in l),
            7: next(l for l in LINES if "-> KEY 38 down" in l),
            8: next(l for l in LINES if l.startswith("kbd: KEY 38")),
        }
        path = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10]
        for i in range(1, len(path)):
            a, b = path[i - 1], path[i]
            arr = arrows[(a, b)]
            self.play(arr.animate.set_color(YELLOW), run_time=0.15)
            self.play(packet.animate.move_to(boxes[b].get_center() + DOWN * 0.75), run_time=0.55)
            if b == 7:
                key = T("KEY_L (38) ↓", 18, ORANGE, mono=True).move_to(packet)
                self.play(ReplacementTransform(packet, key), run_time=0.4)
                packet = key
                self.play(GrowArrow(arrows[(7, 9)].set_color(YELLOW)), run_time=0.3)
            if b == 8:
                ch = T("'l' 0x6c", 18, PURPLE, mono=True).move_to(packet)
                self.play(ReplacementTransform(packet, ch), run_time=0.4)
                packet = ch
            if b in hop_logs:
                n = note(self, n, hop_logs[b], WHITE, pos=[0, -2.4, 0])
            if b in log_for:
                tk.show(log_for[b], rt=0.2)
            self.play(arr.animate.set_color(GREY_B), run_time=0.1)
            if b == 6:
                self.play(Create(resub), FadeIn(rl), run_time=0.6)
                tk.show(next(l for l in LINES if "urb resubmitted" in l), YELLOW, rt=0.2)
        self.wait(0.5)
        # the rest of "ls\n" from the log
        n = note(self, n, "后续报告（日志）：松开 l → KEY 38 up；按下 s → KEY 31；回车 → KEY 28，sh 执行 ls", GREEN, pos=[0, -2.4, 0])
        rest = [l for l in LINES if l.startswith(("usbkbd: report #", "usbkbd:   usage", "kbd: KEY"))]
        for l in rest[:9]:
            tk.show(l, rt=0.3)
            self.wait(0.15)
        self.play(FadeOut(packet), run_time=0.3)
        n = note(self, n, "总结：总线负责“谁配谁”(match/probe)，数据走 URB → 回调 → input core → handler / evdev → 用户进程", YELLOW, pos=[0, -2.4, 0])
        self.wait(2)
