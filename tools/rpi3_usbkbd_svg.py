#!/usr/bin/env python3
"""Diagrams for doc/readme/rpi3/usb-keyboard.md (Raspberry Pi 3, DWC2 host).

  python3 tools/rpi3_usbkbd_svg.py
  for f in usbkbd-comm-steps usbkbd-overview; do
    rsvg-convert -z 1.5 doc/readme/rpi3/images/$f.svg -o doc/readme/rpi3/images/$f.png; done

usbkbd-comm-steps.svg  steps 0..9 as cards: context, call tree, register /
                       packet formats bit by bit, lock, and the matching lines
                       of the QEMU USBKBD_TRACE=2 log
usbkbd-overview.svg    topology, the CPU contexts the data crosses, and the
                       channel-6 interrupt-IN state machine

Log lines are read from doc/readme/rpi3/logs/qemu-usbkbd-trace2.log
(make USBKBD_TRACE=2; python3 tools/qemu_usbkbd_run.py --text 'echo Hi\\n').
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "doc/readme/rpi3/images")
LOG = os.path.join(ROOT, "doc/readme/rpi3/logs/qemu-usbkbd-trace2.log")
LOG3 = os.path.join(ROOT, "doc/readme/rpi3/logs/qemu-usbkbd-trace3-idle.log")

FONT = "'PingFang SC','Hiragino Sans GB','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"

C = dict(blue="#1f5fae", purple="#7b3fb3", gold="#b07d00", orange="#c25a00",
         green="#2e7d32", teal="#00796b", red="#c62828", gray="#666",
         lblue="#dbe9fb", lgreen="#dff3df", lyel="#fff4c7", lpur="#efe3fb",
         lorange="#ffe6d2", lgray="#eceff1", lred="#fde0e0", lteal="#d7f2ee")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, w):
        self.w = w
        self.out = []

    def o(self, s):
        self.out.append(s)

    def text(self, x, y, s, size=13, fill="#222", anchor="start", weight="normal",
             mono=False, italic=False):
        fam = f' font-family="{MONO}"' if mono else ""
        st = ' font-style="italic"' if italic else ""
        self.o(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
               f'text-anchor="{anchor}" font-weight="{weight}"{fam}{st}>{esc(s)}</text>')

    def rect(self, x, y, w, h, fill="none", stroke="#999", rx=4, sw=1, dash=False, op=1):
        d = ' stroke-dasharray="5 3"' if dash else ""
        self.o(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
               f'fill="{fill}" fill-opacity="{op}" stroke="{stroke}" stroke-width="{sw}"{d}/>')

    def line(self, x1, y1, x2, y2, col="#555", sw=1.3, dash=False, arrow=True):
        d = ' stroke-dasharray="5 3"' if dash else ""
        m = ' marker-end="url(#ar)"' if arrow else ""
        self.o(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
               f'stroke="{col}" stroke-width="{sw}"{d}{m}/>')

    def path(self, d, col="#555", sw=1.3, dash=False, arrow=True):
        da = ' stroke-dasharray="5 3"' if dash else ""
        m = ' marker-end="url(#ar)"' if arrow else ""
        self.o(f'<path d="{d}" fill="none" stroke="{col}" stroke-width="{sw}"{da}{m}/>')

    def save(self, path, h):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{h:.0f}" '
                f'viewBox="0 0 {self.w} {h:.0f}" font-family="{FONT}" font-size="13">'
                '<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" '
                'markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
                '<path d="M0,0 L10,5 L0,10 z" fill="context-stroke"/></marker></defs>'
                f'<rect width="{self.w}" height="{h:.0f}" fill="#fafbfc"/>')
        with open(path, "w") as f:
            f.write(head + "\n".join(self.out) + "</svg>\n")
        print("wrote", path)


# ---------------------------------------------------------------- log lines
def load_log():
    if not os.path.exists(LOG):
        return []
    with open(LOG, "rb") as f:
        raw = f.read().decode("utf-8", "replace")
    # The console echo of a typed char lands at the start of the next trace
    # line ("ekbd9: ..."); strip it so every quoted line is a clean record.
    lines = []
    for l in raw.splitlines():
        l = re.sub(r"^[a-zA-Z ](kbd\d:)", r"\1", l.rstrip("\r"))
        lines.append(l)
    return lines


LOGL = load_log()


def grab(pattern, n=1, after=None):
    """First n log lines matching pattern (regex), optionally after a line
    matching `after`."""
    out, armed = [], after is None
    for l in LOGL:
        if not armed:
            armed = re.search(after, l) is not None
            continue
        if re.search(pattern, l):
            out.append(l)
            if len(out) >= n:
                break
    return out


def logs(*specs):
    res = []
    for s in specs:
        if isinstance(s, tuple):
            res += grab(*s)
        else:
            res += grab(s)
    return res


# ------------------------------------------------------------- comm steps
W = 1400
X0, CW = 30, W - 60            # card x / width
LX = 90                        # left label column
BX = 170                       # body x


def card_height(c):
    h = 70 + 18 * len(c.get("desc", []))
    if c.get("calls"):
        h += 22 + 18 * len(c["calls"])
    for f in c.get("fmt", []):
        h += 30 + 36 + 18 * ((len(f.get("legend", [])) + 1) // 2) + 14
    if c.get("logs"):
        h += 40 + 17 * len(c["logs"])
    return h + 10


def bitfield(s, x, y, w, f):
    """32-bit register (or n-byte packet when f['bytes'])."""
    s.text(x - 2, y + 12, "▸ " + f["title"], 12.5, C["orange"])
    y += 24
    nbits = f.get("nbits", 32)
    cell = w / nbits
    # bit numbers
    for b in range(nbits):
        xb = x + (nbits - 1 - b) * cell
        if f.get("bytes"):
            if b % 8 == 7:
                s.text(xb + cell * 4, y - 2, f"byte {(nbits - 1 - b) // 8}", 10, "#888", "middle")
        elif b % 4 == 3 or b == 0 or nbits <= 16:
            s.text(xb + cell / 2, y - 2, str(b), 9.5, "#888", "middle")
    y += 4
    for hi, lo, label, col in f["fields"]:
        if f.get("bytes"):   # hi/lo are byte indexes, byte 0 on the left
            xa, xb = x + hi * 8 * cell, x + (lo + 1) * 8 * cell
        else:
            xa, xb = x + (nbits - 1 - hi) * cell, x + (nbits - lo) * cell
        s.rect(xa, y, xb - xa, 26, C.get(col, col), "#555", rx=0, sw=0.8)
        t = label
        size = 11.5 if (xb - xa) > 7 * len(t) else 9.5
        s.text((xa + xb) / 2, y + 17, t, size, "#222", "middle")
    y += 40
    leg = f.get("legend", [])
    half = (len(leg) + 1) // 2
    for i, (bits, name, mean) in enumerate(leg):
        col = 0 if i < half else 1
        row = i if i < half else i - half
        xx = x + col * w / 2
        yy = y + row * 18
        s.text(xx, yy, bits, 11.5, "#222", mono=True, weight="bold")
        s.text(xx + 70, yy, name, 11.5, C["blue"])
        s.text(xx + 180, yy, mean, 11.5, "#333")
    return y + 18 * half + 14


def draw_card(s, y, c):
    h = card_height(c)
    col = c.get("color", C["blue"])
    s.rect(X0, y, CW, h, "#fff", "#c9ced6", rx=10, sw=1.2)
    s.o(f'<circle cx="{X0 + 32}" cy="{y + 30}" r="17" fill="{col}"/>')
    s.text(X0 + 32, y + 36, str(c["n"]), 17, "#fff", "middle", "bold")
    s.text(X0 + 62, y + 30, c["title"], 17, "#111", weight="bold")
    s.text(X0 + 62, y + 52, c["ctx"], 12.5, col)
    if c.get("lock"):
        s.text(X0 + CW - 20, y + 30, "锁：" + c["lock"], 12, C["gold"], "end")
    yy = y + 74
    for d in c.get("desc", []):
        s.text(X0 + 62, yy, d, 12.8, "#333")
        yy += 18
    if c.get("calls"):
        yy += 8
        s.text(LX, yy + 4, "函数", 12.5, C["orange"])
        for depth, code, note in c["calls"]:
            xx = BX + depth * 22
            if depth:
                s.text(xx - 16, yy + 4, "└", 12, "#999", mono=True)
            s.text(xx, yy + 4, code, 12.2, C["purple"], mono=True)
            if note:
                s.line(xx + 7.3 * len(code) + 8, yy, 880, yy, "#ddd", 1, dash=True, arrow=False)
                s.text(890, yy + 4, note, 11.8, "#555")
            yy += 18
        yy += 4
    if c.get("fmt"):
        s.text(LX, yy + 14, "数据格式", 12.5, C["orange"])
        for f in c["fmt"]:
            yy = bitfield(s, BX, yy, CW - BX - 30, f) + 2
    if c.get("logs"):
        lh = 30 + 17 * len(c["logs"])
        s.rect(BX - 80, yy, CW - BX + 70, lh, "#f5f6f8", "#d5d9df", rx=6)
        s.text(BX - 68, yy + 18, c.get("loghdr", "日志（QEMU raspi3b，make USBKBD_TRACE=2，"
               "键入 \"echo Hi\"；原样摘录）"), 11.5, "#777")
        for i, l in enumerate(c["logs"]):
            if len(l) > 150:
                l = l[:147] + "..."
            s.text(BX - 68, yy + 38 + 17 * i, l, 11.6, "#1b3a57", mono=True)
        yy += lh + 10
    return y + h + 18


HPRT0 = dict(title="HPRT0 @0x440 = 0x0002100d（端口复位后，QEMU）", fields=[
    (31, 19, "0", "lgray"), (18, 17, "Spd=01", "lyel"), (16, 13, "0", "lgray"),
    (12, 12, "Pwr", "lgreen"), (11, 9, "0", "lgray"), (8, 8, "Rst", "lorange"),
    (7, 4, "0", "lgray"), (3, 3, "EnChg", "lpur"), (2, 2, "Ena", "lgreen"),
    (1, 1, "CDet", "lpur"), (0, 0, "Conn", "lgreen")],
    legend=[("b18:17", "PrtSpd", "00 高速 / 01 全速 / 10 低速"),
            ("b12", "PrtPwr", "根口 VBUS 供电（软件置 1）"),
            ("b8", "PrtRst", "软件置 1 保持 50 ms 再清 0"),
            ("b3,1", "EnChng/ConnDet", "写 1 清除（HPRT_W1C，改写时要屏蔽）"),
            ("b2", "PrtEna", "复位结束后硬件置 1"),
            ("b0", "PrtConnSts", "有设备连接")])

SETUP = dict(title="SETUP 包（8 字节，ch0 以 PID=SETUP 发出）：GET_DESCRIPTOR(Device, 8)", bytes=True,
             nbits=64, fields=[(0, 0, "bmReqType=80", "lblue"), (1, 1, "bRequest=06", "lyel"),
                               (2, 3, "wValue=0100", "lgreen"), (4, 5, "wIndex=0000", "lpur"),
                               (6, 7, "wLength=0008", "lorange")],
             legend=[("byte0", "bmRequestType", "b7 方向(1=IN) b6:5 类型 b4:0 接收者"),
                     ("byte1", "bRequest", "06 GET_DESCRIPTOR / 05 SET_ADDRESS / 09 SET_CONFIG"),
                     ("byte2-3", "wValue", "描述符类型<<8 | 索引；SET_ADDRESS 的新地址"),
                     ("byte4-5", "wIndex", "接口号（HID 请求）或 Hub 端口号"),
                     ("byte6-7", "wLength", "数据阶段长度；0 则直接进入状态阶段")])

HCSPLT = dict(title="HCSPLT6 @0x5c4 = 0x8000c081（键盘在 Hub 口 1 后，全速 → split）", fields=[
    (31, 31, "Spl", "lorange"), (30, 17, "0", "lgray"), (16, 16, "Cmp", "lpur"),
    (15, 14, "Pos=3", "lyel"), (13, 7, "HubAddr=1", "lblue"), (6, 0, "PrtAddr=1", "lgreen")],
    legend=[("b31", "SpltEna", "经高速 Hub 的 TT 做 split 事务"),
            ("b16", "CompSplt", "0 = SSPLIT，1 = CSPLIT"),
            ("b15:14", "XactPos", "3 = ALL（整包）"),
            ("b13:7", "HubAddr", "TT 所在 Hub 的地址 = 1"),
            ("b6:0", "PrtAddr", "Hub 下行口 = 1")])

HCCHAR = dict(title="HCCHAR6 @0x5c0 = 0x808c8808 写入（CHENA=1 启动；回读 0x008c8808）", fields=[
    (31, 31, "En", "lred"), (30, 30, "Dis", "lgray"), (29, 29, "Odd", "lgray"),
    (28, 22, "DevAddr=2", "lblue"), (21, 20, "MC=0", "lgray"), (19, 18, "Type=3", "lyel"),
    (17, 17, "LS", "lgray"), (16, 16, "-", "lgray"), (15, 15, "IN", "lgreen"),
    (14, 11, "EP=1", "lpur"), (10, 0, "MPS=8", "lorange")],
    legend=[("b31", "ChEna", "写 1 让通道开始；完成后硬件清 0"),
            ("b28:22", "DevAddr", "SET_ADDRESS 分配的地址 2"),
            ("b19:18", "EPType", "0 控制 / 2 批量 / 3 中断"),
            ("b17", "LSpdDev", "低速设备置 1（QEMU 键盘是全速）"),
            ("b15", "EPDir", "1 = IN"),
            ("b14:11", "EPNum", "中断 IN 端点 1"),
            ("b10:0", "MPS", "wMaxPacketSize = 8")])

HCTSIZ = dict(title="HCTSIZ6 @0x5d0 = 0x00080008（DATA0）/ 0x40080008（DATA1）", fields=[
    (31, 31, "DoPng", "lgray"), (30, 29, "PID", "lyel"), (28, 19, "PktCnt=1", "lgreen"),
    (18, 0, "XferSize=8", "lblue")],
    legend=[("b30:29", "PID", "00 DATA0 / 10 DATA1 / 11 SETUP"),
            ("b28:19", "PktCnt", "ceil(8 / MPS 8) = 1 个包"),
            ("b18:0", "XferSize", "完成后剩余字节；8 - 剩余 = 实收"),
            ("", "toggle", "每收奇数个包 interrupt_in_toggle ^= 1")])

HCINT = dict(title="HCINT6 @0x5c8：数据 = 0x03，空闲轮询 = 0x12（NAK），真机 split 还会见到 ACK/NYET", fields=[
    (31, 11, "0", "lgray"), (10, 10, "DTE", "lred"), (9, 9, "FOv", "lred"), (8, 8, "Bbl", "lred"),
    (7, 7, "XErr", "lred"), (6, 6, "NYET", "lpur"), (5, 5, "ACK", "lpur"), (4, 4, "NAK", "lyel"),
    (3, 3, "STALL", "lred"), (2, 2, "AHB", "lred"), (1, 1, "Hlt", "lgreen"), (0, 0, "Cpl", "lgreen")],
    legend=[("b0", "XferCompl", "收到数据，DMA 已写入 report[]"),
            ("b1", "ChHltd", "通道已停（每次结束都会置）"),
            ("b4", "NAK", "键盘无变化 → 按 bInterval 重试"),
            ("b5", "ACK", "真机：SSPLIT 被 TT 接受 → 发 CSPLIT"),
            ("b6", "NYET", "真机：TT 还没拿到结果 → 再 CSPLIT"),
            ("b2,3,7-10", "错误", "AHB/STALL/XactErr/Babble/FrmOvrun/DTErr")])

REPORT = dict(title="HID boot 键盘报告（8 字节，DMA 写到 kbd->report[]）：按下 Shift+H", bytes=True, nbits=64,
              fields=[(0, 0, "mod=02", "lorange"), (1, 1, "rsv=00", "lgray"),
                      (2, 2, "key1=0b", "lgreen"), (3, 3, "key2=00", "lblue"),
                      (4, 4, "key3", "lblue"), (5, 5, "key4", "lblue"),
                      (6, 6, "key5", "lblue"), (7, 7, "key6", "lblue")],
              legend=[("byte0", "modifier", "b0 LCtrl b1 LShift b2 LAlt b3 LGUI b4-7 右侧"),
                      ("byte1", "reserved", "保留"),
                      ("byte2-7", "keycode[6]", "同时按下的最多 6 个 HID usage"),
                      ("0x04..", "usage", "0x04='a' ... 0x0b='h' 0x28=Enter 0x2c=空格"),
                      ("0x01..03", "ErrorRollOver", "按键过多：保持上次状态")])


def comm_steps():
    cards = [
        dict(n=0, color=C["gray"], title="模块加载：usbhid-kbd（order 2）先于 dwc2（order 4）",
             ctx="进程上下文 · main() → do_initcalls → module_init_ord 顺序", lock="usb_bus 驱动链表",
             desc=["usbkbd 先把 usb_driver 挂到 usb 总线并建好 usbkbd_wq；dwc2 后注册 platform 设备/驱动，",
                   "platform 总线一匹配就调 dwc2_probe() 枚举。这样已经插着的键盘在第一次扫描时就能绑定驱动。"],
             calls=[(0, "usbkbd_driver_init()", "kernel/drivers/input/usbkbd.c"),
                    (1, "init_workqueue(&usbkbd_wq, 1)", "单 worker：报告按顺序处理"),
                    (1, "usb_register_driver(&usbkbd_driver)", "id_table: class 3/1/1"),
                    (0, "dwc2_driver_init()", "MMIO 0x3f980000，IRQ 9"),
                    (1, "platform_device_register + platform_driver_register", ""),
                    (2, "dwc2_probe(): 建 dwc2_periodic_wq / dwc2_hub_wq，init_work ×4", "interrupt_rx[i].work / retry_work"),
                    (2, "dwc2_init()", "→ 步骤 1")],
             logs=logs("^kbd0:", ("^kbd0:", 1, "usbhid-kbd \\(order"),
                       ("workqueue: dwc2", 2))),
        dict(n=1, color=C["blue"], title="主控初始化：供电、软复位、强制 Host、开中断、根口复位",
             ctx="进程上下文 · dwc2_probe → dwc2_init（kernel/drivers/usb/host/dwc2.c）",
             desc=["经 VideoCore 邮箱 SET_POWER_STATE 给 USB HCD 上电；读 GSNPSID 确认是 Synopsys OTG（0x4f54xxxx）。",
                   "GRSTCTL.CSFTRST 软复位后再置 GUSBCFG.FORCEHOST（先置会被复位冲掉）；只开 GINTMSK.HCHINT 一个",
                   "全局中断源，GAHBCFG 打开 DMA 和全局中断。HPRT0 先供电，等 CONNSTS，再复位 50 ms，看到 PrtEna。"],
             calls=[(0, "usb_firmware_power_on()", "mbox 通道 8，tag 0x28001，id 3"),
                    (0, "wr(GRSTCTL, CSFTRST)", "等 AHBIDLE"),
                    (0, "wr(GUSBCFG, FORCEHOST)", "GINTSTS.CURMOD=1"),
                    (0, "wr(GINTMSK, HCHINT); wr(GAHBCFG, DMA_EN|GLBL_INTR_EN)", "只要通道中断"),
                    (0, "wr(HPRT0, PWR) … wr(HPRT0, PWR|RST) … 清 RST", "PrtEna=1，速度 = 全速")],
             fmt=[HPRT0],
             logs=logs("^kbd1:", ("^kbd1:", 1, "GSNPSID"), ("^kbd1:", 1, "core soft"), "^dwc2: host hprt")),
        dict(n=2, color=C["blue"], title="枚举根设备 = Hub（真机 LAN9514，QEMU 自动插入 usb-hub）",
             ctx="进程上下文 · dwc2_init → control() → channel_xfer(ch0) 忙等 HCINT",
             lock="无（启动时单线程；运行期 hub 事件在 dwc2_hub_wq）",
             desc=["控制传输都走通道 0：SETUP(PID=SETUP) → DATA(DATA1) → STATUS(反方向 0 字节 DATA1)，CPU 轮询 HCINT。",
                   "先只读 8 字节设备描述符拿 bMaxPacketSize0，再读完整 18 字节，SET_ADDRESS 0→1；读配置描述符找",
                   "状态变化中断端点（ep1，以后由通道 7 监视热插拔），SET_CONFIGURATION，读 Hub 描述符，各口 PORT_POWER。"],
             calls=[(0, "get_device_descriptor(0, ctrl_buf)", "8 字节 → mps，再 18 字节"),
                    (1, "control(addr, type, req, value, index, data, len)", ""),
                    (2, "channel_xfer(0, …, PID_SETUP) / (PID_DATA1) / 状态阶段", "HCDMA = 0xc0000000|phys"),
                    (0, "control(0, 0x00, SET_ADDRESS, 1)", "usb_ep0_mps[1] = 8"),
                    (0, "control(1, 0x80, GET_DESCRIPTOR, CONFIG<<8)", "找 bEndpointAddress&0x80 且中断型"),
                    (0, "control(1, 0x00, SET_CONFIGURATION, cfg)", ""),
                    (0, "control(1, 0x23, HUB_SET_FEATURE, PORT_POWER, port)", "×8 端口")],
             fmt=[SETUP],
             logs=logs(("^kbd-: ctrl ch0 addr=0 IN", 2), "SET_ADDRESS v=1", "^kbd2: root", "^kbd2: hub")),
        dict(n=3, color=C["purple"], title="Hub 口复位 → split 路由 → 键盘地址 2 → 配置描述符 → SET_CONFIGURATION",
             ctx="进程上下文 · dwc2_init 扫描 Hub 端口（HUB_GET_STATUS / SET_FEATURE(PORT_RESET)）",
             desc=["口 1 有连接（status=0x101）→ PORT_RESET → 状态 0x103（Enable，非高速 = 全速）。全/低速设备在高速 Hub 后面，",
                   "DWC2 必须用 split 事务：usb_route[addr] = {hub=1, port=1}，之后每次访问都写 HCSPLT。地址 0 枚举后",
                   "SET_ADDRESS 2；遍历配置描述符：接口 0 = 3/1/1（boot 键盘），后面的端点描述符给出 ep1 IN、mps 8、bInterval 10。"],
             calls=[(0, "control(1, 0xa3, HUB_GET_STATUS, 0, port)", "wPortStatus / wPortChange"),
                    (0, "control(1, 0x23, HUB_SET_FEATURE, PORT_RESET, port)", "轮询到 Enable"),
                    (0, "usb_route[0] = {hub 1, port 1, low_speed 0}", "→ HCSPLT"),
                    (0, "get_device_descriptor(0) ; SET_ADDRESS 2", "627:1 QEMU USB Keyboard"),
                    (0, "遍历 CONFIG：dtype 4 接口 / dtype 5 端点", "填 usb_hid_child[0]"),
                    (0, "control(2, 0x00, SET_CONFIGURATION, 1)", "设备进入 Configured")],
             fmt=[HCSPLT],
             logs=logs("^kbd3: hub port", ("^kbd-: ctrl ch0 addr=0 split", 1), "^kbd3: config", "^kbd3: SET_CONF")),
        dict(n=4, color=C["gold"], title="注册 usb_device → usb 总线 match → probe",
             ctx="进程上下文 · device_register → bus_probe_device → usb_match / usb_probe（drivers/usb/core/usb.c）",
             lock="device/driver 链表锁",
             desc=["每个 boot-HID 接口发布一个 usb_device（usbH0..3），所以无线接收器的键盘/鼠标接口能各自绑驱动。",
                   "usb_match 只比较 id_table 里 match_flags 指定的字段：CLASS|SUBCLASS|PROTOCOL = 3/1/1，忽略 VID/PID。",
                   "没有内建驱动匹配时（如鼠标 3/1/2），总线用 modalias \"usb:c03s01p02\" 去 request_module 加载 usbmouse.ko。"],
             calls=[(0, "usb_device_register(&usb_hid_child[0])", "dev.bus = &usb_bus"),
                    (1, "device_register → 遍历 usb_bus 的驱动", ""),
                    (2, "usb_match(dev, drv)", "c03 s01 p01 == id_table[0]"),
                    (2, "usb_probe(dev) → usbkbd_probe(udev)", "→ 步骤 5")],
             logs=logs("^kbd4:", ("^kbd4:", 1, "^kbd4: usb_device"), ("^kbd4:", 1, "^kbd4: usb_match"))),
        dict(n=5, color=C["orange"], title="usbkbd_probe：boot 协议、input 设备、提交中断 URB → 通道 6",
             ctx="进程上下文 · usbkbd_probe → usb_submit_urb → dwc2_submit_urb → dwc2_interrupt_schedule_locked",
             lock="urb->lock · usb_lock（dwc2 调度表）",
             desc=["SET_PROTOCOL(0=boot) 固定 8 字节报告格式；SET_IDLE(0) 让键盘只在状态变化时回报。注册 input 设备 event0。",
                   "usb_fill_int_urb 把 8 字节 report[]、完成回调 usbkbd_irq_complete、bInterval 10 填进 URB；提交后 URB 归 HCD：",
                   "dwc2 把它放进 interrupt_rx[] 的空槽（最多 4 个中断端点轮流用一个通道 6），通道空闲就立即写通道寄存器启动。"],
             calls=[(0, "control(2, 0x21, HID_SET_PROTOCOL, 0, intf)", ""),
                    (0, "control(2, 0x21, HID_SET_IDLE, 0, intf)", ""),
                    (0, "input_register_device(kbd->input)", "/dev/input/event0"),
                    (0, "usb_fill_int_urb(urb, udev, 1, report, 8, usbkbd_irq_complete, kbd, 10)", ""),
                    (0, "usb_submit_urb(urb)", "state IDLE → SUBMITTED"),
                    (1, "dwc2_submit_urb(urb)", "interrupt_rx[0].pending = 1"),
                    (2, "dwc2_interrupt_schedule_locked()", "round-robin 选一个 pending"),
                    (3, "dwc2_interrupt_rx_start(req)", "HCSPLT/HCDMA/HCTSIZ/HAINTMSK/HCCHAR")],
             fmt=[HCCHAR],
             logs=logs("^kbd5: usbkbd_probe", "HID_SET_PROTOCOL", "HID_SET_IDLE", "^input: USB HID",
                       "^kbd5: usb_fill", "^kbd5: dwc2_submit", "^kbd6: ch6 arm", "^usbkbd: IRQ-driven")),
        dict(n=6, color=C["teal"], title="通道 6 轮询中断 IN 端点：没按键 = NAK，每个 bInterval 重来一次",
             ctx="硬件（DWC2 发 IN 令牌，DMA）· 软件只在每次结束时介入：dwc2_periodic_wq 的 delayed work",
             lock="usb_lock",
             desc=["DWC2 在这里没有周期调度表：一次只发一个事务。真机：SSPLIT（ACK）→ 等 2 个微帧 → CSPLIT（NYET 再试 / 数据 / NAK）；",
                   "ACK/NYET 在 dwc2_irq 的快路径里当场重发 CSPLIT（微帧时限 125 µs，不能等 kworker）。QEMU 直接给出最终结果。",
                   "NAK：释放通道 6，mod_delayed_work(retry_work, bInterval=10 ms → 1 jiffy)，到点再 pending → 重新 arm（DATA 不翻转）。"],
             calls=[(0, "dwc2_irq → dwc2_interrupt_rx_irq_fast()", "仅 split ACK / 同帧 NYET"),
                    (0, "dwc2_interrupt_rx_complete(): HCINT & NAK", "→ release ch6"),
                    (1, "mod_delayed_work(&dwc2_periodic_wq, &req->retry_work, 1)", "10 ms"),
                    (0, "dwc2_interrupt_retry_work()", "req->pending = 1"),
                    (1, "dwc2_interrupt_schedule_locked() → dwc2_interrupt_rx_start()", "SSPLIT 再来")],
             fmt=[HCTSIZ],
             loghdr="日志（QEMU raspi3b，make USBKBD_TRACE=3，没按键时每 10 ms 重复这一轮；原样摘录）",
             logs=(open(LOG3).read().splitlines()[:4] if os.path.exists(LOG3) else [])),
        dict(n=7, color=C["red"], title="按键：XferCompl → IRQ 9 → dwc2_irq（上半部）",
             ctx="中断上下文 · el1 IRQ 向量 → devintr() → gic_iar()=9（IRQ_PENDING_1 b9）→ dwc2_irq()",
             lock="无（只读寄存器、屏蔽 HAINTMSK，不拿睡眠锁）",
             desc=["键盘有变化时把 8 字节报告 DMA 到 report[]（HCDMA 用 0xc0000000 总线别名），HCINT6 = XferCompl|ChHltd，",
                   "HAINT b6 → GINTSTS.HCHINT → BCM2837 legacy IRQ 9。dwc2_irq 不处理数据：屏蔽 HAINTMSK b6（防止重入风暴），",
                   "dwc2_irq_pending |= 1<<6，schedule_work(&interrupt_rx[0].work) 交给 system_wq 的 kworker。"],
             calls=[(0, "devintr(): irq == DWC2_IRQ", "trap.c"),
                    (1, "dwc2_irq()", ""),
                    (2, "GINTSTS & GINTMSK & HCHINT ; channels = HAINT & HAINTMSK", "= 0x40"),
                    (2, "dwc2_interrupt_rx_irq_fast()", "非 split 中间态 → 0"),
                    (2, "wr(HAINTMSK, ~channels) ; dwc2_irq_pending |= channels", ""),
                    (2, "schedule_work(&ch6_req->work)", "system_wq")],
             fmt=[HCINT],
             logs=logs(("^kbd7:", 1, "=== typing"))),
        dict(n=8, color=C["purple"], title="dwc2 下半部：读结果、翻转 DATA toggle、归还 URB",
             ctx="kworker（system_wq）· dwc2_interrupt_urb_work → dwc2_interrupt_rx_complete → usb_hcd_giveback_urb",
             lock="usb_lock → urb->lock（回调前释放）",
             desc=["在锁内读 HCINT6（=3），HCTSIZ 剩余 0 → 实收 8 字节；dc ivac 使 report[] 的 cache 行失效后 CPU 才看得到 DMA 数据。",
                   "收到奇数个包 → interrupt_in_toggle ^= 1（下次 DATA1/DATA0 交替）。释放通道 6 给别的端点，然后",
                   "usb_hcd_giveback_urb：URB 状态 SUBMITTED → IDLE（归还给类驱动），调用 usbkbd_irq_complete，它只 queue_work。"],
             calls=[(0, "dwc2_interrupt_urb_work(work)", ""),
                    (1, "dwc2_interrupt_rx_complete(req)", "XferCompl，8 字节"),
                    (2, "cache_invalidate_range(report, 8)", "dc ivac"),
                    (2, "dwc2_interrupt_release_locked + schedule_locked", "通道 6 空出"),
                    (1, "usb_hcd_giveback_urb(urb, 0, 8)", "urb->state = URB_IDLE"),
                    (2, "urb->complete(urb) = usbkbd_irq_complete", ""),
                    (3, "queue_work(&usbkbd_wq, &kbd->rx_work)", "")],
             logs=logs(("^kbd8:", 3, "=== typing"), ("^dwc2: ch6 transfer", 1))),
        dict(n=9, color=C["green"], title="usbkbd_rx_work：报告差分 → consoleintr() / input 事件 → 重新提交",
             ctx="kworker（usbkbd_wq）· usbkbd_rx_work → consoleintr()（tty） + input_report_key/input_sync（evdev）",
             lock="kbd->lock · cons.lock · dev->lock（input）",
             desc=["与上一份报告比较：新出现的 usage = 按下，消失的 = 松开；modifier 字节逐位比较。按下的字母键按 Shift/CapsLock/",
                   "Ctrl 查表成字符直接 consoleintr(c)（和 Mini UART 同一个入口：回显、行编辑、^C、唤醒 consoleread）；",
                   "同时报 EV_KEY 给 evdev（/dev/input/event0），input_sync 发 EV_SYN。最后 usb_submit_urb 重新挂上，回到步骤 5/6。"],
             calls=[(0, "usbkbd_rx_work(work)", ""),
                    (1, "for 新 usage: usbkbd_key(kbd, mod, code)", "shiftmap['H']"),
                    (2, "consoleintr('H')", "cons.buf / echo / wakeup(&cons.r)"),
                    (2, "input_report_key(input, KEY_H, 1)", "evdev_pass_event"),
                    (1, "for 消失的 usage: input_report_key(…, 0)", ""),
                    (1, "input_sync(input)", "EV_SYN 帧结束"),
                    (1, "usb_submit_urb(kbd->irq_urb)", "→ dwc2_submit_urb")],
             fmt=[REPORT],
             logs=logs(("^kbd9: usbkbd_rx_work.*\\[2 0 b", 1), ("^kbd9: usage 0xb down -> 'H'", 1),
                       ("^kbd9: input_sync", 1, "'H'"), ("^kbd9: resubmit", 1, "'H'"),
                       ("^kbd9: usage 0x28 down", 1), ("^Hi$", 1))),
    ]
    s = Svg(W)
    s.text(W / 2, 42, "xv6 Raspberry Pi 3：USB 键盘从上电到一个字符进入终端（DWC2，步骤 0–9）", 24, "#111",
           "middle", "bold")
    s.text(W / 2, 70, "DWC2 OTG（host 模式） · Hub(addr 1) 口 1 · 全速 boot 键盘 627:1 (addr 2, ep1 IN, 8 B, bInterval 10) · "
           "kernel/drivers/usb/host/dwc2.c, usb/core/usb.c, input/usbkbd.c", 13, "#555", "middle")
    s.text(W / 2, 90, "日志开关：make USBKBD_TRACE=1|2|3（行首 kbdN: 即步骤 N；kbd-: 为任意步骤里的控制传输）",
           12.5, C["teal"], "middle")
    y = relation_map(s, 110)
    for c in cards:
        y = draw_card(s, y, c)
    s.save(os.path.join(OUT, "usbkbd-comm-steps.svg"), y + 10)


def relation_map(s, y0):
    """Steps 0-9 and how they hand over to each other, by execution context."""
    H = 470
    s.rect(X0, y0, CW, H, "#fff", "#c9ced6", rx=10, sw=1.2)
    s.text(X0 + 20, y0 + 28, "步骤 0–9 的相互关系：谁触发谁、在哪个上下文里跑、哪里形成循环", 17, "#111", weight="bold")
    s.text(X0 + 20, y0 + 48, "实线 = 正常顺序；绿色环 = 每次按键都走一遍的接收循环；灰色 = 没按键时的轮询；"
           "紫色虚线 = 热插拔重新枚举。框里的数字 = 下面卡片的步骤号", 12, "#555")
    rows = [("进程上下文\n启动时一次", y0 + 100, "#f3f5f8"), ("硬件\nDWC2·Hub TT·键盘", y0 + 205, "#eef7f6"),
            ("中断上下文\nIRQ 9", y0 + 300, "#fdf0f0"), ("kworker\n进程上下文", y0 + 395, "#eef3fb")]
    for lab, cy, bg in rows:
        s.rect(X0 + 10, cy - 40, CW - 20, 80, bg, bg, rx=6)
        for i, part in enumerate(lab.split("\n")):
            s.text(X0 + 24, cy - 4 + 16 * i, part, 12 if i == 0 else 10.5, "#333" if i == 0 else "#777",
                   weight="bold" if i == 0 else "normal")
    nodes = {}

    def node(k, n, title, sub, cx, cy, col, w=178):
        s.rect(cx - w / 2, cy - 27, w, 54, "#fff", col, rx=8, sw=1.8)
        if n is not None:
            s.o(f'<circle cx="{cx - w / 2 + 18}" cy="{cy}" r="12" fill="{col}"/>')
            s.text(cx - w / 2 + 18, cy + 5, str(n), 13, "#fff", "middle", "bold")
            s.text(cx - w / 2 + 36, cy - 4, title, 12.5, "#111", weight="bold")
            s.text(cx - w / 2 + 36, cy + 14, sub, 10.5, "#555")
        else:
            s.text(cx, cy - 4, title, 12.5, "#111", "middle", "bold")
            s.text(cx, cy + 14, sub, 10.5, "#555", "middle")
        nodes[k] = (cx, cy, w)

    ya, yb, yc, yd = (r[1] for r in rows)
    xs = [235, 430, 625, 820, 1015, 1210]
    titles = [("模块加载", "usbkbd 先于 dwc2", C["gray"]), ("主控初始化", "复位·host·HPRT0", C["blue"]),
              ("枚举 Hub", "addr 1 · 口供电", C["blue"]), ("枚举键盘", "split · addr · 配置", C["purple"]),
              ("总线匹配", "usb_match→probe", C["gold"]), ("usbkbd_probe", "SET_PROTOCOL·submit", C["orange"])]
    for i, (a, b, c) in enumerate(titles):
        node(i, i, a, b, xs[i], ya, c)
    for i in range(5):
        s.line(xs[i] + 89, ya, xs[i + 1] - 91, ya, "#444", 1.6)
    node(6, 6, "通道 6 轮询", "SSPLIT/CSPLIT · NAK/数据", 820, yb, C["teal"], 200)
    node(7, 7, "dwc2_irq", "屏蔽 b6 · schedule_work", 820, yc, C["red"], 200)
    node(8, 8, "dwc2 下半部", "system_wq · giveback", 820, yd, C["purple"], 200)
    node(9, 9, "usbkbd_rx_work", "usbkbd_wq · 报告差分", 1080, yd, C["green"], 200)
    node("tty", None, "consoleintr / evdev", "sh 读到字符 · event0", 1265, yc, C["green"], 170)
    node("ch7", None, "Hub 状态端点 ch7", "插拔 → 位图非 0", 430, yb, C["purple"], 190)
    node("hubwq", None, "dwc2_hub_wq", "GET_STATUS · reprobe", 430, yd, C["purple"], 190)
    # 5 -> 6
    s.path(f"M{1210},{ya + 27} C1210,{ya + 70} 920,{yb - 60} 860,{yb - 29}", C["orange"], 1.8)
    s.text(1080, ya + 66, "usb_submit_urb → interrupt_rx[0] → arm ch6", 11.5, C["orange"])
    # 6 self (NAK)
    s.path(f"M{920},{yb - 12} C1010,{yb - 40} 1010,{yb + 30} 922,{yb + 12}", C["gray"], 1.6)
    s.text(1000, yb - 22, "NAK：dwc2_periodic_wq 10 ms 后再 arm", 11.5, C["gray"])
    # 6 -> 7
    s.line(820, yb + 27, 820, yc - 29, C["red"], 1.8)
    s.text(828, (yb + yc) / 2 + 4, "HCINT6 XferCompl → IRQ 9", 11.5, C["red"])
    # 7 -> 6 fast path
    s.path(f"M{720},{yc - 10} C650,{yc - 30} 650,{yb + 20} 718,{yb + 8}", C["red"], 1.4, dash=True)
    s.text(500, (yb + yc) / 2 + 4, "SSPLIT ACK / 同帧 NYET：", 11, C["red"])
    s.text(500, (yb + yc) / 2 + 19, "IRQ 内当场续发 CSPLIT", 11, C["red"])
    # 7 -> 8, 8 -> 9
    s.line(820, yc + 27, 820, yd - 29, C["purple"], 1.8)
    s.text(828, (yc + yd) / 2 + 4, "schedule_work(system_wq)", 11.5, C["purple"])
    s.line(920, yd, 978, yd, C["green"], 1.8)
    s.text(949, yd - 14, "queue_work", 10.5, C["green"], "middle")
    # 9 -> tty, 9 -> 6 (resubmit)
    s.path(f"M{1180},{yd - 10} C1230,{yd - 30} 1265,{yd - 40} 1265,{yc + 29}", C["green"], 1.8)
    s.text(1195, yd - 36, "字符/按键事件", 11, C["green"])
    s.path(f"M{1080},{yd - 27} C1080,{yb + 40} 1000,{yb + 30} 922,{yb + 22}", C["green"], 2.2)
    s.text(1092, yc - 6, "usb_submit_urb", 11.5, C["green"])
    s.text(1092, yc + 9, "重新提交（回到 6）", 11.5, C["green"])
    # hotplug: ch7 -> hubwq -> 1
    s.path(f"M{430},{yb + 27} L430,{yd - 29}", C["purple"], 1.4, dash=True)
    s.text(438, (yb + yd) / 2, "IRQ 9 → hub work", 11, C["purple"])
    s.path(f"M{335},{yd} C200,{yd} 200,{ya + 60} {430},{ya + 29}", C["purple"], 1.4, dash=True)
    s.text(130, yc + 10, "连接位图变了：", 11, C["purple"])
    s.text(130, yc + 25, "拆树 → dwc2_init()", 11, C["purple"])
    s.text(130, yc + 40, "（回到步骤 1）", 11, C["purple"])
    s.path(f"M{1015},{ya + 27} C1015,{ya + 55} 430,{yb - 70} 430,{yb - 29}", C["purple"], 1.2, dash=True)
    s.text(600, yb - 48, "步骤 5 之后：dwc2_start_hub_monitor() 在 ch7 上开始监视", 11, C["purple"])
    return y0 + H + 18


# ------------------------------------------------------------- overview
def overview():
    s = Svg(1400)
    s.text(700, 38, "USB 键盘数据路径总览（rpi3 / DWC2）", 22, "#111", "middle", "bold")

    # --- top left: topology
    s.text(40, 80, "① 拓扑与通道分工", 15, C["blue"], weight="bold")
    boxes = [
        (40, 100, 320, 64, "BCM2837 DWC2 OTG（host）", "MMIO 0x3f980000 · IRQ 9 · 8 个 host 通道", "lblue"),
        (40, 210, 320, 64, "高速 Hub  addr 1", "真机 LAN9514（含以太网）· QEMU 自动插入 usb-hub", "lpur"),
        (40, 320, 320, 64, "USB 键盘  addr 2（Hub 口 1）", "全/低速 · 接口 0 = 3/1/1 · ep1 IN 8 B · 10 ms", "lgreen"),
    ]
    for x, y, w, h, t, sub, col in boxes:
        s.rect(x, y, w, h, C[col], "#555", rx=8)
        s.text(x + w / 2, y + 26, t, 14, "#111", "middle", "bold")
        s.text(x + w / 2, y + 47, sub, 11.5, "#444", "middle")
    s.line(200, 164, 200, 208, "#333", 2)
    s.line(200, 274, 200, 318, "#333", 2)
    s.text(212, 191, "根口 HPRT0（UTMI，高速）", 11.5, "#333")
    s.text(212, 301, "TT：split 事务（HCSPLT hub=1 port=1）", 11.5, "#333")
    chans = [("ch0", "控制传输（枚举、Hub 请求、HID 请求）· CPU 轮询 HCINT", C["blue"]),
             ("ch6", "HID 中断 IN（键盘/鼠标，最多 4 个端点轮转）· IRQ", C["red"]),
             ("ch7", "Hub 状态变化中断 IN（热插拔）· IRQ", C["purple"]),
             ("ch1-5", "批量（usbnet / MT7601U）", C["gray"])]
    for i, (c, t, col) in enumerate(chans):
        y = 420 + i * 26
        s.rect(40, y - 15, 56, 21, col, col, rx=4)
        s.text(68, y, c, 12, "#fff", "middle", "bold")
        s.text(106, y, t, 12, "#333")

    # --- top right: ch6 state machine
    s.text(470, 80, "② 通道 6 的一个中断端点（interrupt_rx[i]）状态机", 15, C["blue"], weight="bold")
    st = {"idle": (560, 140, "空闲（槽里无 URB）"), "pend": (800, 140, "pending"),
          "ss": (800, 260, "inflight：SSPLIT"), "cs": (1080, 260, "inflight：CSPLIT（仅真机）"),
          "wait": (560, 380, "等 bInterval（retry_work）"), "done": (1080, 380, "XferCompl → work → giveback")}
    for k, (x, y, t) in st.items():
        w = 210
        s.rect(x - w / 2, y - 22, w, 44, "#fff", "#555", rx=21, sw=1.3)
        s.text(x, y + 5, t, 12.5, "#222", "middle")

    def lab(x, y, t, col):
        s.rect(x - 4, y - 13, 7.2 * len(t.encode("gbk")) / 1.15 + 8, 18, "#fafbfc", "#fafbfc", rx=2)
        s.text(x, y, t, 11.5, col)
    s.line(665, 140, 693, 140, C["teal"], 1.6); lab(600, 120, "dwc2_submit_urb()", C["teal"])
    s.line(800, 162, 800, 236, C["teal"], 1.6); lab(810, 204, "schedule：ch6 空闲 → rx_start", C["teal"])
    s.line(905, 260, 973, 260, C["purple"], 1.6); lab(905, 250, "HCINT ACK", C["purple"])
    s.path("M1120,238 C1160,190 1060,190 1090,236", C["purple"], 1.4)
    lab(1150, 205, "NYET 且同一帧、<3 次：再 CSPLIT", C["purple"])
    s.path("M740,282 L600,358", C["gray"], 1.6); lab(560, 320, "NAK（无按键）", C["gray"])
    s.path("M1000,282 C900,330 760,370 667,378", C["gray"], 1.4)
    lab(810, 338, "NYET 超出窗口 / NAK", C["gray"])
    s.line(1080, 282, 1080, 356, C["green"], 1.6); lab(1090, 324, "XferCompl（有数据）", C["green"])
    s.path("M600,358 C640,300 700,220 745,163", C["gray"], 1.4); lab(560, 250, "10 ms 后 pending", C["gray"])
    s.path("M1185,380 C1270,330 1260,160 910,140", C["green"], 1.4)
    lab(1200, 300, "usbkbd 重新 submit", C["green"])
    s.text(470, 430, "QEMU 不模拟 TT 的 ACK/NYET：SSPLIT 直接拿到最终结果（数据 HCINT=0x03，或 NAK 0x12）。"
           "真机才走 ACK → CSPLIT → NYET 的快路径，", 11.5, "#555")
    s.text(470, 448, "它在 dwc2_irq 里当场完成（dwc2_interrupt_rx_irq_fast），因为 CSPLIT 必须在 SSPLIT 之后 2 个"
           "微帧（250 µs）内发出，等 kworker 就错过了。", 11.5, "#555")

    # --- bottom: contexts swimlane
    top = 530
    s.text(40, top, "③ 一次按键（Shift+H）跨越的执行上下文", 15, C["blue"], weight="bold")
    lanes = [("硬件", "DWC2 通道 6 + DMA", C["teal"]), ("IRQ 9（中断上下文）", "dwc2_irq：上半部", C["red"]),
             ("system_wq kworker", "dwc2_interrupt_urb_work", C["purple"]),
             ("usbkbd_wq kworker", "usbkbd_rx_work", C["green"]),
             ("用户进程 sh", "read(0) → consoleread", C["orange"])]
    lx = [140, 420, 700, 980, 1260]
    for (n, sub, col), x in zip(lanes, lx):
        s.rect(x - 100, top + 15, 200, 46, "#fff", col, rx=6, sw=1.6)
        s.text(x, top + 35, n, 13, col, "middle", "bold")
        s.text(x, top + 53, sub, 11, "#555", "middle")
        s.line(x, top + 61, x, top + 470, col, 1, dash=True, arrow=False)
    msgs = [(0, 0, "SSPLIT IN → Hub TT → 键盘；8 B DMA 到 report[]", "6"),
            (0, 1, "HCINT6=XferCompl → HAINT b6 → GINTSTS.HCHINT", "7"),
            (1, 2, "屏蔽 HAINTMSK b6；schedule_work", "7"),
            (2, 2, "HCTSIZ 剩 0 → 8 B；dc ivac；toggle ^= 1", "8"),
            (2, 3, "giveback → usbkbd_irq_complete → queue_work", "8"),
            (3, 3, "与上次报告比较：0x0b 新按下，mod=LShift", "9"),
            (3, 4, "consoleintr('H')：cons.buf，回显", "9"),
            (3, 3, "input_report_key(KEY_H) + input_sync → event0", "9"),
            (3, 0, "usb_submit_urb → 通道 6 重新 arm（回到步骤 5/6）", "5"),
            (4, 4, "Enter 后 wakeup(&cons.r)，read() 返回整行", "9")]
    y = top + 95
    for a, b, t, n in msgs:
        col = lanes[a][2]
        if a == b and a == len(lx) - 1:
            s.path(f"M{lx[a]-3},{y-9} c-40,0 -40,18 -3,18", col, 1.6)
            s.text(lx[a] - 48, y + 3, t, 11.5, "#222", "end")
        elif a == b:
            s.path(f"M{lx[a]+3},{y-9} c40,0 40,18 3,18", col, 1.6)
            s.rect(lx[a] + 46, y - 12, 7.0 * len(t.encode("gbk")) / 1.1 + 6, 18, "#fafbfc", "#fafbfc", rx=2)
            s.text(lx[a] + 48, y + 3, t, 11.5, "#222")
        else:
            s.line(lx[a], y, lx[b] + (-4 if b > a else 4), y, col, 1.8)
            s.text((lx[a] + lx[b]) / 2, y - 6, t, 11.5, "#222", "middle")
        s.o(f'<circle cx="{lx[a] - 16}" cy="{y}" r="9" fill="{col}"/>')
        s.text(lx[a] - 16, y + 4, n, 10.5, "#fff", "middle", "bold")
        y += 38
    s.text(40, top + 495, "圆圈数字 = usbkbd-comm-steps.svg 里的步骤号；IRQ 9 里不做数据处理，"
           "两个 kworker 分开：HCD 的 work 归还 URB，类驱动的 work 解析报告。", 12, "#555")
    s.save(os.path.join(OUT, "usbkbd-overview.svg"), top + 515)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    comm_steps()
    overview()
