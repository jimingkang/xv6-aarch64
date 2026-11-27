"""xv6-aarch64 (java branch) SDIO Wi-Fi: every bus command, animated.

Scenes:
  Hardware     SDIO host (Arasan) + 6 wires + BCM43455 functions F0/F1/F2
  ChipInside   cores on the AXI backplane, EROM, how the SDIO device core
               exposes them as F0/F1/F2 (brcmf_scan_cores, brcmfmac.c:602)
  Wires        CMD52 uses only CMD; CMD53 data on DAT[3:0]; DAT1 card IRQ
  ArgEncode    CMD52 / CMD53 argument bits with four real examples
  Enumerate    CMD0, CMD5, CMD3, CMD7, CMD52 (bus width, FBR, CIS)  sdio.c
  Download     F1 window -> backplane, firmware chunks, NVRAM, CR4 start
  ControlUp    start_f2 + control plane bring-up commands
  RxIrq        DAT1 interrupt -> NAPI -> CMD52 / F1 / F2 reads -> dispatch
  TxDcmd       BCDC request: frame build, window, CMD53 F2 write, reply

Render: manim -qh --save_sections sdio_cmd_anim.py Hardware ChipInside Wires ArgEncode Enumerate Download ControlUp RxIrq TxDcmd
"""
import os
from manim import *

CJK = os.environ.get("ANIM_CJK", "WenQuanYi Zen Hei")
MONO = os.environ.get("ANIM_MONO", "DejaVu Sans Mono")
config.background_color = WHITE

INK = "#222222"; SUB = "#666666"
BLU = "#2f6fb8"; GRN = "#2e9e4f"; RED_ = "#c0392b"; ORG = "#d35400"
PUR = "#7048b6"; GLD = "#b7860b"; F0C = "#3b8a3e"; F1C = "#3d5bd9"; F2C = "#b7860b"
CPUF = "#e8f1fb"; CHIPF = "#fdf0e6"


def T(s, size=24, color=INK, mono=False, weight=NORMAL, **kw):
    return Text(s, font=MONO if mono else CJK, font_size=size, color=color, weight=weight, **kw)


def title(s):
    return T(s, 34, INK, weight=BOLD).to_edge(UP, buff=0.3)


def footer(s):
    return T(s, 16, SUB).to_edge(DOWN, buff=0.15)


def box(w, h, stroke=BLU, fill=WHITE, r=0.12, sw=2.5):
    return RoundedRectangle(corner_radius=r, width=w, height=h, stroke_color=stroke,
                            stroke_width=sw, fill_color=fill, fill_opacity=1)


def labeled(w, h, head, body=None, stroke=BLU, fill=WHITE, hs=22, bs=16):
    b = box(w, h, stroke, fill)
    g = VGroup(b)
    ht = T(head, hs, stroke, weight=BOLD).move_to(b.get_top() + DOWN * 0.3)
    g.add(ht)
    if body:
        bt = VGroup(*[T(x, bs, INK) for x in body]).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        bt.next_to(ht, DOWN, buff=0.15)
        if bt.width > w - 0.3:
            bt.scale_to_fit_width(w - 0.3)
        g.add(bt)
    return g


def pkt(s, color, size=18, txt=WHITE):
    t = T(s, size, txt, weight=BOLD)
    r = RoundedRectangle(corner_radius=0.08, width=t.width + 0.3, height=t.height + 0.2,
                         fill_color=color, fill_opacity=1, stroke_width=0)
    t.move_to(r)
    return VGroup(r, t)


def caption(s, size=22, color=INK):
    return T(s, size, color).to_edge(DOWN, buff=0.55)


class Base(Scene):
    def mark(self, name):
        self.next_section(name[:120])

    def say(self, mob_old, s, size=22, color=INK, wait=0.2):
        self.mark(s)
        new = caption(s, size, color)
        if mob_old is None:
            self.play(FadeIn(new, shift=UP * 0.2), run_time=0.6)
        else:
            self.play(FadeOut(mob_old), FadeIn(new, shift=UP * 0.2), run_time=0.6)
        self.wait(wait)
        return new


