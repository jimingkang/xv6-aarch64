#!/usr/bin/env python3
"""SDIO Wi-Fi (Pi 3B+ CYW43455) message/step flow, same layout as
diagrams/usbkbd-comm-steps.svg: an overview of processes / contexts /
hardware with numbered messages 0-11 and the locks, then one card per
message (call chain, SDIO transfers, registers and frames bit by bit, and
the WIFI_TRACE=1 lines that the step prints).

  python3 tools/sdio_wifi_diagrams/gen_sdio_wifi_steps.py doc/readme/rpi3/images/sdio-wifi-comm-steps.svg
"""
import sys
from html import escape

W = 1400
MONO = 'DejaVu Sans Mono, Menlo, monospace'
out = []

BLUE, BLUE_S = '#ddf4ff', '#0969da'
YEL, YEL_S = '#fff8c5', '#9a6700'
GREY, GREY_S = '#f6f8fa', '#57606a'
GREEN, GREEN_S = '#dafbe1', '#1a7f37'
PURP, PURP_S = '#fbefff', '#8250df'
ORANGE, ORANGE_S = '#fff1e5', '#bc4c00'
RED = '#cf222e'
LOCKC = '#9a6700'
C = ['#ddf4ff', '#fff8c5', '#dafbe1', '#ffebe9', '#fbefff', '#eaeef2', '#fff1e5']
ZERO = '#eaeef2'


def e(s):
    return escape(str(s), quote=True)


def text(x, y, s, size=13, fill='#1f2328', bold=False, anchor='start', mono=False):
    fam = f' font-family="{MONO}"' if mono else ''
    s = str(s)
    n = len(s) - len(s.lstrip(' '))
    if n:
        s = ' ' * n + s[n:]
    out.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" '
               f'font-weight="{"bold" if bold else "normal"}" text-anchor="{anchor}"{fam}>{e(s)}</text>')


def rect(x, y, w, h, fill='#fff', stroke='#8c959f', sw=1, rx=8, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def line(points, color='#333', marker='a', dash=None, sw=2):
    d = 'M' + ' L'.join(f'{x},{y}' for x, y in points)
    dd = f' stroke-dasharray="{dash}"' if dash else ''
    m = f' marker-end="url(#{marker})"' if marker else ''
    out.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}"{dd}{m}/>')


def twidth(s, size):
    w = 0
    for ch in s:
        w += size if ord(ch) > 0x2e80 else size * 0.6
    return w


def badge(x, y, n, col=BLUE_S):
    out.append(f'<circle cx="{x}" cy="{y}" r="9" fill="{col}"/>')
    text(x, y + 4, str(n), 11 if n < 10 else 9.5, fill='#fff', bold=True, anchor='middle')


def lab(x, y, n, s, col, anchor='start'):
    """numbered arrow label"""
    if anchor == 'start':
        badge(x + 9, y - 4, n, col)
        text(x + 22, y, s, 12, fill=col)
    else:
        text(x - 22, y, s, 12, fill=col, anchor='end')
        badge(x - 9, y - 4, n, col)


