"""USB keyboard on xv6 / Raspberry Pi 3 (DWC2): load, enumerate, bind, and
receive interrupt reports.  Steps are numbered like doc/readme/rpi3/
usb-keyboard.md (0-9); every value shown comes from
doc/readme/rpi3/logs/qemu-usbkbd-trace2.log (QEMU raspi3b, usb-kbd behind
the auto-inserted usb-hub, make USBKBD_TRACE=2, typed "echo Hi<Enter>").

Render (repo root):
    manim -qh --media_dir build/manim tools/manim/rpi3_usbkbd_anim.py UsbKbdRpi3
    cp build/manim/videos/rpi3_usbkbd_anim/1080p60/UsbKbdRpi3.mp4 \
       doc/readme/rpi3/videos/usbkbd-flow.mp4
"""
from manim import *

FONT = "Heiti SC"
MONO = "Menlo"

LANES = [
    ("USB 键盘", "627:1 · Hub 口 1 · addr 2", TEAL),
    ("DWC2 硬件", "ch0 / ch6 · IRQ 9", BLUE),
    ("dwc2.c", "HCD 驱动", PURPLE),
    ("usb core", "总线 match/probe · URB", GOLD),
    ("usbkbd", "HID boot 驱动", ORANGE),
    ("tty + input", "consoleintr / event0", GREEN),
]
LANE_X = [-6.0, -3.6, -1.2, 1.2, 3.6, 6.0]
TOP_Y = 1.95
BOTTOM_Y = -2.6


def T(s, size=22, color=WHITE, font=FONT, **kw):
    return Text(s, font=font, font_size=size, color=color, **kw)


def M(s, size=18, color=WHITE):
    return Text(s, font=MONO, font_size=size, color=color)