# --------------------------------------------------------------------------
class Hardware(Base):
    def construct(self):
        self.add(title("① 硬件：SDIO host、6 根线、芯片里的三个功能号"))
        cpu = box(4.0, 5.6, BLU, CPUF).move_to(LEFT * 4.9 + DOWN * 0.3)
        cpu_t = T("BCM2837（xv6 内核）", 22, BLU, weight=BOLD).next_to(cpu.get_top(), DOWN, buff=0.2)
        layers = VGroup(
            labeled(3.5, 0.85, "brcmfmac.c", ["SDPCM 帧 / BCDC 命令 / NAPI"], PUR, "#f4f0fb", 18, 14),
            labeled(3.5, 0.85, "sdio.c", ["sdio_cmd52() / sdio_cmd53()"], PUR, "#f4f0fb", 18, 14),
            labeled(3.5, 1.15, "Arasan EMMC 控制器", ["= SDIO host 硬件", "ARG1 / CMDTM / RESP0 / DATA"], BLU, WHITE, 18, 14),
            labeled(3.5, 0.75, "GIC  IRQ 62", ["卡中断进 CPU"], RED_, "#fde8e8", 18, 14),
        ).arrange(DOWN, buff=0.18).next_to(cpu_t, DOWN, buff=0.2)
        self.play(FadeIn(cpu), Write(cpu_t))
        self.play(LaggedStart(*[FadeIn(l, shift=DOWN * 0.2) for l in layers], lag_ratio=0.25))

        chip = box(4.6, 5.6, ORG, CHIPF).move_to(RIGHT * 4.6 + DOWN * 0.3)
        chip_t = T("BCM43455 Wi-Fi 芯片", 22, ORG, weight=BOLD).next_to(chip.get_top(), DOWN, buff=0.2)
        f0 = labeled(4.1, 1.15, "F0  CCCR / FBR / CIS", ["SDIO 规范规定：使能、中断、", "总线宽度、厂商号"], F0C, "#eef6ee", 18, 14)
        f1 = labeled(4.1, 1.35, "F1  寄存器 + 32KB 窗口", ["0x1000a-c SBADDR 选窗口", "0x0000-0x7FFF → 芯片内部总线"], F1C, "#e9f0ff", 18, 14)
        f2 = labeled(4.1, 1.15, "F2  数据 FIFO", ["所有 SDPCM 帧：", "控制 ch0 / 事件 ch1 / 数据 ch2"], F2C, "#fff4d6", 18, 14)
        fs = VGroup(f0, f1, f2).arrange(DOWN, buff=0.18).next_to(chip_t, DOWN, buff=0.2)
        self.play(FadeIn(chip), Write(chip_t))
        self.play(LaggedStart(*[FadeIn(f, shift=DOWN * 0.2) for f in fs], lag_ratio=0.25))

        x0, x1 = cpu.get_right()[0], chip.get_left()[0]
        names = [("CLK", "GPIO34", SUB), ("CMD", "GPIO35", F0C), ("DAT0", "GPIO36", F2C),
                 ("DAT1", "GPIO37", RED_), ("DAT2", "GPIO38", F2C), ("DAT3", "GPIO39", F2C)]
        wires = VGroup()
        for i, (n, g, c) in enumerate(names):
            y = 1.6 - i * 0.62
            ln = Line([x0, y, 0], [x1, y, 0], color=c, stroke_width=5 if n == "DAT1" else 3)
            lb = T(f"{n}  {g}", 15, c, weight=BOLD).next_to(ln, UP, buff=0.04)
            wires.add(VGroup(ln, lb))
        self.play(LaggedStart(*[Create(w) for w in wires], lag_ratio=0.15), run_time=2)
        cap = self.say(None, "SDIO host 就是主机侧的硬件控制器：软件写寄存器，它在线上产生时序")
        self.play(Indicate(layers[2], color=BLU, scale_factor=1.05))
        self.wait(1)
        cap = self.say(cap, "一张卡可有多个功能（FN 0..7），每个功能有自己独立的 17 位地址空间")
        for f in fs:
            self.play(Indicate(f, scale_factor=1.05), run_time=0.8)
        cap = self.say(cap, "命令里的 3 位 FN 字段决定这次访问哪一个功能的地址空间")
        self.wait(1.5)
        bp = labeled(4.1, 1.0, "芯片内部 AXI backplane", ["ChipCommon · SDIO core · ARM CR4 · TCM RAM"], ORG, WHITE, 16, 13)
        bp.next_to(chip, DOWN, buff=-1.05).set_z_index(3)
        cap = self.say(cap, "F1 的窗口通向芯片内部总线；F2 只是一个进出数据帧的 FIFO")
        self.play(f2.animate.set_opacity(0.25))
        self.play(FadeIn(bp, shift=UP * 0.2))
        self.play(Circumscribe(f1, color=F1C))
        self.wait(2)