# ---------------------------------------------------------------- overview
def overview():
    text(700, 30, '板载 SDIO Wi-Fi（CYW43455）加载与通信图：进程/上下文、0–11 号消息、每条路径上的锁', 20, bold=True, anchor='middle')
    text(700, 52, '蓝框 = 进程（kthread / 用户进程），黄框 = 中断上下文（不是进程），灰框 = 硬件，紫框 = 网络核心（在调用者上下文里执行），'
         '橙框 = 芯片内固件；🔒 = 自旋锁（持有期间关中断），💤 = 睡眠锁', 12.5, fill='#57606a', anchor='middle')

    # P1 /bin/wifi
    rect(40, 90, 380, 190, BLUE, BLUE_S, 2)
    text(230, 114, '进程 3：/bin/wifi（系统调用里的内核态）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['wifi_connect(ssid, psk) → brcmfmac_connect', '  DOWN/UP、event_msgs、SET_WSEC、PMK …',
                           '  SET_SSID → 轮询 GET_BSSID（≤40 次）', '  M3 到了：装 PTK/GTK，发 M4',
                           '  500 ms 排空 → net_dhcp_dev → SET_PM=0', '每条 BCDC 命令同步等回复（≤3 s）']):
        text(55, 138 + i * 22, s, 12.5, fill='#57606a' if i == 5 else '#1f2328')

    # P2 brcmf0_wq
    rect(470, 90, 430, 240, BLUE, BLUE_S, 2)
    text(685, 114, '进程 2：brcmf0_wq 工作线程（1 个 worker）', 15, bold=True, anchor='middle')
    rect(490, 128, 390, 116, '#fff', BLUE_S, 1, 6)
    text(685, 148, 'NAPI poll：brcmf_napi_poll（budget 16）', 13, bold=True, anchor='middle')
    for i, s in enumerate(['CMD52 读 CCCR INT_PENDING（假中断就退出）', 'F1 读并清 INTSTATUS → CMD53 读 F2 帧',
                           '  ch0 回复 / ch1 事件 / ch2 EAPOL 或 net_rxq', '出队 → 放锁 → net_rx_dev；读空才重开 IRQ']):
        text(500, 168 + i * 18, s, 12)
    text(490, 264, 'delayed work：state_work（连接看门狗 50 ms，10 s 截止）', 12)
    text(490, 282, '             irq_work（假中断后隔 1 tick 再开 CARD_INT）', 12)
    text(490, 306, 'SDIO 访问全在这里（可睡眠）；中断里不碰 SDIO 总线', 12, fill=RED, bold=True)
    text(490, 322, '（同一个 NAPI 串行：一次只有一个 poll，帧不乱序）', 12, fill='#57606a')

    # P1 boot
    rect(1010, 90, 350, 170, BLUE, BLUE_S, 2, dash='6 4')
    text(1185, 114, '启动上下文：main()（CPU0，还没进程）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['sdio_bus_init → do_initcalls：', '  arasan-sdio（order 1）→ 枚举', '  rootfs / FAT32 挂载',
                           '  brcmfmac（late order 2）→ brcmf_probe', '下载固件 → wlan0 → 打开 IRQ，结束']):
        text(1025, 138 + i * 22, s, 12.5, fill='#57606a' if i == 4 else '#1f2328')

    # IRQ
    rect(1010, 300, 350, 120, YEL, YEL_S, 2)
    text(1185, 324, '中断上下文：Arasan IRQ 62', 15, bold=True, anchor='middle')
    for i, s in enumerate(['arasan_sdio_irq：INTERRUPT 有 CARD_INT', '→ IRPT_EN = 0（屏蔽电平源）、写 1 清',
                           '→ pending = 1，irq 计数 +1（trace 用）', '→ brcmfmac_sdio_irq → napi_schedule']):
        text(1025, 348 + i * 18, s, 12.5)

    # net core
    rect(40, 330, 380, 230, PURP, PURP_S, 2)
    text(230, 354, '网络核心（kernel/net/net.c）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['struct net_device wlan0（brcmf_netdev_ops）',
                           '收：net_rx_dev → arp_rx / tcp_rx / UDP / DHCP',
                           '  TCP：窗口 2 KB、SYN 带 MSS、乱序缓存 ≤6 段',
                           '发：tcp_send_at_locked / net_udp_send …',
                           '  → net_output_path（路由 + ARP）',
                           '  → netdev_xmit_on → ops->start_xmit',
                           '  = brcmfmac_xmit（在调用者上下文执行：',
                           '    TCP 定时器 worker 或 write 系统调用）',
                           'DHCP：net_dhcp_dev(wlan0) 三次重试']):
        text(55, 378 + i * 19, s, 12)

    # bootfs
    rect(40, 600, 380, 120, ORANGE, ORANGE_S, 1.5)
    text(230, 624, 'bootfs（microSD FAT32，另一个控制器 SDHOST）', 14, bold=True, anchor='middle')
    for i, s in enumerate(['BCM43455.BIN  CR4 固件代码（~600 KB）', 'BCM43455.TXT  NVRAM 板级参数',
                           'BCM43455.CLM  法规/信道/功率', 'brcmf_probe 用 fat32openroot 流式读']):
        text(55, 648 + i * 18, s, 12)

    # removal
    rect(40, 750, 380, 110, '#ffebe9', RED, 1.5, dash='6 4')
    text(230, 774, '11：读空 / 假中断 / 拆除', 14, bold=True, anchor='middle', fill=RED)
    for i, s in enumerate(['读空 → napi_complete_done → 重开 CARD_INT', '假中断 → irq_work 1 tick 后再开',
                           '拆除：brcmf_remove：ready=0 → sdio_release_irq', '→ unregister_netdev → brcmf_free_bus']):
        text(55, 796 + i * 16, s, 11.5)

    # Arasan
    rect(470, 480, 430, 150, GREY, GREY_S, 2)
    text(685, 504, 'Arasan EMMC/SDHCI（硬件，0x3f300000）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['ARG1 / CMDTM 发命令，RESP0 收 R4/R5/R6 响应',
                           'DATA 口 PIO：CPU 逐个 32 位字搬（无 DMA）',
                           'BLKSIZECNT：单块 ≤512 B（实测 64 B 稳定）',
                           'INTERRUPT.CARD_INT(bit8) ← DAT1；IRPT_EN 开关',
                           'GPIO34–39 ALT3；时钟 400 kHz → 25 MHz，4-bit']):
        text(485, 528 + i * 19, s, 12)

    # SDIO interface
    rect(470, 680, 430, 120, GREY, GREY_S, 2)
    text(685, 704, 'CYW43455 SDIO 接口（芯片里的硬件）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['F0：CCCR（IO_ENABLE/READY、INT_ENABLE/PENDING）',
                           'F1：32 KB backplane 窗口（0x1000a/b/c）',
                           'F2：SDPCM 帧 FIFO；INTSTATUS & HOSTINTMASK → DAT1']):
        text(485, 728 + i * 19, s, 12)

    # firmware
    rect(1010, 640, 350, 130, ORANGE, ORANGE_S, 2)
    text(1185, 664, 'CR4 固件 + D11（芯片内，FullMAC）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['TCM 0x198000：BIN 代码 + NVRAM（尾部）', '扫描 / 认证 / 关联自己完成，发事件',
                           '每个上行帧头 byte 9 = tx_max（发送信用）', '不做 WPA2 supplicant（M1–M4 交给主机）']):
        text(1025, 688 + i * 19, s, 12)

    # AP
    rect(1010, 810, 350, 110, GREEN, GREEN_S, 2)
    text(1185, 834, 'AP（802.11，WPA2-PSK / AES）', 15, bold=True, anchor='middle')
    for i, s in enumerate(['auth / assoc、EAPOL M1–M4', 'DHCP 服务器、网关、DNS', 'PM2 时会把发给 Pi 的帧压到下个 beacon']):
        text(1025, 858 + i * 19, s, 12, fill=GREEN_S if i == 2 else '#1f2328')

    # ---------------------------------------------------------- arrows
    text(1335, 84, '0: 开机 initcall：sdio_bus → arasan-sdio → 挂 rootfs → brcmfmac（late）', 12, fill=BLUE_S, anchor='end')
    # 1/2/3/4: boot -> Arasan
    line([(1010, 245), (945, 245), (945, 520), (905, 520)], BLUE_S, marker='ab')
    lab(952, 446, 1, 'Arasan probe：GPIO、复位、时钟', BLUE_S)
    lab(952, 464, 2, '枚举：CMD0/5/3/7/52', BLUE_S)
    lab(952, 482, 3, 'CHIPID（F1 窗口）', BLUE_S)
    lab(952, 500, 4, '固件 / NVRAM 经 F1 写 TCM', BLUE_S)
    text(955, 534, '启动阶段不拿 💤 mmc-request（还没有进程）', 11.5, fill=LOCKC)
    # 3: boot -> bootfs (another controller), routed round the top and the left margin
    line([(1345, 90), (1345, 66), (20, 66), (20, 660), (36, 660)], ORANGE_S, marker='ao', dash='6 4')
    text(50, 84, '3: 启动上下文读 BIN/TXT/CLM（SDHOST 控制器，不走 SDIO）', 12, fill=ORANGE_S)
    # bus Arasan <-> SDIO
    line([(640, 630), (640, 676)], '#333')
    line([(730, 676), (730, 634)], '#333')
    text(740, 660, '2–10: CMD52 / CMD53（4-bit，25 MHz）', 12, fill='#333')
    # 4/5: SDIO <-> firmware
    line([(905, 705), (1005, 705)], ORANGE_S, marker='ao')
    text(910, 698, '4: 写 TCM', 11, fill=ORANGE_S)
    line([(1005, 750), (909, 750)], GREEN_S, marker='ag')
    text(912, 743, '5: FWREADY', 11, fill=GREEN_S)
    # 7: firmware <-> AP
    line([(1150, 770), (1150, 806)], GREEN_S, marker='ag')
    line([(1220, 806), (1220, 774)], GREEN_S, marker='ag')
    lab(1228, 794, 7, '空口 802.11', GREEN_S)
    # 6/7: /bin/wifi -> Arasan
    line([(300, 280), (300, 296), (450, 296), (450, 540), (465, 540)], BLUE_S, marker='ab')
    lab(456, 384, 6, 'BCDC 命令（ch0）', BLUE_S)
    text(478, 402, '🔒 brcmf-iovar → 🔒 brcmfmac → 💤 mmc-request', 11.5, fill=LOCKC)
    lab(456, 422, 7, 'SET_SSID / 装 key / M4', BLUE_S)
    # 8: DAT1 -> Arasan CARD_INT -> IRQ
    line([(905, 600), (1340, 600), (1340, 424)], RED, marker='ar')
    text(1332, 590, '8: DAT1 → CARD_INT → IRQ 62', 12, fill=RED, anchor='end')
    line([(1010, 340), (905, 300)], YEL_S, marker='ay')
    text(955, 276, '🔒 napi 🔒 brcmf0_wq', 11.5, fill=LOCKC)
    lab(950, 294, 8, 'napi_schedule → queue_work', YEL_S)
    # 9: worker -> Arasan (CMD53 read) and -> net
    line([(840, 330), (840, 476)], BLUE_S, marker='ab')
    text(832, 446, '9: CMD52/CMD53 读帧', 12, fill=BLUE_S, anchor='end')
    text(832, 462, '🔒 brcmfmac 💤 mmc-request', 11.5, fill=LOCKC, anchor='end')
    line([(510, 330), (510, 348), (425, 348)], PURP_S, marker='ap')
    lab(516, 352, 9, 'net_rx_dev（放锁后）', PURP_S)
    # 10: net -> Arasan (xmit)
    line([(420, 560), (465, 560)], PURP_S, marker='ap')
    lab(200, 580, 10, 'xmit：等信用 → CMD53 写 F2', PURP_S)
    # 11
    line([(880, 330), (880, 476)], RED, marker='ar', dash='5 4')
    text(886, 400, '11: 重开', 11, fill=RED)
    text(886, 415, 'CARD_INT', 11, fill=RED)

    # locks box
    y = 950
    rect(40, y, 1320, 212, GREY, GREY_S, 1)
    text(60, y + 24, '这条路径上的锁（自旋锁 = 关中断；单个 CPU 上 IRQ 62 不会撞上已持有的锁，因为顶半部只碰 Arasan 寄存器和 napi）', 15)
    rows = [
        [('🔒 bus->lock（"brcmfmac"）：tx[]/rx[] 缓冲、tx_seq/tx_max、ctl[]、net_rxq[4]、WPA 状态。brcmf_dcmd、brcmfmac_xmit、NAPI poll 互斥；', None)],
        [('   NAPI 在交给 net_rx_dev 前放掉它（协议栈可能马上回 ACK → xmit 又要拿它）。', RED)],
        [('🔒 iovar_lock（"brcmf-iovar"）：iovar_buf。   💤 host->request_lock（"mmc-request"）：一整条 CMD + 数据阶段（myproc()==0 的启动阶段不拿）。', None)],
        [('🔒 napi->lock（"napi"）：scheduled/missed 状态。   🔒 wq->lock（"brcmf0_wq"）：工作队列。   🔒 delayed_lock：state_work / irq_work。', None)],
        [('🔒 tcp conn / udp port / icmp reply（net.c）：协议栈自己的锁，在 bus->lock 之外拿。   printf 的 pr.lock：所以中断顶半部不打印 trace。', None)],
        [('锁顺序：iovar_lock → bus->lock → mmc-request（睡眠锁在自旋锁内？—— 运行时 CMD 在 bus->lock 内发，request_lock 拿不到会自旋等，见 readme 21.6）。', '#1a7f37')],
    ]
    for i, segs in enumerate(rows):
        x = 60
        for s, c in segs:
            text(x, y + 50 + i * 26, s, 12.5, fill=c or '#1f2328')
            x += twidth(s, 12.5) + 8
    return y + 212


