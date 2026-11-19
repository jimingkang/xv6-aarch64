"""Every USB packet of the keyboard on a real Raspberry Pi 3B+, bit by bit.

Bytes come from the board (make USBKBD_TRACE=2):
  doc/readme/rpi3/logs/pi3bplus-usbkbd-bytes-trace2.log  (enumeration)
  keys l / s / Enter from the minicom log quoted in doc/readme/rpi3/usb-keyboard.md
PIDs, token CRC5 and data CRC16 are computed here from those bytes with the
USB 2.0 algorithms (the controller generates them; xv6 never sees them).

Render (repo root):
    manim -qh --media_dir build/manim tools/manim/rpi3_usbkbd_packets_anim.py UsbKbdPackets
    cp build/manim/videos/rpi3_usbkbd_packets_anim/1080p60/UsbKbdPackets.mp4 \
       doc/readme/rpi3/videos/usbkbd-packets-3bplus.mp4
"""
from manim import *

FONT = "Heiti SC"
MONO = "Menlo"
BG = "#14161c"


def T(s, size=22, color=WHITE, **kw):
    return Text(s, font=FONT, font_size=size, color=color, **kw)


def M(s, size=18, color=WHITE):
    return Text(s, font=MONO, font_size=size, color=color)


# ----------------------------------------------------------- USB arithmetic
def crc5(val, n):
    crc = 0x1F
    for i in range(n):
        b = (val >> i) & 1
        crc = ((crc << 1) ^ 0x05) & 0x1F if ((crc >> 4) & 1) ^ b else (crc << 1) & 0x1F
    return crc ^ 0x1F


def crc16(bs):
    crc = 0xFFFF
    for b in bs:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


PIDS = dict(SPLIT=0x8, SETUP=0xD, IN=0x9, OUT=0x1, DATA0=0x3, DATA1=0xB,
            ACK=0x2, NAK=0xA, NYET=0x6)
PID_MEAN = dict(SPLIT="特殊：split 事务（只在高速 Hub 上）", SETUP="令牌：控制传输的 SETUP 阶段",
                IN="令牌：主机要数据", OUT="令牌：主机发数据", DATA0="数据包，toggle=0",
                DATA1="数据包，toggle=1", ACK="握手：收到了", NAK="握手：暂时没有数据/忙",
                NYET="握手：split 结果还没准备好")

# colours
CP, CA, CE, CC, CD, CR = TEAL_C, BLUE_C, PURPLE_B, GRAY_C, GOLD_C, ORANGE


def pid_fields(name):
    p = PIDS[name]
    return [("PID 反码", 4, (~p) & 0xF, GRAY_B, f"高 4 位 = 低 4 位取反（{(~p) & 0xF:04b}），接收方据此校验 PID 没出错"),
            ("PID", 4, p, CP, f"{name} = {p:04b}：{PID_MEAN[name]}。整个字节 0x{((~p & 0xF) << 4) | p:02X}")]


def token(name, addr, ep):
    return pid_fields(name) + [
        ("ADDR", 7, addr, CA, f"设备地址 {addr}（7 位，0 = 还没分配地址的新设备）"),
        ("ENDP", 4, ep, CE, f"端点号 {ep}" + ("（EP0 = 默认控制端点）" if ep == 0 else "（EP1 = 键盘的中断 IN 端点）")),
        ("CRC5", 5, crc5(addr | ep << 7, 11), CC, "对 ADDR+ENDP 这 11 位算的 CRC5（x⁵+x²+1），控制器自动生成"),
    ]


def split_tok(sc, et):
    v = 1 | sc << 7 | 3 << 8 | 1 << 15 | 0 << 16 | et << 17
    return pid_fields("SPLIT") + [
        ("HubAddr", 7, 1, CA, "TT 所在高速 Hub 的地址 = 1（HCSPLT.HubAddr）"),
        ("SC", 1, sc, CD, "0 = Start-Split（把事务交给 TT）" if sc == 0 else "1 = Complete-Split（向 TT 取结果）"),
        ("Port", 7, 3, CE, "键盘接在 Hub 的第 3 口（HCSPLT.PrtAddr）"),
        ("S", 1, 1, CR, "Speed：1 = 低速设备（1.5 Mb/s）"),
        ("E", 1, 0, GRAY_B, "End：控制/中断事务不用，恒 0"),
        ("ET", 2, et, CP, {0: "Endpoint Type 00 = 控制", 3: "Endpoint Type 11 = 中断"}[et]),
        ("CRC5", 5, crc5(v, 19), CC, "对后面 19 位算的 CRC5"),
    ]


def data_pkt(name, bs, notes=None):
    f = pid_fields(name)
    for i, b in enumerate(bs):
        f.append((f"byte{i}", 8, b, CA if i % 2 == 0 else CE, (notes or {}).get(i)))
    c = crc16(bytes(bs))
    f.append(("CRC16", 16, c, CC, f"对数据字节算 CRC16（x¹⁶+x¹⁵+x²+1），线上先发低字节：{c & 0xFF:02x} {c >> 8:02x}"))
    return f


def hs_ack(name="ACK"):
    return pid_fields(name)


def setup_bytes(bs, notes):
    return [(f"byte{i}", 8, b, CA if i % 2 == 0 else CE, notes.get(i)) for i, b in enumerate(bs)]


def bmreq(v):
    d = {0: "主机→设备", 1: "设备→主机"}[v >> 7]
    t = ["标准", "类", "厂商", "保留"][(v >> 5) & 3]
    r = {0: "设备", 1: "接口", 2: "端点", 3: "其他（Hub 端口）"}[v & 0x1F]
    return [("D7 方向", 1, v >> 7, CR, f"D7 = {v >> 7}：{d}（数据阶段方向）"),
            ("D6..5 类型", 2, (v >> 5) & 3, CD, f"D6..5 = {(v >> 5) & 3:02b}：{t}请求"),
            ("D4..0 接收者", 5, v & 0x1F, CE, f"D4..0 = {v & 0x1F:05b}：{r}")]


def bits16(v, names, prefix):
    out = []
    for b in range(15, -1, -1):
        nm, ex = names.get(b, (f"b{b}", None))
        out.append((nm, 1, (v >> b) & 1, CD if (v >> b) & 1 else GRAY_D,
                    (f"{prefix} b{b} = {(v >> b) & 1}：{ex}") if ex else None))
    return out


PORT_ST = {0: ("Conn", "PORT_CONNECTION，口上有设备"), 1: ("En", "PORT_ENABLE，口已使能（复位完成）"),
           2: ("Susp", "PORT_SUSPEND"), 3: ("OC", "PORT_OVER_CURRENT"), 4: ("Rst", "PORT_RESET，正在复位"),
           8: ("Pwr", "PORT_POWER，口已上电"), 9: ("LS", "PORT_LOW_SPEED，低速设备（D- 上拉）"),
           10: ("HS", "PORT_HIGH_SPEED"), 11: ("Test", None), 12: ("Ind", None)}
PORT_CH = {0: ("C_Conn", "C_PORT_CONNECTION，连接状态变了"), 1: ("C_En", "C_PORT_ENABLE"),
           2: ("C_Susp", None), 3: ("C_OC", None), 4: ("C_Rst", "C_PORT_RESET，复位完成")}


def reg32(v, spec, regname):
    """spec: list of (hi, lo, name, colour, explain(fieldvalue) -> str)"""
    out, b = [], 31
    for hi, lo, nm, col, ex in spec:
        if hi < b:
            out.append(("0", b - hi, 0, GRAY_D, None))
        fv = (v >> lo) & ((1 << (hi - lo + 1)) - 1)
        out.append((nm, hi - lo + 1, fv, col, f"{regname}[{hi}:{lo}] {nm} = {fv:#x}：{ex(fv)}" if ex else None))
        b = lo - 1
    if b >= 0:
        out.append(("0", b + 1, 0, GRAY_D, None))
    return out