# --------------------------------------------------------------------------
class Wires(Base):
    def construct(self):
        self.add(title("③ 6 根线怎么配合：CMD52、CMD53、DAT1 中断"))
        host = labeled(2.4, 4.4, "SDIO host", ["Arasan"], BLU, CPUF, 22, 16).move_to(LEFT * 5.6 + DOWN * 0.2)
        card = labeled(2.4, 4.4, "SDIO 卡", ["BCM43455"], ORG, CHIPF, 22, 16).move_to(RIGHT * 5.6 + DOWN * 0.2)
        self.add(host, card)
        x0, x1 = host.get_right()[0], card.get_left()[0]
        names = [("CLK", SUB), ("CMD", F0C), ("DAT0", F2C), ("DAT1", F2C), ("DAT2", F2C), ("DAT3", F2C)]
        ys = {}
        lines = VGroup()
        for i, (n, c) in enumerate(names):
            y = 1.6 - i * 0.7
            ys[n] = y
            ln = Line([x0, y, 0], [x1, y, 0], color=c, stroke_width=3)
            lb = T(n, 16, c, weight=BOLD, mono=True).next_to(ln.get_start(), UR, buff=0.05)
            lines.add(VGroup(ln, lb))
        self.play(Create(lines), run_time=1.5)
        # clock ticks
        ticks = VGroup(*[Line([x, ys["CLK"] - 0.12, 0], [x, ys["CLK"] + 0.12, 0], color=SUB, stroke_width=2)
                         for x in np.arange(x0 + 0.3, x1, 0.3)])
        cap = self.say(None, "CLK 由 host 输出，所有线都按这个时钟一位一位采样")
        self.play(Create(ticks), run_time=1)

        cap = self.say(cap, "CMD52 读 F0 0x05：48 位命令只在 CMD 线上，从 host 串行送到卡")
        c = pkt("CMD52  R  FN0  addr 0x05", F0C).move_to([x0 + 1.6, ys["CMD"], 0])
        self.play(FadeIn(c))
        self.play(c.animate.move_to([x1 - 1.6, ys["CMD"], 0]), run_time=1.6)
        self.play(FadeOut(c))
        r = pkt("R5  flags | data = 0x02", GRN).move_to([x1 - 1.6, ys["CMD"], 0])
        cap = self.say(cap, "卡把那 1 个字节放进 R5 响应的低 8 位，也走 CMD 线回来")
        self.play(FadeIn(r))
        self.play(r.animate.move_to([x0 + 1.6, ys["CMD"], 0]), run_time=1.6)
        idle = T("DAT0-3 全程空闲", 20, SUB).move_to([0, (ys["DAT1"] + ys["DAT2"]) / 2, 0])
        self.play(FadeIn(idle))
        cap = self.say(cap, "CMD52 根本不碰 DAT 线：读写单个寄存器、查中断都很便宜", color=F0C)
        self.wait(1.5)
        self.play(FadeOut(r), FadeOut(idle))

        cap = self.say(cap, "CMD53 读 F2 64 字节：命令和 R5 响应仍然走 CMD 线")
        c = pkt("CMD53  R  FN2  OP=0  64B", F2C).move_to([x0 + 1.6, ys["CMD"], 0])
        self.play(FadeIn(c))
        self.play(c.animate.move_to([x1 - 1.6, ys["CMD"], 0]), run_time=1.2)
        self.play(FadeOut(c))
        r = pkt("R5", GRN).move_to([x1 - 1.0, ys["CMD"], 0])
        self.play(FadeIn(r))
        self.play(r.animate.move_to([x0 + 1.0, ys["CMD"], 0]), run_time=1.0)
        self.play(FadeOut(r))
        cap = self.say(cap, "数据块走 DAT[3:0]：每个时钟同时 4 位，64 字节 = 128 个时钟 + 每线 CRC16")
        blocks = VGroup(*[pkt(f"bit{3-i}…", F2C, 15).move_to([x1 - 1.4, ys[f"DAT{3-i}"], 0]) for i in range(4)])
        self.play(FadeIn(blocks))
        self.play(*[b.animate.move_to([x0 + 1.4, b.get_center()[1], 0]) for b in blocks], run_time=2)
        self.play(FadeOut(blocks))

        cap = self.say(cap, "DAT1 还兼作中断线：DAT 空闲时，卡把 DAT1 拉低 = 有中断", color=RED_)
        d1 = lines[3][0]
        low = Line(d1.get_start() + DOWN * 0.18, d1.get_end() + DOWN * 0.18, color=RED_, stroke_width=6)
        self.play(Transform(d1, low))
        irq = pkt("EMMC_INTERRUPT.INT_CARD_INT → IRQ 62", RED_, 16).next_to(host, UP, buff=0.1).align_to(host, LEFT)
        self.play(FadeIn(irq, shift=DOWN * 0.2))
        cap = self.say(cap, "4 位模式下只能在“中断周期”（不传数据时）拉低；DAT2 是 Read Wait，不是中断")
        self.wait(2.5)


# --------------------------------------------------------------------------
class ArgEncode(Base):
    def construct(self):
        self.add(title("④ CMD52 / CMD53 的 32 位参数：地址是功能里的寄存器地址"))
        fields = [("R/W", 1, F1C), ("FN", 3, F1C), ("RAW|BLK", 1, SUB), ("OP", 1, RED_),
                  ("寄存器地址 (17 位)", 17, F2C), ("·", 1, SUB), ("数据 / 字节数", 8, F0C)]
        unit = 11.5 / 32
        bar = VGroup()
        x = -5.75
        for name, bits, col in fields:
            r = Rectangle(width=bits * unit, height=0.7, stroke_color=INK, stroke_width=1.5,
                          fill_color=col, fill_opacity=0.18).move_to([x + bits * unit / 2, 2.0, 0])
            t = T(name, 15 if bits > 2 else 12, INK, weight=BOLD).move_to(r)
            bar.add(VGroup(r, t))
            x += bits * unit
        bits_lbl = VGroup(T("31", 13, SUB, mono=True).next_to(bar[0], UP, buff=0.05),
                          T("25:9", 13, SUB, mono=True).next_to(bar[4], UP, buff=0.05),
                          T("8:0 (CMD52 用 7:0)", 13, SUB).next_to(bar[6], UP, buff=0.05))
        self.play(LaggedStart(*[FadeIn(b) for b in bar], lag_ratio=0.1), FadeIn(bits_lbl))
        cap = self.say(None, "sdio.c:166  arg = W<<31 | FN<<28 | addr<<9 | value      sdio.c:197 CMD53 另有 OP<<26 和字节数", 18)
        ex = [
            ("CMD52 读 F0 0x05（INT_PENDING）", ["0", "0", "0", "-", "0x00005", "", "0x00"], "0x00000A00",
             "地址 0x05 是 CCCR 寄存器，读回的字节在 R5 里"),
            ("CMD52 写 F1 0x1000a = 0x80（SBADDRLOW）", ["1", "1", "0", "-", "0x1000a", "", "0x80"], "0x92001480",
             "地址 0x1000a 是 F1 的窗口寄存器，0x80 写进去"),
            ("CMD53 写 F1 0x9A00，512 字节（固件一块）", ["1", "1", "0", "1", "0x09A00", "", "0 (=512)"], "0x95340000",
             "OP=1 自增；0x9A00 = 窗口内偏移 0x1A00 | 0x8000"),
            ("CMD53 读 F2 0x8000，64 字节（收帧头）", ["0", "2", "0", "0", "0x08000", "", "64"], "0x21000040",
             "OP=0 地址固定：F2 是 FIFO，地址只是入口"),
        ]
        vals = VGroup()
        rows = VGroup()
        for i, (name, v, hexv, note) in enumerate(ex):
            nv = VGroup(*[T(s, 16, INK, mono=True).move_to([bar[j].get_center()[0], 1.15, 0]) for j, s in enumerate(v)])
            h = T(name, 20, INK, weight=BOLD).move_to([0, 0.35, 0])
            a = T(f"arg = {hexv}", 26, BLU, mono=True, weight=BOLD).move_to([0, -0.35, 0])
            n = T(note, 18, SUB).move_to([0, -0.95, 0])
            grp = VGroup(nv, h, a, n)
            self.mark(name + "  " + hexv)
            if i == 0:
                self.play(FadeIn(grp, shift=UP * 0.2))
            else:
                self.play(FadeOut(rows[-1]), FadeIn(grp, shift=UP * 0.2))
            rows.add(grp)
            self.wait(2.6)
        cap = self.say(cap, "这些地址都不是主机内存地址，也不直接是芯片内存地址，而是“所选功能里的寄存器地址”", 19, RED_)
        layers = VGroup(
            pkt("主机内存地址：只在 xv6 里（brcmf_io_buf），不上总线", BLU, 17),
            pkt("SDIO 功能地址：17 位，CMD52/53 里的 addr 字段", F0C, 17),
            pkt("芯片 backplane 地址：32 位，经 F1 窗口到达", ORG, 17),
        ).arrange(DOWN, buff=0.15).move_to(DOWN * 2.2)
        self.play(FadeOut(rows[-1]), LaggedStart(*[FadeIn(l, shift=UP * 0.2) for l in layers], lag_ratio=0.3))
        self.wait(3)