# ---------------------------------------------------------------- step cards
def byte_table(x, y, fields, cell=None, sizes=True):
    total = sum(n for _, n, _ in fields)
    cell = cell or min(115, 1150 / total)
    cx = x
    for name, n, col in fields:
        w = cell * n
        out.append(f'<rect x="{cx}" y="{y}" width="{w}" height="34" fill="{col}" stroke="#57606a"/>')
        size = 12 if twidth(name, 12) < w - 6 else max(8.5, 12 * (w - 6) / twidth(name, 12))
        text(cx + w / 2, y + 15, name, round(size, 1), anchor='middle')
        if sizes:
            text(cx + w / 2, y + 29, f'{n} B', 10, fill='#57606a', anchor='middle')
        cx += w
    return 40


def bit_table(x, y, fields, nbits=32, width=1150):
    bw = width / nbits
    for b in range(nbits):
        text(x + (nbits - 1 - b) * bw + bw / 2, y + 10, str(b), 10, fill='#57606a', anchor='middle')
    for name, hi, lo, col in fields:
        x0 = x + (nbits - 1 - hi) * bw
        w = (hi - lo + 1) * bw
        out.append(f'<rect x="{x0:.1f}" y="{y + 16}" width="{w:.1f}" height="30" fill="{col}" stroke="#57606a"/>')
        size = 12 if twidth(name, 12) < w - 4 else max(8, 12 * (w - 4) / twidth(name, 12))
        text(x0 + w / 2, y + 36, name, round(size, 1), anchor='middle')
    return 54


def notes(x, y, items, cols=2):
    colw = 590
    rows = (len(items) + cols - 1) // cols
    for i, (bits, name, desc) in enumerate(items):
        c, r = i // rows, i % rows
        xx = x + c * colw
        yy = y + r * 18
        text(xx, yy, bits, 12, bold=True, mono=True)
        text(xx + 62, yy, name, 12, fill=BLUE_S)
        text(xx + 62 + max(70, twidth(name, 12) + 10), yy, desc, 12)
    return rows * 18 + 8


class Step:
    def __init__(self, n, title, path, locks, desc, funcs, sdio, formats, log):
        self.__dict__.update(locals())


def render_step(st, y):
    global out
    y0 = y
    saved = out
    out = []
    cy = y + 24
    text(94, cy - 2, st.title, 15, bold=True)
    text(94, cy + 18, st.path, 12, fill=BLUE_S)
    text(1345, cy - 2, '锁：' + st.locks, 12, fill=LOCKC, anchor='end')
    cy += 38
    for ln in st.desc:
        text(94, cy, ln, 12)
        cy += 18
    cy += 8
    text(94, cy, '函数', 12, fill='#6639ba', bold=True)
    for depth, fn, note in st.funcs:
        fx = 180 + depth * 26
        if depth:
            out.append(f'<path d="M{fx - 16},{cy - 13} V{cy - 4} H{fx - 4}" fill="none" stroke="#8c959f"/>')
        text(fx, cy, fn, 12, fill='#6639ba', mono=True)
        if note:
            endx = fx + len(fn) * 7.3 + 6
            if endx < 835:
                out.append(f'<path d="M{endx:.1f},{cy - 4} H835" stroke="#d0d7de" stroke-dasharray="2 3"/>')
            text(840, cy, note, 11.5, fill='#57606a')
        cy += 17
    cy += 10
    if st.sdio:
        text(94, cy, 'SDIO 传输', 12, fill=BLUE_S, bold=True)
        for ln in st.sdio:
            text(180, cy, ln, 12, fill=BLUE_S)
            cy += 17
        cy += 10
    if st.formats:
        text(94, cy, '数据格式', 12, fill='#bc4c00', bold=True)
        cy += 4
        for kind, title, data, items in st.formats:
            text(170, cy + 14, '▸ ' + title, 12.5, fill='#bc4c00')
            cy += 24
            if kind == 'bytes':
                cy += byte_table(180, cy, data)
            elif kind == 'cells':
                cy += byte_table(180, cy, data, sizes=False)
            elif kind == 'bits8':
                cy += bit_table(180, cy, data, 8, 480)
            elif kind == 'bits16':
                cy += bit_table(180, cy, data, 16, 640)
            else:
                cy += bit_table(180, cy, data)
            cy += 10
            if items:
                cy += notes(180, cy, items)
            cy += 6
    if st.log:
        lh = (len(st.log) - 1) * 16 + 34
        rect(94, cy, 1250, lh, '#f6f8fa', '#d0d7de', 1, 4)
        text(104, cy + 16, st.log[0], 11.5, fill='#57606a')
        for i, ln in enumerate(st.log[1:]):
            text(104, cy + 34 + i * 16, ln, 12, fill='#116329', mono=True)
        cy += lh + 6
    height = cy - y0 + 10
    inner = out
    out = saved
    rect(40, y0, 1320, height, '#fff', '#8c959f', 1)
    out.append(f'<circle cx="68" cy="{y0 + 24}" r="15" fill="#0969da"/>')
    text(68, y0 + 29, str(st.n), 15, fill='#fff', bold=True, anchor='middle')
    out.extend(inner)
    return y0 + height + 16


LOGHEAD = 'WIFI_TRACE=1 日志（make WIFI_TRACE=1；这里是按代码推出的示例值，时间/长度/序号以实机为准）'
CMD52 = [('R/W', 31, 31, C[3]), ('FN', 30, 28, C[0]), ('RAW', 27, 27, ZERO), ('0', 26, 26, ZERO),
         ('寄存器地址（17 位）', 25, 9, C[1]), ('0', 8, 8, ZERO), ('写入数据 / 读时 0', 7, 0, C[2])]
CMD53 = [('R/W', 31, 31, C[3]), ('FN', 30, 28, C[0]), ('BLK=0', 27, 27, ZERO), ('OP', 26, 26, C[4]),
         ('寄存器地址（17 位）', 25, 9, C[1]), ('字节数（512 写 0）', 8, 0, C[2])]
SDPCM = [('len', 2, C[0]), ('~len', 2, C[0]), ('seq', 1, C[1]), ('chan', 1, C[4]), ('next', 1, ZERO),
         ('doff=12', 1, C[2]), ('flow', 1, ZERO), ('tx_max', 1, C[3]), ('rsvd', 2, ZERO)]

