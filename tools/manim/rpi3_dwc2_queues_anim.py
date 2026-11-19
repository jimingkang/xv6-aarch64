"""DWC2 channel "queues" on the Raspberry Pi 3B+, animated -- counterpart of
the tiny210 ehci_qh_queues.py.  Static version: doc/readme/rpi3/images/
dwc2-channel-queues.svg.  Values from doc/readme/rpi3/logs/pi3bplus-*.log.

    manim -qh --media_dir build/manim tools/manim/rpi3_dwc2_queues_anim.py Dwc2Queues
    cp build/manim/videos/rpi3_dwc2_queues_anim/1080p60/Dwc2Queues.mp4 \
       doc/readme/rpi3/videos/dwc2-channel-queues.mp4
"""
from manim import *

FONT, MONO = "Heiti SC", "Menlo"
INK, MUTED = "#24292f", "#57606a"
GREEN, GREEN_BG = "#1a7f37", "#dafbe1"
BLUE, BLUE_BG = "#0969da", "#ddf4ff"
YEL, YEL_BG = "#bf8700", "#fff8c5"
RED, RED_BG = "#cf222e", "#ffebe9"
PUR, PUR_BG = "#8250df", "#fbefff"
GRAY_BG = "#eaeef2"
config.background_color = WHITE


def T(s, size=18, color=INK, **kw):
    return Text(s, font=FONT, font_size=size, color=color, **kw)


def M(s, size=15, color=INK):
    return Text(s, font=MONO, font_size=size, color=color)


def box(w, h, stroke, fill, op=1.0, r=0.08):
    return RoundedRectangle(width=w, height=h, corner_radius=r, stroke_color=stroke, stroke_width=2,
                            fill_color=fill, fill_opacity=op)


def card(title, sub, stroke, fill, w, h, ts=15, ss=11):
    b = box(w, h, stroke, fill)
    g = VGroup(T(title, ts, INK, weight=BOLD), T(sub, ss, MUTED)).arrange(DOWN, buff=0.04)
    if g.width > w - 0.12:
        g.scale_to_fit_width(w - 0.12)
    g.move_to(b)
    return VGroup(b, g)