# --------------------------------------------------------------------------
class SeqScene(Base):
    """Two-lifeline sequence helper."""
    def lifelines(self, left="SDIO host（sdio.c / Arasan）", right="BCM43455", y_top=2.55, y_bot=-3.0):
        self.xl, self.xr = -4.2, 4.2
        hl = pkt(left, BLU, 18).move_to([self.xl, y_top, 0])
        hr = pkt(right, ORG, 18).move_to([self.xr, y_top, 0])
        ll = DashedLine([self.xl, y_top - 0.3, 0], [self.xl, y_bot, 0], color=SUB)
        lr = DashedLine([self.xr, y_top - 0.3, 0], [self.xr, y_bot, 0], color=SUB)
        self.play(FadeIn(hl), FadeIn(hr), Create(ll), Create(lr))
        self.y = y_top - 0.6

    def onarrow(self, t, a):
        bg = Rectangle(width=t.width + 0.16, height=t.height + 0.06, fill_color=WHITE,
                       fill_opacity=1, stroke_width=0)
        return VGroup(bg, t).move_to(a.get_center())

    def exchange(self, req, resp, col, note=None, dy=0.62, size=16):
        self.mark(req + ("  →  " + resp if resp else ""))
        a = Arrow([self.xl, self.y, 0], [self.xr, self.y, 0], buff=0, color=col, stroke_width=3,
                  max_tip_length_to_length_ratio=0.03)
        rt = self.onarrow(T(req, size, col, weight=BOLD), a)
        self.play(GrowArrow(a), FadeIn(rt), run_time=0.7)
        g = VGroup(a, rt)
        if resp:
            b = Arrow([self.xr, self.y - 0.25, 0], [self.xl, self.y - 0.25, 0], buff=0, color=GRN,
                      stroke_width=2, max_tip_length_to_length_ratio=0.03)
            bt = self.onarrow(T(resp, size - 2, GRN), b)
            self.play(GrowArrow(b), FadeIn(bt), run_time=0.6)
            g.add(b, bt)
        if note:
            nt = T(note, 14, SUB).next_to([self.xr, self.y - 0.1, 0], RIGHT, buff=0.15)
            self.play(FadeIn(nt), run_time=0.3)
            g.add(nt)
        self.y -= dy
        self.wait(0.6)
        return g


class Enumerate(SeqScene):
    def construct(self):
        self.add(title("⑤ 枚举：sdio_scan_host()  sdio.c:332-410"))
        self.lifelines()
        cap = self.say(None, "arasan_probe → mmc_add_host → sdio_scan_host：每条命令都是写 ARG1 + CMDTM", 19)
        steps = [
            ("CMD0  GO_IDLE", None, SUB, ":332 复位，无响应"),
            ("CMD5  arg=0", "R4: OCR，bit30:28 = 功能数 2", F0C, ":333"),
            ("CMD5  arg=OCR  （循环）", "R4 bit31 IORDY = 1，卡上电完成", F0C, ":341"),
            ("CMD3  SEND_RELATIVE_ADDR", "R6: 高 16 位 = RCA", F0C, ":351"),
            ("CMD7  arg=RCA<<16  选中卡", "R1", F0C, ":355  之后才能 CMD52/53"),
            ("CMD52  W FN0 0x07 = …10b", "R5  → host 切 4 位总线", F1C, ":358-362"),
            ("CMD52  R FN0 0x100 / 0x109-0x10b", "R5  F1 类别、CIS 指针", F1C, ":392-397  FBR"),
            ("CMD52  R FN0 CIS 元组（逐字节）", "MANFID: 0x02d0 / 0xa9bf", F1C, ":268 read_cis_id"),
        ]
        for req, resp, col, note in steps:
            self.exchange(req, resp, col, note, dy=0.6, size=16)
        cap = self.say(cap, "厂商 0x02d0 = Broadcom，设备 0xa9bf = BCM43455 → 注册 sdio0:1 / sdio0:2，匹配 brcmfmac", 19, GRN)
        self.wait(3)


