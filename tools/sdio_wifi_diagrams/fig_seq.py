from svglib import *


def legend(s, y):
    x = 40
    for kind, lab in (('call', '函数调用'), ('ret', '返回 / 回复'), ('hw', 'SDIO 命令（CMD52/53）'), ('data', 'SDPCM 帧（BCDC 控制/数据）'),
                      ('irq', '中断 / 调度'), ('air', '802.11 空口'), ('warn', '定时 / 看门狗')):
        col, mk, dash = Seq.KIND[kind]
        s.line([(x, y), (x + 34, y)], col, mk, dash, 1.6)
        s.text(x + 42, y + 4, lab, 11.5, fill=col if kind != 'call' else INK)
        x += 70 + twidth(lab, 11.5)


def draw(path, title, subtitle, lanes, rows, w=1400, title_svg=''):
    s = Svg(w, 4000)
    s.text(w / 2, 34, title, 21, bold=True, anchor='middle')
    s.text(w / 2, 58, subtitle, 12.5, fill=SUB, anchor='middle')
    legend(s, 80)
    q = Seq(s, lanes, 100)
    q.heads()
    end = q.run(rows)
    s.h = int(end + 10)
    s.save(path, title_svg or title)


# ----------------------------------------------------------------- 5. bring-up
def fig_seq_init(path):
    lanes = [('fs', 'bootfs FAT32', 'microSD / sdhost', ORANGE, ORANGE_S),
             ('main', 'xv6 启动', 'main() CPU0', GREY, GREY_S),
             ('brcm', 'brcmfmac', 'brcmf_probe', PURP, PURP_S),
             ('core', 'SDIO core', 'sdio.c', BLUE, BLUE_S),
             ('host', 'Arasan host', 'arasan_sdio.c', YEL, YEL_S),
             ('chip', 'CYW43455 SDIO', 'F0 / F1 / F2', YEL, YEL_S),
             ('cr4', 'CR4 固件', 'BCM43455.BIN', ORANGE, ORANGE_S)]
    rows = [
        ('sect', '阶段 1  host 初始化与 SDIO 枚举（400 kHz，1-bit）'),
        ('m', 'main', 'host', 'arasan_sdio_driver_init() → platform 匹配 → arasan_probe()', 'call'),
        ('self', 'host', 'gpio_init()：GPIO34–39 ALT3、上拉；CONTROL1.SRST_HC 复位；\nmailbox 取 EMMC 基准时钟 → 400 kHz'),
        ('m', 'host', 'core', 'mmc_add_host() → sdio_scan_host()', 'call'),
        ('m', 'core', 'chip', 'CMD0 复位', 'hw'),
        ('m', 'core', 'chip', 'CMD5(arg 0) 查询 IO OCR', 'hw'),
        ('m', 'chip', 'core', 'R4：OCR、function 数 = 3', 'ret'),
        ('m', 'core', 'chip', 'CMD5(OCR) 反复发，直到 IORDY（bit31）= 1', 'hw'),
        ('m', 'core', 'chip', 'CMD3 取 RCA → CMD7 选中卡', 'hw'),
        ('m', 'core', 'chip', 'CMD52 读 CCCR / FBR / CIS → vendor 0x02d0、device 0xa9a6', 'hw'),
        ('m', 'core', 'chip', 'CMD52 写 CCCR Bus Interface = 4-bit', 'hw'),
        ('m', 'core', 'host', 'set_bus_width(4)；probe 返回后 arasan_set_clock(25 MHz)', 'call'),
        ('m', 'core', 'main', 'device_add(sdio0:1 / :2 / :3)，等待驱动匹配', 'ret'),
        ('sect', '阶段 2  bootfs 可读后，brcmfmac 匹配并识别芯片'),
        ('m', 'main', 'fs', 'rootdev_init() / fat32mount()', 'call'),
        ('m', 'main', 'brcm', 'brcmfmac_driver_init() → sdio_match → brcmf_probe(F1)', 'call'),
        ('m', 'brcm', 'chip', 'CMD52 CCCR IO_ENABLE |= F1，轮询 IO_READY', 'hw'),
        ('m', 'brcm', 'chip', 'CMD52 F1 CHIPCLKCSR = ALP_AVAIL_REQ，等 ALP_AVAIL', 'hw'),
        ('m', 'brcm', 'chip', 'CMD52×3 设窗口 0x18000000 → CMD53 F1 读 CHIPID', 'hw'),
        ('m', 'chip', 'brcm', '0x15264345 → chip 0x4345 rev 6 = BCM43455', 'ret'),
        ('m', 'brcm', 'fs', 'fat32openroot：BCM43455.BIN / .TXT / .CLM', 'call'),
        ('m', 'fs', 'brcm', '文件句柄（流式读，不整块载入内存）', 'ret'),
        ('sect', '阶段 3  经 F1 backplane 下载固件（25 MHz，CMD53 ≤ transfer_limit）'),
        ('m', 'brcm', 'chip', 'brcmf_scan_cores：读 EROM → ChipCommon / SDIO / CR4 / D11 基址', 'hw'),
        ('m', 'brcm', 'chip', 'brcmf_get_raminfo：CR4 bank 寄存器 → TCM base 0x198000、大小', 'hw'),
        ('m', 'brcm', 'cr4', 'brcmf_set_passive：halt CR4，D11 保持复位', 'hw'),
        ('m', 'brcm', 'chip', 'brcmf_write_firmware：BIN 分块写 TCM（每 32 KB 换一次窗口），回读校验 reset vector', 'hw'),
        ('note', 'brcm', 'chip', ['实机：512/256/128 B 的 CMD53 出现 data CRC 错 → 每次减半重试，64 B 稳定',
                                 '日志：brcmfmac: CMD53 retry with max-chunk=…'], 'o'),
        ('m', 'brcm', 'chip', 'brcmf_write_nvram：TXT → NUL 分隔 + 长度/反码 token，写 RAM 尾部', 'hw'),
        ('m', 'brcm', 'cr4', 'brcmf_set_active：reset vector 写到地址 0，释放 CR4', 'hw'),
        ('self', 'cr4', '固件开始运行：初始化 SDIO core、D11、射频'),
        ('sect', '阶段 4  打开 F2，建立 SDPCM / BCDC 控制面'),
        ('m', 'brcm', 'chip', 'CHIPCLKCSR |= FORCE_HT；TOSBMAILBOXDATA = SDPCM 协议版本 4', 'hw'),
        ('m', 'brcm', 'chip', 'CCCR IO_ENABLE |= F2（固件起来后才 ready）', 'hw'),
        ('m', 'brcm', 'chip', 'HOSTINTMASK；WATERMARK = 0x60；DEVICE_CTL；MESBUSYCTRL = 0xd0', 'hw'),
        ('m', 'cr4', 'brcm', 'TOHOSTMAILBOXDATA：FWREADY / DEVREADY（每 1 ms 轮询，最多 3 s）', 'ret'),
        ('m', 'brcm', 'cr4', 'SMB_INT_ACK；BCDC GET_VAR "ver"（SDPCM ch0，CMD53 F2）', 'data'),
        ('m', 'cr4', 'brcm', '回复：固件版本字符串（按 req id 匹配）', 'ret'),
        ('m', 'brcm', 'cr4', 'clmload（1 KB 一块）· GET "cur_etheraddr" · BRCMF_C_UP', 'data'),
        ('m', 'brcm', 'main', 'register_netdev(wlan0)', 'call'),
        ('m', 'brcm', 'chip', 'sdio_claim_irq：CMD52 CCCR INT_ENABLE = master | F1', 'hw'),
        ('m', 'brcm', 'host', 'napi_enable() · arasan_sdio_irq_enable()：IRPT_EN = CARD_INT', 'call'),
        ('note', 'main', 'brcm', ['之后：/init → /bin/wifi 发起连接（见连接时序图）',
                                 '所有上行帧都由 DAT1 中断驱动（见运行时收发时序图）'], 'g'),
    ]
    draw(path, 'SDIO Wi-Fi 时序图 ①：从上电到 wlan0 注册（brcmf_probe）',
         '按代码执行顺序；同一条竖线上越往下越晚。CMD52/53 实际都经过 SDIO core → Arasan host 发出，图中直接画到芯片以省空间',
         lanes, rows)


