from svglib import *


# ======================================================================= 1. architecture
def fig_arch(path):
    W, H = 1400, 1010
    s = Svg(W, H)
    s.text(W / 2, 34, 'Raspberry Pi 3B+ 板载 SDIO Wi-Fi 软件 / 硬件分层架构（xv6-aarch64）', 21, bold=True, anchor='middle')
    s.text(W / 2, 58, '左：ARM Cortex-A53 上的 xv6 主机侧代码（自上而下每层只调用下一层的接口）    右：CYW43455（BCM4345/6）芯片内部，'
           '运行下载进去的 FullMAC 固件', 12.5, fill=SUB, anchor='middle')

    # ---------------- left: host stack
    LX, LW = 30, 800
    s.rect(LX - 10, 74, LW + 20, 900, '#fbfcfd', '#d0d7de', 1, 10, dash='6 4')
    s.text(LX, 96, 'BCM2837 · Cortex-A53 × 4 · xv6 内核（EL1）+ 用户态（EL0）', 14, bold=True, fill=BLUE_S)

    def layer(y, h, title, path_, lines_, fill, stroke):
        s.rect(LX, y, LW, h, fill, stroke, 1.5, 8)
        s.text(LX + 12, y + 21, title, 14.5, bold=True)
        s.text(LX + LW - 12, y + 21, path_, 11.5, fill=stroke, anchor='end', mono=True)
        for i, l in enumerate(lines_):
            s.text(LX + 14, y + 41 + i * 17, l, 11.8, fill=SUB)

    layer(108, 66, '① 用户态', 'user/wifi.c  user/init.c',
          ['/init → exec /bin/wifi：读 /etc/wifi.conf（SSID + 口令）→ wifi_connect(ssid, psk)；ping / ssh / mpi 走 socket'],
          GREY, GREY_S)
    layer(190, 66, '② 系统调用', 'kernel/net/sysnet.c',
          ['sys_wifi_connect → brcmfmac_connect()；socket/send/recv → 协议栈'], GREY, GREY_S)
    layer(272, 100, '③ 网络协议栈', 'kernel/net/net.c',
          ['TCP（窗口更新、SYN 带 MSS、最多 6 段乱序缓存）/ UDP / ICMP / DHCP / ARP / IPv4 路由',
           'struct net_device wlan0：ops->start_xmit = brcmfmac_xmit；收包入口 net_rx_dev()',
           'NAPI：napi_schedule() → 设备私有 workqueue brcmf0_wq 里跑 poll()'],
          GREEN, GREEN_S)

    # brcmfmac big box with 4 sub boxes
    by = 388
    s.rect(LX, by, LW, 286, PURP, PURP_S, 1.5, 8)
    s.text(LX + 12, by + 21, '④ brcmfmac FullMAC 主机驱动', 14.5, bold=True)
    s.text(LX + LW - 12, by + 21, 'kernel/drivers/net/wireless/brcmfmac.c', 11.5, fill=PURP_S, anchor='end', mono=True)
    sub = [
        (LX + 12, by + 34, 380, 108, '控制面 BCDC', ['brcmf_dcmd(cmd, buf, set)：req id++，', 'SDPCM 通道 0，轮询等同 id 的回复（≤3 s）',
                                                  'brcmf_iovar("ver"/"clmload"/"bus:rxglom")', 'brcmfmac_connect()：SET_SSID / WPA2 / DHCP']),
        (LX + 404, by + 34, 384, 108, '数据面', ['TX：brcmfmac_xmit → brcmf_send_ether_locked', '   等 tx_max 信用 → BCDC 数据头 → CMD53 写 F2',
                                            'RX：brcmf_napi_poll（budget 16）', '   → brcmf_rx_dispatch_locked → 4 帧 net_rxq']),
        (LX + 12, by + 150, 380, 124, 'SDPCM 帧层（F2）', ['brcmf_sdpcm_header：len/~len/seq/channel', 'brcmf_rx_frame_locked：先读 64 B 拿长度，',
                                                     '  再读余下；记录 byte9 = tx_max 信用', '  >4 KB superframe 读完丢弃；坏头 → RF_TERM',
                                                     'brcmf_f2_transfer：按 transfer_limit 切块']),
        (LX + 404, by + 150, 384, 124, 'Backplane 层（F1）+ 固件装载', ['brcmf_set_backplane_window：32 KB 窗口',
                                                                  'brcmf_backplane_read32/write32/transfer', 'brcmf_scan_cores（EROM）→ get_raminfo',
                                                                  'set_passive → write_firmware → write_nvram', '→ set_active → brcmf_start_f2（等 FWREADY）']),
    ]
    for x, y, w, h, t, ls in sub:
        s.rect(x, y, w, h, '#fff', PURP_S, 1, 6)
        s.text(x + 10, y + 18, t, 12.5, bold=True, fill=PURP_S)
        for i, l in enumerate(ls):
            s.text(x + 10, y + 36 + i * 17, l, 11.3, fill=INK, maxw=w - 16)

    layer(690, 100, '⑤ MMC / SDIO core', 'kernel/drivers/mmc/core/sdio.c',
          ['sdio_cmd52(func, rw, addr, &byte)  ·  sdio_cmd53(func, rw, addr, incr, buf, ≤512 B，单块 byte 模式)',
           'sdio_enable_func / sdio_claim_irq / sdio_int_pending（CCCR 0x05）· sdio_scan_host：CMD0/5/3/7',
           'mmc_request_sync()：💤 host->request_lock 睡眠锁串行化整次命令+数据'],
          BLUE, BLUE_S)
    layer(806, 100, '⑥ Arasan SDHCI host 驱动', 'kernel/drivers/mmc/host/arasan_sdio.c',
          ['arasan_request()：写 ARG1/CMDTM → 等 CMD_DONE → PIO 逐字读写 EMMC_DATA → 等 DATA_DONE',
           'arasan_set_clock：400 kHz 枚举 → 25 MHz；set_bus_width：4-bit；gpio_init：GPIO34–39 ALT3',
           'arasan_sdio_irq()（IRQ 62 顶半部）/ arasan_sdio_irq_complete()（重新打开 CARD_INT）'],
          YEL, YEL_S)
    s.rect(LX, 922, LW, 40, '#eaeef2', GREY_S, 1.5, 8)
    s.text(LX + 12, 947, '硬件：Arasan EMMC 控制器 @0x3f300000 → GPIO34 CLK · 35 CMD · 36–39 DAT0–3 → 板上走线',
           13, bold=True)

    # arrows between left layers
    for y0, y1, lab in ((174, 190, ''), (256, 272, ''), (372, 388, 'start_xmit / net_rx_dev'),
                        (674, 690, 'sdio_cmd52 / sdio_cmd53'), (790, 806, 'host->ops->request'),
                        (906, 922, '')):
        s.line([(LX + 300, y0), (LX + 300, y1)], '#333', 'a', sw=1.6, start_marker='a')
        if lab:
            s.text(LX + 312, (y0 + y1) / 2 + 4, lab, 11, fill=SUB, mono=True)

    # ---------------- SDIO bus link
    s.rect(838, 900, 104, 84, '#fff', YEL_S, 1.4, 6)
    s.text(890, 918, 'SDIO 4-bit 总线', 11.5, bold=True, anchor='middle')
    s.text(890, 934, 'CLK / CMD', 10.5, fill=SUB, anchor='middle', mono=True)
    s.text(890, 948, 'DAT0–3', 10.5, fill=SUB, anchor='middle', mono=True)
    s.text(890, 962, 'DAT1 兼作中断', 10.5, fill=RED, anchor='middle')
    s.text(890, 976, '25 MHz', 10.5, fill=SUB, anchor='middle', mono=True)
    s.line([(830, 942), (838, 942)], YEL_S, None, sw=6)
    s.line([(942, 942), (970, 942)], YEL_S, None, sw=6)

    # ---------------- right: chip
    RX, RW = 960, 420
    s.rect(RX, 74, RW, 900, '#fffdf7', ORANGE_S, 2, 12)
    s.text(RX + 14, 98, 'CYW43455 Wi-Fi/BT 芯片（板载模块）', 15, bold=True, fill=ORANGE_S)
    s.text(RX + 14, 116, 'Pi 3B 为 BCM43430；SDIO 厂商 0x02d0，设备 0xa9a6/0xa9bf', 11, fill=SUB)

    # antenna + AP
    s.box(RX + 20, 130, 380, 58, '天线 ⇄ 802.11 AP（如 TP-Link，WPA2-PSK）',
          ['2.4/5 GHz 空口：beacon、auth/assoc、EAPOL、数据帧'], PURP, PURP_S)
    s.box(RX + 20, 206, 380, 70, 'D11 MAC + PHY/射频', ['802.11 帧收发、CCMP 硬件加解密（主机装 PTK/GTK）',
                                                   '省电模式 PM（连上后主机 SET_PM=0 关闭）'], GREY, GREY_S)
    s.box(RX + 20, 294, 380, 92, 'ARM Cortex-R4（CR4）跑 FullMAC 固件', ['BCM43455.BIN：扫描 / 认证 / 关联 / 漫游在芯片内完成',
                                                                   '通过 SDPCM 与主机交换 BCDC 命令、事件、以太网帧',
                                                                   '固件给出 TX 信用 tx_max（每个上行帧的 byte 9）'], ORANGE, ORANGE_S)
    s.box(RX + 20, 404, 380, 76, 'TCM RAM（base 0x198000）', ['主机经 F1 写入 BIN 代码 + NVRAM（RAM 尾部 + token）',
                                                           'reset vector 写到地址 0，释放 CR4 后开始执行'], ORANGE, ORANGE_S)
    s.box(RX + 20, 498, 380, 58, 'ChipCommon core（0x18000000）', ['CHIPID 寄存器 = 0x15264345 → chip 4345 rev 6',
                                                                     ], GREY, GREY_S)
    s.box(RX + 20, 574, 380, 112, 'SDIO device core（backplane 上的寄存器）', ['INTSTATUS +0x20（写 1 清）：FRAME_IND bit6 = F2 有帧',
                                                                           '        HOST_INT bit7 = mailbox 消息',
                                                                           'HOSTINTMASK +0x24 · TOSBMAILBOX +0x40 / DATA +0x48',
                                                                           'TOHOSTMAILBOXDATA +0x4c：FWREADY / DEVREADY / FWHALT',
                                                                           ], BLUE, BLUE_S, mono_lines=False)
    # AXI backplane bar
    s.rect(RX + 404, 206, 10, 720, '#d0d7de', GREY_S, 1, 3)
    s.add(f'<text x="{RX + 412}" y="300" font-size="11" fill="{SUB}" transform="rotate(90 {RX + 412} 300)">AXI backplane（32 位地址空间）</text>')
    for yy in (241, 340, 442, 527, 630):
        s.line([(RX + 400, yy), (RX + 404, yy)], GREY_S, None, sw=2)

    # SDIO function block
    s.rect(RX + 20, 708, 380, 250, '#fff', YEL_S, 1.6, 8)
    s.text(RX + 32, 730, 'SDIO 设备接口（3 个 Function）', 13.5, bold=True, fill=YEL_S)
    funcs = [
        ('F0  CCCR / FBR / CIS', ['IO_ENABLE、IO_READY、INT_ENABLE、INT_PENDING(0x05)', '总线宽度 4-bit；CMD52 访问'], GREY, GREY_S),
        ('F1  Backplane 窗口（控制）', ['0x1000a/b/c 选 32 KB 窗口，0x8000|偏移 读写', '0x1000e CHIPCLKCSR（ALP/HT 时钟）、0x1000d FRAMECTRL'], BLUE, BLUE_S),
        ('F2  SDPCM 帧 FIFO（数据）', ['固件就绪后才能 enable；CMD53 地址固定不自增', '上下行都是 SDPCM 帧：控制 / 事件 / 数据'], GREEN, GREEN_S),
    ]
    for i, (t, ls, f, st) in enumerate(funcs):
        y = 742 + i * 70
        s.rect(RX + 32, y, 356, 62, f, st, 1.2, 6)
        s.text(RX + 42, y + 18, t, 12.5, bold=True)
        for j, l in enumerate(ls):
            s.text(RX + 42, y + 35 + j * 15, l, 10.8, fill=SUB, maxw=336)
    s.line([(RX + 388, 843), (RX + 403, 843)], BLUE_S, 'ab', sw=2)
    s.line([(RX + 388, 913), (RX + 403, 913)], GREEN_S, 'ag', sw=2)
    # bootfs side note (firmware files)
    s.save(path, 'SDIO Wi-Fi 架构')