# --------------------------------------------------------------------------
class Download(Base):
    def construct(self):
        self.add(title("⑥ 下载固件和 NVRAM：F1 窗口如何通向芯片 RAM"))
        # host side
        hostb = labeled(3.0, 2.0, "xv6 内存", ["BCM43455.BIN (595KB)", "brcmf_io_buf[512]"], BLU, CPUF, 20, 15).move_to(LEFT * 5.4 + UP * 1.0)
        # F1 space
        f1 = box(2.8, 4.4, F1C, "#e9f0ff").move_to(LEFT * 1.4 + DOWN * 0.4)
        f1t = T("F1 地址空间（17 位）", 18, F1C, weight=BOLD).next_to(f1, UP, buff=0.1)
        reg = Rectangle(width=2.5, height=1.3, color=F1C, fill_color=WHITE, fill_opacity=1).move_to(f1.get_top() + DOWN * 0.85)
        regt = VGroup(T("0x1000a LOW", 15, INK, mono=True), T("0x1000b MID", 15, INK, mono=True),
                      T("0x1000c HIGH", 15, INK, mono=True)).arrange(DOWN, buff=0.06).move_to(reg)
        win = Rectangle(width=2.5, height=1.9, color=F1C, fill_color=WHITE, fill_opacity=1).move_to(f1.get_bottom() + UP * 1.15)
        wint = VGroup(T("0x0000-0x7FFF", 15, INK, mono=True), T("32KB 窗口", 16, F1C, weight=BOLD),
                      T("(|0x8000 = 4B 访问)", 13, SUB)).arrange(DOWN, buff=0.08).move_to(win)
        # backplane
        bp = box(3.6, 5.5, ORG, CHIPF).move_to(RIGHT * 4.5 + DOWN * 0.05)
        bpt = T("芯片 backplane（32 位）", 18, ORG, weight=BOLD).next_to(bp, UP, buff=0.1)
        cc = Rectangle(width=3.2, height=1.0, color=INK, fill_color=WHITE, fill_opacity=1).move_to(bp.get_top() + DOWN * 0.7)
        cct = VGroup(T("0x18000000 ChipCommon", 14, INK, mono=True), T("+ 各核寄存器（EROM 给出）", 13, SUB)).arrange(DOWN, buff=0.05).move_to(cc)
        ram = Rectangle(width=3.2, height=3.6, color=INK, fill_color=WHITE, fill_opacity=1).move_to(bp.get_bottom() + UP * 2.0)
        ramt = T("TCM RAM", 16, INK, weight=BOLD).next_to(ram.get_top(), DOWN, buff=0.05)
        ra = T("0x198000", 14, SUB, mono=True).next_to(ram.get_corner(UL), LEFT, buff=0.08)
        re_ = T("+ramsize", 14, SUB, mono=True).next_to(ram.get_corner(DL), LEFT, buff=0.08)
        self.play(FadeIn(hostb), FadeIn(f1), Write(f1t), FadeIn(reg), FadeIn(regt), FadeIn(win), FadeIn(wint))
        self.play(FadeIn(bp), Write(bpt), FadeIn(cc), FadeIn(cct), FadeIn(ram), FadeIn(ramt), FadeIn(ra), FadeIn(re_))
        cap = self.say(None, "例子：写固件偏移 0x1A00 那一块 → 目标 backplane 地址 0x198000 + 0x1A00 = 0x199A00", 19)
        self.wait(1)
        cap = self.say(cap, "第 1 步：窗口 = 0x199A00 & 0xffff8000 = 0x198000，>>8 = 0x1980，分 3 次 CMD52 写入", 19)
        vals = ["0x80", "0x19", "0x00"]
        for i in range(3):
            p = pkt(f"CMD52 W F1 → {vals[i]}", F0C, 15).next_to(hostb, DOWN, buff=0.3)
            self.play(FadeIn(p))
            self.play(p.animate.move_to(regt[i].get_center() + RIGHT * 0.0).scale(0.8), run_time=0.8)
            self.play(FadeOut(p), regt[i].animate.set_color(F0C), run_time=0.3)
        # window highlight on RAM top part
        wr = Rectangle(width=3.2, height=1.0, stroke_color=F1C, stroke_width=4, fill_color=F1C, fill_opacity=0.15)
        wr.move_to(ram.get_top() + DOWN * 0.8)
        wl = T("窗口 0x198000-0x19FFFF", 13, F1C, weight=BOLD).move_to(wr.get_top() + DOWN * 0.18)
        l1 = DashedLine(win.get_corner(UR), wr.get_corner(UL), color=F1C)
        l2 = DashedLine(win.get_corner(DR), wr.get_corner(DL), color=F1C)
        self.play(Create(wr), FadeIn(wl), Create(l1), Create(l2))
        cap = self.say(cap, "第 2 步：CMD53 W F1 OP=1，地址 0x9A00（0x1A00|0x8000），512 字节走 DAT[3:0]", 19)
        p = pkt("CMD53 W F1 0x9A00  512B", F1C, 15).next_to(hostb, DOWN, buff=0.3)
        self.play(FadeIn(p))
        self.play(p.animate.move_to(win.get_center()).scale(0.8), run_time=0.9)
        dot = Rectangle(width=3.2, height=0.12, fill_color=F2C, fill_opacity=1, stroke_width=0).move_to(wr.get_center() + DOWN * 0.1)
        self.play(ReplacementTransform(p, dot), run_time=0.9)
        tgt = T("→ 0x199A00", 14, F2C, mono=True, weight=BOLD).next_to(dot, RIGHT, buff=0.1)
        self.play(FadeIn(tgt))
        self.wait(1)
        cap = self.say(cap, "每块都重复“3×CMD52 + 1×CMD53”：约 1190 块，块不会跨 32KB 窗口  brcmfmac.c:452,759", 19)
        fw = Rectangle(width=3.2, height=2.0, fill_color=F2C, fill_opacity=0.55, stroke_width=0)
        fw.align_to(ram, UP).align_to(ram, LEFT).shift(DOWN * 0.3)
        fwt = T("固件 .BIN（第一个字 = 复位向量）", 13, INK).move_to(fw)
        self.play(FadeOut(dot), FadeOut(tgt), FadeOut(wr), FadeOut(wl), FadeOut(l1), FadeOut(l2),
                  GrowFromEdge(fw, UP), run_time=2.5)
        self.play(FadeIn(fwt))
        cap = self.say(cap, "NVRAM = BCM43455.TXT：81 条板级参数（晶振、MAC、天线、功放校准、最大功率…）", 19)
        txt = VGroup(T("xtalfreq=37400", 14, INK, mono=True), T("boardtype=0x6e4", 14, INK, mono=True),
                     T("pa2ga0=-129,6525,-718", 14, INK, mono=True), T("maxp2ga0=80   …", 14, INK, mono=True)
                     ).arrange(DOWN, aligned_edge=LEFT, buff=0.05).next_to(hostb, DOWN, buff=0.4)
        self.play(FadeIn(txt))
        cap = self.say(cap, "brcmf_prepare_nvram：key=value\\0 … \\0 补齐 4B，再追加 token = (~字数<<16)|字数", 19)
        packed = pkt("key=value\\0…\\0 | token", GLD, 14).move_to(txt)
        self.play(ReplacementTransform(txt, packed))
        nv = Rectangle(width=3.2, height=0.45, fill_color=GRN, fill_opacity=0.6, stroke_width=0)
        nv.align_to(ram, DOWN).align_to(ram, LEFT)
        nvt = T("NVRAM（贴着 RAM 末尾）", 13, WHITE, weight=BOLD).move_to(nv)
        cap = self.say(cap, "写到 rambase + ramsize - length：固件启动后读 RAM 最后一个字，就找到 NVRAM", 19)
        self.play(ReplacementTransform(packed, nv), run_time=1.5)
        self.play(FadeIn(nvt))
        cap = self.say(cap, "set_active：复位向量写到地址 0，解除 CR4 的 CPUHALT → 芯片里的 ARM 开始跑固件", 19, GRN)
        cr4 = pkt("ARM CR4 运行", GRN, 16).move_to([1.35, cc.get_center()[1], 0]).set_z_index(5)
        self.play(FadeIn(cr4, scale=1.2))
        self.wait(3)