HCSPLT_SPEC = [(31, 31, "SpltEna", CR, lambda f: "1 = 这个通道走 split"),
               (16, 16, "CompSplt", CD, lambda f: "0 = 先发 SSPLIT" if f == 0 else "1 = 发 CSPLIT"),
               (15, 14, "XactPos", GRAY_B, lambda f: "11 = ALL，整包（只对等时 OUT 有意义）"),
               (13, 7, "HubAddr", CA, lambda f: f"Hub 地址 {f}"),
               (6, 0, "PrtAddr", CE, lambda f: f"Hub 口 {f}")]


def hcchar_spec():
    return [(31, 31, "ChEna", CR, lambda f: "写 1 启动通道（回读时完成就已清 0）"),
            (30, 30, "ChDis", GRAY_B, None), (29, 29, "Odd", GRAY_B, None),
            (28, 22, "DevAddr", CA, lambda f: f"目标设备地址 {f}"),
            (21, 20, "MC", GRAY_B, None),
            (19, 18, "EPType", CD, lambda f: ["00 控制", "01 等时", "10 批量", "11 中断"][f]),
            (17, 17, "LSpd", CR, lambda f: "1 = 低速设备（控制器按低速时序生成令牌）" if f else "0"),
            (15, 15, "EPDir", CE, lambda f: "1 = IN（设备→主机）" if f else "0 = OUT"),
            (14, 11, "EPNum", CP, lambda f: f"端点 {f}"),
            (10, 0, "MPS", CC, lambda f: f"最大包长 {f} 字节")]


HCTSIZ_SPEC = [(30, 29, "PID", CD, lambda f: ["00 DATA0", "01 DATA2", "10 DATA1", "11 SETUP"][f]),
               (28, 19, "PktCnt", CA, lambda f: f"{f} 个包"),
               (18, 0, "XferSize", CE, lambda f: f"{f} 字节；完成后变成剩余量")]


def hcint(v):
    names = {0: ("Cpl", "XferCompl：传输完成，数据已 DMA"), 1: ("Hlt", "ChHltd：通道已停下"),
             2: ("AHB", "AHB 错误"), 3: ("STALL", "设备 STALL"), 4: ("NAK", "收到 NAK"),
             5: ("ACK", "收到 ACK"), 6: ("NYET", "收到 NYET"), 7: ("XErr", "传输错误"),
             8: ("Bbl", "Babble"), 9: ("FOv", "帧溢出"), 10: ("DTE", "DATA toggle 错")}
    out = []
    for b in range(10, -1, -1):
        nm, ex = names[b]
        on = (v >> b) & 1
        out.append((nm, 1, on, CD if on else GRAY_D, f"HCINT b{b} = 1：{ex}" if on else None))
    return out


# --------------------------------------------------- context: lanes + queue
FLOW_LANES = [("键盘", "2a7a:8a47", TEAL_C), ("Hub TT", "addr1 口3", GOLD_C), ("DWC2", "ch0/ch6", BLUE_C),
              ("dwc2.c", "HCD", PURPLE_B), ("usb core", "match/URB", GOLD_C), ("usbkbd", "HID 驱动", ORANGE),
              ("tty+input", "终端/event0", GREEN_C)]
_R = {0: 3, 1: 2, 2: 1, 3: 0}     # old route node (CPU, DWC2, Hub, kbd) -> lane


def context_for(chap, title, route):
    a, b = (_R[route[0]], _R[route[1]]) if route else (None, None)
    lab = title
    if chap == 1:
        return a, b, lab, "步骤 2–3 · 枚举 Hub 口 · ch0 控制传输（高速，不 split）", {"ch0"}
    if chap == 2:
        return a, b, lab, "步骤 3 · split 控制传输：DWC2 ⇄ Hub TT ⇄ 键盘，全在 ch0", {"ch0"}
    if chap == 3:
        if "PROTOCOL" in title or "IDLE" in title or "0x21" in title:
            return 5, 0, "usbkbd_probe → ops->control → ch0", "步骤 5 · usbkbd_probe 发 HID 请求", {"ch0"}
        if "SET_CONFIGURATION" in title:
            return 3, 0, "dwc2_init → SET_CONFIGURATION；之后 usb_device_register → usb core", \
                "步骤 3–4 · 配置后交给 usb 总线 match/probe", {"ch0"}
        return a, b, lab, "步骤 3 · 读描述符（ch0，split）", {"ch0"}
    # chapter 4: interrupt IN on ch6
    if "HCCHAR6" in title:
        return 5, 3, "usb_submit_urb → dwc2_submit_urb → rx_start", "步骤 5 · URB 进请求表，arm 通道 6", {"ch6", "slot"}
    if "SSPLIT 令牌" in title or "IN 令牌" in title:
        return 2, 1, title, "步骤 6 · 通道 6 每 10 ms 轮询（SSPLIT ACK 在 IRQ 快路径续发 CSPLIT）", {"ch6", "slot", "irq"}
    if "没按键" in title:
        return 0, 1, "NAK", "步骤 6 · 没按键", {"ch6", "slot"}
    if "0x12" in title:
        return 2, 3, "HCINT6=0x12 → IRQ 9 → retry_work", "步骤 6 · NAK：释放 ch6，10 ms 后再 arm", {"irq", "sys", "per"}
    if "按下 l" in title:
        return 0, 2, "DATA0 00 00 0f … → DMA → report[]", "步骤 6–7 · CSPLIT 拿到报告", {"ch6", "slot"}
    if "修饰键" in title:
        return 5, 5, "usbkbd_rx_work 解析 byte0", "步骤 9 · 报告差分", {"kbd"}
    if "0x23" in title:
        return 2, 5, "IRQ 9 → system_wq → giveback → usbkbd_wq", "步骤 7–8 · 完成路径", {"irq", "sys", "kbd"}
    if "HAINT" in title:
        return 2, 3, "HAINT b6 → dwc2_irq", "步骤 7 · 上半部", {"irq"}
    return 5, 6, title.split("：")[0] + " → consoleintr / input_report_key", "步骤 9 · 送进终端和 evdev，然后重新提交", {"kbd", "tty", "slot"}


# --------------------------------------------------------------------- scene
NODES = [("CPU", PURPLE_B), ("DWC2", BLUE_C), ("Hub TT", GOLD_C), ("键盘", TEAL_C)]