steps = [
    Step(0, '开机 initcall：先建总线，再起 host，挂好 rootfs，最后才是 brcmfmac',
         'main() CPU0 → sdio_bus_init → do_initcalls（module_init_ord / late_initcall_ord）',
         'device 核心的注册锁',
         ['sdio_bus_init 在 do_initcalls 之前注册 sdio_bus（按 vendor/device/function 匹配）。arasan-sdio 是 order 1 的 module_init：注册 platform',
          'device + driver，match 后立刻 probe → 枚举出 sdio0:1/2/3。brcmfmac 是 late_initcall order 2：它要从刚挂好的 rootfs/FAT32 读固件，',
          '所以必须排在挂载之后；init_workqueue(brcmf0_wq) 后 sdio_register_driver 触发 sdio_match → brcmf_probe(F1)。'],
         [(0, 'main()', ''), (1, 'device_init(); sdio_bus_init(); usb_bus_init()', '总线先存在'),
          (1, 'procinit(); workqueue_init(); netinit(); gicv3init(); timerinit()', ''),
          (1, 'do_initcalls()', ''), (2, 'arasan_sdio_driver_init()', 'order 1 → 消息 1、2'),
          (2, '（rootfs 挂载：FAT32 bootfs）', ''), (2, 'brcmfmac_driver_init()', 'late order 2'),
          (3, 'init_workqueue(&brcmf_wq[i], "brcmf0_wq", 1)', '每设备 1 个 worker'),
          (3, 'sdio_register_driver(&brcmf_driver)', '→ sdio_match → brcmf_probe（消息 3）'),
          (1, 'userinit() → /init → /bin/wifi', '→ 消息 6、7')],
         None, None, None),

    Step(1, 'Arasan host probe：GPIO34–39 切 ALT3、复位控制器、400 kHz 识别时钟',
         'arasan_probe（启动上下文）→ gpio_init → arasan_set_clock',
         '无（单 CPU 启动阶段）',
         ['gpio_init 把 GPIO34–39 的 GPFSEL 设成 ALT3（=SD1，接到 Arasan），GPPUD=2 + GPPUDCLK1=0xf8 给 35–39（CMD、DAT0–3）上拉，CLK 不拉。',
          '（以前误写 GPPUDCLK0，DAT1 浮空，每秒约 5 万次假卡中断。）然后 CONTROL1.SRST_HC 复位、关所有中断、数据超时 0xe，',
          '用 VideoCore 邮箱取 EMMC 基准时钟，按 2 的幂分频到 ≤400 kHz；mmc_add_host 返回后切到 25 MHz。'],
         [(0, 'arasan_probe(dev)', ''), (1, 'gpio_init()', 'GPFSEL3/4 ALT3，GPPUDCLK1'),
          (1, 'CONTROL1 |= SRST_HC; IRPT_EN = 0; IRPT_MASK = ~0', '复位'),
          (1, 'mailbox_clock()', 'GET_CLOCK_RATE(EMMC)'),
          (1, 'arasan_set_clock(400000)', 'CONTROL1 分频，等 CLK_STABLE'),
          (1, 'mmc_add_host(&arasan.mmc)', '→ 消息 2'),
          (1, 'arasan_set_clock(25000000)', '')],
         None,
         [('bits', 'EMMC CONTROL1（0x2c）', [('0', 31, 27, ZERO), ('SRST_DATA', 26, 26, C[3]), ('SRST_CMD', 25, 25, C[3]), ('SRST_HC', 24, 24, C[3]),
                                         ('0', 23, 20, ZERO), ('DATA_TOUNIT', 19, 16, C[1]), ('SDCLK 分频低 8 位', 15, 8, C[0]),
                                         ('分频高 2', 7, 6, C[0]), ('GENSEL', 5, 5, ZERO), ('0', 4, 3, ZERO), ('CLK_EN', 2, 2, C[2]),
                                         ('STABLE', 1, 1, C[2]), ('INTLEN', 0, 0, C[2])],
           [('b24', 'SRST_HC', '整个 host 复位'), ('b19:16', 'DATA_TOUNIT', '数据超时 = 0xe'), ('b15:6', '分频', '10 位，2 的幂'),
            ('b2', 'CLK_EN', 'SD 时钟输出到 GPIO34'), ('b1', 'CLK_STABLE', '内部时钟稳定（只读）'), ('b0', 'CLK_INTLEN', '内部时钟使能')]),
          ('bits', 'GPFSEL3（GPIO30–39，每脚 3 位；ALT3 = 0b111）', [('0', 31, 30, ZERO), ('39', 29, 27, C[2]), ('38', 26, 24, C[2]), ('37 DAT1', 23, 21, C[3]),
                                                                ('36', 20, 18, C[2]), ('35 CMD', 17, 15, C[1]), ('34 CLK', 14, 12, C[0]),
                                                                ('33', 11, 9, ZERO), ('32', 8, 6, ZERO), ('31', 5, 3, ZERO), ('30', 2, 0, ZERO)],
           None)],
         [LOGHEAD, 'wifi1: t=1012 arasan probe EMMC=0xffffff803f300000 GPIO34-39 ALT3 pull-up 35-39 (GPPUDCLK1=0xf8)',
          'wifi1: t=1014 arasan clock 400000 Hz (base 200000000 / divider 0)',
          'wifi1: t=1036 arasan clock 25000000 Hz (base 200000000 / divider 8)']),

    Step(2, 'SDIO 枚举：CMD0 → CMD5 等 IORDY → CMD3/CMD7 → CMD52 读 CCCR/FBR/CIS',
         'mmc_add_host → sdio_scan_host（启动上下文）→ cmd52_host / mmc_command → arasan_request',
         '无（myproc()==0 时 mmc_request_sync 不拿 💤 mmc-request）',
         ['CMD0 复位，CMD5(arg 0) 读 IO OCR：bit30:28 = 3 个 I/O function。再带上 OCR 电压位反复发 CMD5，直到响应 bit31 IORDY=1。',
          'CMD3 让卡报出 RCA，CMD7(RCA<<16) 选中卡。CMD52 读写 F0 的 CCCR：Bus Interface 低 2 位写 2 → 4-bit，然后 host CONTROL0 也切 4-bit。',
          '每个 function 从 FBR 读 class 和 CIS 指针，沿 CIS 找 MANFID 元组得到 vendor 0x02d0 / device 0xa9a6，device_register(sdio0:N)。'],
         [(0, 'sdio_scan_host(host)', ''), (1, 'mmc_command(CMD0)', '无响应'),
          (1, 'mmc_command(CMD5, 0) → OCR', 'R4'), (1, 'for retry: mmc_command(CMD5, ocr & 0xffffff)', '等 IORDY'),
          (1, 'mmc_command(CMD3) → rca; mmc_command(CMD7, rca<<16)', 'R6 / R1'),
          (1, 'cmd52_host(F0, CCCR_BUS_INTERFACE) |= 2', '4-bit'), (2, 'arasan_set_bus_width(4)', 'CONTROL0 bit1'),
          (1, 'for fn: cmd52 FBR_CLASS / FBR_CIS → read_cis_id', ''),
          (2, 'device_register(&f->dev)', 'sdio0:1/2/3 挂到 sdio_bus')],
         ['CMD0、CMD5×N、CMD3、CMD7：只有命令线，没有数据阶段',
          'CMD52 × 若干：每条 = 48 位命令 + 48 位 R5 响应，R5 低 8 位就是读到的字节'],
         [('bits', 'CMD5 响应 R4（IO OCR）', [('IORDY', 31, 31, C[2]), ('NF=3', 30, 28, C[0]), ('MP', 27, 27, ZERO), ('0', 26, 25, ZERO),
                                         ('S18A', 24, 24, ZERO), ('OCR 电压窗口', 23, 0, C[1])],
           [('b31', 'IORDY', '卡内部初始化完成'), ('b30:28', 'NF', 'I/O function 数 = 3'), ('b27', 'MP', '是否还有存储部分（0）'),
            ('b23:0', 'OCR', '支持的电压，回填给第二次 CMD5')]),
          ('bits', 'CMD52 参数（ARG1）', CMD52,
           [('b31', 'R/W', '1 = 写'), ('b30:28', 'FN', '0 = CCCR，1/2 = function'), ('b27', 'RAW', '写后读回'),
            ('b25:9', '地址', '例：0x07 Bus Interface，0x05 INT_PENDING'), ('b7:0', '数据', '写入的字节')])],
         [LOGHEAD, 'wifi2: t=1016 sdio CMD0 reset; CMD5 arg=0 -> OCR=90ff8000 functions=3',
          'wifi2: t=1019 sdio CMD5 arg=ff8000 -> R4=90ff8000 IORDY after 2 tries',
          'wifi2: t=1020 sdio CMD3 -> RCA=1; CMD7 select card',
          'wifi2: t=1021 sdio CMD52 F0 CCCR BUS_INTERFACE=82 (4-bit)',
          'wifi2: t=1025 sdio CMD52 FBR1 class=0 CIS=10f0 -> vendor=2d0 device=a9a6']),

    Step(3, 'brcmf_probe：打开 F1、要 ALP 时钟、经 F1 窗口读 CHIPID、从 bootfs 打开固件',
         '启动上下文 → sdio_match → sdio_probe → brcmf_probe(F1)',
         '无',
         ['sdio_enable_func 写 CCCR IO_ENABLE 的 bit1，轮询 IO_READY。F2 暂不打开（固件跑起来才 ready）。brcmf_clock_prepare 写 F1 CHIPCLKCSR',
          '请求 ALP 时钟并等 ALP_AVAIL，之后才能访问 backplane。读 ChipCommon 0x18000000：先 3 条 CMD52 把窗口设成 0x18000000，',
          '再 CMD53 F1 读 4 字节 @0x8000。chip 0x4345 rev 6 = BCM43455 → 选 BCM43455.BIN/TXT/CLM，用 fat32openroot 打开（不整块读入）。'],
         [(0, 'brcmf_probe(func1)', ''), (1, 'sdio_enable_func(F1)', 'CCCR IO_ENABLE |= 0x02'),
          (1, 'brcmf_clock_prepare(F1)', 'CHIPCLKCSR = ALP_AVAIL_REQ'),
          (1, 'brcmf_backplane_read32(CHIPCOMMON_CHIPID)', ''),
          (2, 'brcmf_set_backplane_window(0x18000000)', '3 × CMD52 写 0x1000a/b/c'),
          (2, 'sdio_cmd53(F1, R, 0x8000, incr, 4)', ''),
          (1, 'brcmf_load_firmware("BCM43455BIN") / TXT', 'fat32openroot'),
          (1, 'fat32openroot("BCM43455CLM")', '可选')],
         ['CMD52 W F0 0x02 IO_ENABLE · CMD52 R F0 0x03 IO_READY（轮询）',
          'CMD52 W F1 0x1000e CHIPCLKCSR=0x08 · CMD52 R 0x1000e 直到 bit6 ALP_AVAIL',
          'CMD52 W F1 0x1000a=0x00 · 0x1000b=0x00 · 0x1000c=0x18 · CMD53 R F1 0x08000 4 B'],
         [('bits', 'ChipCommon CHIPID（0x18000000）= 0x15264345', [('0', 31, 28, ZERO), ('CoreCnt?', 27, 24, ZERO), ('pkg=2', 23, 20, C[4]),
                                                                 ('rev=6', 19, 16, C[1]), ('chip = 0x4345', 15, 0, C[0])],
           [('b15:0', 'chip', '0x4345 → BCM43455（rev≠9）'), ('b19:16', 'rev', '6'), ('b23:20', 'pkg', '2'),
            ('注意', 'SDIO ID', '0xa9a6 和 chip 号是两套编号')]),
          ('bits8', 'F1 CHIPCLKCSR（0x1000e）', [('0', 7, 7, ZERO), ('ALP_AVAIL', 6, 6, C[2]), ('FORCE_HW_CLKREQ_OFF', 5, 5, ZERO), ('0', 4, 4, ZERO),
                                               ('ALP_REQ', 3, 3, C[1]), ('0', 2, 2, ZERO), ('FORCE_HT', 1, 1, C[3]), ('FORCE_ALP', 0, 0, ZERO)],
           None)],
         [LOGHEAD, 'wifi3: t=1840 sdio CMD52 CCCR IO_ENABLE=2 -> IO_READY=2: F1 ready after 180 us',
          'wifi3: t=1843 F1 window 18000000 + CMD53 @8000 -> CHIPID=15264345 chip=4345 rev=6',
          'wifi3: t=1861 bootfs BCM43455 BIN=616000 TXT=2074 CLM=14400 bytes']),

    Step(4, '经 F1 下载固件：解析 EROM → halt CR4 → BIN 写 TCM → NVRAM 写尾部 → 放开 CR4',
         'brcmf_probe → brcmf_scan_cores / get_raminfo / set_passive / write_firmware / write_nvram / set_active',
         '无（但耗时最长：几百 KB 全靠 PIO + 小块 CMD53）',
         ['EROM 描述符给出各 core 的基址和 wrapper；从 CR4 bank 寄存器累加出 TCM 大小，BCM4345/6 的 RAM base 是 0x198000。',
          'set_passive 经 wrapper IOCTL/RESET_CTL 让 CR4 停下、D11 保持复位。BIN 从 FAT32 每次读 512 B，brcmf_backplane_transfer 按 32 KB 窗口',
          '和 transfer_limit 切块（实机 512/256/128 B 出 CRC 错，减半到 64 B）。NVRAM 文本转成 NUL 分隔、补 token，写到 RAM 尾部。最后清 SDIO',
          'INTSTATUS、把 reset vector 写到地址 0、放开 CR4：固件开始运行。'],
         [(0, 'brcmf_scan_cores(F1, &bc)', 'brcmf_erom_desc / regaddr'),
          (0, 'brcmf_get_raminfo(F1, &bc)', 'TCM base / size'),
          (0, 'brcmf_set_passive(F1, &bc)', 'core_disable(CR4)、D11 reset'),
          (0, 'brcmf_write_firmware(F1, &bc, &code, &rv)', ''),
          (1, 'fat32readnext(512) → brcmf_backplane_transfer(W)', '循环 ~1200 次'),
          (2, 'brcmf_set_backplane_window(addr)', '跨 32 KB 才变'),
          (2, 'sdio_cmd53(F1, W, (addr&0x7fff)|0x8000, incr, chunk)', 'CRC 错 → chunk/2 重试'),
          (1, 'brcmf_backplane_read32(rambase) == rv', '回读校验'),
          (0, 'brcmf_prepare_nvram → brcmf_write_nvram', 'RAM 尾部 + token'),
          (0, 'brcmf_set_active(F1, &bc, rv)', '写地址 0、core_reset(CR4)')],
         ['CMD53 W F1 × 约 600 KB / 64 B ≈ 9600 次（每次 64 B 数据 = 16 个 32 位字，CPU 逐字写 EMMC_DATA）',
          'CMD52 × 3 每跨一个 32 KB 窗口（BIN 约 19 个窗口）'],
         [('bits', 'CMD53 参数（F1 backplane 写）', CMD53,
           [('b31', 'R/W', '1 = 写'), ('b30:28', 'FN', '1'), ('b27', 'BLK', '0 = byte 模式'), ('b26', 'OP', '1 = 地址自增（F1）'),
            ('b25:9', '地址', '(addr & 0x7fff) | 0x8000'), ('b8:0', 'count', '64；512 时写 0')]),
          ('bits', 'NVRAM 末尾 token（写在 RAM 最后 4 字节）', [('~(len/4) 低 16 位', 31, 16, C[3]), ('len/4（32 位字数）', 15, 0, C[0])],
           [('b15:0', 'words', 'NVRAM 字数'), ('b31:16', '~words', '按位取反，固件用来校验')])],
         [LOGHEAD, 'wifi4: t=1872 EROM: 7 cores ChipCommon=18000000 SDIO=18002000 CR4=18003000',
          'wifi4: t=1874 TCM base=198000 size=819200 banks=5',
          'wifi4: t=1880 set_passive: CR4 halted, D11 held in reset',
          'wifi4: t=3490 BIN 616000 bytes -> TCM 198000, 19 windows, CMD53 chunk=64, 1610 ms, reset vector=b83ef198',
          'wifi4: t=3502 NVRAM 2052 bytes -> 264dfc (RAM tail) token=fbfe0401',
          'wifi4: t=3505 set_active: reset vector b83ef198 -> address 0, CR4 released']),

    Step(5, 'F2 ready、FWREADY mailbox、第一次 BCDC 往返、注册 wlan0、打开 DAT1 中断',
         'brcmf_start_f2 → brcmf_control_plane_start（启动上下文）',
         'bus->lock（brcmf_dcmd 内）',
         ['CHIPCLKCSR 置 FORCE_HT，TOSBMAILBOXDATA 写协议版本 4，再 CCCR IO_ENABLE |= F2 等 IO_READY（固件起来了才会 ready）。HOSTINTMASK 打开',
          'FRAME_IND/HOST_INT 等位，WATERMARK=0x60、MESBUSYCTRL=0xd0。每 1 ms 读 TOHOSTMAILBOXDATA，看到 FWREADY/DEVREADY 后回 SMB_INT_ACK。',
          '之后用 BCDC 读 "ver"、分块下 CLM、读 MAC、发 UP；register_netdev(wlan0)；sdio_claim_irq 开 CCCR INT_ENABLE；napi_enable；',
          'arasan_sdio_irq_enable 打开 IRPT_EN.CARD_INT —— 从这里起，芯片有帧就拉 DAT1。'],
         [(0, 'brcmf_start_f2(F1, &bc)', ''), (1, 'CHIPCLKCSR |= FORCE_HT; TOSBMAILBOXDATA = 4<<16', ''),
          (1, 'sdio_enable_func(F2)', 'IO_ENABLE = 0x06'),
          (1, 'HOSTINTMASK; WATERMARK; DEVICE_CTL; MESBUSYCTRL', ''),
          (1, 'poll TOHOSTMAILBOXDATA & (FWREADY|DEVREADY)', '≤3 s'),
          (0, 'brcmf_alloc_bus()', 'netif_napi_add(…, brcmf0_wq, poll, 16)'),
          (0, 'brcmf_control_plane_start()', ''), (1, 'brcmf_iovar("ver") · brcmf_download_clm · "cur_etheraddr"', '消息 6 的格式'),
          (1, 'brcmf_dcmd(BRCMF_C_UP)', ''), (1, 'register_netdev(&netdev)', 'wlan0'),
          (1, 'sdio_claim_irq(F1)', 'CCCR INT_ENABLE = 0x03'), (1, 'napi_enable · arasan_sdio_irq_enable()', 'IRPT_EN = CARD_INT')],
         ['CMD52 W F0 0x02 IO_ENABLE=0x06 · CMD53 W F1 HOSTINTMASK / TOSBMAILBOXDATA · CMD53 R F1 TOHOSTMAILBOXDATA（轮询）',
          'CMD52 W F0 0x04 INT_ENABLE=0x03'],
         [('bits', 'TOHOSTMAILBOXDATA（SDIO core +0x4c）', [('0', 31, 24, ZERO), ('协议版本 = 4', 23, 16, C[0]), ('0', 15, 5, ZERO),
                                                         ('FWHALT', 4, 4, C[3]), ('FWREADY', 3, 3, C[2]), ('0', 2, 2, ZERO), ('DEVREADY', 1, 1, C[2]), ('NAK', 0, 0, ZERO)],
           [('b23:16', 'version', 'SDPCM 协议 4'), ('b3', 'FWREADY', '固件起来了'), ('b1', 'DEVREADY', '同上（旧固件）'),
            ('b4', 'FWHALT', '固件崩了：打印并失败')]),
          ('bits8', 'CCCR INT_ENABLE（F0 0x04）', [('0', 7, 4, ZERO), ('F3', 3, 3, ZERO), ('F2', 2, 2, ZERO), ('F1', 1, 1, C[2]), ('IENM', 0, 0, C[2])],
           [('b0', 'IENM', '总开关'), ('b1', 'F1', '只开 F1：帧到达也由 F1 的 INTSTATUS 报')])],
         [LOGHEAD, 'wifi5: t=3561 sdio CMD52 CCCR IO_ENABLE=6 -> IO_READY=6: F2 ready after 52000 us',
          'wifi5: t=3563 FWREADY mailbox=40008 protocol=4 HOSTINTMASK=200000f0',
          'wifi5: t=3661 sdio CMD52 CCCR INT_ENABLE=3 (master + F1)',
          'wifi5: t=3662 wlan0 registered mac=b8:27:eb:12:34:56, NAPI weight 16 on brcmf0_wq',
          'wifi5: t=3662 arasan IRPT_EN=CARD_INT: DAT1 card interrupt -> IRQ 62 enabled']),

    Step(6, 'BCDC 控制命令：brcmf_dcmd 发 SDPCM ch0，按 id 等回复',
         '/bin/wifi 或启动上下文 → brcmf_iovar → brcmf_dcmd → brcmf_f2_transfer → sdio_cmd53(F2)',
         '🔒 brcmf-iovar → 🔒 brcmfmac（bus->lock）→ 💤 mmc-request（每条 CMD）',
         ['brcmf_iovar 把 "名字\\0" + 值放进 iovar_buf，调 brcmf_dcmd(GET_VAR 262 / SET_VAR 263)。dcmd 拿 bus->lock，reqid++，填 16 字节 BCDC 头',
          '（cmd、len、flags = id<<16 | SET），前面加 12 字节 SDPCM 头（seq = tx_seq++，chan 0），补到 4 字节对齐，按 transfer_limit 切成几条',
          'CMD53 写 F2（地址固定）。然后循环 brcmf_rx_dispatch_locked：中途来的事件、数据帧照常分流，直到 ctl_id == id；3 秒超时。'],
         [(0, 'brcmf_iovar(bus, "bus:rxglom", &v, 4, SET)', '🔒 iovar_lock'),
          (1, 'brcmf_dcmd(bus, 263, buf, len, 1)', '🔒 bus->lock'),
          (2, 'id = ++reqid; request->flags = id<<16 | 0x02', ''),
          (2, 'brcmf_sdpcm_header(tx, frame_len, CONTROL_CHANNEL)', 'seq = tx_seq++'),
          (2, 'brcmf_f2_transfer(W, tx, wire_len)', ''),
          (3, 'brcmf_set_backplane_window(cc_base) · sdio_cmd53(F2, W, 固定地址)', '💤 mmc-request'),
          (2, 'while(!timeout) brcmf_rx_dispatch_locked()', '消息 9 的读帧'),
          (3, 'channel 0 → ctl[] / ctl_id', 'id 相同就停'),
          (2, 'reply->status == 0 ?', '-17 = 未关联，-23 = 不支持')],
         ['CMD52 × 3 设窗口 → CMD53 W F2 ⌈wire_len / 64⌉ 次（OP=0）→ 等回复：CMD52 INT_PENDING + CMD53 F1 INTSTATUS + CMD53 R F2'],
         [('bytes', 'F2 写出的控制帧 = SDPCM 头 + BCDC dcmd 头 + 载荷', SDPCM + [('cmd', 4, C[0]), ('len', 4, C[0]), ('flags', 4, C[1]),
                                                                         ('status', 4, C[3]), ('"bus:rxglom\\0" + 值', 8, ZERO)],
           [('b0-1', 'len', '帧长（含头）'), ('b4', 'seq', 'tx_seq++'), ('b5', 'chan', '0 = 控制'), ('b7', 'doff', '12'),
            ('+12', 'cmd', '262 GET_VAR / 263 SET_VAR / 26 SET_SSID …'), ('+20', 'flags', 'id<<16 | SET 0x02 | ERROR 0x01')])],
         [LOGHEAD, 'wifi6: t=3570 dcmd tx ch=0 id=1 cmd=262 ver GET len=259 seq=0',
          'wifi6: t=3573 dcmd rx ch=0 id=1 status=0 len=275 after 3 ms, 1 frames read',
          'wifi6: t=5259 dcmd tx ch=0 id=24 cmd=263 bus:rxglom SET len=15 seq=23',
          'wifi6: t=5280 dcmd rx ch=0 id=30 status=-23 len=16 after 2 ms, 1 frames read']),

    Step(7, '连接 AP：SET_SSID → 固件关联 → 主机做 WPA2 四次握手 → DHCP → 关省电',
         '/bin/wifi → sys_wifi_connect → brcmfmac_connect；EAPOL 在 brcmf0_wq 的 NAPI 里收（消息 9）',
         'bus->lock · iovar_lock · 💤 mmc-request',
         ['准备：DOWN/UP、订阅事件、mpc=0、bus:rxglom=0、SET_INFRA=1、SET_AUTH=0、SET_WSEC=AES、wpa_auth=WPA2_PSK；PBKDF2 算 PMK。',
          'SET_WSEC_PMK 返回不支持 → host_eapol=1。SET_SSID 后固件自己扫描/认证/关联，用事件（ch1）报结果；主机轮询 GET_BSSID 直到有 BSSID，',
          '读出固件实际发出的 RSN IE。M1/M3 由 NAPI 的 brcmf_wpa_rx_locked 处理并发 M2；M3 后 keys_pending=1，connect 装 PTK/GTK、发 M4，',
          'handshake_done=1 后普通数据才能收发。等 500 ms → DHCP → SET_PM=0（PM2 会让 AP 把帧压到下个 beacon，每包 ~100 ms）。'],
         [(0, 'brcmfmac_connect(ssid, psk)', ''), (1, 'brcmf_dcmd(SET_SSID)', 'brcmf_connect_watch_start'),
          (1, 'for i < 40: brcmf_dcmd(GET_BSSID)', ''), (2, 'brcmf_read_assoc_rsn_ie()', 'assoc_info / assoc_req_ies'),
          (1, '〔NAPI〕brcmf_wpa_rx_locked(M1) → brcmf_wpa_send_m2_locked', 'PTK、MIC'),
          (1, '〔NAPI〕brcmf_wpa_rx_locked(M3)', 'MIC 校验、AES unwrap GTK'),
          (1, 'brcmf_finish_wpa_handshake()', ''), (2, 'brcmf_install_ccmp_key(PTK) · (GTK)', 'bsscfg:wsec_key'),
          (2, 'brcmf_wpa_send_m4_locked()', 'handshake_done = 1'),
          (1, 'netdev_poll_one × 500 ms → net_dhcp_dev(wlan0)', ''), (1, 'brcmf_dcmd(SET_PM, 0)', '关省电'),
          (1, 'brcmf_connect_watch_stop()', '')],
         ['全是消息 6（ch0 命令）和消息 9/10（ch2 的 EAPOL 收发），没有新的 SDIO 操作'],
         [('bits16', 'EAPOL-Key Key Information（M1 = 0x008a，M3 = 0x13ca）', [('0', 15, 13, ZERO), ('EncData', 12, 12, C[4]), ('Req', 11, 11, ZERO),
                                                                          ('Err', 10, 10, ZERO), ('Secure', 9, 9, C[2]), ('MIC', 8, 8, C[3]),
                                                                          ('Ack', 7, 7, C[1]), ('Install', 6, 6, C[2]), ('Idx', 5, 4, ZERO),
                                                                          ('Pair', 3, 3, C[0]), ('Ver=2', 2, 0, C[5])],
           [('b3', 'Pairwise', 'M1–M4 都是 1'), ('b7', 'Ack', 'AP 要回复（M1、M3）'), ('b8', 'MIC', 'M2–M4 带 MIC'),
            ('b6', 'Install', 'M3：装 PTK'), ('b9', 'Secure', 'M3/M4'), ('b12', 'EncData', 'M3 的 key data 加密（GTK）'),
            ('b2:0', 'Ver', '2 = HMAC-SHA1 + AES')])],
         [LOGHEAD, 'wifi7: t=5413 join SET_SSID TP-Link_B114 (WPA2-PSK/AES, host EAPOL=1)',
          'wifi7: t=5691 event ch=1 type=3 status=0 reason=0',
          'wifi7: t=5695 EAPOL rx ch=2 len=113 key-info=8a',
          'wifi7: t=5713 EAPOL M2 tx (SNonce, MIC, RSN IE 22 bytes)',
          'wifi7: t=5721 EAPOL M3 rx key-info=13ca key-data=56 (encrypted GTK)',
          'wifi7: t=5731 GET_BSSID poll 2 -> associated bssid=50:d4:f7:aa:b1:14',
          'wifi7: t=5742 WPA2 done: PTK + GTK installed, handshake_done=1, data port open',
          'wifi7: t=6278 DHCP done: wlan0 IPv4 ready',
          'wifi7: t=6282 SET_PM=0: power save off (no PM2 beacon buffering)']),

    Step(8, '帧到了：DAT1 → Arasan CARD_INT → IRQ 62 → 顶半部屏蔽 + napi_schedule',
         '芯片 → GPIO37 → Arasan → BCM2837 IRQ 62 → devintr → arasan_sdio_irq → brcmfmac_sdio_irq',
         '🔒 napi->lock · 🔒 wq->lock（顶半部不拿 bus->lock）',
         ['固件把帧放进 F2 FIFO 并置 INTSTATUS.FRAME_IND；INTSTATUS & HOSTINTMASK ≠ 0 时，芯片在 4-bit 数据块之间的中断周期把 DAT1 拉低。',
          'Arasan 置 INTERRUPT.CARD_INT；IRPT_EN 里开着它，于是 IRQ 62。这是电平中断：卡里的 INTSTATUS 不清，DAT1 一直低，所以顶半部先',
          'IRPT_EN = 0 屏蔽、写 1 清 CARD_INT、pending=1、计数 +1，再 napi_schedule（已在跑就记 missed），立刻返回。顶半部不 printf（pr.lock）。'],
         [(0, 'devintr()', 'gic_iar → irq 62'), (1, 'arasan_sdio_irq()', '中断上下文'),
          (2, 'status = INTERRUPT; if(status & CARD_INT)', 'bit8'),
          (3, 'IRPT_EN = 0; INTERRUPT = CARD_INT', '屏蔽 + W1C'),
          (3, 'arasan_card_irq_pending = 1; arasan_card_irq_count++', ''),
          (3, 'brcmfmac_sdio_irq()', ''), (4, 'napi_schedule(&bus->napi)', '🔒 napi'),
          (5, 'queue_work(brcmf0_wq, &napi->work)', '唤醒 worker → 消息 9')],
         ['无：顶半部只读写 Arasan 自己的寄存器，不发 CMD'],
         [('bits', 'Arasan INTERRUPT（0x30，写 1 清）', [('错误位', 31, 16, C[3]), ('ERR', 15, 15, C[3]), ('0', 14, 9, ZERO), ('CARD_INT', 8, 8, C[2]),
                                                    ('0', 7, 6, ZERO), ('READ_RDY', 5, 5, C[0]), ('WRITE_RDY', 4, 4, C[0]), ('0', 3, 2, ZERO),
                                                    ('DATA_DONE', 1, 1, C[1]), ('CMD_DONE', 0, 0, C[1])],
           [('b8', 'CARD_INT', 'DAT1 中断（电平）'), ('b0', 'CMD_DONE', 'arasan_request 等它'), ('b1', 'DATA_DONE', '数据阶段结束'),
            ('b5/4', 'READ/WRITE_RDY', 'PIO 可以读写 DATA'), ('b31:16', '错误', 'CRC、超时 → SRST_CMD/DATA')]),
          ('bits', 'SDIO core INTSTATUS（+0x20，写 1 清；HOSTINTMASK = 0x200000f0）', [('0', 31, 30, ZERO), ('FC_CHANGE', 29, 29, C[1]), ('0', 28, 8, ZERO),
                                                                                   ('HOST_INT', 7, 7, C[4]), ('FRAME_IND', 6, 6, C[2]),
                                                                                   ('SMB_NAK..', 5, 4, ZERO), ('0', 3, 0, ZERO)],
           [('b6', 'FRAME_IND', 'F2 里有帧'), ('b7', 'HOST_INT', 'mailbox 有消息（读 TOHOSTMAILBOXDATA）'), ('b29', 'FC_CHANGE', '流控状态变了')])],
         [LOGHEAD, '（中断里不打印；下一次 NAPI poll 开头报告这期间来了几次）',
          'wifi8: t=5690 irq DAT1 -> CARD_INT -> IRQ 62 x1 (total 1) -> napi_schedule(brcmf0_wq)']),

    Step(9, 'NAPI 读帧：CMD52 查 INT_PENDING → 读清 INTSTATUS → CMD53 读 F2 → 分流',
         'brcmf0_wq kworker → napi_workfn → brcmf_napi_poll → brcmf_rx_dispatch_locked → brcmf_rx_frame_locked',
         '🔒 brcmfmac（bus->lock）· 💤 mmc-request；交给协议栈前放锁',
         ['先 CMD52 读 CCCR INT_PENDING：F1 位没置就是假中断，直接返回。否则 CMD53 读 INTSTATUS 并写同值清除（DAT1 松开），HOST_INT 就读并 ACK',
          'mailbox。FRAME_IND 或 rx_more 时 CMD53 读 F2 的前 64 B，拿 len/~len；坏头 → FRAMECTRL=RF_TERM；superframe 读进 16 KB 缓冲逐个拆子帧。',
          '正常帧读剩下的 len−64（4 对齐）。byte 9 更新 tx_max。ch0 存回复，ch1 打印事件，ch2：EAPOL 进 WPA，其他（handshake_done 后）进 4 槽',
          'net_rxq。每读到一帧 rx_more=1 继续读；出队时放 bus->lock 再调 net_rx_dev（协议栈可能马上回 ACK → 消息 10 要拿同一把锁）。'],
         [(0, 'brcmf_napi_poll(napi, 16)', '🔒 bus->lock'),
          (1, 'brcmf_rx_dispatch_locked()', ''), (2, 'brcmf_rx_frame_locked()', ''),
          (3, 'sdio_int_pending(F1)', 'CMD52 F0 0x05'),
          (3, 'brcmf_backplane_read32/write32(INTSTATUS)', 'W1C'),
          (3, 'brcmf_f2_transfer(R, rx, 64)', 'SDPCM 头'), (3, 'brcmf_f2_transfer(R, rx+64, rest)', ''),
          (3, 'tx_max = rx[9]; channel = rx[5] & 0xf', ''),
          (2, 'ch2: bcdc → ether → net_rxq[(head+n)%4]', ''),
          (1, 'release(bus->lock) → net_rx_dev(dev, frame, len)', '→ tcp_rx / arp_rx / udp'),
          (2, 'tcp_rx：按序收下 / 乱序缓存 / 回 ACK', '→ 消息 10')],
         ['CMD52 R F0 0x05 · CMD52×3 设窗口 + CMD53 R/W F1 INTSTATUS（4 B）· CMD53 R F2 64 B + CMD53 R F2 ⌈(len−64)/64⌉ 次'],
         [('bytes', 'F2 读到的数据帧 = SDPCM 头 + BCDC 数据头 + 以太网帧', SDPCM + [('flags', 1, C[1]), ('prio', 1, C[1]), ('flags2', 1, ZERO),
                                                                          ('doff', 1, C[1]), ('dst', 6, C[2]), ('src', 6, C[2]),
                                                                          ('type', 2, C[2]), ('IPv4 / ARP / EAPOL …', 8, ZERO)],
           [('b9', 'tx_max', '固件给的发送信用'), ('b5', 'chan', '2 = 数据'), ('+12', 'BCDC flags', '0x20 = 版本 2'),
            ('+15', 'doff', '再跳过 doff×4 字节'), ('+28', 'type', '0x0800 IPv4，0x0806 ARP，0x888e EAPOL')])],
         [LOGHEAD, 'wifi9: t=9105 rx ch=2 len=80 seq=8 tx_max=50 tx_seq=41 intstatus=40',
          'wifi9: t=9106 rx ch=2 len=1474 seq=9 tx_max=50 tx_seq=41 intstatus=0',
          'wifi9: t=9108 rx ch=2 len=1474 seq=10 tx_max=50 tx_seq=41 intstatus=0']),

    Step(10, '发送：brcmfmac_xmit 等 TX 信用 → SDPCM 头 → CMD53 写 F2',
         '调用者上下文（TCP 定时器 / write 系统调用 / NAPI 里回的 ACK）→ netdev_xmit_on → brcmfmac_xmit',
         '🔒 brcmfmac（bus->lock）· 💤 mmc-request',
         ['未 handshake_done 直接返回 -1。拿 bus->lock，BCDC 头 flags=0x20 + 以太网帧。信用：window = (uint8)(tx_max − tx_seq)，',
          '为 0 或 ≥0x80 就不能发 —— 调 brcmf_rx_dispatch_locked 读一个上行帧（它带新的 tx_max，读到的数据帧放进 net_rxq 留给 NAPI），没帧就等 100 µs；',
          '200 次（约 20 ms）还不行就丢掉这帧，靠 TCP 重传。有信用后写 SDPCM 头（seq = tx_seq++、chan 2）、补 4 对齐、按 transfer_limit 切成多条 CMD53 写 F2。'],
         [(0, 'netdev_xmit_on(dev, frame, len) → brcmfmac_xmit', ''),
          (1, 'acquire(bus->lock) · brcmf_send_ether_locked()', '🔒'),
          (2, 'bcdc->flags = 2<<4; memmove(bcdc+1, frame)', ''),
          (2, 'while window==0 || window>=0x80: brcmf_rx_dispatch_locked()', '≤200 × 100 µs'),
          (2, 'brcmf_sdpcm_header(tx, frame_len, DATA_CHANNEL)', 'seq = tx_seq++'),
          (2, 'brcmf_f2_transfer(W, tx, wire_len)', ''),
          (3, 'sdio_cmd53(F2, W, 固定地址, chunk)', '💤 mmc-request'),
          (1, 'release(bus->lock)', '')],
         ['CMD52×3 设窗口 → CMD53 W F2 ⌈wire_len / 64⌉ 次（1500 B 的 TCP 段 ≈ 24 次）'],
         [('cells', '信用窗口（seq 是 8 位，按模 256 比较）', [('0x2c 已发', 1, ZERO), ('0x2d tx_seq', 1, C[1]), ('0x2e', 1, C[2]),
                                                     ('0x2f tx_max', 1, C[3]), ('0x30', 1, '#fff'), ('…', 1, '#fff')],
           [('window', '= 2', 'tx_max − tx_seq：还能发 2 帧'), ('=0', '没信用', '先读上行帧拿新 tx_max'),
            ('≥0x80', '异常', '视为没信用'), ('>0x40', '夹紧', '收到时：tx_max 太超前就当 tx_seq+2')])],
         [LOGHEAD, 'wifi10: t=9110 tx ch=2 len=72 type=800 seq=41 tx_max=50 window=9 credit-wait=0',
          'wifi10: t=9115 tx ch=2 len=1476 type=800 seq=44 tx_max=45 window=1 credit-wait=0',
          'wifi10: t=9119 tx ch=2 len=1476 type=800 seq=45 tx_max=47 window=2 credit-wait=31']),

    Step(11, '读空：napi_complete_done → 重开 CARD_INT；假中断延迟 1 tick；拆除顺序',
         'brcmf_napi_poll 结尾 → arasan_sdio_irq_complete / queue_delayed_work(irq_work)；brcmf_remove',
         '🔒 napi->lock · 🔒 delayed_lock',
         ['读到 len=0（FIFO 空）就 drained；budget 16 用完则保持 scheduled 重新排队，不开中断。drained 且 complete 成功：若这次一帧没读到且',
          'INT_PENDING 里 F1 没置位（Arasan 偶发假卡中断，一开就又来），就不立刻开，queue_delayed_work(irq_work, 1)，10 ms 后 brcmf_irq_worker 再开；',
          '否则 arasan_sdio_irq_complete：pending=0、写 1 清 CARD_INT、IRPT_EN=CARD_INT。拆除：ready=0 → sdio_release_irq → unregister_netdev → free。'],
         [(0, 'brcmf_napi_poll 结尾', ''),
          (1, 'if(keys_pending && !connect_active) brcmf_finish_wpa_handshake()', 'AP 换钥'),
          (1, 'napi_complete_done(napi, work_done)', '🔒 napi：missed 就再排'),
          (2, 'spurious ? queue_delayed_work(wq, &irq_work, 1)', '→ brcmf_irq_worker'),
          (2, ': arasan_sdio_irq_complete()', 'IRPT_EN = CARD_INT'),
          (0, 'brcmf_remove(F1)', '拆除'), (1, 'ready = 0; sdio_release_irq(F1)', 'CCCR INT_ENABLE 清位'),
          (1, 'unregister_netdev(wlan0); brcmf_free_bus()', '')],
         ['CMD52 R F0 0x05 INT_PENDING（poll 开头那条，已在消息 9 里）'],
         [('bits8', 'CCCR INT_PENDING（F0 0x05，只读）', [('0', 7, 4, ZERO), ('F3', 3, 3, ZERO), ('F2', 2, 2, ZERO), ('F1', 1, 1, C[2]), ('0', 0, 0, ZERO)],
           [('b1', 'F1', '1 = 卡真的在要中断'), ('全 0', '假中断', 'host 报了 CARD_INT 但卡没请求')])],
         [LOGHEAD, 'wifi11: t=9109 napi frames=3 budget=16 drained=1 spurious-before=2 -> complete, CARD_INT re-enabled',
          'wifi11: t=9123 napi frames=1 budget=16 drained=1 spurious-before=0 -> complete, CARD_INT re-enabled']),
]


def main(path):
    global out
    body_y = overview()
    y = body_y + 30
    text(40, y, '0–11 号消息逐步说明（函数按调用链逐行缩进，寄存器/帧按位展开；日志行是 WIFI_TRACE=1 的格式，数值为示例）', 18, bold=True)
    y += 20
    for st in steps:
        y = render_step(st, y)
    H = int(y + 20)
    head = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
            f'font-family="PingFang SC, Noto Sans CJK SC, WenQuanYi Zen Hei, sans-serif" font-size="13">',
            '<defs>']
    for mid, col in (('a', '#333'), ('ab', BLUE_S), ('ar', RED), ('ag', GREEN_S), ('ay', YEL_S), ('ap', PURP_S), ('ao', ORANGE_S)):
        head.append(f'<marker id="{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto">'
                    f'<path d="M0,0L10,5L0,10z" fill="{col}"/></marker>')
    head.append('</defs>')
    head.append(f'<rect width="{W}" height="{H}" fill="#fff"/>')
    open(path, 'w').write('\n'.join(head + out + ['</svg>']) + '\n')
    print(path, H)


main(sys.argv[1] if len(sys.argv) > 1 else 'sdio-wifi-comm-steps.svg')
