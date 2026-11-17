"""USB keyboard probe/load flow and data exchange in xv6 (tiny210 port, QEMU).

Every message and byte shown here is taken from qemu-usbkbd-full-trace.log
(QEMU usb-kbd on the cubieboard EHCI0, built with
USB_CORE_TRACE=1 USB_ENUM_TRACE=1 USB_XFER_TRACE=1 USBHID_TRACE=2).

Render (from this directory):
    manim -qh usbkbd_flow.py UsbKbdFlow        # 1080p
    manim -qm usbkbd_flow.py UsbKbdFlow        # 720p, faster
"""
from manim import *

FONT = "Heiti SC"
MONO = "Menlo"

LANES = [
    ("usb-kbd", "QEMU 设备 627:1", TEAL),
    ("EHCI 控制器", "0x01C14000 IRQ39", BLUE),
    ("ehci-hcd", "驱动 / ehci-enum", PURPLE),
    ("usb-core", "总线 match/probe", GOLD),
    ("usbkbd", "HID 驱动", ORANGE),
    ("input + kbd", "event0 / 控制台", GREEN),
]
LANE_X = [-6.0, -3.6, -1.2, 1.2, 3.6, 6.0]
TOP_Y = 1.95
BOTTOM_Y = -2.6


def T(s, size=22, color=WHITE, font=FONT, **kw):
    return Text(s, font=font, font_size=size, color=color, **kw)


def M(s, size=18, color=WHITE):
    return Text(s, font=MONO, font_size=size, color=color)