class UsbKbdPackets(Scene):
    def construct(self):
        self.camera.background_color = BG
        self.topo = self.make_topo()
        self.field_grp = None
        self.head = None
        self.note = None
        self.chain_m = None
        self.intro()
        self.add(self.topo)
        self.chap = 0
        for p in PACKETS:
            if p[0] == "chapter":
                self.chap += 1
                self.chapter(*p[1:])
            else:
                self.packet(*p)
        self.outro()

    # ---- fixed parts: the usbkbd-flow lanes (left) + the DWC2 queue panel (right)
    def make_topo(self):
        g = VGroup()
        # lanes, as in usbkbd-flow.mp4 (plus the Hub TT the split goes through)
        self.lx = [-6.55 + i * 0.98 for i in range(7)]
        for (n, sub, c), x in zip(FLOW_LANES, self.lx):
            b = RoundedRectangle(width=0.92, height=0.5, corner_radius=0.07, color=c,
                                 fill_color=c, fill_opacity=0.18, stroke_width=1.6).move_to([x, 2.72, 0])
            lab = VGroup(T(n, 11, c, weight=BOLD), T(sub, 7, GRAY_B)).arrange(DOWN, buff=0.02)
            if lab.width > 0.86:
                lab.scale_to_fit_width(0.86)
            lab.move_to(b)
            ln = DashedLine([x, 2.47, 0], [x, 1.45, 0], dash_length=0.06, color=c, stroke_opacity=0.5)
            g.add(VGroup(b, lab, ln))
        g.add(T("usbkbd-flow 泳道", 10, GRAY_C).move_to([-3.6, 3.08, 0]))
        # DWC2 queue panel, after dwc2-channel-queues.png
        self.q = {}
        frame = RoundedRectangle(width=6.55, height=2.25, corner_radius=0.1, color=GRAY_D,
                                 stroke_width=1.2).move_to([3.62, 2.3, 0])
        g.add(frame, T("DWC2 通道队列（对照 dwc2-channel-queues.png）", 10, GRAY_C).move_to([3.62, 3.32, 0]))
        spec = [("ch0", "ch0 控制", "CPU 忙等", GREEN_C, [1.05, 2.9]),
                ("ch6", "ch6 中断 IN", "HID", BLUE_C, [1.05, 2.3]),
                ("ch7", "ch7 Hub", "热插拔", PURPLE_B, [1.05, 1.7]),
                ("slot", "interrupt_rx[0]", "键盘 URB", GOLD_C, [2.85, 2.6]),
                ("per", "dwc2_periodic_wq", "retry 10 ms", GRAY_B, [2.85, 1.85]),
                ("irq", "IRQ 9 dwc2_irq", "中断上下文", GOLD_C, [5.4, 2.95]),
                ("sys", "system_wq", "urb_work/giveback", BLUE_C, [5.4, 2.45]),
                ("kbd", "usbkbd_wq", "usbkbd_rx_work", BLUE_C, [5.4, 1.95]),
                ("tty", "consoleintr/evdev", "终端 · event0", GREEN_C, [5.4, 1.45])]
        for k, a, b, c, (x, y) in spec:
            w = 1.55 if k in ("ch0", "ch6", "ch7") else 1.75
            r = RoundedRectangle(width=w, height=0.44, corner_radius=0.06, color=c, stroke_width=1.4,
                                 fill_color=c, fill_opacity=0.12).move_to([x, y, 0])
            t = VGroup(T(a, 10, c), T(b, 7, GRAY_B)).arrange(DOWN, buff=0.01)
            if t.width > w - 0.08:
                t.scale_to_fit_width(w - 0.08)
            t.move_to(r)
            self.q[k] = r
            g.add(VGroup(r, t))
        g.add(Arrow([5.4, 2.73, 0], [5.4, 2.67, 0], buff=0, color=GRAY_C, stroke_width=2, tip_length=0.08),
              Arrow([5.4, 2.23, 0], [5.4, 2.17, 0], buff=0, color=GRAY_C, stroke_width=2, tip_length=0.08),
              Arrow([5.4, 1.73, 0], [5.4, 1.67, 0], buff=0, color=GRAY_C, stroke_width=2, tip_length=0.08))
        self.flow_m = None
        self.step_m = None
        return g

    def set_ctx(self, title, route):
        a, b, lab, step, act = context_for(self.chap, title, route)
        anims = []
        for k, r in self.q.items():
            on = k in act
            anims.append(r.animate.set_stroke(YELLOW if on else r.get_fill_color(), width=3.2 if on else 1.4)
                         .set_fill(opacity=0.45 if on else 0.12))
        old = [m for m in (self.flow_m, self.step_m) if m is not None]
        st = T(step, 12, YELLOW).move_to([-3.6, 1.28, 0])
        grp = VGroup(st)
        if a is not None:
            y = 1.95
            if a == b:
                arr = CurvedArrow([self.lx[a] + 0.05, y + 0.1, 0], [self.lx[a] + 0.05, y - 0.1, 0],
                                  angle=-PI * 1.4, color=YELLOW, stroke_width=3, tip_length=0.1)
            else:
                arr = Arrow([self.lx[a], y, 0], [self.lx[b], y, 0], buff=0.02, color=YELLOW,
                            stroke_width=4, tip_length=0.14, max_tip_length_to_length_ratio=0.3)
            t = T(lab, 11, YELLOW)
            if t.width > 6.4:
                t.scale_to_fit_width(6.4)
            t.move_to([(self.lx[a] + self.lx[b]) / 2 if a != b else self.lx[a] + 1.2, y + 0.26, 0])
            if t.get_left()[0] < -6.95:
                t.shift(RIGHT * (-6.95 - t.get_left()[0]))
            if t.get_right()[0] > 0.2:
                t.shift(LEFT * (t.get_right()[0] - 0.2))
            grp.add(arr, t)
        self.play(*anims, *[FadeOut(m) for m in old], FadeIn(grp), run_time=0.5)
        self.flow_m, self.step_m = grp, None
        if a is not None and a != b:
            d = Dot([self.lx[a], 1.95, 0], radius=0.08, color=WHITE)
            self.add(d)
            self.play(d.animate.move_to([self.lx[b], 1.95, 0]), run_time=0.5)
            self.remove(d)

    def intro(self):
        t = T("树莓派 3B+：USB 键盘的每一个包，逐位讲解", 40, YELLOW, weight=BOLD)
        s = T("DWC2 主机 → Hub 424:2514 的 TT → 低速键盘 2a7a:8a47 · 字节全部来自板子串口（USBKBD_TRACE=2）", 19, GRAY_B)
        c = T("令牌的 CRC5 和数据包的 CRC16 由控制器硬件生成，这里按 USB 2.0 算法从真实字节算出", 17, GRAY_C)
        g = VGroup(t, s, c).arrange(DOWN, buff=0.35)
        self.play(Write(t), run_time=1.2)
        self.play(FadeIn(s), FadeIn(c))
        self.wait(2)
        self.play(FadeOut(g))

    def chapter(self, title, sub):
        old = [m for m in (self.field_grp, self.head, self.note, self.chain_m) if m is not None]
        if old:
            self.play(*[FadeOut(m) for m in old], run_time=0.35)
        self.field_grp = self.head = self.note = self.chain_m = None
        t = T(title, 34, YELLOW, weight=BOLD)
        cover = Rectangle(width=14.4, height=8.2, fill_color=BG, fill_opacity=0.92, stroke_width=0)
        self.play(FadeIn(cover), run_time=0.3)
        s = T(sub, 18, GRAY_B)
        if s.width > 13:
            s.scale_to_fit_width(13)
        g = VGroup(t, s).arrange(DOWN, buff=0.25)
        self.play(FadeIn(g, shift=UP * 0.2), run_time=0.6)
        self.wait(1.8)
        self.play(FadeOut(g), FadeOut(cover), run_time=0.4)

    # ---- one packet / register
    def bitrow(self, fields):
        total = sum(f[1] for f in fields)
        unit = min(0.9, 13.2 / total)
        widths = [max(f[1] * unit, 0.72) for f in fields]
        if sum(widths) > 13.2:
            k = 13.2 / sum(widths)
            widths = [w * k for w in widths]
        boxes = VGroup()
        x = -sum(widths) / 2
        for i, (nm, bits, val, col, ex) in enumerate(fields):
            w = widths[i]
            r = Rectangle(width=w, height=0.62, color=col, fill_color=col, fill_opacity=0.28,
                          stroke_width=1.6).move_to([x + w / 2, 0.0, 0])
            if bits <= 12 and nm.startswith("byte") is False:
                vs = format(val, f"0{bits}b")
            elif nm.startswith("byte"):
                vs = f"{val:02x}"
            else:
                vs = f"{val:#x}"
            vt = M(vs, 17, WHITE)
            if vt.width > w * 0.9:
                vt.scale_to_fit_width(w * 0.9)
            vt.move_to(r)
            lab = T(nm, 13, col)
            if lab.width > max(w * 0.95, 0.5):
                lab.scale_to_fit_width(max(w * 0.95, 0.5))
            up = 0.36 if (w < 1.0 and i % 2) else 0.0
            lab.next_to(r, UP, buff=0.07 + up)
            boxes.add(VGroup(r, vt, lab))
            x += w
        if nm.startswith("byte") or any(f[0].startswith("byte") for f in fields):
            pass
        bitn = T(f"共 {total} 位（左边先画高位/先画第一个字节；线上逐字节先发低位）", 12, GRAY_C)
        bitn.move_to([0, -0.55, 0])
        return VGroup(boxes, bitn)

    def chain_box(self, chain):
        lines = VGroup()
        for depth, txt in chain:
            pre = ("│ " * (depth - 1) + "└ ") if depth else ""
            lines.add(M(pre + txt, 14, PURPLE_A if depth == 0 else WHITE))
        lines.arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        if lines.width > 12.6:
            lines.scale_to_fit_width(12.6)
        if lines.height > 1.25:
            lines.scale_to_fit_height(1.25)
        lab = T("函数调用链", 14, PURPLE_A)
        g = VGroup(lab, lines).arrange(RIGHT, aligned_edge=UP, buff=0.25)
        bg = SurroundingRectangle(g, color=PURPLE_B, buff=0.12, corner_radius=0.08,
                                  fill_color="#0c0d11", fill_opacity=0.95)
        return VGroup(bg, g).move_to([0, -2.95, 0])

    def say(self, s, color=WHITE):
        n = T(s, 21, color)
        if n.width > 13.2:
            n.scale_to_fit_width(13.2)
        bg = RoundedRectangle(width=13.6, height=0.9, corner_radius=0.12, color=GRAY_D,
                              fill_color="#0c0d11", fill_opacity=0.95).move_to([0, -1.35, 0])
        n.move_to(bg)
        g = VGroup(bg, n)
        if self.note is None:
            self.play(FadeIn(g), run_time=0.25)
        else:
            self.play(FadeOut(self.note), FadeIn(g), run_time=0.25)
        self.note = g

    def packet(self, title, sub, route, fields, summary=None, decode=None):
        old = [m for m in (self.field_grp, self.head, self.note, self.chain_m) if m is not None]
        if old:
            self.play(*[FadeOut(m) for m in old], run_time=0.3)
        self.note = None
        self.chain_m = None
        chain = CHAINS.get(title)
        h = T(title, 22, YELLOW, weight=BOLD).move_to([-3.6, 3.72, 0])
        if h.width > 6.9:
            h.scale_to_fit_width(6.9)
        s = M(sub, 11, GRAY_B).move_to([-3.6, 3.38, 0])
        if s.width > 6.9:
            s.scale_to_fit_width(6.9)
        self.head = VGroup(h, s)
        self.play(FadeIn(self.head), run_time=0.35)
        if chain:
            self.chain_m = self.chain_box(chain)
            self.play(FadeIn(self.chain_m), run_time=0.4)
        self.set_ctx(title, route)
        row = self.bitrow(fields)
        self.field_grp = row
        self.play(LaggedStart(*[FadeIn(b, shift=DOWN * 0.1) for b in row[0]], lag_ratio=0.04),
                  FadeIn(row[1]), run_time=1.0)
        if summary:
            self.say(summary, GREEN_B)
            self.wait(1.4)
        for box, f in zip(row[0], fields):
            if not f[4]:
                continue
            hl = SurroundingRectangle(box[0], color=YELLOW, buff=0.04, stroke_width=3)
            self.play(Create(hl), run_time=0.2)
            self.say(f[4])
            self.wait(min(3.2, 0.9 + len(f[4]) * 0.055))
            self.play(FadeOut(hl), run_time=0.15)
        if decode:
            lines = VGroup(*[T(d, 17, GREEN_B) for d in decode]).arrange(DOWN, aligned_edge=LEFT, buff=0.1)
            if lines.width > 13:
                lines.scale_to_fit_width(13)
            box = SurroundingRectangle(lines, color=GREEN_B, buff=0.15, fill_color="#0c0d11", fill_opacity=0.95)
            g = VGroup(box, lines).move_to([0, -2.7, 0])
            self.play(FadeIn(g), run_time=0.4)
            self.wait(2.2 + 0.6 * len(decode))
            self.play(FadeOut(g), run_time=0.3)

    def outro(self):
        self.play(*[FadeOut(m) for m in self.mobjects])
        lines = VGroup(
            T("小结", 34, YELLOW, weight=BOLD),
            T("· 每个包 = PID（4 位 + 4 位反码）+ 内容 + CRC：令牌 CRC5，数据 CRC16，握手只有 PID", 19),
            T("· 低速键盘在高速 Hub 后面：每个事务都先 SSPLIT 交给 TT（ACK），再 CSPLIT 取结果", 19),
            T("· SETUP 8 字节决定一切：bmRequestType 的方向/类型/接收者 + bRequest + wValue/wIndex/wLength", 19),
            T("· 描述符告诉主机：低速、EP0 8 字节、接口 0 是 3/1/1、EP 0x81 中断 IN 8 字节每 10 ms", 19),
            T("· 按键 = 一份 8 字节报告：修饰键位图 + 最多 6 个 usage；DATA0/DATA1 每次交替", 19),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.28)
        self.play(FadeIn(lines, shift=UP * 0.2))
        self.wait(4)