# --------------------------------------------------------------------------
class ControlUp(SeqScene):
    def construct(self):
        self.add(title("⑦ 启动 F2 与控制面  brcmfmac.c:891 / :1734"))
        self.lifelines("xv6 brcmfmac", "BCM43455 固件")
        cap = self.say(None, "前半是 CMD52 + F1 backplane 读写；ready=1 之后全部是 F2 上的 SDPCM 帧", 19)
        steps = [
            ("CMD52 W F1 CHIPCLKCSR |= FORCE_HT", None, F0C, "高速时钟"),
            ("F1 写 TOSBMAILBOXDATA = 协议版本", None, F1C, "邮箱"),
            ("CMD52 W FN0 0x02 bit2；轮询 0x03", "IO_READY bit2 = 1", F0C, "打开 F2"),
            ("F1 写 HOSTINTMASK；CMD52 写 WATERMARK 等", None, F1C, "哪些位能拉 DAT1"),
            ("F1 轮询 TOHOSTMAILBOXDATA", "FWREADY", F1C, "固件就绪"),
            ("F2: iovar ver / cur_etheraddr / clmload", "BCDC 回复（ch0）", F2C, "见 ⑨"),
            ("F2: dcmd UP", "BCDC 回复", F2C, ""),
            ("CMD52 W FN0 0x04 = bit0|bit1", "R5", F0C, "sdio_claim_irq"),
            ("Arasan IRPT_EN |= INT_CARD_INT", None, RED_, "开中断"),
        ]
        for req, resp, col, note in steps:
            self.exchange(req, resp, col, note, dy=0.55, size=15)
        cap = self.say(cap, "connect：DOWN/UP、event_msgs、SET_INFRA/AUTH/WSEC、SET_WSEC_PMK、SET_SSID… 每条都是一次 ⑨", 19, F2C)
        self.wait(3)


