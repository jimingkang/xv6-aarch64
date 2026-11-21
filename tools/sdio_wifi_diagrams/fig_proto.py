from svglib import *

C = ['#ddf4ff', '#fff8c5', '#dafbe1', '#ffebe9', '#fbefff', '#eaeef2', '#fff1e5']
ZERO = '#eaeef2'


def panel(s, y, h, title, sub=''):
    s.rect(24, y, s.w - 48, h, '#fff', '#d0d7de', 1.2, 10)
    s.text(40, y + 26, title, 16, bold=True)
    if sub:
        s.text(40, y + 46, sub, 12, fill=SUB)


def fig_proto(path):
    W, H = 1400, 1610
    s = Svg(W, H)
    s.text(W / 2, 34, 'SDIO Wi-Fi 通信过程：CMD52 / CMD53 → F1 backplane 窗口 → F2 SDPCM 帧 → BCDC → 以太网', 21, bold=True, anchor='middle')
    s.text(W / 2, 58, '每一层都把上一层整个塞进自己的“载荷”；主机看到的所有内容最终都是 Arasan 上的一条 CMD52 或 CMD53', 12.5,
           fill=SUB, anchor='middle')

    # ---------------------------------------------------------------- A. commands
    y = 74
    panel(s, y, 300, 'A. 两条 SDIO 命令（参数写进 Arasan ARG1，命令号写 CMDTM）',
          'sdio.c：cmd52_host() / sdio_cmd53()；响应 R5 的 bit 15..8 是状态，(resp & 0xcb00) ≠ 0 视为出错')
    X, BW = 200, 1150
    s.text(40, y + 92, 'CMD52', 15, bold=True, fill=BLUE_S, mono=True)
    s.text(40, y + 110, '单字节读写', 11.5, fill=SUB)
    bit_table(s, X, y + 64, [('R/W', 31, 31, C[3]), ('功能号 FN', 30, 28, C[0]), ('RAW', 27, 27, ZERO),
                             ('0', 26, 26, ZERO), ('寄存器地址（17 位，0..0x1ffff）', 25, 9, C[1]), ('0', 8, 8, ZERO),
                             ('写入的字节 / 读时为 0', 7, 0, C[2])], BW)
    s.text(X, y + 132, '例：sdio_int_pending() = CMD52 读 F0 地址 0x05（CCCR INT_PENDING）→ 参数 0x00000a00；'
           'brcmf_set_backplane_window() = 3 次 CMD52 写 F1 0x1000a/b/c', 11.5)

    s.text(40, y + 186, 'CMD53', 15, bold=True, fill=GREEN_S, mono=True)
    s.text(40, y + 204, '块 / 字节流', 11.5, fill=SUB)
    bit_table(s, X, y + 158, [('R/W', 31, 31, C[3]), ('功能号 FN', 30, 28, C[0]), ('BLK=0', 27, 27, ZERO),
                              ('OP', 26, 26, C[4]), ('寄存器地址（17 位）', 25, 9, C[1]), ('字节数（512 写成 0）', 8, 0, C[2])], BW)
    s.lines(X, y + 228, ['BLK=0：本工程只用 byte 模式、单个数据块，一次最多 512 B（Arasan BLKSIZECNT = len | 1<<16），数据由 CPU 经 EMMC_DATA 逐字 PIO 搬运。',
                         'OP=1 地址自增：F1 访问 backplane 内存/寄存器；OP=0 地址固定：F2 FIFO（brcmf_f2_transfer 每块都写同一个地址）。',
                         '实机 512/256/128 B 出现 data CRC 错 → brcmf_backplane_transfer 把 brcmf_transfer_limit 逐次减半，64 B 稳定后 F1、F2 都按它切块。'],
            11.5, gap=19)

    # ---------------------------------------------------------------- B. F1 window
    y = 388
    panel(s, y, 270, 'B. Function 1：用 17 位 SDIO 地址访问 32 位 backplane 地址（32 KB 滑动窗口）',
          'brcmf_set_backplane_window(addr) + brcmf_backplane_read32/write32/transfer')
    bit_table(s, X, y + 60, [('窗口号：addr & 0xffff8000 → 写 F1 0x1000a(bit15) / 0x1000b(bit23..16) / 0x1000c(bit31..24)', 31, 15, C[0]),
                             ('窗口内偏移 addr & 0x7fff', 14, 0, C[1])], BW, show=(0, 14, 15, 31))
    s.text(40, y + 92, 'backplane', 13, bold=True, fill=BLUE_S)
    s.text(40, y + 108, '32 位地址', 11.5, fill=SUB)
    s.line([(X + BW * 0.766, y + 112), (X + BW * 0.766, y + 136)], YEL_S, 'ay')
    s.rect(X + BW * 0.53, y + 138, BW * 0.47, 30, C[1], YEL_S, 1, 4)
    s.text(X + BW * 0.765, y + 158, 'CMD53 地址 = 偏移 | 0x8000（SBSDIO_SB_ACCESS_2_4B_FLAG）', 11.5, anchor='middle')
    ex = [('读芯片 ID', 'ChipCommon CHIPID 0x18000000', '窗口 HIGH=0x18 MID=0x00 LOW=0x00；CMD53 F1 读 4 B @0x8000', '→ 0x15264345：chip 0x4345 rev 6 = BCM43455'),
          ('下载固件', 'TCM 0x198000 起，BIN 文件', '每块 = min(0x8000 - 偏移, transfer_limit)；', '跨 32 KB 边界就重设窗口'),
          ('读中断状态', 'SDIO core + 0x20 INTSTATUS', 'brcmf_backplane_read32 → 写回同值清除（W1C）', 'mailbox：TOHOSTMAILBOXDATA +0x4c'),
          ('F2 也借用窗口', 'cc_base（ChipCommon 基址）', 'brcmf_f2_transfer 先设窗口 = cc_base，', 'CMD53 FN=2 地址 (cc_base&0x7fff)|0x8000，OP=0')]
    for i, (t, a, b, c) in enumerate(ex):
        x = 40 + i * 334
        s.rect(x, y + 180, 322, 80, GREY, GREY_S, 1, 6)
        s.text(x + 10, y + 198, t, 12.5, bold=True)
        s.text(x + 10, y + 215, a, 11, fill=BLUE_S, mono=True, maxw=304)
        s.text(x + 10, y + 232, b, 11, maxw=304)
        s.text(x + 10, y + 249, c, 11, fill=SUB, maxw=304)

    # ---------------------------------------------------------------- C. SDPCM frame
    y = 672
    panel(s, y, 470, 'C. Function 2：每次 CMD53 读写的都是 SDPCM 帧（12 B 头 + 载荷，总长补齐到 4 的倍数）',
          'TX：brcmf_sdpcm_header(p, len, channel)   RX：brcmf_rx_frame_locked() 解析   channel = byte5 & 0x0f')
    cell = 1150 / 12
    byte_table(s, X, y + 64, [('长度 len', 2, C[0], 'byte 0–1，含头'), ('~len 校验', 2, C[0], 'byte 2–3'),
                              ('seq', 1, C[1], 'byte 4，tx_seq++'), ('channel', 1, C[4], 'byte 5'), ('next len', 1, ZERO, 'byte 6'),
                              ('data offset', 1, C[2], 'byte 7 = 12'), ('flow ctl', 1, ZERO, 'byte 8'),
                              ('tx_max', 1, C[3], 'byte 9 信用'), ('保留', 2, ZERO, 'byte 10–11')], cell, h=40)
    s.text(40, y + 88, 'SDPCM 头', 14, bold=True, fill=PURP_S)
    s.text(40, y + 106, '12 字节', 11.5, fill=SUB)
    s.lines(X, y + 124, ['• len ^ ~len 必须 = 0xffff，否则是坏头；len 0 + check 0 表示“FIFO 里没有帧”。',
                         '• tx_max 只在固件→主机的帧里有意义：主机允许使用的最大序号；主机发送前检查 (uint8)(tx_max − tx_seq) ∈ [1, 0x7f]。'],
            11.5, gap=18)

    # three channels
    cy = y + 172
    ch = [('channel 0 · 控制', 'brcmf_dcmd() / brcmf_iovar()', C[0],
           [('cmd', 4, C[0]), ('len', 4, C[0]), ('flags', 4, C[1]), ('status', 4, C[3]), ('载荷 …', 6, C[5])],
           ['flags = 请求 id<<16 | SET(0x02) ；回复 ERROR(0x01)', 'GET_VAR/SET_VAR(262/263) 载荷 = "iovar 名\\0" + 值',
            '回复靠 id 匹配：中间插进来的事件/数据帧照常分流']),
          ('channel 1 · 事件', 'brcmf_rx_dispatch_locked() 打印', C[4],
           [('以太网头 14', 14, C[2]), ('bcmeth 10', 10, C[5]), ('brcm_event_msg …', 24, C[4])],
           ['以太类型 0x886c；event_type/status/reason 为大端', '例：SET_SSID 结果、LINK、PSK_SUP', '只打印，用来诊断关联失败']),
          ('channel 2 · 数据', 'brcmf_send_ether_locked() / NAPI', C[2],
           [('BCDC 4B', 4, C[1]), ('dst 6', 6, C[2]), ('src 6', 6, C[2]), ('type', 3, C[2]), ('IP/ARP/EAPOL …', 12, C[5])],
           ['BCDC flags = 2<<4（版本 2），data_offset×4 为额外跳过', 'type 0x888e → EAPOL（WPA2 握手，主机自己处理）',
            '其他类型且 handshake_done → 4 槽 net_rxq → net_rx_dev'])]
    for i, (t, fn, col, fields, notes_) in enumerate(ch):
        x = 40 + i * 446
        s.rect(x, cy, 432, 220, '#fff', '#8c959f', 1, 8)
        s.rect(x, cy, 432, 28, col, '#8c959f', 1, 8)
        s.text(x + 10, cy + 19, t, 13, bold=True)
        s.text(x + 422, cy + 19, fn, 10.5, fill=SUB, anchor='end', mono=True, maxw=230)
        s.text(x + 10, cy + 50, '[SDPCM 12B] +', 11, fill=PURP_S, mono=True)
        tot = sum(f[1] for f in fields)
        byte_table(s, x + 10, cy + 58, fields, 412 / tot, h=34, sizes=(i == 0))
        for j, n in enumerate(notes_):
            s.text(x + 10, cy + 120 + j * 18, n, 11.2, maxw=412)
        if i == 0:
            s.text(x + 10, cy + 186, '例：iovar "bus:rxglom"=0（关闭接收聚合）', 11, fill=BLUE_S, maxw=412)
            s.text(x + 10, cy + 204, '例：BRCMF_C_SET_PM(86)=0（连上后关省电）', 11, fill=BLUE_S, maxw=412)
        elif i == 1:
            s.text(x + 10, cy + 186, 'bcmeth = subtype/length/OUI/usr_subtype', 11, fill=SUB, maxw=412)
        else:
            s.text(x + 10, cy + 186, '发送前必须 handshake_done，否则 xmit 直接返回 -1', 11, fill=RED, maxw=412)
            s.text(x + 10, cy + 204, '帧长 >1600 B 的数据帧不进队列', 11, fill=SUB, maxw=412)
    s.text(40, y + 458, 'event / data 帧可能出现在控制回复之前：brcmf_dcmd 发出请求后循环调用 brcmf_rx_dispatch_locked，按 channel 分流，'
           '直到看到 ctl_id == 自己的 id（最长 3 s）。', 11.5, fill=SUB)

    # ---------------------------------------------------------------- D. RX algorithm
    y = 1156
    panel(s, y, 260, 'D. 读一个 F2 帧：brcmf_rx_frame_locked()（先读 64 B 拿到长度，再读剩余）')
    nodes = [
        (36, 'CMD52 读 CCCR', 'INT_PENDING bit1？', '否 → 假中断，返回 0', GREY),
        (203, 'F1 读 INTSTATUS', '& HOSTINTMASK', '写 1 清；HOST_INT 读 mailbox', BLUE),
        (370, 'FRAME_IND 或', 'rx_more？', '都没有 → 返回 0', GREY),
        (537, 'CMD53 F2 读 64 B', '取 len / ~len', '0/0 → FIFO 空', GREEN),
        (704, '校验 len', '', '', YEL),
        (871, '读余下 len-64', '补齐 4 字节', '', GREEN),
        (1038, '解析头', 'ch=b5&0xf  off=b7', 'tx_max = b9（>0x40 超前则夹紧）', PURP),
        (1205, 'rx_more = 1', '返回 1', '下一轮直接读，不等中断位', GREEN),
    ]
    for x, a, b, c, col in nodes:
        s.rect(x, y + 50, 148, 70, col, '#8c959f', 1.2, 6)
        s.text(x + 74, y + 70, a, 11.8, bold=True, anchor='middle', maxw=140)
        s.text(x + 74, y + 87, b, 11, anchor='middle', maxw=140, mono=True)
        s.text(x + 74, y + 104, c, 10.3, fill=SUB, anchor='middle', maxw=140)
    for i in range(len(nodes) - 1):
        x = nodes[i][0] + 148
        s.line([(x, y + 85), (x + 19, y + 85)], '#333', 'a', sw=1.5)
    # branches under 校验
    s.line([(778, y + 120), (778, y + 150)], RED, 'ar', sw=1.4)
    s.rect(620, y + 152, 340, 96, RED_BG, RED, 1, 6)
    s.lines(632, y + 172, ['len^check ≠ 0xffff 或 len < 12：坏头',
                           '  → brcmf_rx_terminate_locked：F1 FRAMECTRL=RF_TERM(0x02)，',
                           '     等 RFRAMEBC 归零，丢掉这一帧',
                           'len > 4096（superframe，多帧聚合）：分块读完丢弃，',
                           '  rx_more=1 递归读下一帧（TCP 会重传；已 iovar bus:rxglom=0）'], 11, gap=16.5)
    s.text(40, y + 152, '为什么先清 INTSTATUS 再读 FIFO：', 12, bold=True)
    s.lines(40, y + 172, ['INTSTATUS 是电平源；不清，重新打开 CARD_INT', '会立刻再进中断，brcmf0_wq 空转。',
                          '清掉后新到的帧会再次置位，所以不会丢。',
                          '同理：一旦读到帧就 rx_more=1 继续读空 FIFO。'], 11.2, gap=17)
    s.text(980, y + 172, '返回值约定：1 = 拿到一帧，0 = 没有帧，-1 = I/O 错', 11.5, fill=SUB)
    s.text(980, y + 192, '调用者：NAPI poll、brcmf_dcmd 等回复、', 11.5, fill=SUB)
    s.text(980, y + 210, '        发送端等信用时（顺便把帧收进 net_rxq）', 11.5, fill=SUB)

    # ---------------------------------------------------------------- E. credit
    y = 1430
    panel(s, y, 166, 'E. SDPCM 发送信用（流控）：brcmf_send_ether_locked() 发送前的等待')
    # sequence number ruler
    rx0, cw2 = 60, 46
    for i in range(14):
        seq = 0x35 + i
        col = GREEN if 0x37 <= seq < 0x3a else (ZERO if seq < 0x37 else '#fff')
        s.rect(rx0 + i * cw2, y + 62, cw2, 30, col, '#8c959f', 1, 0)
        s.text(rx0 + i * cw2 + cw2 / 2, y + 82, f'{seq:02x}', 11.5, anchor='middle', mono=True)
    s.line([(rx0 + 2.5 * cw2, y + 120), (rx0 + 2.5 * cw2, y + 94)], BLUE_S, 'ab', sw=1.5)
    s.text(rx0 + 2.5 * cw2, y + 135, 'tx_seq = 0x37', 11, anchor='middle', fill=BLUE_S, mono=True)
    s.line([(rx0 + 5.5 * cw2, y + 120), (rx0 + 5.5 * cw2, y + 94)], RED, 'ar', sw=1.5)
    s.text(rx0 + 5.5 * cw2, y + 135, 'tx_max = 0x3a', 11, anchor='middle', fill=RED, mono=True)
    s.text(rx0, y + 155, '灰 = 已发；绿 = 还能发 3 帧（window = 0x3a − 0x37 = 3）', 11, fill=SUB)
    s.lines(760, y + 66, ['window = (uint8)(tx_max − tx_seq)；window ≠ 0 且 < 0x80 才发送。',
                          '没信用：调用 brcmf_rx_dispatch_locked() 读固件上行帧（带新的 tx_max），',
                          '   读到的数据帧放进 4 槽 net_rxq 留给 NAPI；没帧就等 100 µs。',
                          '200 次（约 20 ms）仍无信用 → 丢掉这帧，返回 -1，靠 TCP 重传。',
                          '修复前不看 tx_max，突发 TCP 时固件静默丢帧 → 连接卡住。'], 11.5, gap=19)
    s.save(path, 'SDIO Wi-Fi 通信协议分层')