# ------------------------------------------------------------------ the list
SETUP_GS = [0xA3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00]
SETUP_RST = [0x23, 0x03, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00]
SETUP_DEV8 = [0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00]
DEV8 = [0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x08]
DEV18 = DEV8 + [0x7A, 0x2A, 0x47, 0x8A, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01]
CFG = [0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32]
INTF0 = [0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00]
HIDD = [0x09, 0x21, 0x10, 0x01, 0x00, 0x01, 0x22, 0x40, 0x00]
EP81 = [0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A]
INTF1 = [0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00]
EP82 = [0x07, 0x05, 0x82, 0x03, 0x04, 0x00, 0x0A]
KEY_L = [0, 0, 0x0F, 0, 0, 0, 0, 0]

SETUP_NOTES_GS = {0: "bmRequestType 0xa3 = 1010 0011：设备→主机 · 类请求 · 接收者=其他(端口)，下一张拆开",
                  1: "bRequest 0x00 = GET_STATUS", 2: "wValue 低字节 0", 3: "wValue 高字节 0（GET_STATUS 不用）",
                  4: "wIndex 低字节 3 = 口号 3", 5: "wIndex 高字节 0",
                  6: "wLength 低字节 4：要 4 字节（wPortStatus + wPortChange）", 7: "wLength 高字节 0"}

