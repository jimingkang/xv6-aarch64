"""Raspberry Pi 3B+ SDIO Wi-Fi animation, driven by a WIFI_TRACE=1 serial log.

Every "wifi<step>: t=<ms> ..." line becomes one animation beat, in log order;
the bottom console prints the line itself and the clock shows its t=.

Scenes
  Bringup   wifi1-wifi5 (+ the boot-time wifi6 BCDC commands): Arasan probe,
            CMD0/5/3/7/52 enumeration, CHIPID through the F1 window, firmware
            and NVRAM into TCM, CR4 released, F2 + FWREADY, wlan0, IRQ on
  Join      from the first command after IRQ enable to SET_PM=0: BCDC setup,
            SET_SSID, firmware events, EAPOL M1-M4, DHCP, power save off
  DataPath  the rest: DAT1 IRQ -> NAPI -> RX frames (tx_max credit), TX
            frames (seq / window / credit wait)

Render (repository root):
  tools/sdio_wifi_diagrams/manim/render_sdio_wifi.sh [l|m|h] [log]
Log: SDIO_WIFI_LOG (default: sample-wifi-trace.log next to this file).  A log
whose first line starts with "# SAMPLE" is drawn with a "示例日志" banner.
Capture a real one on the Pi 3B+:  make WIFI_TRACE=1 && make install-rpi3 ...
then save the serial console (e.g. `script -q wifi.log screen /dev/tty.usbserial 115200`).
"""
import os
import re
from pathlib import Path

from manim import (
    BLUE, DOWN, GREEN, GREY, GREY_B, GREY_D, LEFT, ORANGE, PURPLE, RED, RIGHT,
    UP, WHITE, YELLOW, TEAL, Create, Dot, FadeIn, FadeOut, Flash, Indicate, Line,
    Rectangle, RoundedRectangle, Scene, Text, Transform, VGroup, config,
    MoveAlongPath, ReplacementTransform, AnimationGroup, Succession, Wait,
)

CJK = os.environ.get("ANIM_CJK", "PingFang SC")
MONO = os.environ.get("ANIM_MONO", "Menlo")
config.background_color = "#14171c"

LOG = Path(os.environ.get("SDIO_WIFI_LOG", Path(__file__).with_name("sample-wifi-trace.log")))
RAW = LOG.read_text(errors="replace").splitlines()
SAMPLE = bool(RAW) and RAW[0].startswith("# SAMPLE")
RX_LINE = re.compile(r"^.*?(wifi(\d+): t=(\d+) (.*))$")
EVENTS = []
for raw in RAW:
    m = RX_LINE.match(raw.rstrip())
    if m:
        EVENTS.append((int(m.group(2)), int(m.group(3)), m.group(4), m.group(1)))
if not EVENTS:
    raise SystemExit(f"sdio_wifi_anim: no 'wifiN: t=' lines in {LOG}")


def split_scenes():
    """Bringup ends at the IRPT_EN line, Join ends at SET_PM / DHCP."""
    a = next((i for i, e in enumerate(EVENTS) if "IRPT_EN" in e[2]), len(EVENTS) - 1) + 1
    b = next((i for i, e in enumerate(EVENTS) if i >= a and "power save off" in e[2]), None)
    if b is None:
        b = next((i for i, e in enumerate(EVENTS) if i >= a and "IPv4 ready" in e[2]), len(EVENTS) - 1)
    return EVENTS[:a], EVENTS[a:b + 1], EVENTS[b + 1:]


PARTS = split_scenes()


def T(s, size=22, color=WHITE, mono=False, **kw):
    return Text(s, font=MONO if mono else CJK, font_size=size, color=color, **kw)


def fit(m, w):
    if m.width > w:
        m.scale_to_fit_width(w)
    return m


def kv(text, key, default=None, conv=str):
    m = re.search(rf"\b{re.escape(key)}=(-?[0-9a-fx:]+)", text)
    if not m:
        return default
    try:
        return conv(m.group(1))
    except ValueError:
        return default


def box(title, sub, w, h, col, pos):
    r = RoundedRectangle(width=w, height=h, corner_radius=0.12, color=col, stroke_width=2.5)
    r.set_fill(col, opacity=0.12)
    t = fit(T(title, 22, WHITE), w - 0.2)
    s = fit(T(sub, 15, GREY_B, mono=True), w - 0.2)
    g = VGroup(t, s).arrange(DOWN, buff=0.08)
    g.move_to(r)
    v = VGroup(r, g).move_to(pos)
    v.rect = r
    return v