# ======================================================================= 2. hardware connection
def fig_hw(path):
    W, H = 1400, 1060
    s = Svg(W, H)
    s.text(W / 2, 34, 'SDIO Wi-Fi 硬件连接：BCM2837 Arasan EMMC ⇄ GPIO34–39 ⇄ CYW43455，以及中断与时钟', 21, bold=True, anchor='middle')
    s.text(W / 2, 58, '外置 SD 卡（启动盘）和板载 Wi-Fi 是两个不同的控制器、两组不同的 GPIO；Wi-Fi 固件文件却放在 SD 卡的 bootfs 上',
           12.5, fill=SUB, anchor='middle')

    # SoC frame
    s.rect(30, 80, 720, 640, '#fbfcfd', BLUE_S, 2, 12)
    s.text(46, 104, 'BCM2837 SoC（外设物理基址 0x3f000000）', 15, bold=True, fill=BLUE_S)

    s.box(50, 118, 300, 92, 'Cortex-A53 × 4（xv6）', ['devintr()：gic_iar() 读到 irq 62', '→ arasan_sdio_irq() 顶半部',
                                                      'brcmf0_wq kworker 做 CMD52/53'], BLUE, BLUE_S)
    s.box(50, 226, 300, 92, '中断控制器', ['SDIO_IRQ = 62（memlayout.h）', 'BCM2837 pending-2 bit 30',
                                       '电平触发：源不清，IRQ 不停'], RED_BG, RED)
    s.box(50, 334, 300, 78, 'VideoCore 邮箱 @0x3f00b880', ['属性通道 8：GET_CLOCK_RATE(EMMC)', '→ Arasan 基准时钟（mailbox_clock）'],
          GREY, GREY_S)

    # Arasan registers
    s.rect(380, 118, 350, 330, YEL, YEL_S, 1.6, 8)
    s.text(392, 140, 'Arasan EMMC / SDHCI @0x3f300000', 14, bold=True)
    regs = [('0x04 BLKSIZECNT', '块大小（≤512）| 块数 1'), ('0x08 ARG1', 'CMD52/CMD53 32 位参数'),
            ('0x0c CMDTM', '命令号<<24、响应类型、ISDATA、读方向'), ('0x10 RESP0', 'R5 响应：标志位 + 读到的字节'),
            ('0x20 DATA', 'PIO 数据口，32 位一次'), ('0x24 STATUS', 'CMD/DAT_INHIBIT、READ/WRITE_AVAIL'),
            ('0x28 CONTROL0', 'bit1 = 4-bit 总线'), ('0x2c CONTROL1', '时钟分频/使能、SRST_CMD/DATA 复位'),
            ('0x30 INTERRUPT', 'CMD_DONE、DATA_DONE、bit8 CARD_INT、错误'),
            ('0x38 IRPT_EN', '只开 CARD_INT 才会产生 IRQ 62')]
    for i, (r, d) in enumerate(regs):
        y = 162 + i * 28
        s.text(394, y, r, 11.5, mono=True, bold=True, fill=YEL_S)
        s.text(520, y, d, 11.2, fill=INK, maxw=200)
    s.text(394, 162 + 10 * 28 + 2, 'arasan_request()：无 DMA，CPU 逐字搬运', 11.2, fill=RED)

    # GPIO block
    s.rect(380, 466, 350, 236, GREEN, GREEN_S, 1.6, 8)
    s.text(392, 488, 'GPIO @0x3f200000（gpio_init）', 14, bold=True)
    s.text(392, 508, 'GPFSEL3/4：GPIO34–39 设成 ALT3（=SD1，Arasan）', 11.3)
    s.text(392, 526, '上拉：GPPUD=2 → GPPUDCLK1 写 0xf8（GPIO35–39）', 11.3)
    s.text(392, 544, 'GPIO34（CLK）不加上下拉', 11.3)
    s.rect(392, 556, 326, 134, '#fff', RED, 1, 6)
    s.text(402, 576, '实机踩坑（已修）', 12, bold=True, fill=RED)
    s.lines(402, 595, ['原来误写 GPPUDCLK0，把 GPIO3–7 拉高，DAT1 浮空；',
                       '每次打开 CARD_INT 都立刻“来中断”，约 5 万次/秒，',
                       'brcmf0_wq 占满一个 CPU。改为 GPPUDCLK1 后正常。',
                       '另：仍会偶发“假中断”（CCCR INT_PENDING=0），',
                       'NAPI 遇到时把 IRQ 关 1 tick（10 ms）再开。'], 11, gap=17, fill=INK)

    # SDHOST + SD card
    s.box(50, 430, 300, 92, 'BCM SDHOST @0x3f202000', ['GPIO48–53；sdhost.c', '外置 microSD：FAT32 bootfs + fs.img',
                                                       '和 Wi-Fi 不是同一个控制器'], GREY, GREY_S)
    s.box(50, 540, 300, 162, 'microSD 卡 bootfs（FAT32）', ['kernel8-xv6_wifi.img、config.txt',
                                                             'BCM43455.BIN  芯片 CR4 固件代码',
                                                             'BCM43455.TXT  NVRAM 板级参数',
                                                             'BCM43455.CLM  法规/信道/功率库',
                                                             '（Pi 3B 用 BCM43430.*）',
                                                             'brcmf_probe 用 fat32openroot 读'], ORANGE, ORANGE_S)
    s.line([(200, 522), (200, 540)], GREY_S, 'as', sw=1.6)

    # internal arrows in SoC
    s.line([(200, 210), (200, 226)], RED, 'ar', sw=1.6, start_marker=None)
    s.line([(380, 300), (350, 300)], RED, 'ar', sw=1.8)
    s.text(352, 292, '', 10)
    s.line([(350, 372), (380, 372)], GREY_S, 'as', sw=1.6)
    s.line([(555, 448), (555, 466)], '#333', 'a', sw=1.6, start_marker='a')

    # chip module
    CX = 1040
    s.rect(CX, 80, 330, 640, '#fffdf7', ORANGE_S, 2, 12)
    s.text(CX + 14, 104, 'CYW43455 模块（Pi 3B+）', 15, bold=True, fill=ORANGE_S)
    s.text(CX + 14, 122, '金属屏蔽罩下；Pi 3B 为 BCM43430', 11, fill=SUB)
    s.box(CX + 16, 140, 298, 108, 'SDIO 从设备接口', ['F0 CCCR、F1 backplane、F2 FIFO', '电压协商 CMD5（IO OCR）',
                                                  'RCA 由 CMD3 分配，CMD7 选中', 'DAT1 低电平 = 中断请求（4-bit 模式下在', '数据块之间的“中断周期”采样）'],
          YEL, YEL_S, lsize=11)
    s.box(CX + 16, 266, 298, 92, 'SDIO device core 中断逻辑', ['INTSTATUS & HOSTINTMASK ≠ 0', '→ 拉低 DAT1，直到主机写 1 清除',
                                                            '（所以必须先清再 drain F2）'], RED_BG, RED, lsize=11)
    s.box(CX + 16, 376, 298, 72, 'WL_REG_ON / 32 kHz LPO', ['芯片上电与低频时钟由板级电路提供；', '当前 xv6 代码不操作 WL_REG_ON'], GREY, GREY_S, lsize=11)
    s.box(CX + 16, 466, 298, 80, 'BT 部分（本驱动不用）', ['蓝牙走 UART（GPIO30–33），', '与 SDIO Wi-Fi 无关'], GREY, GREY_S, lsize=11, dash='5 4')
    s.box(CX + 16, 564, 298, 140, 'CR4 + TCM + D11 + 射频', ['见架构图：固件在芯片里跑 802.11', '天线（PCB 陶瓷天线）', '',
                                                         '主机只负责：下载固件、交换 SDPCM 帧'], ORANGE, ORANGE_S, lsize=11)

    # six wires
    pins = [('GPIO34', 'SD1_CLK', '时钟 400 kHz → 25 MHz', '无上下拉', '#333'),
            ('GPIO35', 'SD1_CMD', 'CMD0/5/3/7/52/53 + 48 位响应', '上拉', '#333'),
            ('GPIO36', 'SD1_DAT0', '数据 bit0（忙信号）', '上拉', GREEN_S),
            ('GPIO37', 'SD1_DAT1', '数据 bit1 + 卡中断', '上拉', RED),
            ('GPIO38', 'SD1_DAT2', '数据 bit2', '上拉', GREEN_S),
            ('GPIO39', 'SD1_DAT3', '数据 bit3', '上拉', GREEN_S)]
    for i, (g, sig, use, pull, col) in enumerate(pins):
        y = 168 + i * 40
        s.rect(730, y - 9, 12, 18, '#fff', GREEN_S, 1, 2)
        s.line([(742, y), (1040, y)], col, None, sw=2.6 if col == RED else 1.8)
        s.rect(1032, y - 9, 12, 18, '#fff', YEL_S, 1, 2)
        s.text(752, y - 5, f'{g} · {sig}', 11.5, bold=True, mono=True, fill=col if col != '#333' else INK)
        s.text(752, y + 15, f'{use}（{pull}）', 10.8, fill=SUB, maxw=280)
    s.text(890, 140, '板上走线（4-bit SDIO）', 12.5, bold=True, anchor='middle')
    # DAT1 IRQ return path
    s.line([(1056, 330), (1048, 330)], RED, None, sw=1.6, dash='6 4')
    s.line([(1048, 330), (1048, 424), (744, 424)], RED, 'ar', sw=1.6, dash='6 4')
    s.text(752, 442, 'DAT1 低 → INTERRUPT.CARD_INT(bit8) → IRQ 62', 11, fill=RED, bold=True)

    # ---------------- bottom: clock / bring-up table and IRQ chain
    y0 = 742
    s.rect(30, y0, 660, 300, GREY, GREY_S, 1.2, 10)
    s.text(46, y0 + 24, '上电后的总线参数变化（arasan_probe → mmc_add_host → sdio_scan_host）', 13.5, bold=True)
    rows = [('阶段', '时钟', '位宽', '说明'),
            ('host 复位', '—', '—', 'CONTROL1.SRST_HC；IRPT_EN=0；数据超时 0xe'),
            ('枚举', '400 kHz', '1-bit', 'CMD0 → CMD5 循环到 IORDY → CMD3 → CMD7'),
            ('读 CCCR/FBR/CIS', '400 kHz', '1→4-bit', 'CMD52 读厂商/设备 ID，写 Bus Interface 选 4-bit'),
            ('运行', '25 MHz', '4-bit', 'arasan_set_clock(SDIO_DATA_CLOCK)；理论 12.5 MB/s'),
            ('固件下载', '25 MHz', '4-bit', 'CMD53 单块 ≤512 B；CRC 错就把块减半（实测 64 B 稳）'),
            ('F2 收发', '25 MHz', '4-bit', '同一 transfer_limit 切块；FIFO 地址不自增')]
    cw = [130, 80, 80, 350]
    for r, row in enumerate(rows):
        y = y0 + 52 + r * 34
        x = 46
        if r == 0:
            s.rect(40, y - 18, 640, 26, '#eaeef2', '#d0d7de', 1, 3)
        for c, cell in enumerate(row):
            s.text(x, y, cell, 11.5, bold=(r == 0 or c == 0), maxw=cw[c] - 8, mono=(c in (1, 2) and r > 0))
            x += cw[c]

    s.rect(710, y0, 660, 300, RED_BG, RED, 1.2, 10)
    s.text(726, y0 + 24, '一次“有帧到达”的中断链（硬件 → 软件）', 13.5, bold=True, fill=RED)
    chain = ['① 固件把帧放进 F2 FIFO，置 INTSTATUS.FRAME_IND（bit6）',
             '② SDIO device core：INTSTATUS & HOSTINTMASK ≠ 0 → 拉低 DAT1',
             '③ Arasan 在中断周期采样 DAT1 → INTERRUPT.CARD_INT = 1',
             '④ IRPT_EN.CARD_INT = 1 → BCM2837 IRQ 62 → CPU 进 el1 IRQ 向量',
             '⑤ devintr() → arasan_sdio_irq()：IRPT_EN=0 屏蔽、写 1 清 CARD_INT、',
             '     pending=1 → brcmfmac_sdio_irq() → napi_schedule(&bus->napi)',
             '⑥ brcmf0_wq 里 brcmf_napi_poll：CMD52 读 CCCR INT_PENDING，',
             '     CMD53 读并写 1 清 INTSTATUS，再 CMD53 读 F2 直到空',
             '⑦ 清掉后 DAT1 释放；napi_complete_done → arasan_sdio_irq_complete()',
             '     重新打开 CARD_INT（假中断则延迟 1 tick 再开）']
    s.lines(726, y0 + 52, chain, 11.8, gap=24)
    s.save(path, 'SDIO Wi-Fi 硬件连接')