PACKETS = [
    ("chapter", "第 1 部分：Hub 发现口 3 上的键盘",
     "高速控制传输，直接发给 Hub（地址 1），不需要 split。每个控制传输 = SETUP 事务 → 数据事务 → 状态事务"),
    ("SETUP 令牌：GET_STATUS(port 3) 的第一个包", "DWC2 ch0 → Hub addr 1 · 令牌包 24 位", (1, 2),
     token("SETUP", 1, 0), "令牌包告诉总线上所有设备：下一个数据包是给 地址1/端点0 的 SETUP"),
    ("DATA0：8 字节 SETUP 内容", "kbd-:   SETUP DATA0 a3 0 0 0 3 0 4 0  (mps 64)", (1, 2),
     data_pkt("DATA0", SETUP_GS, SETUP_NOTES_GS), "SETUP 阶段的数据永远是 DATA0、恰好 8 字节"),
    ("bmRequestType = 0xa3 逐位", "byte0 of the SETUP packet", None, bmreq(0xA3)),
    ("握手：Hub 回 ACK", "Hub → DWC2 · 握手包只有 PID", (2, 1), hs_ack()),
    ("DATA1：Hub 回 4 字节端口状态", "kbd-:   IN DATA +0: 1 3 1 0", (2, 1),
     data_pkt("DATA1", [1, 3, 1, 0], {0: "wPortStatus 低字节 0x01", 1: "wPortStatus 高字节 0x03 → wPortStatus = 0x0301",
                                      2: "wPortChange 低字节 0x01", 3: "wPortChange 高字节 0 → wPortChange = 0x0001"}),
     "数据阶段的第一个包用 DATA1（SETUP 用了 DATA0）"),
    ("wPortStatus = 0x0301 逐位", "dwc2: hub port 3 status=301 change=1", None, bits16(0x0301, PORT_ST, "wPortStatus")),
    ("wPortChange = 0x0001 逐位", "C_PORT_* = 自上次清除以来变过", None, bits16(0x0001, PORT_CH, "wPortChange")),
    ("DATA0：SET_FEATURE(PORT_RESET, 3)", "kbd-:   SETUP DATA0 23 3 4 0 3 0 0 0", (1, 2),
     data_pkt("DATA0", SETUP_RST, {0: "bmRequestType 0x23：主机→设备 · 类 · 其他(端口)", 1: "bRequest 0x03 = SET_FEATURE",
                                   2: "wValue = 4 = PORT_RESET（特性选择子）", 4: "wIndex = 3：口 3", 6: "wLength 0：没有数据阶段"}),
     "Hub 收到后在口 3 上驱动 SE0 ≥10 ms，复位键盘"),
    ("bmRequestType = 0x23 逐位", "byte0", None, bmreq(0x23)),
    ("复位后 GET_STATUS 返回 03 03 11 00", "kbd-:   IN DATA +0: 3 3 11 0", (2, 1),
     bits16(0x0303, PORT_ST, "wPortStatus"), "wPortStatus = 0x0303：比刚才多了 b1 Enable"),
    ("wPortChange = 0x0011", "dwc2: hub port 3 after-reset status=303 speed=low", None, bits16(0x0011, PORT_CH, "wPortChange"),
     "随后 CLEAR_FEATURE(0x10 C_PORT_CONNECTION) 和 (0x14 C_PORT_RESET) 把这两位清掉"),

    ("chapter", "第 2 部分：一次 split 控制传输，逐包",
     "GET_DESCRIPTOR(Device, 8) 发给地址 0 的低速键盘：SETUP / DATA / STATUS 三个阶段，每阶段 SSPLIT + CSPLIT"),
    ("通道寄存器 HCSPLT0 = 0x8000c083", "dwc2: ch0 SSPLIT ACK addr=0 in=0 hcint=22 hcsplt=8000c083", None,
     reg32(0x8000C083, HCSPLT_SPEC, "HCSPLT"), "软件写这几个字段，DWC2 据此生成 SPLIT 令牌"),
    ("通道寄存器 HCCHAR0 = 0x00020040", "hcchar=20040（回读，ChEna 已清）", None,
     reg32(0x00020040, hcchar_spec(), "HCCHAR"), "注意 MPS=64：usb_ep0_mps[0] 还是 Hub 的值，单包请求所以无害"),
    ("通道寄存器 HCTSIZ0 = 0x60080008", "SETUP 阶段", None, reg32(0x60080008, HCTSIZ_SPEC, "HCTSIZ")),
    ("① SSPLIT 令牌（高速）", "DWC2 → Hub：把后面的低速 SETUP 交给口 3 的 TT", (1, 2), split_tok(0, 0),
     "SPLIT 令牌 32 位，紧跟着就是真正的 SETUP 令牌"),
    ("② SETUP 令牌：地址 0，端点 0", "跟在 SSPLIT 后面", (1, 2), token("SETUP", 0, 0)),
    ("③ DATA0：80 06 00 01 00 00 08 00", "kbd-:   SETUP DATA0 80 6 0 1 0 0 8 0", (1, 2),
     data_pkt("DATA0", SETUP_DEV8, {0: "bmRequestType 0x80：设备→主机 · 标准 · 设备", 1: "bRequest 0x06 GET_DESCRIPTOR",
                                     2: "wValue 低字节 = 描述符索引 0", 3: "wValue 高字节 = 描述符类型 1（设备）",
                                     4: "wIndex 0（语言 ID，设备描述符不用）", 6: "wLength 8：先只要前 8 字节，拿到 EP0 包长"})),
    ("④ TT 回 ACK → HCINT0 = 0x22", "dwc2: ch0 SSPLIT ACK … hcint=22", (2, 1), hcint(0x22),
     "TT 收下了。之后它在低速口上重放 SETUP + DATA0，键盘 ACK"),
    ("⑤ CSPLIT 令牌（SC=1）", "主机回来取 SETUP 的结果 → 得到键盘的 ACK", (1, 2), split_tok(1, 0)),
    ("⑥ 数据阶段：SSPLIT + IN 令牌", "HCCHAR0=00028040（EPDir=IN） HCTSIZ0=40080008（DATA1）", (1, 2), token("IN", 0, 0),
     "TT 又回 ACK（0x22）；然后在低速口发 IN，键盘回 DATA1"),
    ("⑦ 键盘的 DATA1（经 CSPLIT 拿回来）", "kbd-:   IN DATA +0: 12 1 10 1 0 0 0 8", (3, 1),
     data_pkt("DATA1", DEV8, {0: "bLength 0x12 = 18：设备描述符共 18 字节", 1: "bDescriptorType 1 = DEVICE",
                              2: "bcdUSB 低字节 0x10", 3: "bcdUSB 高字节 0x01 → USB 1.10",
                              4: "bDeviceClass 0：类别写在接口描述符里", 5: "bDeviceSubClass 0",
                              6: "bDeviceProtocol 0", 7: "bMaxPacketSize0 = 8：EP0 每包 8 字节 → usb_ep0_mps = 8"})),
    ("⑧ HCINT0 = 0x21：CSPLIT 拿到数据", "dwc2: ch0 CSPLIT complete addr=0 bytes=8/8 hcint=21 hctsiz=0", None, hcint(0x21)),
    ("⑨ 状态阶段：DATA1，0 字节", "SSPLIT + OUT + DATA1（HCTSIZ0=40080000）→ ACK；CSPLIT + OUT → 键盘的 ACK", (1, 2),
     data_pkt("DATA1", []), "0 字节数据包只有 PID + CRC16（=0000），意思是“收到，控制传输结束”"),

    ("chapter", "第 3 部分：键盘返回的描述符，逐字节",
     "设备描述符 18 字节、SET_ADDRESS(4)、配置描述符 59 字节（7 个子描述符）、SET_CONFIGURATION、HID 请求"),
    ("设备描述符 byte 8..17", "kbd-:   IN DATA +0: 12 1 10 1 0 0 0 8 7a 2a 47 8a 1 0 0 1 / +16: 0 1", (3, 1),
     [(f"byte{i}", 8, b, CA if i % 2 == 0 else CE, n) for i, b, n in [
         (8, 0x7A, "idVendor 低字节 0x7a"), (9, 0x2A, "idVendor 高字节 0x2a → VID 0x2a7a"),
         (10, 0x47, "idProduct 低字节 0x47"), (11, 0x8A, "idProduct 高字节 → PID 0x8a47"),
         (12, 0x01, "bcdDevice 0x0001（设备版本）"), (13, 0x00, None),
         (14, 0x00, "iManufacturer 0：没有厂商字符串"), (15, 0x01, "iProduct 1：产品名在字符串描述符 1"),
         (16, 0x00, "iSerialNumber 0：没有序列号"), (17, 0x01, "bNumConfigurations 1：只有一个配置")]],
     "18 字节按 MPS 8 拆成 3 个 split IN：8 + 8 + 2"),
    ("SET_ADDRESS(4)", "kbd-:   SETUP DATA0 0 5 4 0 0 0 0 0", (1, 3),
     setup_bytes([0, 5, 4, 0, 0, 0, 0, 0], {0: "bmRequestType 0：主机→设备 · 标准 · 设备", 1: "bRequest 5 = SET_ADDRESS",
                                             2: "wValue = 4：新地址（dwc2 约定 = 口号 + 1）", 6: "wLength 0"}),
     "状态阶段完成后键盘才改用地址 4；之后所有令牌 ADDR=4"),
    ("配置描述符头 9 字节", "kbd-:   IN DATA +0: 9 2 3b 0 2 1 0 a0 32", (3, 1),
     setup_bytes(CFG, {0: "bLength 9", 1: "bDescriptorType 2 = CONFIGURATION", 2: "wTotalLength 低 0x3b",
                       3: "wTotalLength 高 0 → 整个配置 59 字节（再读一次 59 字节）", 4: "bNumInterfaces 2",
                       5: "bConfigurationValue 1：SET_CONFIGURATION(1) 用它", 6: "iConfiguration 0",
                       7: "bmAttributes 0xa0，下一张逐位", 8: "bMaxPower 0x32 × 2 mA = 100 mA"})),
    ("bmAttributes = 0xa0", "1 0 1 0 0 0 0 0", None,
     [("D7", 1, 1, CD, "D7 = 1：保留，必须为 1"), ("D6", 1, 0, GRAY_D, "D6 = 0：总线供电（不是自供电）"),
      ("D5", 1, 1, CD, "D5 = 1：支持远程唤醒（按键可唤醒挂起的主机）"), ("D4..0", 5, 0, GRAY_D, "保留 0")]),
    ("接口描述符 0（boot 键盘）", "+9: 9 4 0 0 1 3 1 1 0", (3, 1),
     setup_bytes(INTF0, {1: "bDescriptorType 4 = INTERFACE", 2: "bInterfaceNumber 0", 3: "bAlternateSetting 0",
                         4: "bNumEndpoints 1", 5: "bInterfaceClass 3 = HID", 6: "bInterfaceSubClass 1 = Boot",
                         7: "bInterfaceProtocol 1 = Keyboard → usbkbd 的 id_table 匹配 3/1/1", 8: "iInterface 0"})),
    ("HID 描述符", "+18: 9 21 10 1 0 1 22 40 0", (3, 1),
     setup_bytes(HIDD, {1: "bDescriptorType 0x21 = HID", 2: "bcdHID 低 0x10", 3: "bcdHID 高 0x01 → HID 1.10",
                        4: "bCountryCode 0：不区分国家", 5: "bNumDescriptors 1",
                        6: "下级描述符类型 0x22 = 报告描述符", 7: "报告描述符长度 0x40 = 64 字节",
                        8: "（boot 协议的格式是固定的，xv6 不读报告描述符）"})),
    ("端点描述符 0x81", "+27: 7 5 81 3 8 0 a", (3, 1),
     setup_bytes(EP81, {1: "bDescriptorType 5 = ENDPOINT", 2: "bEndpointAddress 0x81，下一张逐位",
                        3: "bmAttributes 0x03：b1..0 = 11 中断传输", 4: "wMaxPacketSize 低 8",
                        5: "wMaxPacketSize 高 0 → 每包 8 字节", 6: "bInterval 0x0a = 10：每 10 ms 轮询一次"})),
    ("bEndpointAddress = 0x81", "1 000 0001", None,
     [("D7 方向", 1, 1, CR, "D7 = 1：IN（设备→主机）"), ("D6..4", 3, 0, GRAY_D, "保留 0"),
      ("D3..0 端点号", 4, 1, CE, "端点号 1 → interrupt_in_ep = 1，令牌里 ENDP=1")]),
    ("接口 1 + 端点 0x82（被忽略）", "+34: 9 4 1 0 1 3 0 0 0 · 9 21 10 1 0 1 22 80 0 · 7 5 82 3 4 0 a", (3, 1),
     setup_bytes(INTF1, {2: "bInterfaceNumber 1", 5: "class 3 HID", 6: "subclass 0：不是 Boot",
                         7: "protocol 0：dwc2 只发布 3/1/1、3/1/2，所以这个接口没人绑定"}),
     "它的 HID 报告描述符 128 字节、EP 0x82 IN 4 字节，多半是多媒体键"),
    ("SET_CONFIGURATION(1)", "kbd-:   SETUP DATA0 0 9 1 0 0 0 0 0", (1, 3),
     setup_bytes([0, 9, 1, 0, 0, 0, 0, 0], {1: "bRequest 9 = SET_CONFIGURATION", 2: "wValue 1 = bConfigurationValue"}),
     "键盘进入 Configured 状态，EP1 开始工作"),
    ("HID SET_PROTOCOL(Boot)", "kbd-:   SETUP DATA0 21 b 0 0 0 0 0 0", (1, 3),
     setup_bytes([0x21, 0x0B, 0, 0, 0, 0, 0, 0], {0: "bmRequestType 0x21：主机→设备 · 类 · 接口",
                                                  1: "bRequest 0x0b = SET_PROTOCOL", 2: "wValue 0 = Boot 协议（1 = Report 协议）",
                                                  4: "wIndex 0 = 接口 0"})),
    ("bmRequestType = 0x21 逐位", "byte0", None, bmreq(0x21)),
    ("HID SET_IDLE(0)", "kbd-:   SETUP DATA0 21 a 0 0 0 0 0 0", (1, 3),
     setup_bytes([0x21, 0x0A, 0, 0, 0, 0, 0, 0], {1: "bRequest 0x0a = SET_IDLE",
                                                  2: "wValue 低字节 = Report ID 0（全部报告）",
                                                  3: "wValue 高字节 = 空闲时长 0 = 无限：状态不变就一直 NAK"})),

    ("chapter", "第 4 部分：通道 6 的中断 IN，按下 l、s、Enter",
     "每 10 ms：SSPLIT(ET=中断) + IN(4,1) → TT ACK；CSPLIT + IN → NAK（没按键）或 DATA0/1 8 字节报告"),
    ("HCCHAR6 = 0x810e8808", "kbd6: ch6 arm SSPLIT addr=4 ep1 IN DATA0 mps=8 … HCCHAR=810e8808", None,
     reg32(0x810E8808, hcchar_spec(), "HCCHAR"), "通道 6 专门轮询键盘的中断端点"),
    ("SSPLIT 令牌（ET = 11 中断）", "高速：每 10 ms 一次", (1, 2), split_tok(0, 3)),
    ("IN 令牌：地址 4，端点 1", "跟在 SSPLIT 后面", (1, 2), token("IN", 4, 1)),
    ("没按键：键盘回 NAK", "低速口上 TT ← 键盘；CSPLIT 时 TT 再把 NAK 交给主机", (3, 2), hs_ack("NAK"),
     "SET_IDLE(0) 之后，状态没变就一直 NAK"),
    ("HCINT6 = 0x12：NAK", "kbd6: ch6 NAK (no key change) -> release ch6, retry in 1 jiffies", None, hcint(0x12),
     "dwc2 释放通道 6，dwc2_periodic_wq 10 ms 后再 arm"),
    ("按下 l：键盘的 DATA0", "dwc2: ch6 transfer complete addr=4 ep=1 bytes=8 data=0,0,f,0,0,0,0,0", (3, 1),
     data_pkt("DATA0", KEY_L, {0: "byte0 修饰键位图 = 0：没按 Ctrl/Shift/Alt/GUI", 1: "byte1 保留",
                               2: "byte2 = HID usage 0x0f = 'l'", 3: "byte3..7 = 0：没有第二个键同时按下"})),
    ("修饰键字节 byte0 逐位", "这次是 0；按 Shift+h 时是 0x02", None,
     [("RGUI", 1, 0, GRAY_D, "b7 右 GUI"), ("RAlt", 1, 0, GRAY_D, "b6 右 Alt"), ("RShift", 1, 0, GRAY_D, "b5 右 Shift"),
      ("RCtrl", 1, 0, GRAY_D, "b4 右 Ctrl"), ("LGUI", 1, 0, GRAY_D, "b3 左 GUI"), ("LAlt", 1, 0, GRAY_D, "b2 左 Alt"),
      ("LShift", 1, 0, GRAY_D, "b1 左 Shift：置 1 时 usbkbd 查 shiftmap 出大写"),
      ("LCtrl", 1, 0, GRAY_D, "b0 左 Ctrl：置 1 时字母变控制字符（Ctrl+C = 3）")]),
    ("HCINT6 = 0x23：有数据", "kbd7: IRQ 9 cpu0 dwc2_irq: GINTSTS.HCHINT HAINT=40 HCINT6=23", None, hcint(0x23),
     "ACK 是 SSPLIT 阶段留下的；XferCompl → IRQ 9 → work → usbkbd_rx_work → consoleintr('l')"),
    ("HAINT = 0x40", "哪个通道在报中断", None,
     [("ch7", 1, 0, GRAY_D, "b7 Hub 状态通道"), ("ch6", 1, 1, CD, "b6 = 1：通道 6（键盘）"),
      ("ch5..1", 5, 0, GRAY_D, None), ("ch0", 1, 0, GRAY_D, "b0 控制通道（忙等，不开中断）")]),
    ("松开 l：DATA1 全 0", "kbd9: usage 0xf up -> EV_KEY 38 value 0", (3, 1),
     data_pkt("DATA1", [0] * 8, {2: "byte2 = 0：没有键按下 → 0x0f 消失 = 松开"}),
     "PID 变成 DATA1：每成功收一个包 toggle 翻转一次"),
    ("按下 s：DATA0", "kbd9: usage 0x16 down -> 's' -> consoleintr()", (3, 1),
     data_pkt("DATA0", [0, 0, 0x16, 0, 0, 0, 0, 0], {2: "usage 0x16 = 's'"})),
    ("按下 Enter：DATA0", "kbd9: usage 0x28 down -> 0xa -> consoleintr()   exec: /bin/ls", (3, 1),
     data_pkt("DATA0", [0, 0, 0x28, 0, 0, 0, 0, 0], {2: "usage 0x28 = Enter → consoleintr('\\n') → sh 执行 ls"})),
]