class UsbKbdRpi3(Scene):
    # ------------------------------------------------------------------ helpers
    def lanes(self):
        g = VGroup()
        for (name, sub, col), x in zip(LANES, LANE_X):
            box = RoundedRectangle(width=2.15, height=0.82, corner_radius=0.12,
                                   stroke_color=col, fill_color=col, fill_opacity=0.18)
            box.move_to([x, TOP_Y + 0.55, 0])
            label = VGroup(T(name, 20, col, weight=BOLD), T(sub, 12, GRAY_B)).arrange(DOWN, buff=0.05)
            label.move_to(box)
            line = DashedLine([x, TOP_Y + 0.12, 0], [x, BOTTOM_Y, 0],
                              dash_length=0.08, color=col, stroke_opacity=0.5)
            g.add(VGroup(box, label, line))
        return g

    def phase(self, title, sub):
        t = T(title, 30, YELLOW, weight=BOLD).to_edge(UP, buff=0.12)
        s = T(sub, 16, GRAY_B).next_to(t, DOWN, buff=0.06)
        return VGroup(t, s)

    def msg(self, a, b, y, label, color=WHITE, data=None, run=0.8, dashed=False):
        start, end = [LANE_X[a], y, 0], [LANE_X[b], y, 0]
        if a == b:
            arr = CurvedArrow([LANE_X[a] + 0.05, y + 0.12, 0], [LANE_X[a] + 0.05, y - 0.12, 0],
                              angle=-PI * 1.4, color=color, stroke_width=3, tip_length=0.12)
            lab = T(label, 15, color)
            if a >= 4:
                lab.next_to(arr, LEFT, buff=0.12)
            else:
                lab.next_to(arr, RIGHT, buff=0.1)
            self.play(Create(arr), FadeIn(lab), run_time=run * 0.7)
            return VGroup(arr, lab)
        cls = DashedLine if dashed else Line
        line = cls(start, end, color=color, stroke_width=3)
        arr = VGroup(line, ArrowTriangleFilledTip(color=color, length=0.16, width=0.14)
                     .move_to(end).rotate(0 if b > a else PI).shift(LEFT * 0.08 if b > a else RIGHT * 0.08))
        lab = T(label, 15, color).next_to(line, UP, buff=0.04)
        if lab.width > abs(LANE_X[b] - LANE_X[a]) + 1.6:
            lab.scale_to_fit_width(abs(LANE_X[b] - LANE_X[a]) + 1.6)
        pkt = Dot(start, radius=0.07, color=color)
        group = [pkt]
        if data:
            chip = M(data, 13, BLACK)
            bg = BackgroundRectangle(chip, color=color, fill_opacity=0.95, buff=0.05)
            tag = VGroup(bg, chip).next_to(pkt, DOWN, buff=0.06)
            tag.add_updater(lambda m, p=pkt: m.next_to(p, DOWN, buff=0.06))
            group.append(tag)
        self.add(*group)
        self.play(Create(line), FadeIn(lab), MoveAlongPath(pkt, Line(start, end)),
                  run_time=run, rate_func=smooth)
        self.add(arr[1])
        if data:
            tag.clear_updaters()
            self.play(FadeOut(tag), FadeOut(pkt), run_time=0.25)
        else:
            self.remove(pkt)
        return VGroup(arr, lab)

    def note(self, s, color=GRAY_A, size=17):
        n = T(s, size, color)
        if n.width > 13.6:
            n.scale_to_fit_width(13.6)
        n.to_edge(DOWN, buff=0.12)
        bg = BackgroundRectangle(n, color=BLACK, fill_opacity=0.85, buff=0.08)
        return VGroup(bg, n)

    def say(self, old, s, color=GRAY_A):
        new = self.note(s, color)
        if old is None:
            self.play(FadeIn(new), run_time=0.4)
        else:
            self.play(ReplacementTransform(old, new), run_time=0.4)
        return new

    def clear_msgs(self, msgs, note=None):
        items = list(msgs)
        if note is not None:
            items.append(note)
        if items:
            self.play(*[FadeOut(m) for m in items], run_time=0.4)

    def regcard(self, title, rows, pos, col=YELLOW):
        lines = [T(title, 18, col, weight=BOLD)] + [M(r, 15) for r in rows]
        g = VGroup(*lines).arrange(DOWN, aligned_edge=LEFT, buff=0.1)
        box = SurroundingRectangle(g, color=col, buff=0.18, fill_color=BLACK, fill_opacity=0.92)
        return VGroup(box, g).move_to(pos)

    # --------------------------------------------------------------- the scene
    def construct(self):
        self.camera.background_color = "#14161c"
        self.intro()
        lanes = self.lanes()
        self.play(LaggedStart(*[FadeIn(l, shift=DOWN * 0.2) for l in lanes], lag_ratio=0.12))
        self.boot()
        self.enumerate_hub()
        self.enumerate_kbd()
        self.bind_probe()
        self.channel6()
        self.data_exchange()
        self.outro()

    def intro(self):
        t = T("xv6 树莓派 3：USB 键盘的加载、配置与数据接收", 38, YELLOW, weight=BOLD)
        s = T("DWC2 OTG 主机控制器 · Hub 后的全速 boot 键盘 · 数值全部取自 QEMU raspi3b 全量日志", 20, GRAY_B)
        c = M("make USBKBD_TRACE=2  ·  -device usb-kbd  ·  sendkey e c h o spc shift-h i ret", 17, TEAL)
        g = VGroup(t, s, c).arrange(DOWN, buff=0.35)
        self.play(Write(t), run_time=1.2)
        self.play(FadeIn(s), FadeIn(c))
        self.wait(1.5)
        self.play(FadeOut(g))

    # ---- 0/1. module order + host controller init -------------------------------
    def boot(self):
        ph = self.phase("步骤 0–1：加载顺序与主控初始化",
                        "module_init_ord：usbhid-kbd(2) 先注册 usb_driver → dwc2(4) 再 probe 并枚举")
        self.play(FadeIn(ph))
        y, msgs = 1.4, []
        msgs.append(self.msg(4, 3, y, "usb_register_driver(usbhid-keyboard)  id c03s01p01", ORANGE)); y -= 0.55
        msgs.append(self.msg(2, 2, y, "dwc2_probe：建 dwc2_periodic_wq / dwc2_hub_wq", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "邮箱 SET_POWER_STATE(USB HCD) · GSNPSID=4f54294a", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "GRSTCTL.CSFTRST → GUSBCFG.FORCEHOST", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "GINTMSK=HCHINT · GAHBCFG=DMA_EN|GLBL_INTR_EN", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "HPRT0：PWR → RST 50ms → 清 RST", PURPLE)); y -= 0.55
        msgs.append(self.msg(1, 2, y, "HPRT0=0002100d：Conn · Ena · Pwr", BLUE, data="2100d", dashed=True)); y -= 0.55
        n = self.say(None, "kbd1: root port HPRT0: PWR -> CONNSTS -> RST 50ms -> ENA      dwc2: host hprt=2100d")
        self.wait(1.6)
        self.clear_msgs(msgs, n)
        self.play(FadeOut(ph))

    # ---- 2. the hub --------------------------------------------------------------
    def enumerate_hub(self):
        ph = self.phase("步骤 2：根口上是 Hub（真机 LAN9514，QEMU 自动插入 usb-hub）",
                        "所有控制传输走通道 0：SETUP → DATA1 → STATUS，CPU 轮询 HCINT0")
        self.play(FadeIn(ph))
        steps = [
            ("GET_DESCRIPTOR(设备, 8)  addr 0", "80 06 00 01 00 00 08 00", "12 01 … 09 … 08", "bMaxPacketSize0=8, class 9"),
            ("SET_ADDRESS(1)", "00 05 01 00 00 00 00 00", None, "地址 0 → 1"),
            ("GET_DESCRIPTOR(配置, 25)", "80 06 00 02 00 00 19 00", "… EP 81 中断 mps 2 bInterval 255", "状态变化端点 → ch7"),
            ("SET_CONFIGURATION(1)", "00 09 01 00 00 00 00 00", None, "Hub 已配置"),
            ("HUB GET_DESCRIPTOR(0x29)", "a0 06 00 29 00 00 09 00", "09 29 08 …", "8 个下行口"),
            ("SET_FEATURE(PORT_POWER) ×8", "23 03 08 00 01 00 00 00", None, "各口供电"),
        ]
        self.ctrl_seq(steps, lane=1)
        self.play(FadeOut(ph))

    def ctrl_seq(self, steps, lane):
        y, msgs, note = 1.4, [], None
        for i, (name, setup, reply, what) in enumerate(steps):
            if i == 3:
                self.clear_msgs(msgs)
                msgs, y = [], 1.4
            msgs.append(self.msg(2, 0, y, f"ch0 SETUP  {name}", PURPLE, data=setup, run=0.85))
            y -= 0.6
            if reply:
                msgs.append(self.msg(0, 2, y, f"DATA1 IN  →  {what}", TEAL, data=reply, run=0.85, dashed=True))
            else:
                msgs.append(self.msg(0, 2, y, f"STATUS (0 字节)  →  {what}", TEAL, run=0.6, dashed=True))
            y -= 0.6
            note = self.say(note, f"kbd-: ctrl ch0 … {name} -> ok      （{what}）")
        self.wait(0.8)
        self.clear_msgs(msgs, note)

    # ---- 3. the keyboard behind the hub -----------------------------------------
    def enumerate_kbd(self):
        ph = self.phase("步骤 3：Hub 口复位 → split 路由 → 键盘地址 2 → 配置",
                        "全速设备在高速 Hub 后面：之后每个事务都写 HCSPLT（hub=1 port=1），由 Hub 的 TT 转发")
        self.play(FadeIn(ph))
        y, msgs = 1.4, []
        msgs.append(self.msg(2, 0, y, "GET_STATUS(口1)=0101 → SET_FEATURE(PORT_RESET)", PURPLE, data="a3 00 00 00 01 00 04 00")); y -= 0.6
        msgs.append(self.msg(0, 2, y, "口状态 0103：Enable，非高速 = 全速", TEAL, data="03 01 …", dashed=True)); y -= 0.6
        msgs.append(self.msg(2, 2, y, "usb_route[0] = {hub 1, port 1} → HCSPLT=8000c081", PURPLE)); y -= 0.6
        msgs.append(self.msg(2, 0, y, "split GET_DESCRIPTOR(设备) → 627:1；SET_ADDRESS(2)", PURPLE, data="00 05 02 00 00 00 00 00")); y -= 0.6
        msgs.append(self.msg(2, 0, y, "split GET_DESCRIPTOR(配置, 34)", PURPLE, data="80 06 00 02 00 00 22 00")); y -= 0.6
        msgs.append(self.msg(0, 2, y, "接口 0：03/01/01 · EP 81 中断 IN mps 8 bInterval 10", TEAL,
                             data="09 04 00 00 01 03 01 01 … 07 05 81 03 08 00 0a", dashed=True)); y -= 0.6
        msgs.append(self.msg(2, 0, y, "split SET_CONFIGURATION(1)", PURPLE, data="00 09 01 00 00 00 00 00")); y -= 0.6
        n = self.say(None, "kbd3: config 1 (34 bytes): interface 0 class 3/1/1 (boot keyboard), interrupt-IN ep1 mps=8 bInterval=10")
        self.wait(1.8)
        self.clear_msgs(msgs, n)
        self.play(FadeOut(ph))

    # ---- 4/5. bind + probe + first submit ---------------------------------------
    def bind_probe(self):
        ph = self.phase("步骤 4–5：总线匹配 → usbkbd_probe → 提交中断 URB",
                        "每个 boot-HID 接口一个 usb_device（usbH0）· 按 class 3/1/1 匹配，不看 VID/PID")
        self.play(FadeIn(ph))
        y, msgs = 1.4, []
        msgs.append(self.msg(2, 3, y, "usb_device_register(usbH0  addr 2  intf 0)", PURPLE)); y -= 0.5
        msgs.append(self.msg(3, 4, y, "usb_match c3s1p1 → usb_probe → usbkbd_probe()", GOLD)); y -= 0.5
        msgs.append(self.msg(4, 0, y, "split SET_PROTOCOL(0 = boot)", ORANGE, data="21 0b 00 00 00 00 00 00")); y -= 0.5
        msgs.append(self.msg(4, 0, y, "split SET_IDLE(0)：只在变化时上报", ORANGE, data="21 0a 00 00 00 00 00 00")); y -= 0.5
        msgs.append(self.msg(4, 5, y, "input_register_device → event0", ORANGE)); y -= 0.5
        msgs.append(self.msg(4, 3, y, "usb_fill_int_urb(ep1, report[8], usbkbd_irq_complete, 10)", ORANGE)); y -= 0.5
        msgs.append(self.msg(3, 2, y, "usb_submit_urb：IDLE → SUBMITTED → dwc2_submit_urb", GOLD)); y -= 0.5
        msgs.append(self.msg(2, 1, y, "interrupt_rx[0] pending → ch6 空闲 → rx_start", PURPLE)); y -= 0.5
        n = self.say(None, "kbd6: ch6 arm SSPLIT addr=2 ep1 IN DATA0 mps=8 HCDMA=c803b080 HCTSIZ=80008 HCCHAR=8c8808")
        self.wait(1.2)
        card = self.regcard("通道 6 寄存器（dwc2_interrupt_rx_start）", [
            "HCSPLT6 = 8000c081   SpltEna · XactPos=ALL · Hub 1 · 口 1",
            "HCDMA6  = c803b080   0xc0000000 总线别名 | report[] 物理地址",
            "HCTSIZ6 = 00080008   PID=DATA0 · PktCnt=1 · XferSize=8",
            "HAINTMSK |= 1<<6     HCINTMSK6 = 7ff",
            "HCCHAR6 = 808c8808   ChEna · addr 2 · 中断 · IN · EP1 · MPS 8",
        ], [0, -1.6, 0])
        self.play(FadeIn(card))
        self.wait(3)
        self.clear_msgs(msgs + [card], n)
        self.play(FadeOut(ph))

    # ---- 6. channel 6 polling ---------------------------------------------------
    def channel6(self):
        ph = self.phase("步骤 6：通道 6 轮询 —— 没按键就是 NAK，10 ms 后再来",
                        "DWC2 这里没有周期调度表：一次一个事务，NAK 后由 dwc2_periodic_wq 的 delayed work 重新 arm")
        self.play(FadeIn(ph))
        cover = Rectangle(width=14.5, height=5.6, fill_color="#14161c", fill_opacity=0.96, stroke_width=0).move_to([0, -0.9, 0])
        self.play(FadeIn(cover), run_time=0.3)

        def st(label, col, pos, sub=""):
            r = RoundedRectangle(width=2.9, height=0.9, corner_radius=0.3, color=col, fill_color=col, fill_opacity=0.2)
            t = VGroup(T(label, 18, col, weight=BOLD), T(sub, 12, GRAY_B)).arrange(DOWN, buff=0.04).move_to(r)
            return VGroup(r, t).move_to(pos)
        idle = st("pending", TEAL, [-4.5, 1.0, 0], "interrupt_rx[0]")
        ss = st("SSPLIT IN 在飞", BLUE, [0, 1.0, 0], "HCCHAR6.ChEna=1")
        cs = st("CSPLIT（仅真机）", PURPLE, [4.5, 1.0, 0], "SSPLIT 后 2 个微帧")
        wait = st("等 bInterval", GRAY_B, [-4.5, -1.6, 0], "retry_work · 1 jiffy=10 ms")
        done = st("XferCompl → IRQ 9", GREEN, [2.2, -1.6, 0], "进入步骤 7")
        a1 = Arrow(idle.get_right(), ss.get_left(), buff=0.1, color=TEAL)
        a2 = Arrow(ss.get_right(), cs.get_left(), buff=0.1, color=PURPLE)
        l2 = T("ACK", 15, PURPLE).next_to(a2, UP, buff=0.05)
        a3 = Arrow(ss.get_bottom(), wait.get_top() + RIGHT * 0.6, buff=0.1, color=GRAY_B)
        l3 = T("HCINT=12：NAK|ChHltd", 14, GRAY_B).next_to(a3.get_center(), RIGHT, buff=0.15)
        a4 = Arrow(wait.get_top() + LEFT * 0.4, idle.get_bottom() + LEFT * 0.4, buff=0.1, color=GRAY_B)
        a5 = Arrow(ss.get_bottom() + RIGHT * 0.5, done.get_top(), buff=0.1, color=GREEN)
        l5 = T("HCINT=03：有报告", 14, GREEN).next_to(a5.get_center(), RIGHT, buff=0.1)
        a6 = Arrow(cs.get_bottom(), done.get_right() + UP * 0.2, buff=0.1, color=GREEN)
        g = VGroup(idle, ss, cs, wait, done, a1, a2, l2, a3, l3, a4, a5, l5, a6)
        self.play(LaggedStart(FadeIn(idle), GrowArrow(a1), FadeIn(ss), GrowArrow(a2), FadeIn(l2), FadeIn(cs),
                              lag_ratio=0.25), run_time=2.2)
        self.play(LaggedStart(GrowArrow(a3), FadeIn(l3), FadeIn(wait), GrowArrow(a4), lag_ratio=0.25), run_time=1.6)
        # three idle polls
        n = None
        for i, fr in enumerate(["38c", "923", "fae"]):
            dot = Dot(idle.get_center(), color=YELLOW)
            path = VMobject().set_points_as_corners([idle.get_center(), ss.get_center(), wait.get_center(), idle.get_center()])
            n = self.say(n, f"kbd6: ch6 arm SSPLIT … frame={fr}  →  kbd7: HCINT6=12  →  kbd6: ch6 NAK -> retry in 1 jiffies")
            self.play(MoveAlongPath(dot, path), run_time=1.1)
            self.remove(dot)
        self.play(GrowArrow(a5), FadeIn(l5), FadeIn(done), GrowArrow(a6), run_time=1.0)
        dot = Dot(idle.get_center(), color=GREEN)
        n = self.say(n, "按下 e：QEMU 直接给出 8 字节（HCINT=03）；真机是 SSPLIT→ACK→CSPLIT→(NYET…)→数据", GREEN)
        self.play(MoveAlongPath(dot, VMobject().set_points_as_corners([idle.get_center(), ss.get_center(), done.get_center()])),
                  run_time=1.2)
        self.wait(1.6)
        self.remove(dot)
        self.play(FadeOut(g), FadeOut(n), FadeOut(cover), FadeOut(ph))

    # ---- 7/8/9. data ------------------------------------------------------------
    def report_bytes(self, mod, keys, col=TEAL):
        cells = VGroup()
        labels = ["mod", "rsv", "k1", "k2", "k3", "k4", "k5", "k6"]
        for lab, v in zip(labels, [mod, 0] + keys):
            sq = Rectangle(width=0.62, height=0.5, color=col, fill_color=col, fill_opacity=0.25 if v == 0 else 0.7)
            t = M(f"{v:02X}", 16, WHITE).move_to(sq)
            l = T(lab, 11, GRAY_B).next_to(sq, DOWN, buff=0.04)
            cells.add(VGroup(sq, t, l))
        cells.arrange(RIGHT, buff=0.03)
        return cells

    def key_round(self, ttl, mod, keys, pid, usage_txt, key_txt, fast=False):
        y, msgs = 1.5, []
        rep = self.report_bytes(mod, keys).scale(0.85).move_to([-1.2, -3.15, 0])
        rl = T(f"8 字节 boot 报告：{ttl}", 15, TEAL).next_to(rep, UP, buff=0.08)
        self.play(FadeIn(rep), FadeIn(rl), run_time=0.4)
        hexs = " ".join(f"{v:02X}" for v in [mod, 0] + keys)
        r = 0.55 if fast else 0.85
        msgs.append(self.msg(0, 1, y, f"SSPLIT IN ep1 {pid} → 8 B DMA 到 report[]", TEAL, data=hexs, run=r)); y -= 0.47
        msgs.append(self.msg(1, 2, y, "HCINT6=03 → HAINT=40 → IRQ 9 → dwc2_irq：schedule_work", BLUE, run=r)); y -= 0.47
        msgs.append(self.msg(2, 2, y, "system_wq：HCTSIZ 剩 0 → 8 B · dc ivac · toggle ^= 1", PURPLE, run=r)); y -= 0.47
        msgs.append(self.msg(2, 3, y, "usb_hcd_giveback_urb(status 0, 8)", PURPLE, run=r)); y -= 0.47
        msgs.append(self.msg(3, 4, y, "complete = usbkbd_irq_complete → queue_work", GOLD, run=r)); y -= 0.47
        msgs.append(self.msg(4, 4, y, f"usbkbd_wq：与上次比较 → {usage_txt}", ORANGE, run=r)); y -= 0.47
        msgs.append(self.msg(4, 5, y, key_txt, ORANGE, run=r)); y -= 0.47
        msgs.append(self.msg(4, 1, y, "usb_submit_urb → ch6 重新 arm", ORANGE, dashed=True, run=r)); y -= 0.47
        return msgs + [rep, rl]

    def data_exchange(self):
        ph = self.phase("步骤 7–9：按键 → IRQ 9 → 两级 workqueue → 控制台 / evdev",
                        "QEMU monitor：sendkey e c h o spc shift-h i ret  （按下、松开各一份报告）")
        self.play(FadeIn(ph))
        typed = T("控制台：", 20, GREEN).move_to([3.6, -3.25, 0])
        shown = M("$ ", 22, GREEN).next_to(typed, RIGHT, buff=0.1)
        self.play(FadeIn(typed), FadeIn(shown))
        text = "$ "

        def type_(s):
            nonlocal text, shown
            text += s
            new = M(text, 22, GREEN).next_to(typed, RIGHT, buff=0.1)
            self.play(Transform(shown, new), run_time=0.3)

        items = self.key_round("按下 e", 0, [0x08, 0, 0, 0, 0, 0], "DATA0", "usage 0x08 新按下",
                               "consoleintr('e') + KEY_E ↓")
        type_("e")
        self.wait(1.2)
        self.play(*[FadeOut(m) for m in items], run_time=0.35)
        items = self.key_round("松开 e", 0, [0, 0, 0, 0, 0, 0], "DATA1", "usage 0x08 消失",
                               "KEY_E ↑ · input_sync (EV_SYN)", fast=True)
        self.wait(0.6)
        self.play(*[FadeOut(m) for m in items], run_time=0.35)
        n = self.say(None, "c h o 空格：同样每个键两份报告（DATA0/DATA1 交替）……")
        for ch in "cho ":
            type_(ch)
        self.play(FadeOut(n), run_time=0.3)
        items = self.key_round("Shift + h", 0x02, [0x0B, 0, 0, 0, 0, 0], "DATA0", "mod b1=LShift, usage 0x0b",
                               "shiftmap → consoleintr('H')", fast=True)
        type_("H")
        self.wait(1.0)
        self.play(*[FadeOut(m) for m in items], run_time=0.35)
        type_("i")
        items = self.key_round("Enter", 0, [0x28, 0, 0, 0, 0, 0], "DATA0", "usage 0x28",
                               "consoleintr('\\n') → wakeup(&cons.r)", fast=True)
        self.wait(0.6)
        self.play(*[FadeOut(m) for m in items], run_time=0.35)
        out = M("Hi", 22, WHITE).next_to(shown, DOWN, aligned_edge=LEFT, buff=0.12)
        n = self.say(None, "sh 的 read(0) 返回 \"echo Hi\\n\"，/bin/echo 打印 Hi —— 整条链路打通", GREEN)
        self.play(FadeIn(out))
        self.wait(2.5)
        self.play(FadeOut(n), FadeOut(ph), FadeOut(typed), FadeOut(shown), FadeOut(out))

    def outro(self):
        self.play(*[FadeOut(m) for m in self.mobjects])
        lines = VGroup(
            T("小结", 34, YELLOW, weight=BOLD),
            T("0–1  usbkbd 先注册；dwc2_init：邮箱上电、软复位、FORCEHOST、只开 HCHINT、根口复位", 19),
            T("2–3  通道 0 控制传输：Hub(addr 1) 配置、口复位；键盘全速 → split，addr 2，接口 3/1/1", 19),
            T("4–5  usb 总线按 class 匹配 → usbkbd_probe：boot 协议、event0、提交 8 字节中断 URB", 19),
            T("6    通道 6 轮询：NAK → 10 ms 后重来；真机 split 的 ACK/NYET 在 IRQ 里当场续发 CSPLIT", 19),
            T("7–9  IRQ 9 只排队 → system_wq 归还 URB → usbkbd_wq 差分报告 → consoleintr + evdev → 重新提交", 19),
            T("调试：make USBKBD_TRACE=1|2|3，行首 kbdN: 对应步骤 N", 19, TEAL),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.28)
        self.play(FadeIn(lines, shift=UP * 0.2))
        self.wait(3.5)
