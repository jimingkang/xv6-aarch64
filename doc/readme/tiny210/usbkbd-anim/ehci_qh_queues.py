"""EHCI QH queues, steps 0-8: how usbkbd is loaded/configured and how
interrupt reports come in.  Animated version of newtiny210_ehci-qh-queues.svg
(doc/readme/tiny210/images/ehci-qh-queues.svg): Tiny210 board, S5PV210 EHCI,
USB2514 hub at address 1, low-speed keyboard at address 2 on hub port 3.

    manim -qh ehci_qh_queues.py EhciQhQueues
"""
from manim import *

FONT = "Heiti SC"
MONO = "Menlo"
BG = "#ffffff"
INK = "#24292f"
MUTED = "#57606a"
GREEN_C_ = "#1a7f37"
GREEN_BG = "#dafbe1"
BLUE_C_ = "#0969da"
BLUE_BG = "#ddf4ff"
YEL = "#bf8700"
YEL_BG = "#fff8c5"
RED_C_ = "#cf222e"
GRAY_BG = "#eaeef2"

config.background_color = BG


def T(s, size=18, color=INK, **kw):
    return Text(s, font=FONT, font_size=size, color=color, **kw)


def M(s, size=15, color=INK, **kw):
    return Text(s, font=MONO, font_size=size, color=color, **kw)


def box(w, h, stroke, fill, op=1.0, dashed=False, r=0.08):
    b = RoundedRectangle(width=w, height=h, corner_radius=r, stroke_color=stroke,
                         stroke_width=2, fill_color=fill, fill_opacity=op)
    if dashed:
        b = DashedVMobject(b, num_dashes=28)
        b = VGroup(RoundedRectangle(width=w, height=h, corner_radius=r, stroke_width=0,
                                    fill_color=fill, fill_opacity=op), b)
    return b


def qh_box(title, sub, stroke, fill=WHITE, w=2.55, h=0.62):
    b = box(w, h, stroke, fill)
    t = VGroup(T(title, 15, INK, weight=BOLD), T(sub, 11, MUTED)).arrange(DOWN, buff=0.03)
    if t.width > w - 0.15:
        t.scale_to_fit_width(w - 0.15)
    t.move_to(b)
    return VGroup(b, t)


def qtd_box(title, sub, active=True, dummy=False, w=0.85, h=0.5):
    if dummy:
        b = box(w, h, MUTED, GRAY_BG, dashed=True)
    else:
        b = box(w, h, YEL if active else MUTED, YEL_BG if active else WHITE)
    t = VGroup(T(title, 12, INK, weight=BOLD), T(sub, 9, MUTED)).arrange(DOWN, buff=0.02)
    if t.width > w - 0.08:
        t.scale_to_fit_width(w - 0.08)
    t.move_to(b)
    return VGroup(b, t)


def harrow(a, b, color=INK, buff=0.05):
    return Arrow(a, b, buff=buff, stroke_width=2.5, color=color, max_tip_length_to_length_ratio=0.25,
                 tip_length=0.14)