class Dwc2Queues(Scene):
    def construct(self):
        self.note_m = None
        self.title()
        self.layout()
        self.step0()
        self.step1_control()
        self.step2_submit()
        self.step3_idle()
        self.step4_key()
        self.step5_roundrobin()
        self.step6_hub()
        self.compare()

    # ----------------------------------------------------------- helpers
    def note(self, s, color=INK):
        n = T(s, 17, color)
        if n.width > 13.4:
            n.scale_to_fit_width(13.4)
        bg = box(13.8, 0.6, MUTED, "#f6f8fa").move_to([0, -3.62, 0])
        n.move_to(bg)
        g = VGroup(bg, n)
        if self.note_m is None:
            self.play(FadeIn(g), run_time=0.3)
        else:
            self.play(FadeOut(self.note_m), FadeIn(g), run_time=0.3)
        self.note_m = g

    def step_title(self, s):
        t = T(s, 22, INK, weight=BOLD).to_edge(UP, buff=0.12)
        if getattr(self, "st", None) is not None:
            self.play(ReplacementTransform(self.st, t), run_time=0.4)
        else:
            self.play(FadeIn(t), run_time=0.4)
        self.st = t

    def set_reg(self, key, text_, color=INK):
        old = self.regv[key]
        new = M(text_, 13, color).move_to(old, aligned_edge=LEFT)
        self.play(Transform(old, new), run_time=0.35)

    def flash(self, mob, color=RED):
        self.play(Indicate(mob, color=color, scale_factor=1.06), run_time=0.6)

    def pkt(self, a, b, label, color, run=0.7):
        d = VGroup(Dot(radius=0.07, color=color), T(label, 12, color)).arrange(RIGHT, buff=0.06).move_to(a)
        self.add(d)
        self.play(d.animate.move_to(b), run_time=run)
        self.remove(d)

    def title(self):
        t = T("DWC2 通道“队列”：树莓派 3B+ 上 USB 键盘怎么被轮询", 34, INK, weight=BOLD)
        s = T("对照 tiny210 的 EHCI QH 动画 · DWC2 没有 QH/qTD/帧表，只有 8 个通道寄存器组 + 驱动的请求表 + workqueue", 17, MUTED)
        g = VGroup(t, s).arrange(DOWN, buff=0.3)
        self.play(Write(t), run_time=1.2)
        self.play(FadeIn(s))
        self.wait(2)
        self.play(FadeOut(g))

    # ----------------------------------------------------------- layout
    def layout(self):
        # topology strip (top right)
        self.topo = VGroup()
        tx = [2.3, 4.25, 6.1]
        names = [("DWC2", "根口 · 高速"), ("Hub addr 1", "TT · 口 3"), ("键盘 addr 4", "低速")]
        cols = [BLUE, YEL, GREEN]
        self.tnode = []
        for (a, b), x, c in zip(names, tx, cols):
            n = card(a, b, c, WHITE, 1.65, 0.62, 13, 10).move_to([x, 2.95, 0])
            self.tnode.append(n)
            self.topo.add(n)
        self.topo.add(Line([3.13, 2.95, 0], [3.42, 2.95, 0], color=BLUE, stroke_width=4),
                      DashedLine([5.08, 2.95, 0], [5.27, 2.95, 0], color=GREEN, stroke_width=3, dash_length=0.05))
        # channels column (left)
        self.ch = {}
        chans = [(0, "ch0 控制", "CPU 轮询 HCINT0", GREEN, GREEN_BG), (6, "ch6 中断 IN", "HID：键盘/鼠标", BLUE, BLUE_BG),
                 (7, "ch7 Hub 状态", "热插拔位图", PUR, PUR_BG), (1, "ch1–5 批量", "usbnet / MT7601U", MUTED, GRAY_BG)]
        col = VGroup()
        for i, (n, a, b, c, f) in enumerate(chans):
            k = card(a, b, c, f, 2.4, 0.66, 14, 10).move_to([-5.6, 2.2 - i * 0.82, 0])
            self.ch[n] = k
            col.add(k)
        # ch6 register block
        self.regv = {}
        rows = [("HCSPLT6", "--------"), ("HCCHAR6", "--------"), ("HCTSIZ6", "--------"),
                ("HCDMA6", "--------"), ("HCINT6", "--------"), ("HAINTMSK", "--------")]
        rg = VGroup()
        for i, (k, v) in enumerate(rows):
            kl = M(k, 13, BLUE).move_to([-6.1, -1.25 - i * 0.33, 0], aligned_edge=LEFT)
            vl = M(v, 13, INK).move_to([-4.45, -1.25 - i * 0.33, 0], aligned_edge=LEFT)
            self.regv[k] = vl
            rg.add(kl, vl)
        rbox = SurroundingRectangle(rg, color=BLUE, buff=0.12, corner_radius=0.06)
        rlab = T("通道 6 寄存器", 13, BLUE).next_to(rbox, UP, buff=0.05, aligned_edge=LEFT)
        self.regs = VGroup(rbox, rlab, rg)
        # request table (middle)
        self.slots = []
        sg = VGroup()
        for i in range(4):
            s = card(f"interrupt_rx[{i}]", "空", MUTED, GRAY_BG, 3.0, 0.56, 13, 10).move_to([-0.9, 1.95 - i * 0.68, 0])
            self.slots.append(s)
            sg.add(s)
        slab = T("请求表 interrupt_rx[4]（usb_lock）", 13, INK).next_to(sg, UP, buff=0.1)
        self.inflight = M("inflight = -1", 13, MUTED).next_to(sg, DOWN, buff=0.12)
        # workqueues (right)
        self.wq = {}
        wqs = [("irq", "IRQ 9 · dwc2_irq", "中断上下文", YEL, YEL_BG),
               ("sys", "system_wq", "dwc2_interrupt_urb_work", BLUE, BLUE_BG),
               ("per", "dwc2_periodic_wq", "retry_work（bInterval）", BLUE, BLUE_BG),
               ("kbd", "usbkbd_wq", "usbkbd_rx_work", BLUE, BLUE_BG),
               ("tty", "consoleintr / evdev", "sh 读到字符", GREEN, GREEN_BG)]
        wg = VGroup()
        for i, (k, a, b, c, f) in enumerate(wqs):
            w = card(a, b, c, f, 2.9, 0.62, 13, 10).move_to([4.2, 1.85 - i * 0.78, 0])
            self.wq[k] = w
            wg.add(w)
        # microframe timeline
        self.uf = VGroup()
        for i in range(8):
            r = Rectangle(width=0.62, height=0.42, stroke_color=MUTED, stroke_width=1.2,
                          fill_color=WHITE, fill_opacity=1).move_to([-1.75 + i * 0.62, -2.55, 0])
            self.uf.add(VGroup(r, T(f"µ{i}", 10, MUTED).next_to(r, DOWN, buff=0.03)))
        ufl = T("一帧 1 ms = 8 个微帧（125 µs）", 12, MUTED).next_to(self.uf, UP, buff=0.08)
        self.report = VGroup(*[VGroup(Square(0.34, color=GREEN, fill_color=WHITE, fill_opacity=1),
                                      M("--", 11)) for _ in range(8)]).arrange(RIGHT, buff=0.02)
        for c in self.report:
            c[1].move_to(c[0])
        self.report.move_to([4.2, -2.55, 0])
        rl = T("report[8]（DMA 目标）", 12, GREEN).next_to(self.report, UP, buff=0.06)
        self.play(FadeIn(self.topo), FadeIn(col), run_time=0.8)
        self.play(FadeIn(sg), FadeIn(slab), FadeIn(self.inflight), FadeIn(self.regs), run_time=0.8)
        self.play(FadeIn(wg), FadeIn(self.uf), FadeIn(ufl), FadeIn(self.report), FadeIn(rl), run_time=0.8)

    # ----------------------------------------------------------- steps
    def step0(self):
        self.step_title("0  dwc2_init：主机模式、只开 HCHINT、8 个通道各有分工")
        self.note("GAHBCFG=DMA_EN|GLBL_INTR_EN · GINTMSK=HCHINT · HPRT0=0x100f（高速、已使能）· 没有帧表要建")
        self.wait(1.5)
        for k, s in [(0, "ch0：所有控制传输，CPU 写寄存器后忙等 HCINT0（不进中断）"),
                     (6, "ch6：HID 中断 IN，最多 4 个端点轮流用 —— 这就是 EHCI 周期调度的替代品"),
                     (7, "ch7：Hub 状态变化端点，插拔时回非 0 位图"),
                     (1, "ch1–5：批量传输（网卡、WiFi），和键盘无关")]:
            self.flash(self.ch[k], BLUE)
            self.note(s)
            self.wait(1.4)

    def step1_control(self):
        self.step_title("1  枚举：ch0 上的 split 控制传输（SETUP / DATA / STATUS，每段 SSPLIT + CSPLIT）")
        self.flash(self.ch[0], GREEN)
        dw, hb, kb = (n.get_center() for n in self.tnode)
        for stage, lab in [("SETUP", "80 06 00 01 00 00 08 00"), ("DATA", "12 01 10 01 00 00 00 08"), ("STATUS", "0 B")]:
            self.note(f"{stage} 阶段：SSPLIT 把事务交给 Hub 口 3 的 TT → TT 回 ACK（HCINT0=0x22）")
            self.pkt(dw, hb, "SSPLIT", BLUE)
            self.pkt(hb, dw, "ACK", YEL, 0.5)
            self.pkt(hb, kb, stage, GREEN, 0.5)
            self.pkt(kb, hb, lab if stage == "DATA" else "ACK", GREEN, 0.5)
            self.note("125 µs 后 CSPLIT 取结果" + ("：DATA1 8 字节，HCINT0=0x21" if stage == "DATA" else "：ACK"))
            self.pkt(dw, hb, "CSPLIT", BLUE, 0.5)
            self.pkt(hb, dw, lab if stage == "DATA" else "ACK", YEL, 0.6)
        self.note("同样的方式完成 SET_ADDRESS(4)、配置描述符 59 B、SET_CONFIGURATION、SET_PROTOCOL、SET_IDLE —— 不需要队列，全是同步的")
        self.wait(2)

    def step2_submit(self):
        self.step_title("2  usbkbd_probe → usb_submit_urb：请求进表，调度器把通道 6 给它")
        new = card("interrupt_rx[0]", "键盘 urb · pending", YEL, YEL_BG, 3.0, 0.56, 13, 10).move_to(self.slots[0])
        self.note("dwc2_submit_urb：找绑定此 URB 的槽或空槽 → pending=1（这一步像 EHCI 的 qh_append 挂 qTD）")
        self.play(Transform(self.slots[0], new))
        self.wait(1)
        self.note("dwc2_interrupt_schedule_locked：ch6 空闲 → 从 rotate 开始找 pending 槽 → inflight，写通道 6 寄存器")
        new = card("interrupt_rx[0]", "键盘 urb · inflight（占 ch6）", BLUE, BLUE_BG, 3.0, 0.56, 13, 10).move_to(self.slots[0])
        self.play(Transform(self.slots[0], new), Transform(self.inflight, M("inflight = 0", 13, BLUE).move_to(self.inflight)))
        for k, v in [("HCSPLT6", "8000c083"), ("HCCHAR6", "810e8808"), ("HCTSIZ6", "00080008"),
                     ("HCDMA6", "c03de080"), ("HCINT6", "00000000"), ("HAINTMSK", "|= 1<<6")]:
            self.set_reg(k, v, BLUE)
        self.note("HCCHAR6 的 ChEna 一写，DWC2 就在下一个合适的微帧发 SSPLIT（这里没有帧表，只有这一个事务）")
        self.flash(self.ch[6], BLUE)
        self.wait(1.2)

    def uf_mark(self, i, label, color):
        c = self.uf[i][0].copy().set_fill(color, 0.6)
        t = T(label, 10, INK).move_to(c)
        g = VGroup(c, t)
        self.play(FadeIn(g), run_time=0.25)
        return g

    def step3_idle(self):
        self.step_title("3  没按键：SSPLIT → ACK（IRQ 快路径）→ µ2 CSPLIT → NAK → 10 ms 后再来")
        dw, hb, kb = (n.get_center() for n in self.tnode)
        for rnd in range(2):
            marks = [self.uf_mark(0, "SSPLIT", GREEN_BG)]
            self.pkt(dw, hb, "SSPLIT IN", BLUE, 0.5)
            self.pkt(hb, dw, "ACK", YEL, 0.4)
            self.set_reg("HCINT6", "00000022", YEL)
            self.flash(self.wq["irq"], YEL)
            if rnd == 0:
                self.note("HCINT6=0x22 → IRQ 9 → dwc2_interrupt_rx_irq_fast()：中断里等到 µ2，置 CompSplt 再 ChEna（kworker 太慢会错过）")
                self.wait(1.6)
            marks.append(self.uf_mark(1, "TT IN", YEL_BG))
            self.pkt(hb, kb, "IN", GREEN, 0.4)
            self.pkt(kb, hb, "NAK", RED, 0.4)
            self.set_reg("HCSPLT6", "8001c083", BLUE)
            marks.append(self.uf_mark(2, "CSPLIT", BLUE_BG))
            self.pkt(dw, hb, "CSPLIT", BLUE, 0.4)
            self.pkt(hb, dw, "NAK", RED, 0.4)
            self.set_reg("HCINT6", "00000012", RED)
            self.flash(self.wq["irq"], YEL)
            self.flash(self.wq["sys"], BLUE)
            if rnd == 0:
                self.note("HCINT6=0x12（NAK|Hlt）：IRQ 屏蔽 b6 → system_wq 里 release ch6，mod_delayed_work(retry_work, 1 jiffy = 10 ms)")
            new = card("interrupt_rx[0]", "等 bInterval（retry_work）", MUTED, GRAY_BG, 3.0, 0.56, 13, 10).move_to(self.slots[0])
            self.play(Transform(self.slots[0], new),
                      Transform(self.inflight, M("inflight = -1", 13, MUTED).move_to(self.inflight)))
            self.flash(self.wq["per"], BLUE)
            if rnd == 0:
                self.wait(1.2)
                self.note("10 ms 后 dwc2_interrupt_retry_work：槽重新 pending → schedule_locked → 再 arm 一次 SSPLIT（NAK 不翻 DATA toggle）")
            new = card("interrupt_rx[0]", "键盘 urb · inflight（占 ch6）", BLUE, BLUE_BG, 3.0, 0.56, 13, 10).move_to(self.slots[0])
            self.play(Transform(self.slots[0], new),
                      Transform(self.inflight, M("inflight = 0", 13, BLUE).move_to(self.inflight)),
                      *[FadeOut(m) for m in marks])
            self.set_reg("HCSPLT6", "8000c083", BLUE)
            self.wait(0.6)

    def step4_key(self):
        self.step_title("4  按下 l：CSPLIT 拿到 DATA0 8 字节 → IRQ 9 → system_wq → usbkbd_wq → 终端")
        dw, hb, kb = (n.get_center() for n in self.tnode)
        m0 = self.uf_mark(0, "SSPLIT", GREEN_BG)
        self.pkt(dw, hb, "SSPLIT IN", BLUE, 0.4)
        self.pkt(hb, dw, "ACK", YEL, 0.3)
        m1 = self.uf_mark(1, "TT IN", YEL_BG)
        self.pkt(hb, kb, "IN", GREEN, 0.4)
        self.pkt(kb, hb, "DATA0 00 00 0f …", GREEN, 0.7)
        m2 = self.uf_mark(2, "CSPLIT", BLUE_BG)
        self.pkt(dw, hb, "CSPLIT", BLUE, 0.4)
        self.pkt(hb, dw, "DATA0 8 B", GREEN, 0.6)
        vals = ["00", "00", "0f", "00", "00", "00", "00", "00"]
        self.play(*[Transform(c[1], M(v, 11, GREEN if v != "00" else INK).move_to(c[0])) for c, v in zip(self.report, vals)],
                  run_time=0.5)
        self.set_reg("HCINT6", "00000023", GREEN)
        self.set_reg("HCTSIZ6", "40000000", BLUE)
        self.note("HCINT6=0x23 = XferCompl|ChHltd|ACK：8 字节已 DMA 进 report[]（0xc0000000 总线别名），HCTSIZ 剩 0")
        self.wait(1.4)
        chain = [("irq", "dwc2_irq：GINTSTS.HCHINT → HAINT=0x40 → 屏蔽 HAINTMSK b6 → schedule_work"),
                 ("sys", "system_wq：读 HCINT/HCTSIZ，dc ivac report[]，toggle → DATA1，release ch6，usb_hcd_giveback_urb"),
                 ("kbd", "usbkbd_irq_complete → queue_work(usbkbd_wq)；rx_work 与上份报告比：usage 0x0f 新按下"),
                 ("tty", "consoleintr('l')：回显、进 cons.buf；同时 input_report_key(KEY_L) + input_sync → event0")]
        for k, s in chain:
            self.flash(self.wq[k], BLUE if k != "irq" else YEL)
            self.note(s)
            self.wait(1.5)
        new = card("interrupt_rx[0]", "键盘 urb · pending（DATA1）", YEL, YEL_BG, 3.0, 0.56, 13, 10).move_to(self.slots[0])
        self.note("usb_submit_urb 重新提交同一个 URB：回到请求表 → 调度 → 下一次 SSPLIT 用 DATA1（HCTSIZ6=40080008）")
        self.play(Transform(self.slots[0], new), FadeOut(m0), FadeOut(m1), FadeOut(m2))
        self.set_reg("HCTSIZ6", "40080008", BLUE)
        self.wait(1.5)

    def step5_roundrobin(self):
        self.step_title("5  两个中断端点（例如键鼠接收器）怎样共用一个通道 6")
        new1 = card("interrupt_rx[1]", "鼠标 urb · pending（假设）", YEL, YEL_BG, 3.0, 0.56, 13, 10).move_to(self.slots[1])
        self.note("如果同一个接收器还有 boot 鼠标接口（3/1/2），usbmouse 的 URB 进 interrupt_rx[1]；这块键盘的接口 1 是 3/0/0，所以实际为空")
        self.play(Transform(self.slots[1], new1))
        self.wait(1.4)
        for who in (0, 1, 0):
            other = 1 - who
            a = card(f"interrupt_rx[{who}]", ("键盘" if who == 0 else "鼠标") + " · inflight（占 ch6）", BLUE, BLUE_BG,
                     3.0, 0.56, 13, 10).move_to(self.slots[who])
            b = card(f"interrupt_rx[{other}]", ("键盘" if other == 0 else "鼠标") + " · pending", YEL, YEL_BG,
                     3.0, 0.56, 13, 10).move_to(self.slots[other])
            self.play(Transform(self.slots[who], a), Transform(self.slots[other], b),
                      Transform(self.inflight, M(f"inflight = {who}", 13, BLUE).move_to(self.inflight)), run_time=0.5)
            self.set_reg("HCCHAR6", "810e8808" if who == 0 else "(鼠标 EP)", BLUE)
            self.wait(0.6)
        self.note("每次通道 6 释放，schedule_locked 从 rotate 游标开始找下一个 pending —— 公平轮转（EHCI 里两个 QH 挂在帧表上由硬件各自轮询）")
        self.wait(2)
        back = card("interrupt_rx[1]", "空", MUTED, GRAY_BG, 3.0, 0.56, 13, 10).move_to(self.slots[1])
        self.play(Transform(self.slots[1], back))

    def step6_hub(self):
        self.step_title("6  ch7：Hub 状态变化端点 → 插拔时整棵树重新枚举")
        self.flash(self.ch[7], PUR)
        self.note("Hub EP1 IN（bInterval 12 ≈ 256 ms）：回 0 → 再提交；插上键盘时回位图 0x08（口 3）→ dwc2_hub_wq")
        self.wait(2)
        self.note("日志：dwc2: hub IRQ change=8 connected=2->a; reprobe —— 拆掉所有 usb_device，重新 dwc2_init()（步骤 0–2 再来一遍）")
        self.wait(2.2)

    def compare(self):
        self.play(*[FadeOut(m) for m in self.mobjects])
        self.note_m = None
        rows = [("", "EHCI（tiny210）", "DWC2（rpi3）"),
                ("调度结构", "async 环 + 帧表 1024 项", "8 个通道 + interrupt_rx[4]"),
                ("一个事务", "qTD（Active/IOC）", "HCTSIZ + HCCHAR.ChEna"),
                ("split 定时", "QH 的 S-mask/C-mask，硬件", "dwc2_irq 快路径，软件等 2 个微帧"),
                ("轮询周期", "每 1 ms 帧表命中", "NAK 后 delayed_work 10 ms"),
                ("NAK", "qTD 保持 Active，硬件重试", "通道停下，软件重 arm"),
                ("完成", "USBINT → ehci_intr_reap", "HCHINT/HAINT → schedule_work")]
        tbl = VGroup()
        for i, (a, b, c) in enumerate(rows):
            col = INK if i else MUTED
            r = VGroup(T(a, 18, INK, weight=BOLD).set_width(1.6) if a else T(" ", 18),
                       T(b, 18, GREEN if i == 0 else col), T(c, 18, BLUE if i == 0 else col))
            r[0].move_to([-5.4, 2.4 - i * 0.65, 0], aligned_edge=LEFT)
            r[1].move_to([-3.4, 2.4 - i * 0.65, 0], aligned_edge=LEFT)
            r[2].move_to([1.6, 2.4 - i * 0.65, 0], aligned_edge=LEFT)
            tbl.add(r)
        hd = T("EHCI 与 DWC2 对照", 30, INK, weight=BOLD).to_edge(UP, buff=0.3)
        self.play(FadeIn(hd), LaggedStart(*[FadeIn(r) for r in tbl], lag_ratio=0.15), run_time=2)
        self.wait(5)