class Stage(Scene):
    PART = 0
    TITLE = ""

    # ------------------------------------------------------------ layout
    def setup_stage(self):
        title = T(self.TITLE, 30, WHITE).to_edge(UP, buff=0.25)
        self.clock = T("t = 0 ms", 20, GREY_B, mono=True).to_corner(UP + RIGHT, buff=0.3)
        self.add(title, self.clock)
        if SAMPLE:
            ban = T("示例日志（按代码推出，非实机记录）", 18, ORANGE).move_to([-4.6, -2.35, 0])
            self.add(ban)

        self.brcm = box("brcmfmac", "brcmf0_wq · NAPI", 3.0, 0.95, PURPLE, [-5.2, 1.7, 0])
        self.core = box("SDIO core", "sdio_cmd52/53", 3.0, 0.85, BLUE, [-5.2, 0.45, 0])
        self.host = box("Arasan host", "0x3f300000 · IRQ 62", 3.0, 0.85, YELLOW, [-5.2, -0.8, 0])
        self.f0 = box("F0 CCCR", "CMD52", 1.7, 0.62, GREY_B, [0.6, -1.55, 0])
        self.f1 = box("F1 backplane", "32 KB window", 1.7, 0.62, BLUE, [0.6, -0.8, 0])
        self.f2 = box("F2 FIFO", "SDPCM", 1.7, 0.62, GREEN, [0.6, -0.05, 0])
        self.cr4 = box("CR4 固件", "BCM43455.BIN", 2.0, 0.85, ORANGE, [3.2, 0.9, 0])
        self.ap = box("AP", "WPA2-PSK", 1.6, 0.85, PURPLE, [6.1, 0.9, 0])
        chip = RoundedRectangle(width=5.2, height=3.9, corner_radius=0.2, color=ORANGE, stroke_width=1.5)
        chip.move_to([2.15, 0.15, 0]).set_stroke(opacity=0.6)
        chiplab = T("CYW43455", 16, ORANGE).next_to(chip, UP, buff=0.05).align_to(chip, LEFT)
        # TCM bar
        self.tcm = Rectangle(width=2.0, height=0.3, color=GREY_B, stroke_width=1.5).move_to([3.2, 2.0, 0])
        self.tcm_lab = T("TCM 0x198000", 14, GREY_B, mono=True).next_to(self.tcm, UP, buff=0.05)
        self.tcm_fill = Rectangle(width=0.001, height=0.28, stroke_width=0).set_fill(ORANGE, 0.8)
        self.tcm_fill.move_to(self.tcm.get_left(), aligned_edge=LEFT)
        # bus
        self.bus = VGroup(*[Line([-3.7, -0.8 + d, 0], [-0.25, -0.8 + d, 0], stroke_width=1.6,
                                 color=RED if i == 3 else GREY) for i, d in enumerate([-0.25, -0.15, -0.05, 0.05, 0.15, 0.25])])
        self.bus_lab = T("SDIO 4-bit · 400 kHz", 15, GREY_B, mono=True).next_to(self.bus, DOWN, buff=0.12)
        self.dat1 = T("DAT1 = IRQ", 13, RED).next_to(self.bus, UP, buff=0.06)
        links = VGroup(
            Line(self.brcm.get_bottom(), self.core.get_top(), color=GREY_D),
            Line(self.core.get_bottom(), self.host.get_top(), color=GREY_D),
            Line([-0.25, -0.8, 0], self.f1.get_left(), color=GREY_D),
            Line(self.f1.get_left() + [-0.1, 0, 0], self.f0.get_left(), color=GREY_D),
            Line(self.f1.get_left() + [-0.1, 0, 0], self.f2.get_left(), color=GREY_D),
            Line(self.f2.get_right(), self.cr4.get_bottom() + [-0.5, 0, 0], color=GREY_D),
            Line(self.cr4.get_right(), self.ap.get_left(), color=PURPLE).set_stroke(opacity=0.5),
        )
        # credit gauge
        self.gauge_lab = T("TX 信用  tx_seq=–  tx_max=–", 17, TEAL, mono=True).move_to([3.2, -2.3, 0])
        # console
        self.console = VGroup()
        self.cons_bg = Rectangle(width=14.2, height=1.05, stroke_width=0).set_fill("#0b0d10", 1).to_edge(DOWN, buff=0.05)
        self.cap = T("", 22)
        self.add(chip, chiplab, links, self.brcm, self.core, self.host, self.f0, self.f1, self.f2,
                 self.cr4, self.ap, self.tcm, self.tcm_lab, self.tcm_fill, self.bus, self.bus_lab,
                 self.dat1, self.gauge_lab, self.cons_bg)
        self.lines = []
        if self.PART >= 1:      # state left behind by the earlier scenes
            full = Rectangle(width=2.0 * 0.85, height=0.28, stroke_width=0).set_fill(ORANGE, 0.8)
            full.move_to(self.tcm.get_left(), aligned_edge=LEFT)
            tail = Rectangle(width=0.15, height=0.28, stroke_width=0).set_fill(TEAL, 0.9)
            tail.move_to(self.tcm.get_right(), aligned_edge=RIGHT)
            self.remove(self.tcm_fill)
            self.tcm_fill = full
            self.add(full, tail)
            self.cr4.rect.set_color(GREEN)
            self.remove(self.bus_lab)
            self.bus_lab = T("SDIO 4-bit · 25 MHz", 15, GREY_B, mono=True).next_to(self.bus, DOWN, buff=0.12)
            self.add(self.bus_lab)
        if self.PART >= 2:
            self.ap.rect.set_color(GREEN)

    # ------------------------------------------------------------ helpers
    def log(self, line, t):
        self.lines = (self.lines + [line])[-3:]
        new = VGroup(*[fit(T(l, 15, GREEN if i == len(self.lines) - 1 else GREY, mono=True), 13.8)
                       for i, l in enumerate(self.lines)]).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        new.move_to(self.cons_bg).align_to(self.cons_bg, LEFT).shift(RIGHT * 0.15)
        clock = T(f"t = {t} ms", 20, GREY_B, mono=True).move_to(self.clock, aligned_edge=RIGHT)
        self.remove(self.console)
        self.console = new
        self.add(new)
        self.remove(self.clock)
        self.clock = clock
        self.add(clock)

    def caption(self, s, color=WHITE):
        c = fit(T(s, 24, color), 9.5).move_to([-1.0, 2.75, 0])
        anim = ReplacementTransform(self.cap, c) if len(self.cap) else FadeIn(c)
        self.cap = c
        return anim

    def packet(self, path, label, color, rt=0.7):
        pts = [p if not hasattr(p, "get_center") else p.get_center() for p in path]
        d = Dot(pts[0], radius=0.11, color=color)
        lab = fit(T(label, 15, color, mono=True), 3.2)
        lab.add_updater(lambda m: m.next_to(d, UP, buff=0.06))
        self.add(d, lab)
        for a, b in zip(pts, pts[1:]):
            self.play(MoveAlongPath(d, Line(a, b)), run_time=rt / (len(pts) - 1))
        lab.clear_updaters()
        self.play(FadeOut(d), FadeOut(lab), run_time=0.15)

    def flash(self, m, color=YELLOW):
        self.play(Indicate(m, color=color, scale_factor=1.06), run_time=0.45)

    def set_gauge(self, seq, tmax, extra=""):
        win = (tmax - seq) & 0xff if seq is not None and tmax is not None else None
        col = TEAL if win and win < 0x80 else RED
        s = f"TX 信用  tx_seq={seq}  tx_max={tmax}  window={win}{extra}"
        g = fit(T(s, 17, col, mono=True), 5.6).move_to([3.2, -2.3, 0])
        self.play(Transform(self.gauge_lab, g), run_time=0.25)

    # ------------------------------------------------------------ one log line
    def beat(self, step, t, text, line, fast=False):
        self.log(line, t)
        HOST = [self.brcm, self.core, self.host, [-0.25, -0.8, 0]]
        if step == 1 and "probe" in text:
            self.play(self.caption("① Arasan host：GPIO34–39 切到 ALT3，35–39 上拉"), run_time=0.4)
            self.flash(self.host)
        elif step == 1 and "clock" in text:
            hz = kv(text, "clock", 0, int) if "clock=" in text else int(re.search(r"clock (\d+)", text).group(1))
            lab = fit(T(f"SDIO 4-bit · {hz // 1000} kHz" if hz < 1000000 else f"SDIO 4-bit · {hz // 1000000} MHz",
                        15, GREY_B, mono=True), 4).move_to(self.bus_lab)
            self.play(Transform(self.bus_lab, lab), Indicate(self.bus, color=YELLOW), run_time=0.5)
        elif step == 2:
            if "BUS_INTERFACE" in text or "bus width" in text:
                lbl = "CMD52 4-bit" if "CCCR" in text else "CONTROL0 4-bit"
            else:
                m = re.search(r"(CMD\d+)", text)
                lbl = m.group(1) if m else "CMD"
                if "FBR" in text:
                    lbl = "CMD52 " + re.search(r"FBR\d", text).group(0) + " → " + (kv(text, "device", "") or "")
            if not fast:
                self.play(self.caption("② SDIO 枚举：CMD0 / CMD5 / CMD3 / CMD7 / CMD52"), run_time=0.3)
            self.packet([self.core, self.host, [-0.25, -0.8, 0], self.f0], lbl, BLUE, 0.6 if not fast else 0.3)
        elif step == 3:
            if "CHIPID" in text:
                self.play(self.caption("③ F1 窗口 0x18000000 → 读 ChipCommon CHIPID"), run_time=0.3)
                self.packet([self.brcm, self.core, self.host, [-0.25, -0.8, 0], self.f1], "CMD53 F1 @8000", BLUE)
                self.packet([self.f1, [-0.25, -0.8, 0], self.host, self.core, self.brcm],
                            f"{kv(text, 'CHIPID', '?')} = BCM{kv(text, 'chip', '?')}", GREEN)
            elif "bootfs" in text:
                self.play(self.caption("③ 从 bootfs 读 BIN / TXT / CLM：" +
                                       f"{kv(text, 'BIN', '?')} / {kv(text, 'TXT', '?')} / {kv(text, 'CLM', '?')} B"), run_time=0.4)
                self.flash(self.brcm, ORANGE)
            else:
                self.play(self.caption("③ CMD52 CCCR IO_ENABLE |= F1"), run_time=0.3)
                self.flash(self.f1, BLUE)
        elif step == 4:
            if "EROM" in text:
                self.play(self.caption("④ 解析 EROM：ChipCommon / SDIO / CR4 / D11"), run_time=0.4)
            elif "TCM base" in text:
                self.play(self.caption(f"④ TCM RAM {kv(text, 'size', '?')} 字节"), Indicate(self.tcm), run_time=0.5)
            elif "set_passive" in text:
                self.play(self.caption("④ halt CR4（D11 保持复位）"), self.cr4.rect.animate.set_color(RED), run_time=0.5)
            elif "BIN window" in text:
                done, total = map(int, re.search(r"(\d+)/(\d+) bytes", text).groups())
                self.grow_tcm(done / max(total, 1) * 0.85, 0.2)
            elif "BIN " in text:
                n = int(re.search(r"(\d+) windows", text).group(1))
                ms = re.search(r"(\d+) ms", text)
                self.play(self.caption(f"④ 固件经 F1 写入 TCM：{n} 个 32 KB 窗口，CMD53 每块 {kv(text, 'chunk', '?')} B，"
                                       f"{ms.group(1) if ms else '?'} ms"), run_time=0.4)
                for i in range(min(n, 19)):
                    self.packet([self.brcm, self.host, [-0.25, -0.8, 0], self.f1, self.tcm], f"窗口 {i}", ORANGE, 0.22)
                    self.grow_tcm((i + 1) / n * 0.85, 0.05)
            elif "NVRAM" in text:
                nv = re.search(r"NVRAM (\d+)", text).group(1)
                self.play(self.caption(f"④ NVRAM {nv} B 写到 RAM 尾部"), run_time=0.3)
                tail = Rectangle(width=0.15, height=0.28, stroke_width=0).set_fill(TEAL, 0.9)
                tail.move_to(self.tcm.get_right(), aligned_edge=RIGHT)
                self.play(FadeIn(tail), run_time=0.3)
            elif "set_active" in text:
                self.play(self.caption("④ reset vector 写地址 0，释放 CR4 → 固件开始运行"),
                          self.cr4.rect.animate.set_color(GREEN), run_time=0.6)
        elif step == 5:
            if "F2 ready" in text:
                self.play(self.caption("⑤ CCCR IO_ENABLE |= F2：固件起来后 F2 才 ready"), run_time=0.3)
                self.flash(self.f2, GREEN)
            elif "FWREADY" in text:
                self.play(self.caption("⑤ mailbox FWREADY，SDPCM 协议 4"), run_time=0.3)
                self.packet([self.cr4, self.f1, [-0.25, -0.8, 0], self.host, self.brcm], "FWREADY", GREEN, 0.6)
            elif "INT_ENABLE" in text:
                self.play(self.caption("⑤ CCCR INT_ENABLE = master | F1"), run_time=0.3)
                self.packet([self.core, self.host, [-0.25, -0.8, 0], self.f0], "CMD52 INT_ENABLE", BLUE, 0.4)
            elif "wlan0" in text:
                self.play(self.caption("⑤ register_netdev(wlan0)，NAPI 挂在 brcmf0_wq"), run_time=0.4)
                self.flash(self.brcm, PURPLE)
            elif "IRPT_EN" in text:
                self.play(self.caption("⑤ Arasan IRPT_EN = CARD_INT：DAT1 → IRQ 62 打开"), Indicate(self.dat1, color=RED), run_time=0.6)
        elif step == 6:
            self.dcmd(text, fast)
        elif step == 7:
            self.join(text)
        elif step == 8:
            n = re.search(r"x(\d+)", text)
            self.play(self.caption(f"⑧ DAT1 拉低 → CARD_INT → IRQ 62（×{n.group(1) if n else 1}）→ 顶半部屏蔽 + napi_schedule", RED),
                      Indicate(self.dat1, color=RED), run_time=0.35)
            self.play(Flash(self.host.get_right(), color=RED, line_length=0.3), Indicate(self.host, color=RED), run_time=0.4)
            self.flash(self.brcm, RED)
        elif step == 9:
            ch = kv(text, "ch", "?")
            lab = {"0": "ch0 回复", "1": "ch1 事件", "2": "ch2 数据"}.get(ch, "ch" + ch) + f" {kv(text, 'len', '?')}B"
            if "superframe" in text:
                lab, col = "superframe 丢弃", RED
            elif "bad header" in text:
                lab, col = "坏头 RF_TERM", RED
            else:
                col = GREEN
            self.packet([self.f2, self.f1, [-0.25, -0.8, 0], self.host, self.core, self.brcm], lab, col, 0.45 if fast else 0.7)
            seq, tmax = kv(text, "tx_seq", None, int), kv(text, "tx_max", None, int)
            if seq is not None and tmax is not None:
                self.set_gauge(seq, tmax)
        elif step == 10:
            seq, tmax, wait = kv(text, "seq", None, int), kv(text, "tx_max", None, int), kv(text, "credit-wait", 0, int)
            typ = kv(text, "type", "")
            name = {"888e": "EAPOL", "800": "IPv4", "806": "ARP"}.get(typ, typ)
            if wait:
                self.set_gauge(seq, seq, f"  等信用 ×{wait}")
                self.play(self.caption(f"⑩ window = 0：先读上行帧拿新 tx_max（等了 {wait} 次 × 100 µs）", ORANGE), run_time=0.4)
                self.packet([self.f2, self.f1, [-0.25, -0.8, 0], self.host, self.brcm], "新 tx_max", TEAL, 0.5)
            self.set_gauge(seq, tmax)
            self.packet([self.brcm, self.core, self.host, [-0.25, -0.8, 0], self.f1, self.f2, self.cr4, self.ap],
                        f"seq {seq} {name} {kv(text, 'len', '?')}B", PURPLE, 0.6 if fast else 0.9)
        elif step == 11:
            m = re.search(r"frames=(\d+)", text)
            sp = kv(text, "spurious-before", "0")
            extra = f"（之前 {sp} 次假中断）" if sp not in (None, "0") else ""
            self.play(self.caption(f"⑪ NAPI 读了 {m.group(1) if m else '?'} 帧，FIFO 空 → 重开 CARD_INT{extra}", TEAL),
                      Indicate(self.brcm, color=TEAL), run_time=0.5)
        else:
            self.wait(0.2)

    def grow_tcm(self, frac, rt):
        new = Rectangle(width=max(0.001, 2.0 * frac), height=0.28, stroke_width=0).set_fill(ORANGE, 0.8)
        new.move_to(self.tcm.get_left(), aligned_edge=LEFT)
        self.play(Transform(self.tcm_fill, new), run_time=rt)

    NAMES = {"2": "UP", "3": "DOWN", "20": "SET_INFRA", "22": "SET_AUTH", "23": "GET_BSSID", "26": "SET_SSID",
             "86": "SET_PM", "134": "SET_WSEC", "268": "SET_WSEC_PMK"}

    def dcmd(self, text, fast):
        if " tx " in text:
            cmd = kv(text, "cmd", "?")
            m = re.search(r"cmd=\d+ (\S+) (GET|SET)", text)
            name = m.group(1) if m else self.NAMES.get(cmd, "cmd " + cmd)
            idn = kv(text, "id", "?")
            if not fast:
                self.play(self.caption(f"⑥ BCDC 控制命令 id={idn}：{name}（SDPCM ch0 → F2）", BLUE), run_time=0.3)
            self.packet([self.brcm, self.core, self.host, [-0.25, -0.8, 0], self.f1, self.f2, self.cr4],
                        f"id{idn} {name}", BLUE, 0.35 if fast else 0.8)
        else:
            st = kv(text, "status", "0")
            col = GREEN if st == "0" else RED
            self.packet([self.cr4, self.f2, self.f1, [-0.25, -0.8, 0], self.host, self.core, self.brcm],
                        f"id{kv(text, 'id', '?')} status={st}", col, 0.35 if fast else 0.8)

    def join(self, text):
        if "join SET_SSID" in text:
            self.play(self.caption("⑦ SET_SSID：固件自己扫描 / 认证 / 关联", PURPLE), run_time=0.4)
            self.packet([self.cr4, self.ap], "auth / assoc", PURPLE, 0.6)
            self.packet([self.ap, self.cr4], "assoc resp", PURPLE, 0.6)
        elif text.startswith("event"):
            self.play(self.caption(f"⑦ 固件事件 type={kv(text, 'type', '?')} status={kv(text, 'status', '?')}（ch1）", PURPLE), run_time=0.4)
        elif "EAPOL rx" in text:
            ki = kv(text, "key-info", "")
            msg = "M1（ANonce）" if ki in ("8a", "08a") else "M3（加密 GTK）" if ki and ki.endswith("ca") else "EAPOL"
            self.play(self.caption(f"⑦ AP → {msg}，经 F2 进 NAPI", PURPLE), run_time=0.3)
            self.packet([self.ap, self.cr4, self.f2], msg, PURPLE, 0.6)
        elif "M2 tx" in text or "M4 tx" in text:
            msg = "M2" if "M2" in text else "M4"
            self.play(self.caption(f"⑦ 主机算好 {msg}（PTK / MIC）→ AP", PURPLE), run_time=0.3)
            self.packet([self.cr4, self.ap], msg, PURPLE, 0.5)
        elif "M3 rx" in text:
            self.play(self.caption("⑦ 校验 M3 的 MIC，AES unwrap 出 GTK", PURPLE), run_time=0.4)
        elif "associated" in text:
            self.play(self.caption("⑦ GET_BSSID 返回 AP 的 BSSID：已关联", GREEN), Indicate(self.ap, color=GREEN), run_time=0.5)
        elif "WPA2 done" in text:
            self.play(self.caption("⑦ PTK + GTK 装进固件，handshake_done = 1，数据口打开", GREEN),
                      self.ap.rect.animate.set_color(GREEN), run_time=0.6)
        elif "DHCP" in text:
            self.play(self.caption("⑦ DHCP 完成：wlan0 有了 IPv4 地址", GREEN), run_time=0.5)
        elif "power save" in text:
            self.play(self.caption("⑦ SET_PM = 0：关省电，不再等 beacon（每包少 ~100 ms）", GREEN), run_time=0.6)
        else:
            self.play(self.caption("⑦ " + text[:40]), run_time=0.3)

    def construct(self):
        self.setup_stage()
        evs = PARTS[self.PART]
        if not evs:
            self.play(self.caption("（日志里没有这一段）", GREY), run_time=0.5)
            self.wait(1)
            return
        prev_step, run = None, 0
        for step, t, text, line in evs:
            run = run + 1 if step == prev_step else 1
            prev_step = step
            fast = (step in (2, 6, 9, 10) and run > 4)
            self.beat(step, t, text, line, fast)
        self.wait(1.2)


class Bringup(Stage):
    PART = 0
    TITLE = "SDIO Wi-Fi ①  上电 → 枚举 → 下载固件 → wlan0"


class Join(Stage):
    PART = 1
    TITLE = "SDIO Wi-Fi ②  BCDC 命令 → 关联 → WPA2 → DHCP"


class DataPath(Stage):
    PART = 2
    TITLE = "SDIO Wi-Fi ③  DAT1 中断 → NAPI 收帧 → 信用流控发送"
