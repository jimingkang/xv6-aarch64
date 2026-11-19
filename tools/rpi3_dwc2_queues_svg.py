#!/usr/bin/env python3
"""DWC2 "queue" picture for the Raspberry Pi 3B+ -- the counterpart of the
tiny210 ehci-qh-queues.svg.  DWC2 has no QH/qTD lists and no frame list: it has
8 host channels (register sets) that software arms one transaction at a time,
plus the driver's own request table and workqueues.

  python3 tools/rpi3_dwc2_queues_svg.py
  rsvg-convert -z 1.5 doc/readme/rpi3/images/dwc2-channel-queues.svg \
      -o doc/readme/rpi3/images/dwc2-channel-queues.png

Values are from the 3B+ logs in doc/readme/rpi3/logs/.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "doc/readme/rpi3/images/dwc2-channel-queues.svg")
FONT = "'PingFang SC','Hiragino Sans GB','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"
INK, MUTED = "#24292f", "#57606a"
GREEN, GREEN_BG = "#1a7f37", "#dafbe1"
BLUE, BLUE_BG = "#0969da", "#ddf4ff"
YEL, YEL_BG = "#bf8700", "#fff8c5"
RED, RED_BG = "#cf222e", "#ffebe9"
PUR, PUR_BG = "#8250df", "#fbefff"
GRAY_BG = "#eaeef2"

o = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def t(x, y, s, size=12.5, fill=INK, anchor="start", weight="normal", mono=False):
    fam = f' font-family="{MONO}"' if mono else ""
    o.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
             f'font-weight="{weight}"{fam}>{esc(s)}</text>')


def r(x, y, w, h, fill="#fff", stroke=MUTED, rx=6, sw=1.4, dash=False):
    d = ' stroke-dasharray="5 3"' if dash else ""
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" '
             f'stroke-width="{sw}"{d}/>')


def ln(x1, y1, x2, y2, col=INK, sw=1.6, dash=False, arrow=True):
    d = ' stroke-dasharray="5 4"' if dash else ""
    m = f' marker-end="url(#a{col[1:]})"' if arrow else ""
    o.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{col}" stroke-width="{sw}"{d}{m}/>')


def pth(d, col=INK, sw=1.6, dash=False, arrow=True):
    da = ' stroke-dasharray="5 4"' if dash else ""
    m = f' marker-end="url(#a{col[1:]})"' if arrow else ""
    o.append(f'<path d="{d}" fill="none" stroke="{col}" stroke-width="{sw}"{da}{m}/>')


def node(x, y, w, h, title, lines, stroke, fill, tsize=13.5):
    r(x, y, w, h, fill, stroke)
    t(x + 10, y + 20, title, tsize, INK, weight="bold")
    for i, l in enumerate(lines):
        t(x + 10, y + 38 + 16 * i, l, 11, MUTED)


W, H = 1400, 1560
o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="{FONT}" font-size="13">')
o.append("<defs>" + "".join(
    f'<marker id="a{c[1:]}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
    f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>'
    for c in [INK, GREEN, BLUE, YEL, RED, PUR, MUTED]) + "</defs>")
r(0, 0, W, H, "#fff", "#fff", 0)
t(W / 2, 34, "DWC2 通道“队列”收发图：3B+ 上的低速键盘 2a7a:8a47（Hub 1 口 3），对照 tiny210 的 EHCI QH 图", 20, INK, "middle")
t(W / 2, 56, "DWC2 没有 QH/qTD 链表和帧表：硬件只有 8 个 host channel（寄存器组），一次 arm 一个事务；"
  "“队列”是驱动自己的请求表 + workqueue。绿 = 控制（ch0）· 蓝 = 中断 IN（ch6）· 紫 = Hub 状态（ch7）",
  12, MUTED, "middle")

# ---- global registers
r(40, 72, 1320, 66, "#fff", MUTED, 8, 2)
t(56, 98, "DWC2 全局寄存器", 13.5, INK, weight="bold")
regs = [("GAHBCFG", "DMA_EN | GLBL_INTR_EN"), ("GINTMSK", "只开 HCHINT（b25）"),
        ("GINTSTS", "HCHINT → IRQ 9（legacy 中断控制器）"), ("HAINT/HAINTMSK", "每通道 1 位：b0 ch0 … b6 ch6 … b7 ch7"),
        ("HFNUM", "帧号 + 微帧（split 定时用）"), ("HPRT0", "根口：0x100f 高速、已使能")]
for i, (n, v) in enumerate(regs):
    x = 200 + (i % 3) * 390
    y = 98 + (i // 3) * 24
    t(x, y, n, 12, INK, weight="bold", mono=True)
    t(x + 128, y, v, 11.5, MUTED)

# ---- left: control on ch0
L = (40, 156, 650, 640)
r(*L, "#f6fff8", GREEN, 10, 2)
t(L[0] + 325, 182, "控制传输：通道 0，CPU 忙等（没有中断、没有队列）", 15, GREEN, "middle")
t(L[0] + 325, 202, "control() → channel_xfer(0, …) ×3：SETUP / DATA / STATUS；全在 usb_lock 外、进程上下文里同步完成", 11.5, MUTED, "middle")
node(60, 220, 270, 128, "通道 0 寄存器（一次一个事务）",
     ["HCSPLT0 = 8000c083  split→Hub1 口3", "HCCHAR0 = 0102 8008  addr4 EP0 控制 LS IN", "HCTSIZ0 = 4008 0008  DATA1 1 包 8 B",
      "HCDMA0  = 0xc0000000 | V2P(ctrl_buf)", "HCINT0  ← CPU 轮询，不进 IRQ"], GREEN, "#fff")
# stage chain
sx, sy = 350, 222
stages = [("SETUP", "8 B · PID SETUP", YEL_BG), ("DATA", "≤ MPS 一包 · DATA1/0", YEL_BG), ("STATUS", "0 B · DATA1", YEL_BG)]
for i, (a, b, c) in enumerate(stages):
    x = sx + i * 108
    r(x, sy, 98, 46, c, YEL)
    t(x + 49, sy + 19, a, 12.5, INK, "middle", "bold")
    t(x + 49, sy + 36, b, 9.5, MUTED, "middle")
    if i < 2:
        ln(x + 98, sy + 23, x + 106, sy + 23, YEL)
t(sx, sy + 66, "每一阶段在 split 路由下都是一对事务：", 11.5, INK)
# split loop diagram
y0 = 310
boxes = [("arm SSPLIT", "HCSPLT.CompSplt=0", 360), ("HCINT ACK|Hlt", "= 0x22，TT 收下", 470),
         ("udelay(125)", "等 1 个微帧", 580)]
for a, b, x in boxes:
    r(x, y0, 100, 42, "#fff", GREEN)
    t(x + 50, y0 + 17, a, 11.5, INK, "middle", "bold")
    t(x + 50, y0 + 33, b, 9.5, MUTED, "middle")
ln(460, y0 + 21, 468, y0 + 21, GREEN)
ln(570, y0 + 21, 578, y0 + 21, GREEN)
r(470, y0 + 64, 210, 42, "#fff", GREEN)
t(575, y0 + 81, "arm CSPLIT（CompSplt=1）", 11.5, INK, "middle", "bold")
t(575, y0 + 97, "取 TT 的结果", 9.5, MUTED, "middle")
pth(f"M630,{y0 + 42} L630,{y0 + 62}", GREEN)
outs = [("NYET", "TT 还没做完 → 125 µs 后再 CSPLIT", 362, y0 + 130, PUR),
        ("NAK", "设备忙 → 整个事务从 SSPLIT 重来", 362, y0 + 178, MUTED),
        ("XferCompl", "HCTSIZ 剩余 → 实收字节；IN 时 dc ivac", 362, y0 + 226, GREEN)]
for a, b, x, y, c in outs:
    r(x, y, 318, 38, "#fff", c)
    t(x + 10, y + 16, a, 12, c, weight="bold")
    t(x + 10, y + 31, b, 10.5, MUTED)
pth(f"M560,{y0 + 106} L560,{y0 + 128}", PUR)
pth(f"M362,{y0 + 149} C330,{y0 + 140} 330,{y0 + 84} 468,{y0 + 84}", PUR, dash=True)
pth(f"M362,{y0 + 197} C300,{y0 + 180} 300,{y0 + 30} 358,{y0 + 21}", MUTED, dash=True)
t(60, 380, "split 拆包：MPS=8 时 18 B 描述符 =", 11.5, INK)
t(60, 397, "3 个独立的 split IN（8+8+2），", 11.5, INK)
t(60, 414, "每包 DATA0/DATA1 交替。", 11.5, INK)
t(60, 438, "高速设备（Hub 本身）不 split：", 11.5, INK)
t(60, 455, "HCSPLT0 = 0，一次 IN 就是一个事务。", 11.5, INK)
t(60, 590, "谁在用 ch0", 12.5, GREEN, weight="bold")
users = ["枚举：GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION",
         "Hub：GET_STATUS / SET_FEATURE(PORT_POWER/RESET) / CLEAR_FEATURE",
         "HID：SET_PROTOCOL(Boot) / SET_IDLE(0)（usbkbd_probe）",
         "热插拔 worker（dwc2_hub_wq）里的端口查询",
         "互斥：所有调用都在 dwc2_init 或单 worker 的 dwc2_hub_wq 里，",
         "从不并发，所以 ch0 不需要请求队列"]
for i, u in enumerate(users):
    t(72, 612 + 18 * i, "· " + u, 11.5, MUTED)
r(60, 730, 610, 50, GREEN_BG, GREEN)
t(72, 750, "对照 EHCI：控制传输是挂在 async 环上的 EP0 QH + SETUP/DATA/STATUS 三个 qTD，HC 自己执行；", 11.5, INK)
t(72, 768, "DWC2 上每个 qTD 相当于一次 channel_xfer()，由 CPU 依次写寄存器、等 HCINT。", 11.5, INK)

# ---- right: interrupt on ch6
R = (710, 156, 650, 640)
r(*R, "#f5faff", BLUE, 10, 2)
t(R[0] + 325, 182, "中断 IN：通道 6 + 请求表 interrupt_rx[4] + delayed work（软件周期调度）", 15, BLUE, "middle")
t(R[0] + 325, 202, "没有帧表：每个端点的“下一次轮询”是 dwc2_periodic_wq 里的一个 delayed_work；通道 6 一次只服务一个端点", 11.5, MUTED, "middle")
t(730, 232, "interrupt_rx[DWC2_INT_REQUESTS = 4]（受 usb_lock 保护）", 12.5, INK, weight="bold")
slots = [("[0]", "键盘 usbH0 · urb → report[8]", "pending / inflight", True),
         ("[1]", "空（若有鼠标接口：usbH1）", "", False), ("[2]", "空", "", False), ("[3]", "空", "", False)]
for i, (a, b, c, used) in enumerate(slots):
    y = 244 + i * 40
    r(730, y, 360, 34, YEL_BG if used else GRAY_BG, YEL if used else MUTED, dash=not used)
    t(742, y + 22, a, 12.5, INK, weight="bold", mono=True)
    t(780, y + 15, b, 11.5, INK)
    if c:
        t(780, y + 29, "mps 8 · " + c + " · retry_work · work", 10, MUTED)
t(730, 420, "interrupt_rx_inflight = 0（占着 ch6 的槽，-1 = 空闲）", 11, MUTED, mono=True)
t(730, 436, "interrupt_rx_rotate：round-robin 游标，公平轮转", 11, MUTED, mono=True)
node(1110, 244, 230, 150, "通道 6 寄存器",
     ["HCSPLT6 = 8000c083（+b16 CSPLIT）", "HCCHAR6 = 810e8808", "  ChEna addr4 中断 LS IN EP1 MPS8",
      "HCTSIZ6 = 00080008 / 40080008", "HCDMA6  = c03de080 → report[]", "HCINTMSK6 = 7ff · HAINTMSK b6",
      "HCINT6 → IRQ 9"], BLUE, "#fff")
ln(1090, 260, 1108, 260, BLUE)
t(1092, 252, "arm", 10, BLUE)
# poll timeline
t(730, 470, "一次轮询（真机）：每格 = 一个 125 µs 微帧", 12.5, INK, weight="bold")
cells = [("SSPLIT", "IN", GREEN_BG), ("TT 执行", "LS IN", YEL_BG), ("CSPLIT", "取结果", BLUE_BG),
         ("(CSPLIT)", "NYET 时", BLUE_BG), ("", "", "#fff"), ("", "", "#fff"), ("", "", "#fff"), ("", "", "#fff")]
for i, (a, b, c) in enumerate(cells):
    x = 730 + i * 76
    r(x, 482, 76, 44, c, MUTED, 0, 1)
    t(x + 38, 500, a, 11, INK, "middle")
    t(x + 38, 516, b, 9.5, MUTED, "middle")
    t(x + 38, 540, f"µ{i}", 10, MUTED, "middle")
lines = ["µ0 SSPLIT → TT 回 ACK（HCINT6=0x22）→ IRQ 9 → dwc2_irq 快路径 dwc2_interrupt_rx_irq_fast()：",
         "    等到 µ2（dwc2_wait_frame_delta），置 CompSplt 重新 ChEna —— 都在中断里，不经过 kworker",
         "µ2 CSPLIT → NYET：同一帧内 <3 次再 CSPLIT；NAK（0x12）：没按键；DATA0/1 + XferCompl（0x23）：有报告",
         "NAK / 超窗 NYET：release ch6 → mod_delayed_work(retry_work, 10 ms) → 下一个 pending 槽接手",
         "到点 dwc2_interrupt_retry_work()：槽重新 pending → schedule_locked() → 新的 SSPLIT"]
for i, l in enumerate(lines):
    t(730, 566 + 18 * i, l, 11.2, INK if i in (0, 2, 3) else MUTED)
r(730, 668, 610, 112, BLUE_BG, BLUE)
cmp_ = ["对照 EHCI：", "· 帧表 1024 项 → anchor QH → 中断 QH，HC 每 1 ms 自己走 ⟷ DWC2：delayed_work 每 10 ms 软件 arm 一次",
        "· QH 的 S-mask/C-mask 决定在哪个微帧发 SSPLIT/CSPLIT ⟷ DWC2：dwc2_irq 里按 HFNUM 手工等 2 个微帧",
        "· qTD 的 Active 位 + IOC 中断 ⟷ HCCHAR.ChEna + HCINT6 XferCompl",
        "· NAK 时 qTD 留着下帧硬件自动重试 ⟷ DWC2：NAK 会让通道停下，必须软件重 arm"]
for i, l in enumerate(cmp_):
    t(742, 688 + 18 * i, l, 10.6, INK, weight="bold" if i == 0 else "normal")

# ---- completion path
Y = 816
r(40, Y, 1320, 196, "#fffdf2", YEL, 10, 2)
t(56, Y + 26, "完成路径：通道 6 XferCompl → IRQ 9 → 两级 workqueue → 重新 arm（键盘、鼠标各自一条，共用通道 6）", 14, YEL)
steps = [("HCINT6 0x23", "XferCompl|Hlt|ACK", "DMA 已写 report[]", YEL_BG, YEL),
         ("dwc2_irq", "IRQ 9 · 中断上下文", "屏蔽 HAINTMSK b6", YEL_BG, YEL),
         ("system_wq", "dwc2_interrupt_urb_work", "HCTSIZ→8B · toggle", BLUE_BG, BLUE),
         ("giveback_urb", "SUBMITTED→IDLE", "→ usbkbd_irq_complete", BLUE_BG, BLUE),
         ("usbkbd_wq", "usbkbd_rx_work", "差分→consoleintr/evdev", BLUE_BG, BLUE),
         ("usb_submit_urb", "dwc2_submit_urb", "槽 pending→schedule", BLUE_BG, BLUE)]
for i, (a, b, c, f, s) in enumerate(steps):
    x = 60 + i * 216
    r(x, Y + 46, 190, 70, f, s)
    t(x + 95, Y + 68, a, 13, INK, "middle", "bold", mono=True)
    t(x + 95, Y + 86, b, 10.5, MUTED, "middle")
    t(x + 95, Y + 103, c, 10.5, MUTED, "middle")
    if i < 5:
        ln(x + 190, Y + 81, x + 214, Y + 81, YEL)
pth(f"M1215,{Y + 116} L1215,{Y + 140} L120,{Y + 140} L120,{Y + 118}", BLUE, dash=True)
t(660, Y + 156, "回到 interrupt_rx[0]：如果另一个端点（鼠标）在等，它先拿到通道 6", 11.5, BLUE, "middle")
t(60, Y + 182, "黄 = 中断上下文（只读寄存器、屏蔽、排队，不 printf 以免错过微帧）；蓝 = kworker 进程上下文。"
  "IRQ 里只有 SSPLIT ACK / 同帧 NYET 这两种“中间态”在快路径里当场续发。", 11.5, MUTED)

# ---- ch7 + topology
Y2 = 1032
r(40, Y2, 650, 210, "#fcf7ff", PUR, 10, 2)
t(56, Y2 + 26, "通道 7：Hub 状态变化端点（热插拔）", 14, PUR)
for i, l in enumerate(["Hub 424:2514 的 EP1 IN（mps 1，bInterval 12 = 2^11 微帧 ≈ 256 ms）",
                       "dwc2_start_hub_monitor()：dwc2_hub_urb 以 dwc2_root_hub 的身份提交",
                       "回 0 → 重新提交；回非 0 位图（b3 = 口 3 变化）→ dwc2_hub_wq：",
                       "  GET_STATUS 该口、清 C_* 位；连接位图变了 → 整棵树拆掉重新 dwc2_init()",
                       "日志：dwc2: hub IRQ change=8 connected=2->a; reprobe（插上键盘）",
                       "注意：只监视地址 1 的 Hub；插在嵌套 Hub（地址 2）口上的设备要重启才能发现"]):
    t(60, Y2 + 52 + 22 * i, l, 11.5, INK if i < 4 else MUTED)

r(710, Y2, 650, 210, "#fff", MUTED, 10, 2)
t(726, Y2 + 26, "硬件链路（3B+，LAN7515 = 2514 Hub + 7800 网卡）", 14, INK)
chain = [(730, Y2 + 52, 150, "DWC2 OTG", "根口 HPRT0 · 高速"),
         (920, Y2 + 52, 200, "Hub 424:2514 addr 1", "4 口 · 单 TT（alt 0；未切 MTT）"),
         (1160, Y2 + 40, 180, "嵌套 Hub addr 2", "口 1 → LAN7800 addr 8"),
         (1160, Y2 + 120, 180, "键盘 2a7a:8a47 addr 4", "口 3 · 低速 1.5 Mb/s")]
for x, y, w, a, b in chain:
    r(x, y, w, 52, "#fff", INK)
    t(x + w / 2, y + 22, a, 12, INK, "middle", "bold")
    t(x + w / 2, y + 40, b, 10.5, MUTED, "middle")
ln(880, Y2 + 78, 918, Y2 + 78, INK)
t(898, Y2 + 70, "480M", 9.5, MUTED, "middle")
ln(1120, Y2 + 70, 1158, Y2 + 66, INK)
pth(f"M1120,{Y2 + 92} C1140,{Y2 + 120} 1140,{Y2 + 146} 1158,{Y2 + 146}", "#1a7f37", dash=True)
t(1100, Y2 + 140, "1.5M", 9.5, GREEN, "end")
t(730, Y2 + 136, "高速段：所有包带 SPLIT 令牌，Hub 的 TT 负责", 11, MUTED)
t(730, Y2 + 154, "转换成低速事务（每 1 ms 帧 / 125 µs 微帧）", 11, MUTED)
t(730, Y2 + 186, "对照 tiny210：S5PV210 EHCI → USB2514 → 同一个键盘（地址 2，口 3）", 11, MUTED)

# ---- EHCI vs DWC2 table
Y3 = 1262
r(40, Y3, 1320, 280, "#fff", MUTED, 10, 2)
t(56, Y3 + 26, "EHCI（tiny210）与 DWC2（rpi3）逐项对照", 14, INK)
rows = [("调度结构", "async 环（QH horiz）+ periodic 帧表 1024 项", "8 个通道寄存器组；驱动的 interrupt_rx[4] 表"),
        ("一个事务", "qTD（token：PID/字节数/Active/IOC/CERR）", "HCTSIZ（PID/PktCnt/XferSize）+ HCCHAR.ChEna"),
        ("端点描述", "QH epchar/epcap（地址、EP、MPS、速度、Hub/口、S/C-mask）", "HCCHAR（地址、EP、MPS、LSpd、类型）+ HCSPLT（Hub/口/CompSplt）"),
        ("split 定时", "硬件按 S-mask µ0 发 SSPLIT、C-mask µ2..4 发 CSPLIT", "软件：ACK 中断里等 2 个微帧再 ChEna（dwc2_irq 快路径）"),
        ("轮询周期", "每 1 ms 帧表命中（bInterval 被忽略）", "NAK 后 delayed_work 10 ms（按 bInterval，向上取整到 jiffy）"),
        ("NAK", "qTD 保持 Active，下帧自动重试", "通道停下，软件重 arm"),
        ("完成通知", "USBSTS.USBINT（IOC）→ IRQ 55 → ehci_intr_reap 扫 intrs[]", "HAINT b6 → GINTSTS.HCHINT → IRQ 9 → schedule_work"),
        ("DMA 地址", "物理地址", "0xc0000000 | 物理地址（VideoCore 总线别名）")]
t(70, Y3 + 54, "", 12)
t(200, Y3 + 54, "EHCI", 12.5, GREEN, weight="bold")
t(790, Y3 + 54, "DWC2", 12.5, BLUE, weight="bold")
for i, (a, b, c) in enumerate(rows):
    y = Y3 + 80 + i * 24
    if i % 2 == 0:
        r(50, y - 16, 1300, 24, "#f6f8fa", "#f6f8fa", 0, 0)
    t(66, y, a, 12, INK, weight="bold")
    t(200, y, b, 11.5, INK)
    t(790, y, c, 11.5, INK)

o.append("</svg>")
with open(OUT, "w") as f:
    f.write("\n".join(o))
print("wrote", OUT)
