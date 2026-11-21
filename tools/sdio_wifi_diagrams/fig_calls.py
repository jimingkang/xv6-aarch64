from svglib import *

INIT = [
    (0, 'main()', '启动 CPU0，此时还没有进程', 'k'),
    (1, 'sdio_bus_init()', '注册 sdio_bus（vendor/device/function 匹配）', 'c'),
    (1, 'arasan_sdio_driver_init()', 'platform device + driver', 'h'),
    (2, 'arasan_probe(dev)', '', 'h'),
    (3, 'gpio_init()', 'GPIO34–39 ALT3，35–39 上拉', 'h'),
    (3, 'mailbox_clock() → arasan_set_clock(400 kHz)', '', 'h'),
    (3, 'mmc_add_host(&arasan.mmc)', '', 'c'),
    (4, 'sdio_scan_host(host)', 'CMD0 → CMD5×N → CMD3 → CMD7', 'c'),
    (5, 'cmd52_host() / read_cis_id()', 'CCCR、FBR、CIS：0x02d0/0xa9a6', 'c'),
    (5, 'set_bus_width(4) · device_add(sdio0:1/2/3)', '', 'c'),
    (3, 'arasan_set_clock(25 MHz)', '', 'h'),
    (1, 'rootdev_init() / fat32mount()', 'bootfs 可读（固件在上面）', 'k'),
    (1, 'brcmfmac_driver_init() → sdio_register_driver()', '', 'b'),
    (2, 'sdio_match() → sdio_probe() → brcmf_probe(F1)', '', 'b'),
    (3, 'sdio_enable_func(F1) · brcmf_clock_prepare()', 'CCCR IO_ENABLE；ALP 时钟', 'b'),
    (3, 'brcmf_backplane_read32(CHIPID)', '0x4345 rev 6 → BCM43455', 'b'),
    (3, 'brcmf_load_firmware(BIN/TXT) · fat32openroot(CLM)', '', 'b'),
    (3, 'brcmf_scan_cores() → brcmf_erom_desc/regaddr', 'EROM：CC/SDIO/CR4/D11', 'b'),
    (3, 'brcmf_get_raminfo()', 'TCM base 0x198000', 'b'),
    (3, 'brcmf_set_passive() → core_disable/core_reset', 'halt CR4', 'b'),
    (3, 'brcmf_write_firmware() → brcmf_backplane_transfer()', '32 KB 窗口切块', 'b'),
    (3, 'brcmf_prepare_nvram() → brcmf_write_nvram()', 'RAM 尾部 + token', 'b'),
    (3, 'brcmf_set_active(reset_vector)', '释放 CR4', 'b'),
    (3, 'brcmf_start_f2()', 'HT 时钟，协议 4，F2，等 FWREADY', 'b'),
    (3, 'brcmf_alloc_bus()', 'brcmf0_wq · napi(16) · state/irq_work', 'b'),
    (3, 'brcmf_control_plane_start()', '', 'b'),
    (4, 'brcmf_iovar("ver") · brcmf_download_clm()', 'BCDC 首次往返', 'b'),
    (4, 'brcmf_iovar("cur_etheraddr") · brcmf_dcmd(UP)', 'MAC', 'b'),
    (4, 'register_netdev(wlan0)', '', 'n'),
    (4, 'sdio_claim_irq(F1) · napi_enable()', 'CCCR INT_ENABLE', 'c'),
    (4, 'arasan_sdio_irq_enable()', 'IRPT_EN = CARD_INT', 'h'),
]