# --------------------------------------------------------------------------
class RxIrq(Base):
    def construct(self):
        self.add(title("⑧ 收一帧：DAT1 中断 → NAPI → CMD52/F1/F2"))
        names = [("固件 / SDIO core", ORG), ("SDIO 总线", F0C), ("Arasan / GIC", RED_),
                 ("kworker NAPI", PUR), ("上层", BLU)]
        xs = [-5.6, -2.8, 0.0, 2.8, 5.6]
        heads = VGroup()
        for (n, c), x in zip(names, xs):
            heads.add(pkt(n, c, 16).move_to([x, 2.75, 0]))
            self.add(DashedLine([x, 2.5, 0], [x, -3.1, 0], color="#cccccc"))
        self.play(FadeIn(heads))
        steps = [
            (0, None, "帧进 F2 FIFO，INTSTATUS bit6 置位", ORG),
            (0, 2, "DAT1 拉低", RED_),
            (2, 3, "IRQ62：关 IRPT_EN，napi_schedule", RED_),
            (3, 1, "CMD52 R FN0 0x05", F0C),
            (1, 3, "R5 bit1=1（F1 有中断；0 则伪中断）", F0C),
            (3, 1, "F1：3×CMD52 窗口 + CMD53 读 INTSTATUS", F1C),
            (3, 1, "F1：CMD53 写回 INTSTATUS（W1C）", F1C),
            (1, 0, "卡释放 DAT1", RED_),
            (3, 1, "F2：CMD53 读 64B，len / ~len", F2C),
            (3, 1, "F2：CMD53 读剩余 (len-64)", F2C),
            (3, 4, "ch0 → ctl[]；ch2 → net_rx_dev()", PUR),
            (3, 2, "FIFO 空 → irq_complete 重开 IRPT_EN", RED_),
        ]
        y = 2.2
        cap = None
        for a, b, label, col in steps:
            self.mark(label)
            if b is None:
                m = pkt(label, col, 14).move_to([xs[a] + 1.4, y + 0.08, 0])
                self.play(FadeIn(m), run_time=0.6)
            else:
                ar = Arrow([xs[a], y, 0], [xs[b], y, 0], buff=0, color=col, stroke_width=3,
                           max_tip_length_to_length_ratio=0.04)
                t = T(label, 13, col)
                if t.width > abs(xs[a] - xs[b]) + 1.5:
                    t.scale_to_fit_width(abs(xs[a] - xs[b]) + 1.5)
                bg = Rectangle(width=t.width + 0.12, height=t.height + 0.05, fill_color=WHITE,
                               fill_opacity=1, stroke_width=0)
                t = VGroup(bg, t).move_to(ar.get_center())
                self.play(GrowArrow(ar), FadeIn(t), run_time=0.7)
            y -= 0.46
            self.wait(0.4)
        self.wait(1)
        cap = self.say(cap, "电平中断：INTSTATUS 不清零，DAT1 就一直低；清零后继续读 F2 直到 len=0", 18, RED_)
        self.wait(3)


# --------------------------------------------------------------------------
class TxDcmd(Base):
    def construct(self):
        self.add(title("⑨ 发一条 BCDC 命令：brcmf_dcmd()  brcmfmac.c:1530"))
        parts = [("SDPCM 头 12B", 3.0, F1C, "len|~len|seq|ch0|doff"), ("BCDC 头 16B", 3.0, PUR, "cmd|len|flags(id)|status"),
                 ("参数", 3.6, F2C, "iovar: \"name\\0\" + 值"), ("pad", 0.9, SUB, "→4B")]
        frame = VGroup()
        for n, w, c, s in parts:
            r = Rectangle(width=w, height=0.9, stroke_color=INK, stroke_width=1.5, fill_color=c, fill_opacity=0.2)
            t = VGroup(T(n, 17, INK, weight=BOLD), T(s, 12, SUB)).arrange(DOWN, buff=0.05).move_to(r)
            frame.add(VGroup(r, t))
        frame.arrange(RIGHT, buff=0).move_to(UP * 1.9)
        lbl = T("tx[]（内核静态内存）", 16, SUB).next_to(frame, UP, buff=0.1)
        cap = self.say(None, "持 brcmf_bus.lock，在 tx[] 里拼帧：iovar_buf 的内容拷进参数区", 19)
        self.play(FadeIn(lbl), LaggedStart(*[FadeIn(f, shift=DOWN * 0.2) for f in frame], lag_ratio=0.3))
        seq = VGroup(
            pkt("3×CMD52 W F1 SBADDR → 窗口 = cc_base", F0C, 16),
            pkt("CMD53 W FN2 OP=0 addr=0x8000，每次 ≤512B，数据走 DAT[3:0]", F2C, 16),
            pkt("固件从 FIFO 取帧，执行命令", ORG, 16),
            pkt("回复走 ⑧：ch0 → ctl[]，ctl_id == id 即完成", GRN, 16),
        ).arrange(DOWN, buff=0.25).move_to(DOWN * 0.6)
        for i, s in enumerate(seq):
            self.mark(s[1].text)
            self.play(FadeIn(s, shift=RIGHT * 0.3), run_time=0.7)
            self.wait(1.0)
        cap = self.say(cap, "等回复时先 100µs 轮询 2ms，之后释放锁、睡一个 tick 再查（今天修复后的行为），最长 3 秒", 19, BLU)
        self.wait(2)
        cap = self.say(cap, "发数据帧同理：SDPCM ch2 + 4B BCDC 头 + 以太网帧，发送方向不需要中断", 19)
        self.wait(3)