# ----------------------------------------------------------------- 6. connect
def fig_seq_connect(path):
    lanes = [('wifi', '/bin/wifi', 'user/wifi.c', GREY, GREY_S),
             ('conn', 'brcmfmac_connect', '/bin/wifi 的内核态', PURP, PURP_S),
             ('wq', 'brcmf0_wq', 'NAPI / state_work', RED_BG, RED),
             ('fw', 'CYW43455 固件', 'CR4 + D11', ORANGE, ORANGE_S),
             ('ap', 'AP', 'WPA2-PSK / DHCP', PURP, PURP_S)]
    rows = [
        ('sect', '准备：重置接口、订阅事件、设置安全参数（全部是 brcmf_dcmd / brcmf_iovar 同步往返）'),
        ('m', 'wifi', 'conn', 'wifi_connect(ssid, psk) → sys_wifi_connect', 'call'),
        ('m', 'conn', 'fw', 'DOWN / UP · event_msgs · mpc=0 · bus:rxglom=0 · country', 'data'),
        ('m', 'conn', 'fw', 'SET_INFRA=1 · SET_AUTH=0（open）· SET_WSEC=AES · bsscfg:wpa_auth=WPA2_PSK', 'data'),
        ('self', 'conn', 'wpa_pbkdf2(psk, ssid, 4096 轮) → 32 B PMK（不打印）'),
        ('m', 'conn', 'fw', 'SET_WSEC_PMK', 'data'),
        ('m', 'fw', 'conn', '不支持（firmware 7.45 无内置 supplicant）→ host_eapol = 1', 'ret'),
        ('sect', '加入网络：固件自己扫描 / 认证 / 关联'),
        ('m', 'conn', 'fw', 'SET_SSID(ssid)', 'data'),
        ('m', 'conn', 'wq', 'brcmf_connect_watch_start：state_work 每 5 tick（50 ms），10 s 截止', 'warn'),
        ('m', 'fw', 'ap', 'probe req / 802.11 open auth / assoc req（含 RSN IE）', 'air'),
        ('m', 'ap', 'fw', 'auth resp / assoc resp', 'air'),
        ('m', 'fw', 'wq', 'event 帧（ch1）→ DAT1 → IRQ 62 → NAPI 打印 type/status/reason', 'irq'),
        ('m', 'conn', 'fw', 'GET_BSSID（循环 ≤ 40 次）', 'data'),
        ('m', 'fw', 'conn', '-17 未关联 …… 之后返回 AP 的 BSSID', 'ret'),
        ('m', 'conn', 'fw', 'GET assoc_info / assoc_req_ies → 取出固件实际发出的 RSN IE', 'data'),
        ('sect', 'WPA2 四次握手（固件不做，主机做 supplicant；EAPOL 走 ch2 数据通道）'),
        ('m', 'ap', 'fw', 'EAPOL M1（ANonce）', 'air'),
        ('m', 'fw', 'wq', 'ch2 数据帧，type 0x888e → DAT1 IRQ → NAPI', 'irq'),
        ('self', 'wq', 'brcmf_wpa_rx_locked：生成 SNonce，PTK = PRF(PMK, MAC, Nonce)，算 MIC'),
        ('m', 'wq', 'fw', 'EAPOL M2（RSN IE 必须与关联请求逐字节相同）', 'data'),
        ('m', 'fw', 'ap', 'M2', 'air'),
        ('m', 'ap', 'fw', 'EAPOL M3（加密的 GTK）', 'air'),
        ('m', 'fw', 'wq', 'M3 → NAPI', 'irq'),
        ('self', 'wq', '校验 MIC，AES key unwrap 出 GTK → keys_pending = 1'),
        ('m', 'conn', 'fw', 'brcmf_finish_wpa_handshake：bsscfg:wsec_key 装 PTK（pairwise）、GTK（group）', 'data'),
        ('m', 'conn', 'fw', 'EAPOL M4 → handshake_done = 1（之后才允许普通数据收发）', 'data'),
        ('m', 'fw', 'ap', 'M4', 'air'),
        ('self', 'conn', '500 ms：netdev_poll_one 排空 RX，等 AP 打开受控端口'),
        ('sect', 'IP 配置与收尾'),
        ('m', 'conn', 'ap', 'net_dhcp_dev(wlan0)：Discover / Request（经 xmit → F2 → 空口，各重试 3 次）', 'data'),
        ('m', 'ap', 'conn', 'Offer / Ack → 地址、掩码、网关、DNS；加直连/默认路由与 ARP', 'ret'),
        ('m', 'conn', 'fw', 'BRCMF_C_SET_PM = 0（PM_OFF，关省电）', 'data'),
        ('note', 'conn', 'ap', ['为什么关省电：默认 PM2 下 AP 把发给 Pi 的帧缓存到下一个 beacon，每包约 100 ms',
                               '（Mac ↔ Pi 的 MPI ping-pong 单程实测 121 ms）；关掉后恢复正常'], 'o'),
        ('m', 'conn', 'wq', 'brcmf_connect_watch_stop()：同步取消 state_work', 'warn'),
        ('m', 'conn', 'wifi', '返回 0 → 打印 “wifi: associated”', 'ret'),
    ]
    draw(path, 'SDIO Wi-Fi 时序图 ②：连接 AP、WPA2 四次握手、DHCP',
         'brcmfmac_connect() 跑在 /bin/wifi 进程里，同步等待每个 BCDC 回复；EAPOL 帧由 DAT1 中断 → brcmf0_wq 的 NAPI 收进来',
         lanes, rows)