# ------------------------------------------------- call chain for each packet
_CTRL_HUB = lambda req, stage: [
    (0, "dwc2_probe() → dwc2_init()            dwc2.c，进程上下文（启动时）"),
    (1, f"control(1, {req})"),
    (2, f"channel_xfer(0, addr 1, ep 0, {stage})   ch0，CPU 轮询"),
    (3, "wr(HCSPLT0=0) wr(HCDMA0) wr(HCTSIZ0) wr(HCCHAR0|ChEna) → while(!(rd(HCINT0) & XferCompl))"),
]
_SPLIT = lambda stage, extra=None: [
    (0, "dwc2_init() → get_device_descriptor(0, ctrl_buf)"),
    (1, "control(0, 0x80, GET_DESCRIPTOR, 0x100, 0, buf, 8)"),
    (2, f"channel_xfer(0, 0, 0, {stage})：split = usb_route[0].hub != 0"),
    (3, extra or "wr(HCSPLT0, SPLTENA|XACTPOS_ALL|HUBADDR(1)|PRTADDR(3)) → HCCHAR0|ChEna"),
]
_IRQ = [(0, "kernelvec → kerneltrap() → devintr()：gic_iar() = 9（IRQ_PENDING_1 b9）"),
        (1, "dwc2_irq()：GINTSTS & GINTMSK & HCHINT；channels = HAINT & HAINTMSK")]