# --------------------------------------------------------------------------
class ChipInside(Base):
    def construct(self):
        self.add(title("② 芯片内部：各核、AXI backplane 统一地址、SDIO 怎么包装"))
        bus = Rectangle(width=12.6, height=0.55, fill_color=ORG, fill_opacity=0.85, stroke_width=0).move_to(UP * 0.35)
        bust = T("AXI backplane（片内互连）：所有核的寄存器和 RAM 都在同一个 32 位地址空间里", 18, WHITE, weight=BOLD).move_to(bus)
        cap = self.say(None, "BCM43455 里有好几颗“核”，通过 AXI 互连挂在一起：谁都用同一套 32 位地址", 19)
        self.play(FadeIn(bus), Write(bust))
        cores = [
            ("ChipCommon", "id 0x800", ["0x18000000（固定）", "chipid、时钟/电源", "+0xfc = EROM 地址"], INK),
            ("ARM Cortex-R4", "id 0x83e", ["跑 FullMAC 固件", "TCM RAM 0x198000 起", "（固件 + NVRAM + 缓冲）"], GRN),
            ("D11  802.11 MAC", "id 0x812", ["+ PHY / 射频", "收发空口帧，DMA 进 RAM", "下载固件时被复位"], BLU),
            ("SDIO device core", "id 0x829", ["对外是 SDIO 卡", "INTSTATUS +0x20 / 邮箱", "F1 窗口 + F2 DMA"], F1C),
        ]
        boxes = VGroup()
        for i, (n, idn, body, c) in enumerate(cores):
            b = labeled(2.95, 2.0, n, [idn] + body, c, WHITE, 18, 14)
            b.move_to([-4.75 + i * 3.17, -1.45, 0])
            boxes.add(b)
        links = VGroup(*[Line(b.get_top(), [b.get_center()[0], bus.get_bottom()[1], 0], color=c, stroke_width=4)
                         for b, (_, _, _, c) in zip(boxes, cores)])
        for b, l in zip(boxes, links):
            self.play(FadeIn(b, shift=UP * 0.2), Create(l), run_time=0.7)
        cap = self.say(cap, "每个核占一页寄存器（0x18000000 往上按 4KB 排开），另有一页 wrapper（复位/时钟控制）", 19)
        regs = VGroup(*[pkt(t, c, 14) for t, c in [("寄存器页 base", INK), ("wrapper 页 wrap", SUB)]]).arrange(RIGHT, buff=0.2)
        regs.next_to(bus, UP, buff=0.15).shift(LEFT * 3.5)
        self.play(FadeIn(regs))
        cap = self.say(cap, "具体基址不写死：驱动先读 0x18000000+0xfc 得到 EROM，再逐条读出每个核的 id / base / wrap", 19)
        erom = VGroup(T("EROM（芯片里的设备清单）  brcmf_scan_cores  brcmfmac.c:602", 16, INK, weight=BOLD),
                      T("core id=800 base=18000000 wrap=…", 15, INK, mono=True),
                      T("core id=83e base=…        wrap=…", 15, INK, mono=True),
                      T("core id=829 base=…        wrap=…", 15, INK, mono=True),
                      T("（数值以启动日志为准）", 13, SUB)).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        eb = SurroundingRectangle(erom, color=INK, buff=0.12, fill_color=WHITE, fill_opacity=1)
        eg = VGroup(eb, erom).next_to(bus, UP, buff=0.15).shift(RIGHT * 2.3)
        self.play(FadeIn(eg, shift=DOWN * 0.2))
        self.wait(2.5)
        self.play(FadeOut(eg), FadeOut(regs))
        cap = self.say(cap, "SDIO device core 把这一切包装成 SDIO 卡：F0 是规范寄存器，F1 变成 AXI 访问，F2 接 DMA", 19)
        sd = boxes[3]
        pins = VGroup(*[pkt(t, c, 14) for t, c in [("F0 CCCR", F0C), ("F1 窗口 → AXI 读写", F1C),
                                                    ("F2 FIFO ↔ DMA", F2C), ("DAT1 中断", RED_)]]).arrange(DOWN, buff=0.08)
        pins.next_to(bus, UP, buff=0.12).align_to(sd, RIGHT)
        host = pkt("SDIO host（xv6）", BLU, 16).next_to(pins, LEFT, buff=1.0)
        hl = Arrow(host.get_right(), pins.get_left(), buff=0.1, color=BLU)
        self.play(FadeIn(pins, shift=UP * 0.2), FadeIn(host), GrowArrow(hl))
        cap = self.say(cap, "例：主机 CMD53 读 F1 → SDIO core 作为 AXI 主设备去读 ChipCommon 的 chipid", 19, F1C)
        dot = Dot(color=F1C, radius=0.12).move_to(pins[1])
        path = VMobject().set_points_as_corners([pins[1].get_center(), [sd.get_center()[0], bus.get_center()[1], 0],
                                                 [boxes[0].get_center()[0], bus.get_center()[1], 0], boxes[0].get_top()])
        self.play(MoveAlongPath(dot, path), run_time=2.2)
        self.play(Indicate(boxes[0], color=F1C))
        self.play(MoveAlongPath(dot, path.copy().reverse_points()), run_time=1.6)
        self.play(FadeOut(dot))
        cap = self.say(cap, "例：空口收到一帧 → D11 DMA 进 TCM → CR4 固件加 SDPCM 头 → SDIO core DMA 到 F2 FIFO → DAT1", 19, F2C)
        d = Dot(color=F2C, radius=0.12).move_to(boxes[2].get_bottom())
        p1 = VMobject().set_points_as_corners([boxes[2].get_bottom(), boxes[2].get_top(),
                                               [boxes[2].get_center()[0], bus.get_center()[1], 0],
                                               [boxes[1].get_center()[0], bus.get_center()[1], 0], boxes[1].get_top()])
        self.play(MoveAlongPath(d, p1), run_time=2)
        self.play(Indicate(boxes[1], color=GRN))
        p2 = VMobject().set_points_as_corners([boxes[1].get_top(), [boxes[1].get_center()[0], bus.get_center()[1], 0],
                                               [sd.get_center()[0], bus.get_center()[1], 0], pins[2].get_center()])
        self.play(MoveAlongPath(d, p2), run_time=2)
        self.play(Indicate(pins[3], color=RED_), Indicate(pins[2], color=F2C))
        self.play(FadeOut(d))
        cap = self.say(cap, "所以：F1 = 主机伸进芯片总线的一只手；F2 = 固件和主机之间的帧通道；DAT1 = 门铃", 20, INK)
        self.wait(3)
