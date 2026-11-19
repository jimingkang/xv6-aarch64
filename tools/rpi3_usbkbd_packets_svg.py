#!/usr/bin/env python3
"""Packet-by-packet USB keyboard diagram for the Raspberry Pi 3B+ (real board).

  python3 tools/rpi3_usbkbd_packets_svg.py
  rsvg-convert -z 1.5 doc/readme/rpi3/images/usbkbd-packets-3bplus.svg \
      -o doc/readme/rpi3/images/usbkbd-packets-3bplus.png

Every byte below was printed by the board (make USBKBD_TRACE=2, which dumps
the 8 SETUP bytes and the returned data of each control transfer):
  doc/readme/rpi3/logs/pi3bplus-usbkbd-bytes-trace2.log   enumeration
  doc/readme/rpi3/logs/pi3bplus-usbkbd-boot-trace2.log    boot, full width
The interrupt reports (keys l, s, Enter) come from the user's minicom log of
the same kernel, quoted in doc/readme/rpi3/usb-keyboard.md.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "doc/readme/rpi3/images/usbkbd-packets-3bplus.svg")

FONT = "'PingFang SC','Hiragino Sans GB','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"
C = dict(cpu="#7b3fb3", hs="#1f5fae", hub="#b07d00", ls="#00796b", red="#c62828",
         green="#2e7d32", gray="#666", orange="#c25a00")

W = 1640
LANES = [("CPU · dwc2.c", "控制传输 control() / 中断 work", C["cpu"]),
         ("DWC2 主机控制器", "ch0 控制 · ch6 中断 · 高速 480 Mb/s", C["hs"]),
         ("Hub 424:2514 addr 1", "LAN7515 内 · 口 3 的 TT（事务转换器）", C["hub"]),
         ("键盘 2a7a:8a47", "低速 1.5 Mb/s · addr 0 → 4", C["ls"])]
LX = [110, 330, 560, 790]
EX = 905            # explanation column x
EW = W - EX - 24


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


out = []


def o(s):
    out.append(s)


def text(x, y, s, size=12.5, fill="#222", anchor="start", weight="normal", mono=False):
    fam = f' font-family="{MONO}"' if mono else ""
    o(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
      f'font-weight="{weight}"{fam}>{esc(s)}</text>')


def rect(x, y, w, h, fill, stroke="none", rx=4, sw=1, op=1):
    o(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}" '
      f'fill-opacity="{op}" stroke="{stroke}" stroke-width="{sw}"/>')


def line(x1, y1, x2, y2, col, sw=1.6, dash=False, arrow=True):
    d = ' stroke-dasharray="6 4"' if dash else ""
    m = ' marker-end="url(#ar)"' if arrow else ""
    o(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{col}" '
      f'stroke-width="{sw}"{d}{m}/>')


# ---------------------------------------------------------------- content
# A row: (src, dst, label, colour, packet-bytes or None, [explanation lines], log or None)
# src == dst -> a note on that lane.  dst None -> spans nothing (section text).
SECTIONS = []


def sec(title, sub, rows):
    SECTIONS.append((title, sub, rows))


HS, HUB, LS, CPU = C["hs"], C["hub"], C["ls"], C["cpu"]

sec("A  Hub 口 3 发现键盘：高速控制传输直接发给 Hub（地址 1，不需要 split）",
    "每个控制传输 = SETUP 事务（SETUP 令牌 + DATA0 8 字节 + ACK）→ 可选 DATA 事务（DATA1…）→ STATUS 事务（反方向 0 字节 DATA1）",
    [
     (1, 2, "SETUP · DATA0 → ACK", HS, "a3 00 00 00 03 00 04 00",
      ["HUB GET_STATUS(port 3)：bmRequestType a3 = 设备→主机 | 类请求 | 接收者=其他(端口)",
       "bRequest 00 GET_STATUS · wValue 0 · wIndex 3（口号）· wLength 4"], None),
     (2, 1, "IN → DATA1 4 B · ACK", HS, "01 03 01 00",
      ["wPortStatus = 0x0301：b0 Connection · b8 Power · b9 LowSpeed（D- 上拉 = 低速）",
       "wPortChange = 0x0001：b0 C_PORT_CONNECTION（刚插上）"], "dwc2: hub port 3 status=301 change=1"),
     (1, 2, "SETUP · DATA0 → ACK；STATUS IN 0 B", HS, "23 03 04 00 03 00 00 00",
      ["SET_FEATURE(PORT_RESET=4, port 3)：bmRequestType 23 = 主机→设备 | 类 | 端口",
       "Hub 在口 3 上驱动 SE0 复位 ≥10 ms，之后使能该口"], None),
     (2, 3, "SE0 复位 → 使能（低速信号）", LS, None,
      ["复位后键盘处于地址 0、默认配置，只回应 EP0"], None),
     (1, 2, "GET_STATUS(port 3)", HS, "a3 00 00 00 03 00 04 00 → 03 03 11 00",
      ["wPortStatus 0x0303：+ b1 Enable；wPortChange 0x0011：C_CONNECTION | C_RESET"],
      "dwc2: hub port 3 after-reset status=303 speed=low"),
     (1, 2, "CLEAR_FEATURE ×2", HS, "23 01 10 00 03 00 00 00 / 23 01 14 00 03 00 00 00",
      ["清 C_PORT_CONNECTION(0x10) 与 C_PORT_RESET(0x14)，让 Hub 的状态变化端点安静下来",
       "随后 dwc2 记 usb_route[0] = {hub 1, port 3, low_speed 1}：之后访问地址 0/4 全部走 split"], None),
    ])

sec("B  一次 split 控制传输逐包展开：GET_DESCRIPTOR(Device, 8) 发给地址 0",
    "主机和 Hub 之间是高速（SSPLIT/CSPLIT 令牌包住原来的低速事务）；Hub 的 TT 自己在低速口上和键盘完成真正的事务。每个阶段两次通道 0 事务。",
    [
     (1, 2, "SSPLIT · SETUP(0,0) · DATA0 8 B", HS, "SPLIT{hub 1, SC 0, port 3, S 1, E 0, ET 00} SETUP{addr 0, ep 0} 80 06 00 01 00 00 08 00",
      ["SPLIT 令牌（4 字节）：PID 0x78 · HubAddr 7 位=1 · SC=0 开始 · Port 7 位=3 · S=1 低速 · ET=00 控制 · CRC5",
       "通道寄存器：HCSPLT0=8000c083  HCCHAR0=00020040（addr 0 · EP0 · 控制 · LSpdDev · MPS 64）",
       "HCTSIZ0=60080008：PID=SETUP · 1 包 · 8 字节。setup 包 = GET_DESCRIPTOR(类型 1 设备, 索引 0, 8 字节)"],
      None),
     (2, 1, "ACK（TT 收下）", HUB, None,
      ["HCINT0 = 0x22 = ACK | ChHltd：TT 已缓存这次事务，主机要稍后用 CSPLIT 取结果",
       "channel_xfer() 置 complete_split=1，udelay(125) 等一个微帧"],
      "dwc2: ch0 SSPLIT ACK addr=0 in=0 hcint=22 hcsplt=8000c083 hcchar=20040"),
     (2, 3, "SETUP · DATA0 8 B → ACK（低速）", LS, "80 06 00 01 00 00 08 00",
      ["TT 在低速口上重放 SETUP 事务，键盘 ACK"], None),
     (1, 2, "CSPLIT · SETUP → ACK", HS, None,
      ["CSPLIT（SC=1）取回键盘的握手：ACK → HCINT0 XferCompl，SETUP 阶段结束"], None),
     (1, 2, "SSPLIT · IN(0,0)", HS, "HCCHAR0=00028040（+EPDir IN） HCTSIZ0=40080008（DATA1，8 字节）",
      ["数据阶段：IN 令牌也要先 SSPLIT，TT 回 ACK（HCINT0=0x22）"],
      "dwc2: ch0 SSPLIT ACK addr=0 in=1 hcint=22 hcsplt=8000c083 hcchar=28040"),
     (2, 3, "IN → DATA1 8 B → ACK（低速）", LS, "12 01 10 01 00 00 00 08",
      ["键盘回设备描述符前 8 字节，TT 代替主机回 ACK"], None),
     (2, 1, "CSPLIT · IN → DATA1 8 B", HUB, "12 01 10 01 00 00 00 08",
      ["HCINT0 = 0x21 = XferCompl | ACK，HCTSIZ 剩 0 → 收到 8 字节",
       "bLength 0x12=18 · bDescriptorType 1 · bcdUSB 0x0110（USB 1.1）· class 0 · sub 0 · proto 0",
       "bMaxPacketSize0 = 8 → usb_ep0_mps[0] = 8；之后 18 字节的请求按 8 字节一包拆开（8+8+2）"],
      "dwc2: ch0 CSPLIT complete addr=0 bytes=8/8 hcint=21 hctsiz=0"),
     (1, 2, "SSPLIT · OUT · DATA1 0 B", HS, "HCTSIZ0=40080000（DATA1，0 字节）",
      ["状态阶段方向与数据相反：主机发 0 字节 DATA1 表示“收到”，TT ACK（0x22）"],
      "dwc2: ch0 SSPLIT ACK addr=0 in=0 hcint=22 … hctsiz=40080000"),
     (2, 3, "OUT · DATA1 0 B → ACK", LS, None, ["键盘确认，控制传输完成"], None),
     (1, 2, "CSPLIT · OUT → ACK", HS, None,
      ["dwc2 的日志只给 IN 数据阶段打 “CSPLIT complete”，SETUP/STATUS 的 CSPLIT 不打印"], None),
     (0, 0, "注：这一次用的 MPS 是 64", C["red"], None,
      ["第一次请求 HCCHAR 的 MPS=0x40：usb_ep0_mps[0] 还留着 Hub 枚举时的 64。8 字节请求只有一个包，所以无害；",
       "读到 bMaxPacketSize0=8 后立即改成 8（后面的 HCCHAR 都是 …008）"], None),
    ])

sec("C  枚举键盘：每个控制传输的 SETUP 包和键盘返回的全部字节（split，同 B 的包序）",
    "左边是 SETUP 包 8 字节（bmRequestType bRequest wValue wIndex wLength，小端），右边是数据阶段收到的字节和逐字段解码",
    [
     (1, 3, "GET_DESCRIPTOR(Device, 18)", CPU, "80 06 00 01 00 00 12 00",
      ["数据阶段按 MPS 8 拆成 3 个 split IN：8 + 8 + 2 字节（DATA1, DATA0, DATA1 交替）"], None),
     (3, 1, "18 B 设备描述符", LS, "12 01 10 01 00 00 00 08 | 7a 2a 47 8a 01 00 00 01 | 00 01",
      ["idVendor 0x2a7a · idProduct 0x8a47 · bcdDevice 0x0001 · iManufacturer 0 · iProduct 1 · iSerial 0",
       "bNumConfigurations 1；设备类 0 = 类别写在接口描述符里"],
      "dwc2: hub port=3 child class=0 vid=2a7a pid=8a47 mps=8"),
     (1, 3, "SET_ADDRESS(4)", CPU, "00 05 04 00 00 00 00 00",
      ["无数据阶段；状态阶段是 IN 0 字节。之后 usb_route[4] = usb_route[0]，usb_ep0_mps[4] = 8",
       "地址 = 端口号 + 1（dwc2_init 的约定）"], "kbd3: hub port 3: … child 2a7a:8a47 addr 0 -> 4"),
     (1, 3, "GET_DESCRIPTOR(Config, 9)", CPU, "80 06 00 02 00 00 09 00 → 09 02 3b 00 02 01 00 a0 32",
      ["wTotalLength 0x003b = 59 · bNumInterfaces 2 · bConfigurationValue 1 · iConfiguration 0",
       "bmAttributes 0xa0 = 总线供电 + 远程唤醒 · bMaxPower 0x32 × 2 mA = 100 mA"], None),
     (1, 3, "GET_DESCRIPTOR(Config, 59)", CPU, "80 06 00 02 00 00 3b 00",
      ["数据阶段 8 个 split IN：7 × 8 + 3 字节"], None),
     (3, 1, "接口 0（boot 键盘）", LS, "09 04 00 00 01 03 01 01 00",
      ["bInterfaceNumber 0 · alt 0 · 1 个端点 · class 03 HID · subclass 01 Boot · protocol 01 Keyboard"], None),
     (3, 1, "HID 描述符", LS, "09 21 10 01 00 01 22 40 00",
      ["bcdHID 1.10 · 国家码 0 · 1 个下级描述符：类型 0x22 报告描述符，长度 0x40 = 64 字节（boot 协议不读它）"], None),
     (3, 1, "端点 0x81", LS, "07 05 81 03 08 00 0a",
      ["bEndpointAddress 0x81 = EP1 IN · bmAttributes 03 中断 · wMaxPacketSize 8 · bInterval 10 ms",
       "→ usb_hid_child[0]：interrupt_in_ep 1 · mps 8 · interval 10"], None),
     (3, 1, "接口 1（非 boot）", LS, "09 04 01 00 01 03 00 00 00 | 09 21 10 01 00 01 22 80 00 | 07 05 82 03 04 00 0a",
      ["class 03 / subclass 00 / protocol 00：不是 boot 接口（多媒体键之类，报告描述符 128 字节），EP 0x82 IN mps 4",
       "dwc2 只为 3/1/1、3/1/2 发布 usb_device，所以这一接口被忽略；配置描述符共读了两遍（扫描时一次、配置 HID 时一次）"],
      "kbd3: config 1 (59 bytes): interface 0 class 3/1/1 (boot keyboard), interrupt-IN ep1 mps=8 bInterval=10"),
     (1, 3, "SET_CONFIGURATION(1)", CPU, "00 09 01 00 00 00 00 00",
      ["设备进入 Configured，EP1 开始可用"], "kbd3: SET_CONFIGURATION 1 on addr 4 -> configured"),
     (1, 3, "HID SET_PROTOCOL(Boot)", C["orange"], "21 0b 00 00 00 00 00 00",
      ["bmRequestType 21 = 主机→设备 | 类 | 接口 · bRequest 0x0b · wValue 0 = Boot 协议 · wIndex 0 = 接口 0",
       "usbkbd_probe 发出：从此报告固定为 8 字节 boot 格式"], None),
     (1, 3, "HID SET_IDLE(0)", C["orange"], "21 0a 00 00 00 00 00 00",
      ["wValue 高字节 = 空闲时长 0（无限）：键盘只在按键状态变化时才回数据，否则一律 NAK"],
      "input: USB HID Boot Keyboard registered as event0"),
    ])

sec("D  中断 IN（通道 6）：按下 l、松开、s、Enter",
    "通道 6 每 10 ms 发一次 split 中断 IN；没变化时 TT 从键盘拿到 NAK。SSPLIT 的 ACK 和同帧 NYET 在 dwc2_irq 快路径里处理（CSPLIT 必须在 2 个微帧内发出）",
    [
     (0, 1, "usb_submit_urb → ch6 arm", CPU, "HCSPLT6=8000c083 HCCHAR6=810e8808 HCTSIZ6=00080008 HCDMA6=c03de080",
      ["HCCHAR6：ChEna · DevAddr 4 · EPType 3 中断 · LSpdDev · IN · EP1 · MPS 8；HCTSIZ6：DATA0 · 1 包 · 8 字节",
       "HCDMA6 = 0xc0000000 | V2P(kbd->report)"],
      "kbd6: ch6 arm SSPLIT addr=4 ep1 IN DATA0 mps=8 HCDMA=c03de080 HCTSIZ=80008 HCCHAR=810e8808"),
     (1, 2, "SSPLIT{ET 11 中断} · IN(4, 1)", HS, None,
      ["TT 回 ACK → HCINT6 0x22 → IRQ 9 → dwc2_irq → dwc2_interrupt_rx_irq_fast()：等到 SSPLIT 后第 2 个微帧，",
       "置 HCSPLT6.CompSplt 重新使能通道发 CSPLIT（这一步不打印，否则串口几毫秒就错过微帧窗口）"], None),
     (2, 3, "IN(4, 1) → 没按键：NAK", LS, None,
      ["SET_IDLE(0) 后，状态没变键盘就回 NAK"], None),
     (1, 2, "CSPLIT · IN → NAK / NYET", HUB, None,
      ["NAK（HCINT6 0x12）：释放通道 6，dwc2_periodic_wq 10 ms 后再 arm；NYET：TT 还没结果，同一帧内最多再 CSPLIT 3 次",
       "启动日志里的 “dwc2 ch6 final … int=0” 不是这里的事件（是别的通道的中断被旧调试行标成了 ch6）"], None),
     (2, 3, "IN(4, 1) → DATA0 8 B → ACK", LS, "00 00 0f 00 00 00 00 00",
      ["按下 l：byte0 修饰键 0 · byte1 保留 · byte2 = HID usage 0x0f（'l'）· byte3..7 = 0（只按了一个键）"], None),
     (2, 1, "CSPLIT · IN → DATA0 8 B", HUB, "00 00 0f 00 00 00 00 00",
      ["HCINT6 = 0x23 = XferCompl | ChHltd | ACK（ACK 是 SSPLIT 阶段留下的）· HCTSIZ 剩 0 → 8 字节已 DMA 到 report[]",
       "IRQ 9：dwc2_irq 屏蔽 HAINTMSK b6 → schedule_work"],
      "kbd7: IRQ 9 cpu0 dwc2_irq: GINTSTS.HCHINT HAINT=40 HCINT6=23 -> mask HAINTMSK, schedule_work"),
     (1, 0, "work：dc ivac · toggle → DATA1 · giveback", CPU, None,
      ["dwc2_interrupt_rx_complete：1 个包 → interrupt_in_toggle ^= 1；usb_hcd_giveback_urb → usbkbd_irq_complete → queue_work"],
      "kbd8: dwc2 work: HCINT=23 XFERCOMPL, HCTSIZ left=0 -> 8 bytes DMA'd ... giveback"),
     (0, 0, "usbkbd_rx_work：0x0f 新按下 → consoleintr('l') + KEY_L", C["green"], None,
      ["然后 usb_submit_urb：下一次 ch6 用 DATA1（HCTSIZ6=40080008）"],
      "kbd9: usage 0xf down -> 'l' -> consoleintr()"),
     (3, 1, "松开 l：DATA1 8 B", LS, "00 00 00 00 00 00 00 00",
      ["全 0 = 没有键按下 → usage 0x0f 消失 → EV_KEY KEY_L 值 0；toggle → DATA0"],
      "kbd9: usage 0xf up -> EV_KEY 38 value 0"),
     (3, 1, "按下 s：DATA0 / 松开：DATA1", LS, "00 00 16 00 00 00 00 00 / 00 00 00 00 00 00 00 00",
      ["usage 0x16 = 's' → consoleintr('s')"], "kbd9: usage 0x16 down -> 's' -> consoleintr()"),
     (3, 1, "按下 Enter：DATA0", LS, "00 00 28 00 00 00 00 00",
      ["usage 0x28 = Enter → consoleintr('\\n') → wakeup(&cons.r)：sh 的 read 返回 \"ls\\n\""],
      "kbd9: usage 0x28 down -> 0xa -> consoleintr()   exec: /bin/ls"),
    ])


# ---------------------------------------------------------------- layout
def row_h(r):
    n = len(r[5]) + (1 if r[4] else 0) + (1 if r[6] else 0)
    return max(46, 18 * n + 16)


H = 150
for title, sub, rows in SECTIONS:
    H += 64 + sum(row_h(r) for r in rows) + 18
H += 40

o(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
  f'font-family="{FONT}" font-size="13">')
o('<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
  'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="context-stroke"/></marker></defs>')
rect(0, 0, W, H, "#fafbfc", rx=0)
text(W / 2, 36, "树莓派 3B+ 真机：USB 键盘的每一个数据包（DWC2 → Hub TT → 低速键盘）", 23, "#111", "middle", "bold")
text(W / 2, 60, "字节全部来自板子串口（make USBKBD_TRACE=2，控制传输打印 SETUP 8 字节和返回数据）；"
     "实线 = 高速总线（主机↔Hub），虚线 = 低速总线（Hub TT↔键盘）", 13, "#555", "middle")
y = 82
for (n, sub, col), x in zip(LANES, LX):
    rect(x - 100, y, 200, 48, "#fff", col, rx=7, sw=1.8)
    text(x, y + 21, n, 14, col, "middle", "bold")
    text(x, y + 39, sub, 10.5, "#555", "middle")
rect(EX, y, EW, 48, "#fff", "#999", rx=7)
text(EX + 12, y + 21, "包内容（十六进制）与逐字段解释", 14, "#333", weight="bold")
text(EX + 12, y + 39, "灰色等宽行 = 对应的串口日志", 10.5, "#777")
y += 68
lane_top = y

for title, sub, rows in SECTIONS:
    rect(16, y, W - 32, 50, "#eef2f7", "#c8d1dc", rx=6)
    text(28, y + 21, title, 15.5, "#123", weight="bold")
    text(28, y + 41, sub, 11.5, "#456")
    y += 64
    for i, r in enumerate(rows):
        a, b, lab, col, pkt, expl, logl = r
        h = row_h(r)
        if i % 2 == 0:
            rect(16, y - 4, W - 32, h, "#ffffff", op=0.8, rx=0)
        for x in LX:
            line(x, y - 4, x, y - 4 + h, "#d0d4da", 1, dash=True, arrow=False)
        ay = y + 20
        if a == b:
            rect(LX[a] - 95, ay - 14, 190 + (LX[3] - LX[a] if a == 0 else 0), 22, "#fff", col, rx=11, sw=1.4)
            text(LX[a] - 85, ay + 2, lab, 12, col, weight="bold")
        else:
            ls = {a, b} == {2, 3}
            line(LX[a], ay, LX[b] + (-6 if b > a else 6), ay, col, 2.2, dash=ls)
            text((LX[a] + LX[b]) / 2, ay - 6, lab, 11.8, col, "middle", "bold")
        yy = y + 14
        if pkt:
            text(EX, yy, pkt, 12.3, "#0b3d91", mono=True, weight="bold")
            yy += 18
        for e in expl:
            text(EX, yy, e, 11.8, "#222")
            yy += 18
        if logl:
            text(EX, yy, logl, 11, "#777", mono=True)
        y += h
    y += 18

text(W / 2, y + 16, "SPLIT 令牌：PID 0x78 | HubAddr(7) | SC(1: 0 开始 / 1 完成) | Port(7) | S(1: 低速) | E(1) | ET(2: 00 控制 / 10 批量 / 11 中断) | CRC5。"
     "DWC2 用 HCSPLT 的 SpltEna/CompSplt/HubAddr/PrtAddr 生成它。", 11.5, "#555", "middle")
o("</svg>")
with open(OUT, "w") as f:
    f.write("\n".join(out))
print("wrote", OUT, H)