class UsbKbdFlow(Scene):
    # ------------------------------------------------------------------ helpers
    def lanes(self):
        g = VGroup()
        for (name, sub, col), x in zip(LANES, LANE_X):
            box = RoundedRectangle(width=2.15, height=0.82, corner_radius=0.12,
                                   stroke_color=col, fill_color=col, fill_opacity=0.18)
            box.move_to([x, TOP_Y + 0.55, 0])
            label = VGroup(T(name, 20, col, weight=BOLD), T(sub, 13, GRAY_B)).arrange(DOWN, buff=0.05)
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
        """Arrow from lane a to lane b at height y; label above it. A small
        packet travels along the arrow; `data` (hex string) rides on it."""
        start, end = [LANE_X[a], y, 0], [LANE_X[b], y, 0]
        if a == b:   # self call: a little loop
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
        items = [m for m in msgs]
        if note is not None:
            items.append(note)
        if items:
            self.play(*[FadeOut(m) for m in items], run_time=0.4)

    # --------------------------------------------------------------- the scene
    def construct(self):
        self.camera.background_color = "#14161c"
        self.intro()
        lanes = self.lanes()
        self.play(LaggedStart(*[FadeIn(l, shift=DOWN * 0.2) for l in lanes], lag_ratio=0.12))
        self.boot(lanes)
        self.enumerate_dev()
        self.bind_probe()
        self.schedule_view()
        self.data_exchange()
        self.outro()

    def intro(self):
        t = T("xv6 USB 键盘：探测、加载与数据交换", 40, YELLOW, weight=BOLD)
        s = T("tiny210 内核 · QEMU cubieboard EHCI0 · 数据全部来自全量 USB trace 日志", 20, GRAY_B)
        c = M("USB_CORE_TRACE=1 USB_ENUM_TRACE=1 USB_XFER_TRACE=1 USBHID_TRACE=2", 18, TEAL)
        g = VGroup(t, s, c).arrange(DOWN, buff=0.35)
        self.play(Write(t), run_time=1.2)
        self.play(FadeIn(s), FadeIn(c))
        self.wait(1.5)
        self.play(FadeOut(g))

    # ---- 0. boot: drivers register, EHCI starts --------------------------------
    def boot(self, lanes):
        ph = self.phase("① 启动：注册驱动，启动 EHCI", "initcall 顺序：usbcore(4) → kbd(5) → usbkbd/usbmouse(6) → ehci-s5p(6)")
        self.play(FadeIn(ph))
        y = 1.4
        msgs = []
        msgs.append(self.msg(4, 3, y, "usb_register_driver(usbkbd)", GOLD)); y -= 0.55
        msgs.append(self.msg(2, 2, y, "ehci_probe: request_irq(39), 起 ehci-enum 线程", PURPLE)); y -= 0.6
        msgs.append(self.msg(2, 1, y, "USBCMD=RESET；建 async 头 QH + periodic 1024 帧表", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "USBCMD=10031 (RUN|ASE|PSE)  CONFIGFLAG=1", PURPLE)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "读 PORTSC(0)", PURPLE)); y -= 0.55
        msgs.append(self.msg(1, 2, y, "PORTSC=1003：CCS=1 已连接", BLUE, data="1003", dashed=True)); y -= 0.55
        msgs.append(self.msg(2, 1, y, "PORT_RESET 50ms → PE=1：高速设备", PURPLE)); y -= 0.55
        n = self.say(None, "日志：ehci: port 0: high-speed device connected —— QEMU 的 usb-kbd 在 EHCI 上报告 480M")
        self.wait(1.6)
        self.clear_msgs(msgs, n)
        self.play(FadeOut(ph))

    # ---- 1. enumeration: control transfers on EP0 --------------------------------
    def enumerate_dev(self):
        ph = self.phase("② 枚举：EP0 控制传输（async 调度，SETUP/DATA/STATUS）",
                        "enumerate(): 每次 control 都是 qh-create 的 EP0 QH 上挂 qTD 链，IOC 中断 status=1 唤醒")
        self.play(FadeIn(ph))
        steps = [
            ("GET_DESCRIPTOR(设备, 8)", "80 06 00 01 00 00 08 00", "12 01 00 02 00 00 00 40", "mps0=64", 0),
            ("SET_ADDRESS(1)", "00 05 01 00 00 00 00 00", None, "地址 0 → 1", 0),
            ("GET_DESCRIPTOR(设备, 18)", "80 06 00 01 00 00 12 00", "… 27 06 01 00 …", "vid:pid=627:1", 1),
            ("GET_DESCRIPTOR(配置, 9)", "80 06 00 02 00 00 09 00", "09 02 22 00 01 01 …", "total=34", 1),
            ("GET_DESCRIPTOR(配置, 34)", "80 06 00 02 00 00 22 00", "接口 03/01/01 · EP 81 中断 8B", "HID Boot 键盘", 1),
            ("SET_CONFIGURATION(1)", "00 09 01 00 00 00 00 00", None, "配置生效", 1),
        ]
        y = 1.4
        msgs = []
        note = None
        for i, (name, setup, reply, what, addr) in enumerate(steps):
            if i == 3:
                self.clear_msgs(msgs)
                msgs = []
                y = 1.4
            msgs.append(self.msg(2, 0, y, f"SETUP addr={addr}  {name}", PURPLE, data=setup, run=0.9))
            y -= 0.6
            if reply:
                msgs.append(self.msg(0, 2, y, f"IN 数据  →  {what}", TEAL, data=reply, run=0.9, dashed=True))
            else:
                msgs.append(self.msg(0, 2, y, f"STATUS (0 字节)  →  {what}", TEAL, run=0.6, dashed=True))
            y -= 0.6
            note = self.say(note, f"usb-xfer: control begin … irq=39 status=1 … control end   ({what})")
        self.wait(1.0)
        res = VGroup(
            T("解析结果", 20, YELLOW, weight=BOLD),
            M("class/sub/proto = 3/1/1   (HID / Boot / Keyboard)", 16),
            M("interrupt-IN  EP1 (0x81)  mps=8  interval=7", 16),
            M("bulk in 0  out 0   intr in 1", 16),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.12)
        box = SurroundingRectangle(res, color=YELLOW, buff=0.2, fill_color=BLACK, fill_opacity=0.9)
        card = VGroup(box, res).move_to([0, -2.6, 0])
        self.play(FadeIn(card))
        self.wait(2)
        self.clear_msgs(msgs + [card], note)
        self.play(FadeOut(ph))

    # ---- 2. bind + usbkbd_probe ----------------------------------------------------
    def bind_probe(self):
        ph = self.phase("③ 绑定：usb-core 按 class 匹配，调用 usbkbd_probe()",
                        "usb_device_register → match class=3/1/1 → probe begin driver=usbkbd")
        self.play(FadeIn(ph))
        y = 1.4
        msgs = []
        msgs.append(self.msg(2, 3, y, "usb_device_register(627:1 addr=1)", PURPLE)); y -= 0.5
        msgs.append(self.msg(3, 4, y, "match 3/1/1 → usbkbd_probe()", GOLD)); y -= 0.5
        msgs.append(self.msg(4, 0, y, "SET_PROTOCOL(boot)  21 0B 00 00", ORANGE, data="21 0B 00 00 00 00 00 00")); y -= 0.5
        msgs.append(self.msg(4, 0, y, "SET_IDLE(0)  只在变化时上报", ORANGE, data="21 0A 00 00 00 00 00 00")); y -= 0.5
        msgs.append(self.msg(4, 0, y, "SET_REPORT(输出) LED=00", ORANGE, data="21 09 00 02 … + 00")); y -= 0.5
        msgs.append(self.msg(4, 5, y, "input_register_device → /dev/input/event0", ORANGE)); y -= 0.5
        msgs.append(self.msg(4, 2, y, "usb_fill_int_urb + usb_submit_urb(EP1, 8B)", ORANGE)); y -= 0.5
        msgs.append(self.msg(2, 1, y, "qh-intr：中断 QH 挂到 periodic 锚 QH 后面", PURPLE)); y -= 0.5
        n = self.say(None, "usbkbd: 627:1 keyboard on ep1 in, mps 8, interval 7   ·   probe end result=0")
        self.wait(2)
        self.clear_msgs(msgs, n)
        self.play(FadeOut(ph))

    # ---- 2b. what the controller actually walks ------------------------------------
    def schedule_view(self):
        ph = self.phase("④ 控制器视角：两条调度链", "async 环跑控制/批量；periodic 帧表每 1ms 指向锚 QH → 键盘中断 QH")
        self.play(FadeIn(ph))
        cover = Rectangle(width=14.5, height=5.6, fill_color="#14161c", fill_opacity=0.96, stroke_width=0).move_to([0, -1.0, 0])
        self.play(FadeIn(cover), run_time=0.3)

        def qh(label, col, sub=""):
            r = RoundedRectangle(width=2.3, height=0.9, corner_radius=0.1, color=col, fill_color=col, fill_opacity=0.2)
            t = VGroup(T(label, 17, col, weight=BOLD), M(sub, 12, GRAY_B)).arrange(DOWN, buff=0.04).move_to(r)
            return VGroup(r, t)

        # periodic
        frames = VGroup(*[Square(0.32, color=BLUE_B, stroke_width=2) for _ in range(8)]).arrange(RIGHT, buff=0.04)
        frames.move_to([-4.6, 0.9, 0])
        fl = T("帧表 1024 项（每项 1ms）", 15, BLUE_B).next_to(frames, UP, buff=0.1)
        anchor = qh("锚 QH", GRAY_B, "S-mask 0，不执行").move_to([-1.0, 0.9, 0])
        kq = qh("键盘中断 QH", ORANGE, "addr1 EP1 mps8").move_to([2.3, 0.9, 0])
        qtd = qh("qTD (IN 8B)", TEAL, "token ACTIVE|IOC").move_to([5.4, 0.9, 0])
        a1 = Arrow(frames.get_right(), anchor.get_left(), buff=0.1, color=BLUE_B)
        a2 = Arrow(anchor.get_right(), kq.get_left(), buff=0.1, color=GRAY_B)
        a3 = Arrow(kq.get_right(), qtd.get_left(), buff=0.1, color=ORANGE)
        per = VGroup(frames, fl, anchor, kq, qtd, a1, a2, a3)
        # async
        head = qh("async 头 QH", GRAY_B, "H 位, 指向自己").move_to([-4.6, -1.7, 0])
        ep0a = qh("EP0 QH addr0", PURPLE, "枚举早期").move_to([-1.0, -1.7, 0])
        ep0b = qh("EP0 QH addr1", PURPLE, "控制传输").move_to([2.3, -1.7, 0])
        b1 = Arrow(head.get_right(), ep0a.get_left(), buff=0.1, color=PURPLE)
        b2 = Arrow(ep0a.get_right(), ep0b.get_left(), buff=0.1, color=PURPLE)
        back = CurvedArrow(ep0b.get_bottom() + DOWN * 0.05, head.get_bottom() + DOWN * 0.05, angle=-PI / 4, color=PURPLE)
        asy = VGroup(head, ep0a, ep0b, b1, b2, back)
        self.play(LaggedStart(FadeIn(frames), FadeIn(fl), GrowArrow(a1), FadeIn(anchor), GrowArrow(a2),
                              FadeIn(kq), GrowArrow(a3), FadeIn(qtd), lag_ratio=0.2), run_time=2.5)
        self.play(LaggedStart(FadeIn(head), GrowArrow(b1), FadeIn(ep0a), GrowArrow(b2), FadeIn(ep0b), Create(back),
                              lag_ratio=0.2), run_time=2)
        # one frame tick sweeps the periodic list
        for i in range(3):
            hl = frames[i].copy().set_fill(YELLOW, 0.8)
            self.play(FadeIn(hl, run_time=0.15))
            dot = Dot(frames[i].get_center(), color=YELLOW)
            path = VMobject().set_points_as_corners([frames[i].get_center(), anchor.get_center(), kq.get_center(), qtd.get_center()])
            self.play(MoveAlongPath(dot, path), run_time=0.7)
            self.remove(dot, hl)
        n = self.say(None, "没按键时 qTD 收到 NAK、保持 ACTIVE；有报告时 qTD 完成 → USBSTS.USBINT → IRQ39")
        self.wait(2)
        self.play(FadeOut(per), FadeOut(asy), FadeOut(n), FadeOut(cover), FadeOut(ph))

    # ---- 3. data exchange ---------------------------------------------------------
    def report_bytes(self, mod, keys, col=TEAL):
        cells = VGroup()
        labels = ["mod", "rsv", "k1", "k2", "k3", "k4", "k5", "k6"]
        vals = [mod, 0] + keys
        for lab, v in zip(labels, vals):
            sq = Rectangle(width=0.62, height=0.5, color=col, fill_color=col, fill_opacity=0.25 if v == 0 else 0.7)
            t = M(f"{v:02X}", 16, WHITE).move_to(sq)
            l = T(lab, 11, GRAY_B).next_to(sq, DOWN, buff=0.04)
            cells.add(VGroup(sq, t, l))
        cells.arrange(RIGHT, buff=0.03)
        return cells

    def key_round(self, ttl, mod, keys, usage_txt, key_txt, console_txt, leds=None, release_txt=None):
        y = 1.4
        msgs = []
        rep = self.report_bytes(mod, keys).scale(0.85).move_to([-0.6, -3.15, 0])
        rl = T(f"8 字节 Boot 报告：{ttl}", 15, TEAL).next_to(rep, UP, buff=0.08)
        self.play(FadeIn(rep), FadeIn(rl), run_time=0.5)
        hexs = " ".join(f"{v:02X}" for v in [mod, 0] + keys)
        msgs.append(self.msg(0, 1, y, "IN 事务（每帧轮询，这次有数据）", TEAL, data=hexs, run=1.0)); y -= 0.47
        msgs.append(self.msg(1, 2, y, "qTD 完成 → USBSTS=USBINT → IRQ 39", BLUE)); y -= 0.47
        msgs.append(self.msg(2, 4, y, "ehci_intr_reap → urb->complete = usbkbd_irq", PURPLE)); y -= 0.47
        msgs.append(self.msg(4, 4, y, "queue_work(usbkbd_wq)  中断里只排队", ORANGE)); y -= 0.47
        msgs.append(self.msg(4, 4, y, f"rx_work: usbkbd_report 与上次比较 → {usage_txt}", ORANGE)); y -= 0.47
        if key_txt:
            msgs.append(self.msg(4, 5, y, key_txt, ORANGE)); y -= 0.47
        if console_txt:
            msgs.append(self.msg(5, 5, y, console_txt, GREEN)); y -= 0.47
        if leds is not None:
            msgs.append(self.msg(4, 0, y, f"SET_REPORT(输出) LED={leds:02X}  → Caps 灯", RED, data=f"21 09 00 02 … + {leds:02X}")); y -= 0.47
        msgs.append(self.msg(4, 2, y, "usb_submit_urb 重新挂 qTD（循环）", ORANGE, dashed=True)); y -= 0.47
        return msgs + [rep, rl]

    def data_exchange(self):
        ph = self.phase("⑤ 数据交换：按键 → 中断 IN 报告 → input 事件 → 控制台",
                        "QEMU monitor: sendkey a / shift-b / caps_lock / c / ret（日志 report #1…#11）")
        self.play(FadeIn(ph))
        rounds = [
            dict(ttl="按下 a", mod=0x00, keys=[0x04, 0, 0, 0, 0, 0], usage_txt="usage 0x04 按下",
                 key_txt="input_report_key(KEY_A=30, 1)", console_txt="kbd: KEY 30 → 控制台 'a' (0x61)"),
            dict(ttl="松开 a", mod=0x00, keys=[0, 0, 0, 0, 0, 0], usage_txt="usage 0x04 松开",
                 key_txt="input_report_key(KEY_A=30, 0)", console_txt=None),
            dict(ttl="Shift + b", mod=0x02, keys=[0x05, 0, 0, 0, 0, 0], usage_txt="mod 0xE1 LShift + usage 0x05",
                 key_txt="KEY_LEFTSHIFT=42 ↓, KEY_B=48 ↓", console_txt="kbd: KEY 48 mods=1 → 'B' (0x42)"),
            dict(ttl="Caps Lock", mod=0x00, keys=[0x39, 0, 0, 0, 0, 0], usage_txt="usage 0x39 → caps 翻转",
                 key_txt="KEY_CAPSLOCK=58 ↓", console_txt=None, leds=0x02),
            dict(ttl="c（Caps 开）", mod=0x00, keys=[0x06, 0, 0, 0, 0, 0], usage_txt="usage 0x06 按下",
                 key_txt="KEY_C=46 ↓", console_txt="kbd: KEY 46 caps=1 → 'C' (0x43)"),
        ]
        typed = T("控制台：", 20, GREEN).move_to([3.4, -3.25, 0])
        shown = M("$ ", 22, GREEN).next_to(typed, RIGHT, buff=0.1)
        self.play(FadeIn(typed), FadeIn(shown))
        out = {0: "a", 2: "B", 4: "C"}
        text = "$ "
        for i, r in enumerate(rounds):
            items = self.key_round(**r)
            if i in out:
                text += out[i]
                new = M(text, 22, GREEN).next_to(typed, RIGHT, buff=0.1)
                self.play(Transform(shown, new), run_time=0.4)
            self.wait(1.0 if i in (0, 3) else 0.5)
            self.play(*[FadeOut(m) for m in items], run_time=0.35)
        n = self.say(None, "Enter 后 shell 收到 \"aBC\"（exec aBC failed —— 没有这个程序，说明整条输入链路通了）", GREEN)
        self.wait(2.5)
        self.play(FadeOut(n), FadeOut(ph), FadeOut(typed), FadeOut(shown))

    def outro(self):
        self.play(*[FadeOut(m) for m in self.mobjects])
        lines = VGroup(
            T("小结", 34, YELLOW, weight=BOLD),
            T("1. ehci-enum 线程在启动时扫一次根端口：复位 → 6 个 EP0 控制传输完成枚举", 20),
            T("2. usb-core 按 class 3/1/1 匹配 usbkbd，probe 发 SET_PROTOCOL/SET_IDLE/SET_REPORT", 20),
            T("3. 中断 IN URB 挂在 periodic 调度，8 字节报告经 IRQ39 → workqueue → input → 控制台", 20),
            T("4. 不支持热插拔：开机后 device_add 的设备不会被枚举", 20, GRAY_B),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.3)
        self.play(FadeIn(lines, shift=UP * 0.2))
        self.wait(3)