CHAINS = {
    "SETUP 令牌：GET_STATUS(port 3) 的第一个包": _CTRL_HUB("0xa3, GET_STATUS, 0, port 3, ctrl_buf, 4", "OUT, PID_SETUP, setup_buf, 8"),
    "DATA0：8 字节 SETUP 内容": _CTRL_HUB("0xa3, GET_STATUS, 0, 3, ctrl_buf, 4", "OUT, PID_SETUP, setup_buf, 8")
        [:2] + [(2, "setup_buf = {type, request, value, index, length}（struct usb_setup，packed）"),
                (2, "channel_xfer(0, 1, 0, OUT, CONTROL, 64, setup_buf, 8, PID_SETUP) → cache_clean_range")],
    "bmRequestType = 0xa3 逐位": [(0, "control()：s->type = 0xa3；int in = (type & 0x80) != 0 → 数据阶段方向")],
    "握手：Hub 回 ACK": [(0, "channel_xfer()：rd(HCINT0) = XferCompl|ChHltd|ACK → 退出轮询"),
                        (1, "返回 len（OUT 时 ACK 即算成功）")],
    "DATA1：Hub 回 4 字节端口状态": _CTRL_HUB("0xa3, GET_STATUS, 0, 3, ctrl_buf, 4", "IN, PID_DATA1, ctrl_buf, 4")[:3]
        + [(3, "XferCompl → left = HCTSIZ0 & 0x7ffff → cache_invalidate_range(ctrl_buf) → return 4 - left")],
    "wPortStatus = 0x0301 逐位": [(0, "dwc2_init() 扫 Hub 端口：initial_status = ctrl_buf[0] | ctrl_buf[1] << 8"),
                                 (1, "if(initial_status & HUB_PORT_CONNECTION) → dwc2_hub_bitmap |= 1 << 3")],
    "wPortChange = 0x0001 逐位": [(0, "initial_change = ctrl_buf[2] | ctrl_buf[3] << 8"),
                                 (1, "printf(\"dwc2: hub port %d status=%x change=%x\")")],
    "DATA0：SET_FEATURE(PORT_RESET, 3)": _CTRL_HUB("0x23, SET_FEATURE, HUB_PORT_RESET, port 3, ctrl_buf, 0", "OUT, PID_SETUP, setup_buf, 8"),
    "bmRequestType = 0x23 逐位": [(0, "control(1, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_RESET, port, ctrl_buf, 0)")],
    "复位后 GET_STATUS 返回 03 03 11 00": [(0, "dwc2_init()：for(reset_wait < 50) { udelay(10000); control(1, 0xa3, GET_STATUS…) }"),
                                         (1, "直到 !(status & HUB_PORT_RESET_STAT) && (status & HUB_PORT_ENABLE)")],
    "wPortChange = 0x0011": [(0, "control(1, 0x23, CLEAR_FEATURE, HUB_C_PORT_CONNECTION(16), 3)"),
                            (0, "control(1, 0x23, CLEAR_FEATURE, HUB_C_PORT_RESET(20), 3)"),
                            (0, "usb_route[0] = {hub 1, port 3, low_speed = status & HUB_PORT_LOW_SPEED}")],
    "通道寄存器 HCSPLT0 = 0x8000c083": _SPLIT("OUT, PID_SETUP"),
    "通道寄存器 HCCHAR0 = 0x00020040": _SPLIT("OUT, PID_SETUP", "hcchar = DEVADDR(0)|EPNUM(0)|EPTYPE(CONTROL)|MPS(mps)；usb_route[0].low_speed → |LSPDDEV"),
    "通道寄存器 HCTSIZ0 = 0x60080008": _SPLIT("OUT, PID_SETUP", "wr(HCTSIZ0, XFERSIZE(8)|PKTCNT(1)|PID(PID_SETUP=3))"),
    "① SSPLIT 令牌（高速）": _SPLIT("OUT, PID_SETUP", "complete_split = 0 → HCSPLT0 不带 CompSplt → 硬件发 SSPLIT"),
    "② SETUP 令牌：地址 0，端点 0": _SPLIT("OUT, PID_SETUP", "硬件按 HCCHAR0（addr 0、EP0）和 HCTSIZ0.PID=SETUP 生成令牌"),
    "③ DATA0：80 06 00 01 00 00 08 00": _SPLIT("OUT, PID_SETUP", "HCDMA0 = 0xc0000000|V2P(setup_buf)：DMA 读出这 8 字节"),
    "④ TT 回 ACK → HCINT0 = 0x22": [(0, "channel_xfer()：if(split && !complete_split && (intr & ACK) && (intr & CHHLTD))"),
                                   (1, "halt_channel(0); wr(HCINT0, intr); complete_split = 1; udelay(125)"),
                                   (1, "goto restart_channel")],
    "⑤ CSPLIT 令牌（SC=1）": [(0, "restart_channel：wr(HCSPLT0, … | HCSPLT_COMPSPLT)"),
                             (1, "wr(HCCHAR0, hcchar | ChEna) → 硬件发 CSPLIT + SETUP"),
                             (1, "NYET → udelay(125) 再 CSPLIT；ACK + XferCompl → 返回 8")],
    "⑥ 数据阶段：SSPLIT + IN 令牌": [(0, "control()：if(len) channel_xfer(0, 0, 0, in=1, CONTROL, mps, data, 8, PID_DATA1, …)"),
                                   (1, "split && len > mps ? 按 mps 拆成多次递归 channel_xfer（每包翻 DATA0/1）")],
    "⑦ 键盘的 DATA1（经 CSPLIT 拿回来）": [(0, "channel_xfer()：CSPLIT → XferCompl"),
                                         (1, "left = rd(HCTSIZ0) & 0x7ffff；cache_invalidate_range(buf, len)"),
                                         (1, "get_device_descriptor()：ep0_mps = descriptor[7]；usb_ep0_mps[0] = 8")],
    "⑧ HCINT0 = 0x21：CSPLIT 拿到数据": [(0, "channel_xfer()：if(intr & (XFERCOMPL|CHHLTD)) break → return len - left")],
    "⑨ 状态阶段：DATA1，0 字节": [(0, "control()：channel_xfer(0, addr, 0, !in, CONTROL, mps, ctrl_buf, 0, PID_DATA1, …)"),
                                (1, "packets = len ? … : 1 → HCTSIZ0 = PKTCNT(1)|XFERSIZE(0)|PID(DATA1)")],
    "设备描述符 byte 8..17": [(0, "get_device_descriptor(0)：第二次 control(0, 0x80, GET_DESCRIPTOR, 0x100, 0, buf, 18)"),
                             (1, "dwc2_init()：vid = ctrl_buf[8] | ctrl_buf[9] << 8；pid = ctrl_buf[10] | ctrl_buf[11] << 8")],
    "SET_ADDRESS(4)": [(0, "dwc2_init()：usb_address = port + 1"),
                       (1, "control(0, 0x00, USB_SET_ADDRESS, usb_address, 0, ctrl_buf, 0)"),
                       (1, "udelay(5000); usb_ep0_mps[4] = 8; usb_route[4] = usb_route[0]")],
    "配置描述符头 9 字节": [(0, "dwc2_init()：control(4, 0x80, GET_DESCRIPTOR, CONFIG << 8, 0, ctrl_buf, 9)"),
                           (1, "total = ctrl_buf[2] | ctrl_buf[3] << 8 = 59 → 再 control(…, ctrl_buf, total)")],
    "bmAttributes = 0xa0": [(0, "（xv6 不检查供电/唤醒属性，只用 bConfigurationValue = ctrl_buf[5]）")],
    "接口描述符 0（boot 键盘）": [(0, "dwc2_init()：for(off; ctrl_buf[off] >= 2; off += ctrl_buf[off])"),
                                 (1, "dtype == 4 && class 3 && subclass 1 && protocol 1|2"),
                                 (2, "pending = &usb_hid_child[0]：class/subclass/protocol/interface_number")],
    "HID 描述符": [(0, "dtype == 0x21：dwc2 跳过（boot 协议格式固定，不读报告描述符）")],
    "端点描述符 0x81": [(0, "dtype == 5 && (addr & 0x80) && (attr & 3) == EPTYPE_INTERRUPT"),
                        (1, "pending->interrupt_in_ep = 1；interrupt_in_max_packet = 8；interrupt_in_interval = 10")],
    "bEndpointAddress = 0x81": [(0, "pending->interrupt_in_ep = ctrl_buf[off + 2] & 0xf")],
    "接口 1 + 端点 0x82（被忽略）": [(0, "dtype == 4 但 subclass 0 → pending = 0 → 不发布 usb_device")],
    "SET_CONFIGURATION(1)": [(0, "dwc2_init()：control(4, 0x00, USB_SET_CONFIG, cfg = 1, 0, ctrl_buf, 0)"),
                             (1, "usb_device_register(&usb_hid_child[0]) → usb_match → usb_probe → usbkbd_probe")],
    "HID SET_PROTOCOL(Boot)": [(0, "usbkbd_probe(udev)"),
                               (1, "udev->ops->control = dwc2_usb_control(udev, 0x21, HID_REQ_SET_PROTOCOL, 0, intf 0)"),
                               (2, "control(4, 0x21, 0x0b, 0, 0, 0, 0) → channel_xfer(ch0, split)")],
    "bmRequestType = 0x21 逐位": [(0, "usbkbd.c：udev->ops->control(udev, 0x21, …)")],
    "HID SET_IDLE(0)": [(0, "usbkbd_probe()：udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0, intf, 0, 0)"),
                        (1, "→ input_register_device → usb_fill_int_urb → usb_submit_urb")],
    "HCCHAR6 = 0x810e8808": [(0, "usb_submit_urb(urb) → dwc2_submit_urb(urb)：interrupt_rx[0].pending = 1"),
                             (1, "dwc2_interrupt_schedule_locked() → dwc2_interrupt_rx_start(req)"),
                             (2, "wr(HCSPLT6) wr(HCDMA6) wr(HCTSIZ6) wr(HAINTMSK|=1<<6) wr(HCCHAR6|ChEna)")],
    "SSPLIT 令牌（ET = 11 中断）": [(0, "dwc2_interrupt_rx_start()：req->complete_split = 0 → HCSPLT6 = 8000c083"),
                                   (1, "HCCHAR6.EPType = 3（中断）→ SPLIT 令牌 ET = 11")],
    "IN 令牌：地址 4，端点 1": [(0, "HCCHAR6：DEVADDR(4) | EPNUM(urb->endpoint = 1) | EPDIR_IN"),
                               (1, "TT ACK → HCINT6 0x22 → IRQ 9 → dwc2_irq() → dwc2_interrupt_rx_irq_fast()"),
                               (2, "dwc2_wait_frame_delta(issue, 2) → complete_split = 1 → dwc2_interrupt_rx_start()（CSPLIT）")],
    "没按键：键盘回 NAK": _IRQ + [(2, "dwc2_interrupt_rx_irq_fast() 返回 0（不是 split 中间态）"),
                                (2, "wr(HAINTMSK, ~0x40)；dwc2_irq_pending |= 0x40；schedule_work(&req->work)")],
    "HCINT6 = 0x12：NAK": [(0, "system_wq：dwc2_interrupt_urb_work() → dwc2_interrupt_rx_complete(req)"),
                           (1, "intr & HCINT_NAK → dwc2_interrupt_release_locked(req)"),
                           (1, "mod_delayed_work(&dwc2_periodic_wq, &req->retry_work, 1 jiffy)"),
                           (2, "10 ms 后 dwc2_interrupt_retry_work() → pending = 1 → schedule_locked() → 再 arm")],
    "按下 l：键盘的 DATA0": _IRQ + [(2, "schedule_work(&interrupt_rx[0].work)"),
                                  (2, "→ dwc2_interrupt_urb_work → dwc2_interrupt_rx_complete：XferCompl, left = 0")],
    "修饰键字节 byte0 逐位": [(0, "usbkbd_rx_work()：for(i < 8) old_down = previous[0] & 1<<i; new_down = report[0] & 1<<i"),
                             (1, "usbkbd_key()：shifted = modifiers & (1<<1 | 1<<5)；control = modifiers & (1<<0 | 1<<4)")],
    "HCINT6 = 0x23：有数据": [(0, "dwc2_interrupt_rx_complete()：cache_invalidate_range(report, 8)；toggle ^= 1"),
                             (1, "release + schedule_locked → usb_hcd_giveback_urb(urb, 0, 8)"),
                             (2, "usbkbd_irq_complete() → queue_work(&usbkbd_wq, &kbd->rx_work)"),
                             (3, "usbkbd_rx_work() → usbkbd_key() → consoleintr('l') + input_report_key(KEY_L, 1)")],
    "HAINT = 0x40": _IRQ + [(2, "if((channels & 1<<6) && dwc2_interrupt_rx_irq_fast()) → 中间态已处理")],
    "松开 l：DATA1 全 0": [(0, "usbkbd_rx_work()：previous[2] = 0x0f 在新报告里找不到"),
                         (1, "input_report_key(input, KEY_L, 0) → input_sync() → memmove(previous, report)"),
                         (1, "usb_submit_urb(kbd->irq_urb) → 回到 dwc2_submit_urb")],
    "按下 s：DATA0": [(0, "usbkbd_rx_work() → usbkbd_key(kbd, 0, 0x16) → consoleintr(keymap[0x16] = 's')")],
    "按下 Enter：DATA0": [(0, "usbkbd_key(kbd, 0, 0x28) → consoleintr('\\n')"),
                         (1, "console.c：cons.w = cons.e；wakeup(&cons.r) → sh 的 consoleread() 返回 \"ls\\n\""),
                         (2, "sh → fork → exec(\"/bin/ls\")")],
}