CONNECT = [
    (0, '/bin/wifi → wifi_connect(ssid, psk)', '用户态', 'k'),
    (1, 'sys_wifi_connect() → brcmfmac_connect()', 'kernel/net/sysnet.c', 'b'),
    (2, 'brcmf_dcmd(DOWN) · brcmf_dcmd(UP)', '清掉旧状态', 'b'),
    (2, 'brcmf_iovar("event_msgs")', '订阅 join/auth/link 事件', 'b'),
    (2, 'brcmf_iovar("mpc"=0 · "bus:rxglom"=0 · "country")', '', 'b'),
    (2, 'brcmf_dcmd(SET_INFRA · SET_AUTH · SET_WSEC=AES)', '', 'b'),
    (2, 'brcmf_bsscfg_iovar_int("wpa_auth" · "sup_wpa")', 'sup_wpa 不支持', 'b'),
    (2, 'wpa_pbkdf2(psk, ssid) → PMK', 'wpa_crypto.c', 'w'),
    (2, 'brcmf_dcmd(SET_WSEC_PMK)', '失败 → host_eapol=1', 'b'),
    (2, 'brcmf_dcmd(SET_SSID) · brcmf_connect_watch_start()', '10 s 截止', 'b'),
    (2, 'for ≤40：brcmf_dcmd(GET_BSSID)', '-17 = 尚未关联', 'b'),
    (3, 'brcmf_read_assoc_rsn_ie()', 'M2 要原样带 RSN IE', 'b'),
    (3, '〔NAPI 中〕brcmf_wpa_rx_locked(M1/M3)', '', 'n'),
    (4, 'brcmf_wpa_send_m2_locked()', 'SNonce、PTK、MIC', 'w'),
    (4, 'M3：校验 MIC，AES unwrap GTK', 'keys_pending = 1', 'w'),
    (3, 'brcmf_finish_wpa_handshake()', '', 'b'),
    (4, 'brcmf_install_ccmp_key(PTK) · (GTK)', 'bsscfg:wsec_key', 'b'),
    (4, 'brcmf_wpa_send_m4_locked()', 'handshake_done = 1', 'w'),
    (3, '500 ms：netdev_poll_one() 排空', '等 AP 开端口', 'n'),
    (3, 'net_dhcp_dev(wlan0)', 'Discover/Request ×3', 'n'),
    (3, 'brcmf_dcmd(SET_PM = 0)', '关省电：去掉 ~100 ms 延迟', 'b'),
    (3, 'brcmf_connect_watch_stop()', '', 'b'),
]

TX = [
    (0, 'net_tcp_write() / net_udp_send() / net_icmp_send()', 'kernel/net/net.c', 'n'),
    (1, 'tcp_start_queued_locked() → tcp_send_at_locked()', '≤ 4×MSS 在途', 'n'),
    (2, 'net_output_path() · arp_lookup()', '路由 + 下一跳 MAC', 'n'),
    (2, 'netdev_xmit_on(dev) → ops->start_xmit', '', 'n'),
    (3, 'brcmfmac_xmit(netdev, frame, len)', '未 handshake_done → -1', 'b'),
    (4, 'acquire(bus->lock) · brcmf_send_ether_locked()', '🔒 自旋锁', 'b'),
    (5, '填 BCDC 头 flags=0x20 + 以太网帧', '', 'b'),
    (5, 'while 无信用：brcmf_rx_dispatch_locked()', '≤200×100 µs', 'b'),
    (5, 'brcmf_sdpcm_header(ch=2, seq=tx_seq++)', '', 'b'),
    (5, 'brcmf_f2_transfer(write, wire_len)', '', 'b'),
    (6, 'brcmf_set_backplane_window(cc_base)', '3×CMD52', 'c'),
    (6, 'sdio_cmd53(F2, W, 固定地址) × ⌈len/limit⌉', '', 'c'),
    (7, 'mmc_request_sync() → arasan_request()', '💤 request_lock', 'h'),
]