# ----------------------------------------------------------------- 7. runtime rx/tx
def fig_seq_rxtx(path):
    lanes = [('app', '用户进程 / socket', 'read · write · epoll', GREY, GREY_S),
             ('tcp', 'TCP / IPv4', 'kernel/net/net.c', GREEN, GREEN_S),
             ('ndev', 'net_device', 'wlan0 · netdev_xmit_on', BLUE, BLUE_S),
             ('brcm', 'brcmfmac', 'SDPCM / BCDC', PURP, PURP_S),
             ('wq', 'brcmf0_wq', 'NAPI · budget 16', RED_BG, RED),
             ('host', 'Arasan SDIO', 'CMD52/53 · IRQ 62', YEL, YEL_S),
             ('radio', 'CYW43455 / AP', 'F2 FIFO ⇄ 802.11', ORANGE, ORANGE_S)]
    rows = [
        ('sect', '步骤 7｜运行前提：关联、WPA2 和 DHCP 已建立 wlan0'),
        ('m', 'app', 'brcm', '/bin/wifi → brcmfmac_connect(SSID, PSK)', 'call'),
        ('m', 'brcm', 'radio', 'SET_SSID → 802.11 认证/关联 → WPA2 M1–M4', 'air'),
        ('self', 'brcm', 'PTK / GTK 安装完成；handshake_done = 1；普通 ch2 数据口开放'),
        ('m', 'brcm', 'tcp', 'net_dhcp_dev(wlan0) → IP / 掩码 / 网关 / DNS / ARP / 路由', 'data'),
        ('m', 'brcm', 'radio', 'BRCMF_C_SET_PM = 0：关闭 PM2，避免 AP 缓存下行帧', 'data'),
        ('note', 'app', 'radio', ['此后 TCP 栈处理的是以太网帧；CYW43455 固件在下方负责 802.11、CCMP、重试和速率控制。',
                                  '接收和发送都使用 SDIO Function 2（F2 FIFO）；Function 1 用于寄存器 / INTSTATUS。'], 'g'),

        ('sect', '步骤 8｜硬件接收：AP 帧进入 F2 FIFO，DAT1 产生 IRQ 62'),
        ('m', 'radio', 'host', 'AP TCP 段 → 802.11/CCMP 解密 → 以太网帧 → SDPCM ch2 进 F2 FIFO', 'air'),
        ('m', 'radio', 'host', 'INTSTATUS.FRAME_IND 置位，DAT1 拉低 → CARD_INT → IRQ 62', 'irq'),
        ('self', 'host', 'arasan_sdio_irq()：屏蔽 IRPT_EN.CARD_INT，写 1 清 host 中断（顶半部不发 CMD）', 'irq'),
        ('m', 'host', 'brcm', 'brcmfmac_sdio_irq()', 'irq'),
        ('m', 'brcm', 'wq', 'napi_schedule() → queue_work(brcmf0_wq)', 'irq'),
        ('note', 'wq', 'host', ['中断只把收包工作移交给可睡眠 worker；IRQ 上下文不获取 bus->lock，也不做 CMD52/53。'], 'o'),

        ('sect', '步骤 9｜NAPI 拆包：SDPCM/BCDC → Ethernet → IPv4 → TCP'),
        ('self', 'wq', 'brcmf_napi_poll(budget = 16) 获取 bus->lock；一次批量排空多帧'),
        ('m', 'wq', 'host', 'CMD52 读 CCCR INT_PENDING；CMD53 F1 读/清 INTSTATUS', 'hw'),
        ('m', 'wq', 'host', 'CMD53 F2 先读 64 B，再读 len - 64（按 transfer_limit 切块）', 'hw'),
        ('m', 'host', 'wq', 'SDPCM：len / channel=2 / tx_max；BCDC + Ethernet frame', 'ret'),
        ('self', 'wq', '更新 tx_max；去 SDPCM/BCDC 头；以太网帧入 net_rxq[4]；release(bus->lock)'),
        ('m', 'wq', 'ndev', 'net_rx_dev(wlan0, frame, len)', 'data'),
        ('m', 'ndev', 'tcp', 'EtherType 0x0800 → IPv4 校验 / proto=6 → tcp_rx()', 'data'),
        ('self', 'tcp', '校验 TCP checksum；按四元组查 tcp_conn；状态机 / ACK / 窗口 / 乱序缓存≤6段'),
        ('m', 'tcp', 'app', '数据入 socket RX buffer → wakeup() + tcp_epollnotify() → read/epoll 可见', 'ret'),
        ('note', 'tcp', 'wq', ['对新数据、重复段、乱序段和 FIN，tcp_rx() 会生成当前 rcv_nxt 的 ACK，立即进入下面步骤 10。',
                               'rx_more=1 时 worker 不等新中断，继续读 F2，直到 FIFO 空或用完 budget。'], 'b'),

        ('sect', '步骤 10｜TCP 发送：用户数据或 RX 路径产生的 ACK 汇合到 wlan0'),
        ('m', 'app', 'tcp', 'write(fd) → socket f_op->write → net_tcp_write()（入 txq）', 'call'),
        ('self', 'tcp', 'tcp_start_queued_locked / tcp_send_at_locked：组 TCP + IPv4 + Ethernet；设 seq/ack/window/checksum'),
        ('m', 'tcp', 'ndev', 'netdev_xmit_on(wlan0, frame) → ops->start_xmit', 'call'),
        ('m', 'ndev', 'brcm', 'brcmfmac_xmit：handshake_done 检查 → acquire(bus->lock)', 'call'),
        ('self', 'brcm', '加 BCDC 头（flags=0x20）+ SDPCM 头（seq=tx_seq++，ch=2），4 B 对齐'),
        ('note', 'brcm', 'wq', ['SDIO 信用：window = (uint8)(tx_max - tx_seq)；0 或 ≥ 0x80 表示暂无可发配额。',
                                '无信用时主动读取一个 F2 上行帧以获取新 tx_max；最多等约 20 ms，超时丢帧由 TCP 重传。'], 'y'),
        ('m', 'brcm', 'host', 'CMD52×3 设 backplane 窗口 → CMD53 F2 分块写入', 'hw'),
        ('m', 'host', 'radio', '固件取帧 → 802.11 封装 / CCMP / MAC 重试 → AP → 对端', 'air'),
        ('self', 'brcm', 'release(bus->lock) → start_xmit 返回；TCP 保留未确认数据供超时重传'),

        ('sect', '步骤 11｜排空与重新开中断：在吞吐与 IRQ 风暴之间收敛'),
        ('self', 'wq', 'FIFO 已空且 work_done < 16 → napi_complete_done()'),
        ('m', 'wq', 'host', 'arasan_sdio_irq_complete() → 重开 IRPT_EN.CARD_INT', 'irq'),
        ('self', 'wq', '若 budget=16 已用完：保持 NAPI scheduled，继续 worker 轮询，不先重开中断'),
        ('note', 'wq', 'host', ['假中断：INT_PENDING 没有 F1 pending 时，queue_delayed_work(irq_work, 1)，10 ms 后再重开。',
                                  '这避免 CARD_INT 假触发把 CPU 打满；真实帧最多增加 1 tick 延迟。'], 'o'),
        ('m', 'radio', 'host', '下一个 TCP ACK / 数据帧 到达 → 再次 DAT1 / IRQ 62，回到步骤 8', 'air'),
        ('note', 'tcp', 'host', ['上下文与串行化：IRQ 顶半部只屏蔽+调度；brcmf0_wq 可睡眠；发送者、NAPI、dcmd 用 bus->lock 串行 F1/F2；',
                                  '每条 SDIO 请求再经 mmc_request_sync / host->request_lock 串行到 Arasan 控制器。'], 'p'),
    ]
    draw(path, 'SDIO Wi-Fi TCP/IP 完整收发：从 AP 到 socket，再回到空口',
         '展开详细步骤图的 7–11：建链前提 → DAT1 中断 → NAPI 解封装 → TCP 状态机 → 信用流控发送 → 重开中断',
         lanes, rows, w=1680)