class EhciQhQueues(Scene):
    # ------------------------------------------------------------- layout
    def build_static(self):
        title = T("EHCI QH 队列收发：usbkbd 加载配置与中断数据接收（0–8 步）", 26, INK, weight=BOLD).move_to([0, 3.72, 0])
        legend = T("绿 = 异步调度（控制/批量）   蓝 = 周期调度（中断）   黄 qTD = Active 等 HC 执行   虚线 = dummy（Halted 队尾）",
                   12, MUTED).next_to(title, DOWN, buff=0.06)
        # register bar
        regs = [("USBCMD", "ITC(1)|PSE|ASE|RUN"), ("ASYNCLIST", "→ head_qh"),
                ("PERIODICLIST", "→ frame_list[1024]"), ("USBSTS", "USBINT/ERR → IRQ 55")]
        bar = box(13.8, 0.42, MUTED, "#f6f8fa").move_to([0, 3.0, 0])
        cells = VGroup()
        for i, (k, v) in enumerate(regs):
            c = VGroup(M(k, 12, INK, weight=BOLD), M(v, 11, MUTED)).arrange(RIGHT, buff=0.12)
            c.move_to([-5.2 + i * 3.45, 3.0, 0])
            cells.add(c)
        self.reg = {k: cells[i] for i, (k, _) in enumerate(regs)}
        # panels
        ap = box(6.85, 3.85, GREEN_C_, "#f6fff8").move_to([-3.5, 0.82, 0])
        at = T("异步调度（ASYNCLIST）：控制/批量端点，一个 QH 环", 14, GREEN_C_, weight=BOLD).move_to([-3.5, 2.55, 0])
        pp = box(6.85, 3.85, BLUE_C_, "#f5fbff").move_to([3.55, 0.82, 0])
        pt = T("周期调度（PERIODICLIST）：中断 IN，每帧走一遍", 14, BLUE_C_, weight=BOLD).move_to([3.55, 2.55, 0])
        # step panel
        sp = box(13.8, 2.95, MUTED, WHITE).move_to([0, -2.58, 0])
        self.add(title, legend, bar, cells, ap, at, pp, pt, sp)
        self.step_items = VGroup()

    def step_header(self, n, title, chain, lock):
        circ = Circle(radius=0.24, color=BLUE_C_, fill_color=BLUE_C_, fill_opacity=1).move_to([-6.5, -1.42, 0])
        num = T(str(n), 20, WHITE, weight=BOLD).move_to(circ)
        t = T(title, 19, INK, weight=BOLD).next_to(circ, RIGHT, buff=0.18).align_to(circ, UP).shift(DOWN * 0.02)
        if t.width > 9.5:
            t.scale_to_fit_width(9.5)
        c = M(chain, 12, BLUE_C_).next_to(t, DOWN, buff=0.07, aligned_edge=LEFT)
        if c.width > 12.6:
            c.scale_to_fit_width(12.6)
        lk = T("锁：" + lock, 12, YEL)
        if lk.width > 2.5:
            lk.scale_to_fit_width(2.5)
        lk.move_to([6.75, -1.38, 0], aligned_edge=RIGHT)
        return VGroup(circ, num, t, c, lk)

    def step_lines(self, lines, width=7.6, top=-2.05):
        g = VGroup(*[T("· " + s, 14, INK) for s in lines]).arrange(DOWN, aligned_edge=LEFT, buff=0.1)
        for m in g:
            if m.width > width:
                m.scale_to_fit_width(width)
        g.move_to([-6.75, top, 0], aligned_edge=UL)
        return g

    def show_step(self, n, title, chain, lock, lines, right=None, line_width=7.6):
        old = self.step_items
        head = self.step_header(n, title, chain, lock)
        body = self.step_lines(lines, width=line_width)
        new = VGroup(head, body)
        if right is not None:
            new.add(right)
        if len(old):
            self.wait(3.0)          # reading time for the previous step
            self.play(FadeOut(old, shift=UP * 0.15), run_time=0.35)
        self.play(FadeIn(head, shift=UP * 0.1), run_time=0.45)
        self.play(LaggedStart(*[FadeIn(l, shift=RIGHT * 0.1) for l in body], lag_ratio=0.25),
                  run_time=0.4 + 0.25 * len(lines))
        if right is not None:
            self.play(FadeIn(right), run_time=0.5)
        self.step_items = new

    def bitfield(self, title, fields, value=None, w=5.6):
        """fields: list of (label, nbits, text) msb..lsb."""
        total = sum(f[1] for f in fields)
        cells = VGroup()
        for lab, nb, txt in fields:
            r = Rectangle(width=w * nb / total, height=0.38, stroke_color=BLUE_C_, stroke_width=1.5,
                          fill_color=BLUE_BG, fill_opacity=1)
            l = M(lab, 10, INK)
            if l.width > r.width - 0.04:
                l.scale_to_fit_width(max(r.width - 0.04, 0.05))
            l.move_to(r)
            v = M(txt, 10, RED_C_)
            if v.width > r.width + 0.1:
                v.scale_to_fit_width(r.width + 0.1)
            cells.add(VGroup(r, l, v))
        cells.arrange(RIGHT, buff=0)
        for c in cells:
            c[2].next_to(c[0], DOWN, buff=0.04)
        hd = T(title, 13, BLUE_C_, weight=BOLD)
        g = VGroup(hd, cells).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        if value:
            g.add(M(value, 12, INK).next_to(cells, DOWN, buff=0.3, aligned_edge=LEFT))
        return g

    def bytes_row(self, title, items, color=BLUE_C_, fill=BLUE_BG, cw=0.62):
        cells = VGroup()
        for top, bot in items:
            r = Rectangle(width=cw, height=0.4, stroke_color=color, stroke_width=1.5, fill_color=fill, fill_opacity=1)
            t = M(top, 12, INK).move_to(r)
            b = T(bot, 9, MUTED).next_to(r, DOWN, buff=0.03)
            if t.width > cw - 0.05:
                t.scale_to_fit_width(cw - 0.05)
            if b.width > cw + 0.08:
                b.scale_to_fit_width(cw + 0.08)
            cells.add(VGroup(r, t, b))
        cells.arrange(RIGHT, buff=0.02)
        hd = T(title, 13, color, weight=BOLD)
        return VGroup(hd, cells).arrange(DOWN, aligned_edge=LEFT, buff=0.08)

    def place_right(self, m, top=-1.95):
        m.move_to([1.15, top, 0], aligned_edge=UL)
        if m.width > 5.5:
            m.scale_to_fit_width(5.5)
            m.move_to([1.15, top, 0], aligned_edge=UL)
        return m

    def packet(self, path_pts, label, color=YEL, run=1.0):
        d = Dot(path_pts[0], radius=0.07, color=color)
        tag = M(label, 10, INK)
        bg = BackgroundRectangle(tag, color=YEL_BG if color == YEL else BLUE_BG, fill_opacity=1, buff=0.04)
        t = VGroup(bg, tag)
        t.add_updater(lambda m: m.next_to(d, UP, buff=0.04))
        self.add(d, t)
        path = VMobject().set_points_as_corners(path_pts)
        self.play(MoveAlongPath(d, path), run_time=run, rate_func=smooth)
        t.clear_updaters()
        self.play(FadeOut(d), FadeOut(t), run_time=0.2)

    # ------------------------------------------------------------- scene
    def construct(self):
        self.build_static()
        self.step0()
        self.step1()
        self.step2()
        self.step3()
        self.step4()
        self.step5()
        self.step6()
        self.step7()
        self.step8()
        self.finale()

    # 0 ---------------------------------------------------------------
    def step0(self):
        log = M("ehci: 1 port(s), hccparams a010, running, async schedule on, periodic on", 11, MUTED)
        right = self.place_right(VGroup(T("预期日志", 12, MUTED, weight=BOLD), log).arrange(DOWN, aligned_edge=LEFT, buff=0.06), top=-2.0)
        self.show_step(0, "ehci_start：建两个调度的根——异步环头 head_qh、周期帧表 + anchor_qh",
                       "ehci-enum kthread → 写 ASYNCLIST / PERIODICLIST → USBCMD 打开 ASE + PSE",
                       "无（单线程初始化）",
                       ["head_qh = &qhs[0]：H=1、overlay Halted、horiz 指向自己，作环的“回收头”",
                        "anchor_qh = &qhs[1]：S-mask=0，HC 不执行它，只跟它的 horiz",
                        "frame_list[0..1023] 全部 = phys(anchor_qh)|Typ=QH（4 KB 对齐）",
                        "wr(USBCMD, ITC(1)|PSE|ASE|RUN)；CONFIGFLAG=1，端口归 EHCI"],
                       right=right)
        # async: head_qh with self loop
        self.head = qh_box("head_qh（环头）", "H=1，overlay Halted，从不执行", GREEN_C_, GRAY_BG).move_to([-5.45, 1.95, 0])
        loop = CurvedArrow(self.head[0].get_left() + DOWN * 0.15, self.head[0].get_left() + UP * 0.15,
                           angle=-PI * 1.2, color=GREEN_C_, stroke_width=2.5, tip_length=0.12)
        nxt = T("next = T（无 qTD）", 11, MUTED).next_to(self.head, RIGHT, buff=0.2)
        self.play(FadeIn(self.head, shift=DOWN * 0.1), Create(loop), FadeIn(nxt))
        self.head_loop = loop
        self.head_next = nxt
        # periodic: frame list + anchor
        labels = ["[0]", "[1]", "[2]", "[n] 当前帧", "…", "[1023]"]
        fl = VGroup()
        for i, s in enumerate(labels):
            r = Rectangle(width=1.05, height=0.3, stroke_color=BLUE_C_, stroke_width=1.5, fill_color=WHITE, fill_opacity=1)
            t = M(s, 11, RED_C_ if "n" in s else INK).move_to(r)
            fl.add(VGroup(r, t))
        fl.arrange(DOWN, buff=0).move_to([0.75, 1.35, 0])
        self.fl = fl
        self.anchor = qh_box("anchor_qh（帧尾锚）", "S-mask=0：不执行，只跟 horiz", BLUE_C_, GRAY_BG).move_to([2.95, 2.0, 0])
        fan = VGroup(*[Line(c[0].get_right(), self.anchor[0].get_left() + DOWN * 0.15, stroke_width=1.2, color=BLUE_C_)
                       for c in fl])
        self.endT = VGroup(box(1.5, 0.32, BLUE_C_, WHITE), M("horiz = T（结束）", 10, INK)).move_to([2.95, 0.95, 0])
        self.endT[1].move_to(self.endT[0])
        self.a_to_end = harrow(self.anchor.get_bottom(), self.endT.get_top(), BLUE_C_)
        self.play(LaggedStart(*[FadeIn(c) for c in fl], lag_ratio=0.1), run_time=0.8)
        self.play(FadeIn(self.anchor), LaggedStart(*[Create(l) for l in fan], lag_ratio=0.08), run_time=1.0)
        self.play(GrowArrow(self.a_to_end), FadeIn(self.endT))
        self.fan = fan
        for k in ("ASYNCLIST", "PERIODICLIST", "USBCMD"):
            self.play(Indicate(self.reg[k], color=BLUE_C_, scale_factor=1.08), run_time=0.45)
        self.wait(2.2)

    # 1 ---------------------------------------------------------------
    def step1(self):
        setup = self.bytes_row("SETUP 包：GET_DESCRIPTOR(CONFIG) = 80 06 0002 0000 0900",
                               [("80", "bmReqType"), ("06", "bRequest"), ("00 02", "wValue"),
                                ("00 00", "wIndex"), ("09 00", "wLength")], cw=1.02)
        right = self.place_right(setup)
        self.show_step(1, "枚举：每个设备在异步环上得到一个 EP0 QH，控制传输 = SETUP/DATA/STATUS 三个 qTD",
                       "enumerate → qh_create(addr,0,…,control=1,hub,port) → control_qh → qh_append → wait_qtd",
                       "睡眠锁 xfer_lock · 自旋锁 ehci_lock",
                       ["键盘是低速设备，挂在高速 Hub 后：EP0 QH 写 Hub=1/口3，C=1，EPS=01",
                        "HC 因此发 SSPLIT/CSPLIT 给 Hub 的 TT；DTC=1，翻转位由 qTD 给",
                        "控制传输是同步的：wait_qtd 忙等 STATUS qTD 的 Active 清零，超时 1 s",
                        "SET_ADDRESS(2) 后建地址 2 的 EP0 QH；地址 0 的旧 QH 留在环里空闲"],
                       right=right)
        # addr0 QH inserted after head
        q0 = qh_box("QH 地址0 EP0", "SET_ADDRESS 前用，之后空闲", GREEN_C_).move_to([-5.45, 1.05, 0])
        a0 = harrow(self.head.get_bottom(), q0.get_top(), GREEN_C_)
        d0 = qtd_box("dummy", "Halted", dummy=True).next_to(q0, RIGHT, buff=0.35)
        a0d = harrow(q0.get_right(), d0.get_left(), INK)
        self.play(FadeOut(self.head_next), FadeIn(q0, shift=DOWN * 0.2), GrowArrow(a0), FadeIn(d0), GrowArrow(a0d))
        self.packet([d0.get_center(), d0.get_center() + RIGHT * 2.0], "GET_DESCRIPTOR(DEV,8) → mps0=8", GREEN_C_, run=0.9)
        self.packet([d0.get_center(), d0.get_center() + RIGHT * 2.0], "SET_ADDRESS(2)", GREEN_C_, run=0.7)
        # addr2 QH inserted right after head: addr0 moves down
        q2 = qh_box("QH 地址2 EP0（键盘）", "LS，C=1，split → Hub1/口3", GREEN_C_).move_to([-5.45, 1.05, 0])
        grp0 = VGroup(q0, d0, a0d)
        self.play(grp0.animate.shift(DOWN * 0.95), FadeOut(a0), run_time=0.6)
        a2 = harrow(self.head.get_bottom(), q2.get_top(), GREEN_C_)
        a20 = harrow(q2.get_bottom(), q0.get_top(), GREEN_C_)
        self.play(FadeIn(q2, shift=DOWN * 0.2), GrowArrow(a2), GrowArrow(a20))
        ring = VMobject(color=GREEN_C_, stroke_width=2.5).set_points_as_corners([
            q0[0].get_left(), q0[0].get_left() + LEFT * 0.22, self.head[0].get_left() + LEFT * 0.22 + UP * 0.0,
            self.head[0].get_left()])
        self.play(FadeOut(self.head_loop), Create(ring), run_time=0.6)
        self.ring = ring
        # three control qTDs on addr2 QH
        s = qtd_box("SETUP 8 B", "PID=2，DT=0")
        d = qtd_box("DATA IN", "DT=1")
        st = qtd_box("STATUS", "OUT 0B，IOC")
        dm = qtd_box("dummy", "Halted", dummy=True)
        row = VGroup(s, d, st, dm).arrange(RIGHT, buff=0.18).next_to(q2, RIGHT, buff=0.3)
        arrs = VGroup(harrow(q2.get_right(), s.get_left()), *[harrow(row[i].get_right(), row[i + 1].get_left()) for i in range(3)])
        self.play(LaggedStart(*[FadeIn(x) for x in row], lag_ratio=0.2), LaggedStart(*[GrowArrow(a) for a in arrs], lag_ratio=0.2), run_time=1.2)
        for q, lab in ((s, "SSPLIT SETUP + DATA0 8B"), (d, "IN → DATA1 9 B"), (st, "OUT 0 B → ACK")):
            self.play(q[0].animate.set_fill(GRAY_BG), run_time=0.15)
            self.packet([q.get_top() + UP * 0.05, q.get_top() + UP * 0.45], lab, GREEN_C_, run=0.5)
            self.play(q[0].animate.set_stroke(MUTED), run_time=0.15)
        self.play(FadeOut(VGroup(s, d, st, arrs[1:], arrs[0])), dm.animate.next_to(q2, RIGHT, buff=0.35))
        self.q2, self.q2_dummy = q2, dm
        self.q2_arrow = harrow(q2.get_right(), dm.get_left())
        self.play(GrowArrow(self.q2_arrow), run_time=0.3)
        self.async_misc = VGroup(q0, d0, a0d, a2, a20)
        self.wait(1.2)

    # 2 ---------------------------------------------------------------
    def step2(self):
        cfg = self.bytes_row("键盘 59 字节配置描述符 = 9 + 2 ×（接口 9 + HID 9 + 端点 7）",
                             [("CONFIG", "9 B"), ("IF0", "3/1/1"), ("HID", "9 B"), ("EP 81", "IN 中断"),
                              ("IF1", "3/0/0?"), ("HID", "9 B"), ("EP 82", "IN 中断")], cw=0.76)
        for i in (1, 2, 3):
            cfg[1][i][0].set_fill("#ffebe9")
        right = self.place_right(cfg)
        self.show_step(2, "一个设备多个接口：键盘接口 1 共用地址 2；鼠标插上则是另一个地址",
                       "parse_config → ifdevs[] → usb_device_register；另一设备 → enumerate 新地址 → 自己的 EP0 QH",
                       "无新增锁",
                       ["接口 0（class 3/1/1，EP1 IN）→ usb-core 匹配 usbkbd → probe",
                        "接口 1（3/0/0，多半是多媒体键）建 ifdevs[k]，共用 ehci_dev 和 EP0 QH",
                        "3/0/0 没有驱动绑定，只多打一行日志；EP 0x82 不会建中断 QH",
                        "日志：usbkbd: 2a7a:8a47 keyboard on ep1 in, mps 8, interval 10"],
                       right=right)
        tag = VGroup(box(1.6, 0.36, ORANGE, "#fff1e5"), T("→ usbkbd_probe", 12, INK)).next_to(self.q2_dummy, RIGHT, buff=0.25)
        tag[1].move_to(tag[0])
        self.play(FadeIn(tag, shift=LEFT * 0.2))
        self.wait(2.3)
        self.play(FadeOut(tag))

    # 3 ---------------------------------------------------------------
    def step3(self):
        bf = self.bitfield("epcap（DW2）= 0x41811c01：Mult | Port | Hub | C-mask | S-mask",
                           [("Mult", 2, "1"), ("Port", 7, "3"), ("Hub", 7, "1"), ("C-mask", 8, "0x1c"), ("S-mask", 8, "0x01")],
                           value="epchar（DW1）= 0x00081102：RL=0 MPL=8 DTC=0 EPS=01(LS) EP=1 Addr=2")
        right = self.place_right(bf)
        self.show_step(3, "第一次 usb_submit_urb：qh_create_intr 建中断 QH，插到 anchor_qh 后面",
                       "usbkbd_probe → usb_submit_urb → ehci_submit_urb → qh_create_intr(2, 1, LOW, 8, hub 1, port 3)",
                       "自旋锁 ehci_lock（intrs[] 表）",
                       ["中断 QH：RL=0（周期 QH 必须 0），DTC=0（翻转位 HC 记在 overlay）",
                        "S-mask=0x01：每帧 µ0 发 SSPLIT；C-mask=0x1c：µ2/3/4 发 CSPLIT",
                        "插入只要两次 32 位写：① q->horiz = anchor->horiz",
                        "② dsb(); anchor->horiz = phys(q)|QH —— HC 任何时刻都看到完整链表"],
                       right=right)
        self.kq = qh_box("QH 地址2 EP1 IN（键盘）", "epchar 0x00081102 · epcap 0x41811c01", BLUE_C_).move_to([2.95, 0.95, 0])
        self.kq_dummy = qtd_box("dummy", "Halted", dummy=True).next_to(self.kq, RIGHT, buff=0.3)
        kd_arr = harrow(self.kq.get_right(), self.kq_dummy.get_left())
        # 1st write: new QH points at what anchor pointed to (T)
        self.play(self.endT.animate.move_to([2.95, -0.15, 0]), FadeOut(self.a_to_end), run_time=0.5)
        self.play(FadeIn(self.kq, shift=LEFT * 0.3), FadeIn(self.kq_dummy), GrowArrow(kd_arr))
        w1 = harrow(self.kq.get_bottom(), self.endT.get_top(), BLUE_C_)
        n1 = T("① q->horiz = anchor->horiz", 12, RED_C_).next_to(w1, RIGHT, buff=0.1)
        self.play(GrowArrow(w1), FadeIn(n1))
        w2 = harrow(self.anchor.get_bottom(), self.kq.get_top(), BLUE_C_)
        n2 = T("② anchor->horiz = q（发布）", 12, RED_C_).next_to(w2, RIGHT, buff=0.1)
        self.play(GrowArrow(w2), FadeIn(n2))
        self.play(Indicate(w2, color=RED_C_), run_time=0.6)
        self.wait(1.6)
        self.play(FadeOut(n1), FadeOut(n2))
        self.kd_arr = kd_arr
        self.per_arrows = VGroup(w1, w2)

    # 4 ---------------------------------------------------------------
    def step4(self):
        bf = self.bitfield("qTD token（DW2）挂上时 = 0x00088d80",
                           [("DT", 1, "0"), ("Bytes", 15, "8"), ("IOC", 1, "1"), ("C_Page", 3, "0"),
                            ("CERR", 2, "3"), ("PID", 2, "1 IN"), ("Status", 8, "0x80 Active")])
        right = self.place_right(bf)
        self.show_step(4, "qh_append：dummy qTD 法挂一个 qTD——HC 从不看到半成品",
                       "ehci_submit_urb → qtd_fill(report, 8, PID_IN|IOC) → qh_append(q, t, t) → 登记 intrs[slot]",
                       "自旋锁 ehci_lock（换 dummy 时）",
                       ["QH overlay 永远停在队尾 dummy 上（Halted，HC 读到就跳过）",
                        "① nd = qtd_alloc() 新 dummy；② 把 t 的内容复制进旧 dummy，旧->next = nd",
                        "③ q->dummy = nd; dsb(); 最后一次写：旧 dummy token |= ACTIVE ← 发布",
                        "原 qTD t 只是模板，qtd_free(t)；intrs[slot].qtd = d"],
                       right=right)
        old = self.kq_dummy
        nd = qtd_box("dummy", "新 · Halted", dummy=True).next_to(old, RIGHT, buff=0.22)
        if nd.get_right()[0] > 6.85:
            nd.shift(LEFT * (nd.get_right()[0] - 6.85))
        tmpl = qtd_box("t 模板", "IN 8B IOC", active=False).next_to(old, DOWN, buff=0.35)
        self.play(FadeIn(tmpl, shift=UP * 0.1))
        self.play(FadeIn(nd, shift=LEFT * 0.1))
        lk = harrow(old.get_right(), nd.get_left())
        self.play(tmpl.animate.move_to(old.get_center()).set_opacity(0.0), GrowArrow(lk), run_time=0.8)
        self.remove(tmpl)
        filled = qtd_box("IN 8 B", "IOC·URB 缓冲", active=False).move_to(old)
        self.play(FadeOut(old), FadeIn(filled), run_time=0.4)
        act = qtd_box("IN 8 B", "Active·IOC", active=True).move_to(old)
        flash = T("token |= ACTIVE  ← 发布", 12, RED_C_).next_to(act, DOWN, buff=0.12)
        self.play(ReplacementTransform(filled, act), FadeIn(flash), run_time=0.6)
        self.kq_qtd, self.kq_dummy, self.kq_link = act, nd, lk
        self.wait(1.8)
        self.play(FadeOut(flash))

    # 5 ---------------------------------------------------------------
    def step5(self):
        names = ["SSPLIT\nIN", "TT 执行\nLS IN", "CSPLIT", "CSPLIT", "CSPLIT", "异步", "异步", "异步"]
        cols = [GREEN_BG, YEL_BG, BLUE_BG, BLUE_BG, BLUE_BG, WHITE, WHITE, WHITE]
        cells = VGroup()
        for i, (nm, c) in enumerate(zip(names, cols)):
            r = Rectangle(width=0.68, height=0.52, stroke_color=MUTED, stroke_width=1.2, fill_color=c, fill_opacity=1)
            t = T(nm, 10, INK).move_to(r)
            u = M(f"µ{i}", 10, MUTED).next_to(r, DOWN, buff=0.03)
            cells.add(VGroup(r, t, u))
        cells.arrange(RIGHT, buff=0)
        strip = VGroup(T("一帧 1 ms = 8 个微帧，对低速中断 QH 的 split 事务", 13, BLUE_C_, weight=BOLD), cells).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        right = self.place_right(strip)
        self.show_step(5, "每 1 ms：HC 走一遍周期链表，对键盘 QH 做一次 split IN",
                       "HC：FRINDEX>>3 → frame_list[n] → anchor_qh → 键盘 QH → T；剩余时间去转异步环",
                       "无（纯硬件，CPU 不参与）",
                       ["QH overlay 指向的 qTD Active 且 S-mask 命中 µ0 → 发 SSPLIT IN 给 Hub1 的 TT",
                        "µ2–µ4 发 CSPLIT 取结果：NYET = TT 还没结果，下个 C 位再问",
                        "没按键 → NAK，qTD 保持 Active，下一帧再来；CPU 一个中断都收不到",
                        "有按键 → DATA0/1 8 B，HC DMA 进 report[]，token Active=0、Bytes 8→0"],
                       right=right)
        nidx = 3
        path = [self.fl[nidx].get_right(), self.anchor[0].get_left(), self.anchor.get_bottom(), self.kq.get_top(),
                self.kq[0].get_right() + LEFT * 0.1, self.kq_qtd.get_center()]
        # three NAK frames
        for k in range(2):
            hl = self.fl[nidx][0].copy().set_fill(YEL_BG, 1).set_stroke(RED_C_, 2)
            self.add(hl, self.fl[nidx][1])
            d = Dot(path[0], color=BLUE_C_, radius=0.07)
            self.add(d)
            self.play(MoveAlongPath(d, VMobject().set_points_as_corners(path)), run_time=0.9)
            nak = T("NAK", 12, RED_C_, weight=BOLD).next_to(self.kq_qtd, UP, buff=0.08)
            self.play(FadeIn(nak), cells[0][0].animate.set_stroke(RED_C_, 3), run_time=0.25)
            self.play(cells[2][0].animate.set_stroke(RED_C_, 3), run_time=0.15)
            self.play(FadeOut(nak), FadeOut(d), cells[0][0].animate.set_stroke(MUTED, 1.2),
                      cells[2][0].animate.set_stroke(MUTED, 1.2), run_time=0.25)
            self.remove(hl)
        # key pressed: data comes back
        hl = self.fl[nidx][0].copy().set_fill(YEL_BG, 1).set_stroke(RED_C_, 2)
        self.add(hl, self.fl[nidx][1])
        d = Dot(path[0], color=BLUE_C_, radius=0.07)
        self.add(d)
        self.play(MoveAlongPath(d, VMobject().set_points_as_corners(path)), run_time=0.9)
        for i in (0, 1, 2, 3):
            self.play(cells[i][0].animate.set_stroke(RED_C_, 3), run_time=0.15)
        data = VGroup(box(2.25, 0.34, YEL, YEL_BG), M("DATA1: 00 00 28 00 00 00 00 00", 10, INK))
        data[1].move_to(data[0])
        data.next_to(self.kq_qtd, UP, buff=0.3).shift(LEFT * 0.4)
        self.play(FadeIn(data, shift=DOWN * 0.15))
        self.play(data.animate.scale(0.3).move_to(self.kq_qtd.get_center()).set_opacity(0), run_time=0.6)
        done = qtd_box("IN 8 B", "Active=0 完成", active=False).move_to(self.kq_qtd)
        done[0].set_stroke(GREEN_C_, 2.5)
        self.play(ReplacementTransform(self.kq_qtd, done), FadeOut(d), run_time=0.4)
        self.kq_qtd = done
        for i in (0, 1, 2, 3):
            cells[i][0].set_stroke(MUTED, 1.2)
        self.remove(hl)
        self.wait(1.4)

    # 6 ---------------------------------------------------------------
    def step6(self):
        bf = self.bitfield("完成后的 token = 0x80008d00（QEMU 日志 token=8d00 / -7fff7300）",
                           [("DT", 1, "1"), ("Bytes", 15, "0 剩余"), ("IOC", 1, "1"), ("C_Page", 3, "0"),
                            ("CERR", 2, "3"), ("PID", 2, "IN"), ("Status", 8, "0x00 OK")],
                           value="实收 = 8 − 剩余 0 = 8 B；Status≠0 → urb->status=-1")
        right = self.place_right(bf)
        self.show_step(6, "IRQ 55：ehci_intr_reap 找到 Active=0 的 qTD，摘下来把 URB 交还驱动",
                       "GIC IRQ 55 → ehci_irq → ehci_intr_reap → usb_hcd_giveback_urb → usbkbd_irq → queue_work",
                       "ehci_lock · kbd->lock",
                       ["中断只说“有 qTD 完成了”，不说是哪个：reap 扫 intrs[4] 每个槽",
                        "摘下后 overlay 已前进到 dummy，队列回到“只剩 dummy”的空闲状态",
                        "len = d->length − TOK_LEFT(tok)；qtd_free(d)",
                        "中断上下文只做摘 qTD + queue_work(&usbkbd_wq, &kbd->rx_work)，几微秒退出"],
                       right=right)
        chain = ["USBSTS.USBINT", "ehci_irq\nW1C 清零", "ehci_intr_reap\n扫 intrs[4]", "giveback_urb\n→ usbkbd_irq",
                 "queue_work\nusbkbd_wq"]
        nodes = VGroup()
        for i, s in enumerate(chain):
            b = box(1.22, 0.62, YEL, YEL_BG)
            t = T(s, 10, INK).move_to(b)
            nodes.add(VGroup(b, t))
        nodes.arrange(RIGHT, buff=0.12).move_to([-3.5, -0.55, 0])
        cover = Rectangle(width=6.75, height=0.85, fill_color="#f6fff8", fill_opacity=1, stroke_width=0).move_to([-3.5, -0.55, 0])
        arrs = VGroup(*[harrow(nodes[i].get_right(), nodes[i + 1].get_left(), YEL, buff=0.02) for i in range(4)])
        self.play(Indicate(self.reg["USBSTS"], color=RED_C_), run_time=0.5)
        self.play(FadeIn(cover), run_time=0.2)
        self.play(LaggedStart(*[FadeIn(n) for n in nodes], lag_ratio=0.25),
                  LaggedStart(*[GrowArrow(a) for a in arrs], lag_ratio=0.25), run_time=1.6)
        slots = VGroup()
        for i, s in enumerate(["slot0 键盘", "slot1 鼠标", "slot2 空", "slot3 空"]):
            b = box(1.0, 0.3, MUTED, WHITE)
            t = T(s, 10, INK).move_to(b)
            slots.add(VGroup(b, t))
        slots.arrange(RIGHT, buff=0.08).move_to([3.55, -0.62, 0])
        self.play(FadeIn(slots))
        for s in slots:
            self.play(s[0].animate.set_stroke(RED_C_, 2.5), run_time=0.15)
            self.play(s[0].animate.set_stroke(MUTED, 2), run_time=0.1)
        self.play(slots[0][0].animate.set_fill(YEL_BG, 1).set_stroke(YEL, 2.5), run_time=0.3)
        # unlink the finished qTD: queue back to only-dummy
        self.play(FadeOut(self.kq_qtd, shift=UP * 0.3), FadeOut(self.kq_link), run_time=0.6)
        self.play(self.kq_dummy.animate.next_to(self.kq, RIGHT, buff=0.3), run_time=0.5)
        self.wait(1.4)
        self.irq_chain = VGroup(cover, nodes, arrs)
        self.slots = slots

    # 7 ---------------------------------------------------------------
    def step7(self):
        rep = self.bytes_row("键盘 boot 报告（DMA 进 usbkbd.report[]）：keys=28 → Enter",
                             [("00", "mod"), ("00", "rsv"), ("28", "key1"), ("00", "key2"), ("00", "key3"),
                              ("00", "key4"), ("00", "key5"), ("00", "key6")], color=YEL, fill=YEL_BG, cw=0.6)
        rep[1][2][0].set_fill("#ffd8b5")
        sr = M("Caps Lock: SETUP 21 09 0002 0000 0100 → OUT 1 B → STATUS", 10, RED_C_)
        right = self.place_right(VGroup(rep, sr).arrange(DOWN, aligned_edge=LEFT, buff=0.15))
        self.show_step(7, "工作线程处理报告，再 usb_submit_urb：同一个 QH 上又挂一个 qTD",
                       "kworker(usbkbd_wq) → usbkbd_rx_work → usbkbd_report → input_report_key … input_sync → usb_submit_urb",
                       "kbd->lock · input_dev.lock · ehci_lock",
                       ["进程上下文：和上一份报告比较 → KEY 28 down → kbd handler → consoleintr",
                        "每个 URB 永远只有一个 qTD 在 QH 上：处理完才重挂，报告不乱序",
                        "处理期间设备新数据被 NAK 着等一下（设备自己缓存），对键盘足够",
                        "Caps Lock 时 rx_work 在 EP0 QH 上做 SET_REPORT——这条链里唯一的“发”"],
                       right=right)
        # process context node replaces chain
        proc = VGroup(box(2.6, 0.62, BLUE_C_, BLUE_BG), T("rx_work：解析报告\n→ input core → 控制台", 11, INK))
        proc[1].move_to(proc[0])
        proc.move_to([-4.6, -0.55, 0])
        sub = VGroup(box(2.4, 0.62, BLUE_C_, BLUE_BG), T("usb_submit_urb\nqh_append 再挂 1 个 qTD", 11, INK))
        sub[1].move_to(sub[0])
        sub.move_to([-1.7, -0.55, 0])
        a = harrow(proc.get_right(), sub.get_left(), BLUE_C_)
        cover = Rectangle(width=6.75, height=0.85, fill_color="#f6fff8", fill_opacity=1, stroke_width=0).move_to([-3.5, -0.55, 0])
        self.play(FadeOut(self.irq_chain), FadeIn(cover), run_time=0.4)
        self.play(FadeIn(proc), run_time=0.5)
        self.play(GrowArrow(a), FadeIn(sub), run_time=0.6)
        # back to step 4: re-append on the periodic QH
        back = DashedLine(sub.get_right(), self.kq_dummy.get_bottom(), color=BLUE_C_, stroke_width=2)
        lab = T("虚线：回到第 4 步的 dummy 位置", 11, BLUE_C_).next_to(sub, DOWN, buff=0.04)
        self.play(Create(back), FadeIn(lab), run_time=0.7)
        newq = qtd_box("IN 8 B", "Active·IOC", active=True).move_to(self.kq_dummy)
        nd = qtd_box("dummy", "Halted", dummy=True).next_to(newq, RIGHT, buff=0.22)
        lk = harrow(newq.get_right(), nd.get_left())
        self.play(FadeOut(self.kq_dummy), FadeIn(newq), FadeIn(nd), GrowArrow(lk), run_time=0.7)
        self.kq_qtd, self.kq_dummy, self.kq_link = newq, nd, lk
        self.play(self.slots[0][0].animate.set_fill(WHITE, 1).set_stroke(MUTED, 2), run_time=0.2)
        # Caps Lock: SET_REPORT on the EP0 QH (async)
        s = qtd_box("SETUP 8 B", "21 09 0002…")
        d = qtd_box("DATA OUT", "1 B LED")
        st = qtd_box("STATUS IN", "0 B，IOC")
        row = VGroup(s, d, st).arrange(RIGHT, buff=0.16).next_to(self.q2, RIGHT, buff=0.3)
        self.play(self.q2_dummy.animate.next_to(row, RIGHT, buff=0.16), FadeOut(self.q2_arrow), run_time=0.4)
        arrs = VGroup(harrow(self.q2.get_right(), s.get_left()), harrow(s.get_right(), d.get_left()),
                      harrow(d.get_right(), st.get_left()), harrow(st.get_right(), self.q2_dummy.get_left()))
        self.play(LaggedStart(*[FadeIn(x) for x in row], lag_ratio=0.2), LaggedStart(*[GrowArrow(x) for x in arrs], lag_ratio=0.2), run_time=1.0)
        self.packet([d.get_top() + UP * 0.05, d.get_top() + UP * 0.45], "LED = 02（Caps）", RED_C_, run=0.6)
        self.play(FadeOut(row), FadeOut(arrs), self.q2_dummy.animate.next_to(self.q2, RIGHT, buff=0.35), run_time=0.5)
        self.q2_arrow = harrow(self.q2.get_right(), self.q2_dummy.get_left())
        self.play(GrowArrow(self.q2_arrow), run_time=0.3)
        self.wait(1.2)
        self.proc_items = VGroup(cover, proc, sub, a, back, lab)

    # 8 ---------------------------------------------------------------
    def step8(self):
        logs = VGroup(T("预期日志", 12, MUTED, weight=BOLD),
                      M("usbkbd: interrupt transfer error; resubmitting", 11, MUTED),
                      M("usbkbd: 32 transfer errors in a row; keyboard stopped", 11, MUTED),
                      M("ehci: control 21/9 val=200 failed token=…", 11, MUTED)).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        right = self.place_right(logs)
        self.show_step(8, "出错和撤销：Halted 的 QH 要先停调度再 qh_flush，并保留数据翻转位",
                       "ehci_submit_urb（发现 Halted）/ ehci_kill_urb / 控制超时 → periodic_stop 或 async_stop → qh_flush",
                       "ehci_lock；停调度期间 HC 不碰 QH",
                       ["STALL 或 CERR 减到 0 → token Halted，HC 不再执行，整条队列卡住",
                        "periodic_stop：USBCMD.PSE=0，等 USBSTS.PSS=0",
                        "qh_flush(q, dummy)：释放 qTD，overlay 指回 dummy，token &= DT（保留翻转位）",
                        "periodic_start；kill_urb（拔出/remove）也是“停调度 → qh_flush → 开调度”"],
                       right=right)
        self.play(FadeOut(self.proc_items), run_time=0.3)
        halted = qtd_box("IN 8 B", "Halted（CERR=0）", active=False).move_to(self.kq_qtd)
        halted[0].set_stroke(RED_C_, 3).set_fill("#ffebe9")
        self.play(ReplacementTransform(self.kq_qtd, halted), self.kq[0].animate.set_stroke(RED_C_, 3), run_time=0.6)
        pse = T("PSE=0 周期调度停", 12, RED_C_, weight=BOLD).next_to(self.anchor, LEFT, buff=0.15).shift(DOWN * 0.45)
        self.play(Indicate(self.reg["USBCMD"], color=RED_C_), FadeIn(pse), self.fan.animate.set_stroke(opacity=0.2), run_time=0.7)
        self.play(FadeOut(halted, shift=DOWN * 0.2), FadeOut(self.kq_link), self.kq_dummy.animate.next_to(self.kq, RIGHT, buff=0.3), run_time=0.6)
        dt = T("qh_flush：overlay → dummy，保留 DT", 12, BLUE_C_).move_to([4.6, 0.4, 0])
        self.play(FadeIn(dt), self.kq[0].animate.set_stroke(BLUE_C_, 2), run_time=0.5)
        self.play(FadeOut(pse), self.fan.animate.set_stroke(opacity=1), run_time=0.5)
        newq = qtd_box("IN 8 B", "Active·IOC", active=True).move_to(self.kq_dummy)
        nd = qtd_box("dummy", "Halted", dummy=True).next_to(newq, RIGHT, buff=0.22)
        lk = harrow(newq.get_right(), nd.get_left())
        self.play(FadeOut(self.kq_dummy), FadeIn(newq), FadeIn(nd), GrowArrow(lk), run_time=0.6)
        self.wait(2.0)
        self.play(FadeOut(dt))

    def finale(self):
        self.wait(3.0)
        self.play(FadeOut(self.step_items), run_time=0.4)
        summary = VGroup(
            T("小结：控制走异步环（同步等待），键盘报告走周期调度（中断驱动）", 18, INK, weight=BOLD),
            T("0 建根 → 1 EP0 枚举 → 2 解析接口 → 3 中断 QH 挂到 anchor 后 → 4 dummy 法挂 qTD", 15, INK),
            T("→ 5 HC 每 1 ms split IN → 6 IRQ55 摘 qTD → 7 rx_work 处理并重挂 → 8 Halted 时停调度 qh_flush", 15, INK),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.18).move_to([0, -2.55, 0])
        self.play(FadeIn(summary, shift=UP * 0.1))
        self.wait(3.5)