RX = [
    (0, 'IRQ 62 → el1 向量 → devintr()', '硬中断', 'k'),
    (1, 'arasan_sdio_irq()', 'IRPT_EN=0，清 CARD_INT', 'h'),
    (2, 'brcmfmac_sdio_irq() → napi_schedule()', 'queue_work(brcmf0_wq)', 'b'),
    (0, 'brcmf0_wq kworker → napi_workfn()', '内核线程，可睡眠', 'k'),
    (1, 'brcmf_napi_poll(napi, budget=16)', '🔒 bus->lock', 'b'),
    (2, 'brcmf_rx_dispatch_locked()', '每轮 1 帧', 'b'),
    (3, 'brcmf_rx_frame_locked()', '', 'b'),
    (4, 'sdio_int_pending() → INTSTATUS 读+清', 'CMD52 + CMD53', 'c'),
    (4, 'brcmf_f2_transfer(read, 64) + 余下', 'tx_max = byte9', 'c'),
    (3, 'ch0 → ctl[] · ch1 → printf · ch2 ↓', '', 'b'),
    (3, 'EAPOL → brcmf_wpa_rx_locked() · 其他 → net_rxq', '4 槽', 'b'),
    (2, '出队 → release → net_rx_dev(dev, frame)', '放锁再进协议栈', 'n'),
    (3, 'arp_rx() / tcp_rx() / UDP / ICMP / DHCP', '', 'n'),
    (2, 'keys_pending → brcmf_finish_wpa_handshake()', 'AP 重新换钥', 'b'),
    (2, 'napi_complete_done() → arasan_sdio_irq_complete()', '重开 CARD_INT', 'h'),
    (3, '假中断：queue_delayed_work(irq_work, 1 tick)', '10 ms 后再开', 'h'),
]

COL = {'k': INK, 'h': YEL_S, 'c': BLUE_S, 'b': PURP_S, 'n': GREEN_S, 'w': ORANGE_S}


def tree(s, x, y, w, title, sub, items, fill, stroke, gap=19, note_x=None):
    h = 54 + len(items) * gap + 6
    s.rect(x, y, w, h, '#fff', stroke, 1.6, 10)
    s.rect(x, y, w, 30, fill, stroke, 1.6, 10)
    s.text(x + 12, y + 21, title, 14.5, bold=True)
    s.text(x + w - 12, y + 21, sub, 11, fill=SUB, anchor='end')
    note_x = note_x or x + w * 0.62
    cy = y + 52
    stack = {}
    for depth, fn, note, kind in items:
        fx = x + 16 + depth * 18
        if depth:
            s.add(f'<path d="M{fx - 12:.1f},{cy - 14:.1f} V{cy - 4:.1f} H{fx - 3:.1f}" fill="none" stroke="#afb8c1"/>')
        s.text(fx, cy, fn, 11.3, fill=COL[kind], mono=True, maxw=note_x - fx - 8)
        if note:
            s.text(note_x, cy, note, 11, fill=SUB, maxw=x + w - note_x - 10)
        cy += gap
    return h


def fig_calls(path):
    W = 1400
    s = Svg(W, 1300)
    s.text(W / 2, 34, 'SDIO Wi-Fi 函数调用链：初始化 · 连接 · 发送 · 接收', 21, bold=True, anchor='middle')
    # legend
    lx = 330
    for k, lab in (('h', 'Arasan host（arasan_sdio.c）'), ('c', 'SDIO core（sdio.c）'), ('b', 'brcmfmac.c'),
                   ('w', 'WPA2（wpa_crypto.c / 主机握手）'), ('n', '网络栈（net.c）')):
        s.rect(lx, 50, 14, 14, '#fff', COL[k], 2, 3)
        s.text(lx + 20, 62, lab, 12, fill=COL[k])
        lx += twidth(lab, 12) + 52
    h1 = tree(s, 24, 80, 664, '① 初始化（启动时，CPU0）', 'main.c → … → brcmf_probe', INIT, BLUE, BLUE_S, note_x=24 + 400)
    h2 = tree(s, 712, 80, 664, '② 连接 AP（/bin/wifi 进程上下文）', 'brcmfmac_connect', CONNECT, PURP, PURP_S, note_x=712 + 410)
    y2 = 80 + max(h1, h2) + 20
    h3 = tree(s, 24, y2, 664, '③ 发送（调用者上下文：TCP 定时器 / write 系统调用）', 'TX', TX, GREEN, GREEN_S, note_x=24 + 450)
    h4 = tree(s, 712, y2, 664, '④ 接收（IRQ 顶半部 → brcmf0_wq 下半部）', 'RX', RX, RED_BG, RED, note_x=712 + 440)
    s.h = int(y2 + max(h3, h4) + 24)
    s.save(path, 'SDIO Wi-Fi 函数调用链')
