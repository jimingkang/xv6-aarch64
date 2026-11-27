# xv6-aarch64 Raspberry Pi 3 SDIO Wi-Fi 驱动架构

本文以当前工程代码为准，总结 Raspberry Pi 3 板载 BCM43430/BCM43455
Wi-Fi 从硬件控制器、总线枚举、固件装载、网络设备注册，到 WPA2、DHCP 和
Internet 数据收发的完整路径。

相关实现主要位于：

- `kernel/drivers/base/device.c`：bus/device/driver 核心；
- `kernel/drivers/mmc/host/arasan_sdio.c`：BCM2837 Arasan SDHCI host；
- `kernel/drivers/mmc/core/sdio.c`、`kernel/sdio.h`：最小 MMC/SDIO core；
- `kernel/drivers/net/wireless/brcmfmac.c`：Broadcom FullMAC 驱动；
- `kernel/drivers/net/wireless/wpa_crypto.c`：WPA2-PSK 密码学；
- `kernel/net/net.c`、`kernel/net.h`：`net_device`、Ethernet、ARP、IPv4、UDP、ICMP、DHCP；
- `kernel/net/sysnet.c`：用户态网络系统调用入口；
- `user/wifi.c`：读取 `/etc/wifi.conf` 并请求连接；
- `user/init.c`：开机自动执行 `/bin/wifi`。

---

## 1. 一张图看完整架构

```mermaid
flowchart TB
    APP["用户态 /bin/wifi<br/>读取 /etc/wifi.conf"]
    SYSCALL["wifi_connect 系统调用"]
    NET["xv6 网络层<br/>net_device / Ethernet / ARP / IPv4<br/>UDP / ICMP / DHCP / route"]
    BRCM["brcmfmac FullMAC 驱动<br/>BCDC 控制 + SDPCM 数据"]
    WPA["主机 WPA2 补充逻辑<br/>PBKDF2 / PTK / MIC / AES unwrap"]
    SDIO["MMC/SDIO core<br/>CMD5 / CMD52 / CMD53<br/>sdio_bus 匹配"]
    HOST["Arasan SDHCI host<br/>0x3f300000<br/>GPIO34--39 ALT3"]
    CHIP["BCM43430 / BCM43455<br/>Wi-Fi 芯片 + RAM + CR4 MCU"]
    AIR["802.11 AP / 无线网络"]

    APP --> SYSCALL --> BRCM
    BRCM <--> WPA
    BRCM <--> NET
    BRCM <--> SDIO
    SDIO <--> HOST
    HOST <--> CHIP
    CHIP <--> AIR
```

这里有两条容易混淆的边界：

1. **Arasan 是 SDIO host controller**，负责产生 CMD52/CMD53、时钟以及搬运
   数据；它不是 Wi-Fi 芯片。
2. **BCM43455 是 FullMAC Wi-Fi 芯片**，芯片内固件处理大部分 802.11 MAC
   工作；xv6 的 `brcmfmac` 是运行在 ARM CPU 上的主机驱动。

---

## 2. 硬件拓扑：SD 存储和 SDIO Wi-Fi 不是同一条链路

```mermaid
flowchart LR
    CPU["Cortex-A53"]
    SDHOST["BCM SDHOST<br/>0x3f202000<br/>GPIO48--53"]
    ARASAN["Arasan SDHCI<br/>0x3f300000<br/>GPIO34--39"]
    CARD["外置 SD 卡<br/>FAT32 bootfs + FS.IMG"]
    WIFI["板载 BCM43455<br/>SDIO Function 1/2/3"]

    CPU --> SDHOST --> CARD
    CPU --> ARASAN --> WIFI
```

外置 SD 卡走 SD Memory 协议，由 `kernel/drivers/mmc/host/sdhost.c` 管理；板载 Wi-Fi 走
SDIO 协议，由 `kernel/drivers/mmc/host/arasan_sdio.c` 管理。把两者混到同一个 host 驱动会造成
GPIO、时钟和控制器所有权冲突。

---

## 3. Linux 风格的设备层级

当前实现不是把 Wi-Fi 驱动直接写死在 `main()`，而是通过逐层注册和匹配建立：

```mermaid
flowchart TD
    PDEV["platform_device<br/>bcm2837-arasan-sdio"]
    PBUS["platform_bus"]
    PDRV["platform_driver<br/>Arasan driver"]
    HOST["struct mmc_host"]
    CARD["struct sdio_card"]
    F1["struct sdio_func 1"]
    F2["struct sdio_func 2"]
    F3["struct sdio_func 3"]
    SBUS["sdio_bus"]
    WDRV["struct sdio_driver<br/>brcmfmac"]
    NDEV["struct net_device<br/>wlan0"]

    PDEV --> PBUS
    PDRV --> PBUS
    PBUS -->|"name match + probe"| HOST
    HOST --> CARD
    CARD --> F1
    CARD --> F2
    CARD --> F3
    F1 --> SBUS
    F2 --> SBUS
    F3 --> SBUS
    WDRV --> SBUS
    SBUS -->|"vendor/device/function match"| WDRV
    WDRV --> NDEV
```

关键匹配条件为：

```text
vendor   = 0x02d0 (Broadcom)
device   = 0xa9a6 或 0xa9bf
function = 1
```

SDIO product ID 只用于初步匹配。驱动之后还会读取 Broadcom backplane 的
ChipCommon chip ID，以判断真实芯片及固件组合。

---

## 4. SDIO 枚举过程

`arasan_sdio_driver_init()` 注册 platform device/driver；probe 成功后建立
`mmc_host` 并调用 `mmc_add_host()`。SDIO core 随后执行：

```mermaid
sequenceDiagram
    participant Main as xv6 main
    participant Host as Arasan host
    participant Core as MMC/SDIO core
    participant Card as BCM43455 SDIO
    participant Bus as sdio_bus

    Main->>Host: arasan_sdio_driver_init()
    Host->>Core: mmc_add_host(mmc_host)
    Core->>Card: CMD0 复位
    Core->>Card: CMD5 查询/协商 IO OCR
    Core->>Card: CMD3 获取 RCA
    Core->>Card: CMD7 选择 card
    Core->>Card: CMD52 读取 CCCR/FBR/CIS
    Core->>Card: CMD52 设置 4-bit bus
    Core->>Bus: 注册 sdio0:1 / :2 / :3
    Bus->>Bus: vendor/device/function 匹配
    Bus->>Main: 调用 brcmfmac probe
```

三种常用命令的职责：

| 命令 | 用途 |
|---|---|
| CMD5 | 识别 SDIO card、协商电压并等待 ready |
| CMD52 | 单字节读写 CCCR、FBR 和 function 寄存器 |
| CMD53 | 批量传输 backplane、固件及 SDPCM 帧 |

当前 SDIO core 的 CMD53 以单块/byte-mode 为主。短控制帧、固件下载和普通网络
帧已经可用；接近 2 KiB 的 legacy scan-results 响应尚需完整的 multi-block
CMD53 支持。因此连接流程不把 `BRCMF_C_SCAN_RESULTS` 当作前置条件，而让
FullMAC 固件根据 `SET_SSID` 自行扫描和加入目标网络。

---

## 5. Function 1 和 Function 2 的分工

```mermaid
flowchart LR
    CPU["ARM 主机驱动"]
    F1["SDIO Function 1<br/>backplane/control"]
    BP["Broadcom backplane<br/>ChipCommon / SDIO core / CR4 / RAM"]
    F2["SDIO Function 2<br/>SDPCM FIFO"]
    FW["Wi-Fi firmware"]

    CPU <-->|"CMD52/CMD53<br/>窗口寄存器"| F1
    F1 <--> BP
    CPU <-->|"CMD53<br/>control/event/data"| F2
    F2 <--> FW
```

- **F1**：设置 backplane window，访问芯片核心寄存器与 RAM，下载固件、NVRAM
  并控制 CR4 reset。
- **F2**：固件启动后才 ready，是运行时 SDPCM FIFO，承载 BCDC 控制响应、事件、
  EAPOL 和普通 Ethernet 数据。
- **F3**：可以被枚举出来，但当前不作为独立网络设备绑定。

F2 不能在固件下载前强行启用；启动日志中的
`F2 deferred until firmware starts` 正是这一生命周期约束。

---

## 6. 固件、NVRAM 与 CLM 的启动流程

BCM43455 并不是从永久存储直接运行完整 Wi-Fi 固件。每次冷启动时，主机从
bootfs 读取并下载：

```text
BCM43455.BIN  芯片 CR4 执行代码
BCM43455.TXT  板级 NVRAM 参数、天线和校准配置
BCM43455.CLM  国家/信道/功率法规数据库
```

```mermaid
sequenceDiagram
    participant FAT as FAT32 bootfs
    participant Driver as brcmfmac
    participant F1 as SDIO F1
    participant CR4 as BCM43455 CR4
    participant F2 as SDIO F2

    Driver->>F1: 读取 ChipCommon chip ID
    Driver->>Driver: 解析 EROM，发现 core 与 RAM
    Driver->>CR4: halt/reset
    FAT-->>Driver: BIN/TXT/CLM
    Driver->>F1: CMD53 下载 BIN 到 TCM RAM
    Driver->>F1: 写入整理后的 NVRAM + token
    Driver->>CR4: 写 reset vector，release reset
    CR4-->>Driver: FWREADY mailbox
    Driver->>F2: enable function 2
    Driver->>F2: SDPCM protocol negotiation
    Driver->>F2: BCDC 查询版本、MAC，下载 CLM
    Driver->>Driver: register_netdev(wlan0)
```

这解释了为什么 firmware 文件必须在 bootfs 可读后才能调用
`brcmfmac_driver_init()`。

---

## 7. SDPCM 与 BCDC

运行时数据不是裸 Ethernet 直接放入 CMD53，而是分层封装：

```text
CMD53 transfer
  └─ SDPCM header
       ├─ control channel
       │    └─ BCDC dcmd / iovar
       ├─ event channel
       │    └─ association/link/auth events
       └─ data channel
            └─ BCDC data header
                 └─ Ethernet frame
```

BCDC 控制命令使用递增 request ID。`brcmf_dcmd()` 发送请求后轮询 F2，直到
收到相同 ID 的 control response；event/data 帧可能插在控制响应之前，因此
`brcmf_rx_dispatch_locked()` 必须按 channel 分流，不能假定下一帧一定是回复。

---

## 8. 谁触发 Wi-Fi 连接

驱动 probe 只完成硬件和 `wlan0` 初始化，**不会自行选择用户网络**。
开机自动连接来自用户态：

```mermaid
sequenceDiagram
    participant Init as /init
    participant Wifi as /bin/wifi
    participant Sys as sys_wifi_connect
    participant Brcm as brcmfmac
    participant AP as TP-Link AP

    Init->>Wifi: exec("/bin/wifi")
    Wifi->>Wifi: 读取 /etc/wifi.conf
    Wifi->>Sys: wifi_connect(ssid, psk)
    Sys->>Brcm: brcmfmac_connect()
    Brcm->>AP: scan/join/auth/associate
    Brcm->>AP: WPA2 四次握手
    Brcm->>AP: DHCP Discover/Request
    Brcm-->>Wifi: 返回成功
    Wifi-->>Init: 打印 wifi: associated
    Init->>Init: 启动 ttyS0 login
```

所以：

- `brcmfmac: ...` 是内核驱动输出；
- `wifi: associated` 是 `/bin/wifi` 输出；
- 删除 `user/init.c` 中的自动 `exec("/bin/wifi")` 后，驱动仍会初始化，但需要
  登录后手动运行 `/bin/wifi` 才会连接 AP。

---

## 9. WPA2-PSK 数据路径

当前固件对 `sup_wpa` 和 `SET_WSEC_PMK` 返回 unsupported，因此主机补充处理
EAPOL 四次握手：

```mermaid
sequenceDiagram
    participant AP
    participant FW as BCM43455 firmware
    participant Host as xv6 brcmfmac + wpa_crypto

    Host->>Host: PBKDF2(passphrase, SSID) -> PMK
    AP->>FW: EAPOL M1 (ANonce)
    FW->>Host: SDPCM data / EAPOL M1
    Host->>Host: 生成 SNonce，派生 PTK，计算 MIC
    Host->>FW: EAPOL M2
    FW->>AP: M2
    AP->>FW: EAPOL M3 + encrypted GTK
    FW->>Host: EAPOL M3
    Host->>Host: 验证 MIC，AES unwrap GTK
    Host->>FW: 安装 pairwise CCMP key
    Host->>FW: 安装 group CCMP key
    Host->>FW: EAPOL M4
    Host->>Host: handshake_done = 1
```

只有 `handshake_done` 后，普通 Ethernet 数据才允许通过 `start_xmit` 和 RX
delivery。M4 提交后还会保留约 500 ms 的接收窗口，让 AP 打开 controlled port，
再开始 DHCP。

---

## 10. DHCP、ARP、路由和 ping

```mermaid
flowchart LR
    DHCP["DHCP<br/>地址/掩码/网关/DNS"]
    ROUTE["route table<br/>直连路由 + 默认路由"]
    ARP["ARP cache<br/>next-hop IP -> MAC"]
    IP["IPv4 / ICMP / UDP"]
    DEV["wlan0 start_xmit"]

    DHCP --> ROUTE
    DHCP --> ARP
    IP --> ROUTE --> ARP --> DEV
```

`net_dhcp_dev(wlan0)` 明确绑定目标设备，避免双网卡环境中误用最后注册的默认
设备。Discover 和 Request 都会重试三次。成功后加入：

- 本地子网直连路由；
- 通过 DHCP gateway 的默认路由；
- DHCP server/gateway 的 ARP 项；
- DNS server 地址。

之后 `ping google.com` 的路径为：DNS UDP 查询 → 路由选择 → gateway ARP →
ICMP Echo → `wlan0` → BCDC data → SDPCM → CMD53 → BCM43455。

---

## 11. 接收、发送与 IRQ/workqueue 模型

### 发送

```mermaid
flowchart LR
    L3["ARP/IPv4/ICMP/UDP"] --> ETH["Ethernet frame"]
    ETH --> XMIT["brcmfmac_xmit"]
    XMIT --> BCDC["BCDC data header"]
    BCDC --> SDPCM["SDPCM data channel"]
    SDPCM --> CMD53["F2 CMD53 write"]
    CMD53 --> CHIP["BCM43455 firmware"]
```

### 接收

```mermaid
flowchart LR
    CHIP["BCM43455 firmware"] --> CMD53["F2 CMD53 read"]
    CMD53 --> DISP["brcmf_rx_dispatch_locked"]
    DISP -->|control| CTL["BCDC response"]
    DISP -->|event| EVT["link/auth/assoc event"]
    DISP -->|data| ETH["Ethernet/EAPOL"]
    ETH --> RX["net_rx_dev"]
    RX --> PROTO["ARP / IPv4 / UDP / ICMP / DHCP"]
```

当前 xv6 使用 **SDIO DAT1 IRQ + 每设备私有NAPI/workqueue**：BCM43455 拉起 DAT1
后，Arasan 产生 legacy IRQ 62。顶半部屏蔽 card interrupt、设置 pending bit，
调用 `brcmfmac_sdio_irq()`，调度该设备的`napi`对象。顶半部
不执行 CMD53，也不进入网络协议栈。

```mermaid
flowchart LR
    DAT1["BCM43455 DAT1"] --> IRQ["Arasan IRQ 62 top half"]
    IRQ --> MASK["mask card IRQ + pending=1"]
    MASK --> Q["napi_schedule(bus->napi)"]
    Q --> W["brcmf0_wq kworker"]
    W --> C["CMD53 / SDPCM / BCDC"]
    C --> N["net_rx_dev"]
    C -->|drained| RE["napi_complete_done + re-enable DAT1 IRQ"]
    C -->|budget exhausted| Q
```

`brcmf0_wq` 属于具体 `brcmf_bus_state`，不再借用全局 `system_wq` 或网络公共
work。这避免 SDIO 慢速 CMD53 阻塞 DWC2、TCP timer 等其它下半部。每轮最多
处理16个SDPCM项；未排空时NAPI核心保持scheduled并重新投递，排空后才complete
并解除DAT1屏蔽。重复IRQ由scheduled/missed状态合并，不会无限堆积节点。

这里经历过两个中间实现：最早由 CPU0 的 10 ms Generic Timer 直接轮询；之后
IRQ 只设置 pending，Timer 再把全局 `net_deferred_work` 排队。现在数据接收完全
由 DAT1 IRQ 驱动，Timer 只推进 workqueue 的有序 delayed-work 时钟。

连接阶段另有每设备私有 `state_work`：`SET_SSID` 后每 50 ms 检查一次连接截止
时间，10 秒到期便设置 timeout；只有发现 Arasan card IRQ pending 时才补调度
NAPI，所以它是连接 watchdog，不是恢复周期性 SDIO 轮询。连接成功、失败或
设备移除时都会同步取消 delayed work。连接状态由设备 `bus->lock` 保护。

这与芯片内部固件自己的调度不是一回事：所有 `brcmf_*` C 函数都由 ARM CPU
执行；BCM43455 内部运行的是下载进去的 `.BIN` 固件。

---

## 12. 与 MT7601U USB Wi-Fi 的关系

MT7601U 属于 DWC2 USB SoftMAC 路径，不是 SDIO 总线设备。它的枚举、firmware、
DMA、host-channel IRQ、SoftMAC、WPA2 和 `wlan1` 数据面已迁移到专门文档：
[readme_usb_wifi_MT7601.md](readme_usb_wifi_MT7601.md)。本文件后续只描述板载
BCM43430/43455 SDIO FullMAC。

---

## 13. 初始化顺序与依赖

```mermaid
flowchart TD
    A["device_init"] --> B["sdio_bus_init"]
    B --> P["procinit + workqueue_init"]
    P --> C["netinit"]
    C --> D["arasan_sdio_driver_init"]
    D --> E["rootdev_init / fat32mount"]
    E --> F["brcmfmac_driver_init"]
    F --> G["下载 firmware/NVRAM/CLM"]
    G --> H["register_netdev wlan0"]
    H --> I["userinit"]
    I --> J["/init -> /bin/wifi"]
```

顺序不能随意交换：

- 没有 `sdio_bus_init()`，SDIO function 无法绑定驱动；
- 没有 `arasan_sdio_driver_init()`，不会枚举出 BCM43455；
- 没有 `rootdev_init()` 挂起 bootfs（`fat32mount()`），驱动读取不到 bootfs firmware；
- 没有 `netinit()`，不能注册 `wlan0`；
- `/bin/wifi` 必须等上述内核层全部 ready 后才能发起连接。

---

## 14. 当前能力与待完善项

### 已实现

- platform → MMC host → SDIO card/function → brcmfmac 的设备模型；
- Arasan 时钟、GPIO34--39、4-bit SDIO；
- CMD5、CMD52、CMD53；
- Broadcom backplane、EROM、TCM RAM 发现；
- BCM43430/BCM43455 firmware、NVRAM、CLM 加载；
- F1/F2 生命周期和 SDPCM/BCDC；
- `wlan0` 的 `register_netdev()` 与 remove/unregister；
- WPA2-PSK 主机四次握手补充路径；
- DHCP、ARP、DNS、路由和 Internet ping；
- SDIO DAT1 → Arasan IRQ 62 顶半部 → 每设备 `brcmf*_wq` 下半部；
- 私有 RX work、连接 watchdog delayed work 及同步取消生命周期；

### 后续改进

1. CMD53 block-mode/multi-block，完整支持大型控制响应；
2. 更完整的固件 flow-control/credit 管理；
3. 断线重连、漫游、扫描结果用户接口；
4. WPA3、开放网络和更多加密组合；
5. 网络配置工具与多网卡策略路由；
6. SDIO IRQ 风暴、丢中断和 remove 生命周期压力测试；
7. 增加 RX/TX 字节数、IRQ 合并、budget 用尽和队列延迟统计，再据此调优 budget；
8. 将连接主流程也改为完全异步状态机，移除用户系统调用中的 250 ms 等待循环。

---

## 15. 常用验证日志

驱动初始化成功：

```text
sdio: sdio0:1 vendor=2d0 device=a9a6 class=0
brcmfmac: firmware running; SDPCM transport ready
brcmfmac: wlan0 mac=...
brcmfmac: control plane ready
```

连接成功：

```text
brcmfmac: direct join target=TP-Link_B114
brcmfmac: 802.11 associated bssid=...
brcmfmac: WPA2 four-way handshake complete
dhcp: dev=wlan0 address=... gateway=... dns=...
brcmfmac: wlan0 IPv4 ready
wifi: associated
```

网络验证：

```sh
ping google.com
cat /proc/devices
```

`wifi: associated` 来自用户程序；它代表驱动连接与 DHCP 均已成功返回，而不只是
芯片完成初始化。

---

## 16. Function 1 backplane window 细节

BCM43455 内部的 ChipCommon、SDIO core、ARM CR4、D11 和 TCM RAM 位于
Broadcom backplane 地址空间。SDIO Function 1 只能通过一个 32 KiB aperture
访问它：

```text
target backplane address
        |
        +-- window = address & 0xffff8000
        |      写 F1 0x1000a / 0x1000b / 0x1000c
        |
        +-- CMD53 address = (address & 0x7fff) | 0x8000
```

跨越 32 KiB 边界的 firmware/NVRAM 传输必须重新设置 window，不能让一次
CMD53 直接跨过 aperture。真实芯片返回的 ChipCommon 值例如 `0x15264345`：

```text
chip = 0x4345
revision = 6
package = 2
```

SDIO product ID `0xa9a6` 和 ChipCommon chip ID 属于不同编号空间，不能直接比较。
驱动依据 backplane chip/revision 选择 BCM43430 或 BCM43455 firmware。

---

## 17. Firmware 启动七阶段与实机经验

当前 `brcmfmac` 的完整启动顺序为：

1. 从 ChipCommon `EROMPTR` 解析 DMP/AI EROM descriptors；
2. 定位 ChipCommon、SDIO、ARM CR4、D11 cores，并从 CR4 bank registers 计算
   TCM RAM；BCM4345/6 的 RAM base 为 `0x198000`；
3. 通过 AI wrapper `IOCTL`/`RESET_CTL` halt CR4，并保持无线 core reset；
4. FAT32 流式读取 `.BIN`，处理 32 KiB window 边界，写入 TCM 并验证 reset
   vector；
5. 把 `.TXT` 转换为 NUL 分隔 NVRAM，补双 NUL、四字节对齐及
   word-count/complement token，写入 RAM 尾部并验证；
6. 清 SDIO core interrupt，向 RAM 地址 0 写 reset vector，释放 CR4；
7. 写 SDPCM protocol version，等待 F2 ready 和 FWREADY/DEVREADY mailbox，
   配置 watermark 与 host interrupt mask。

典型成功日志：

```text
brcmfmac: EROM at ...
brcmfmac: core[...] id=83e ...
brcmfmac: core[...] id=829 ...
brcmfmac: TCM RAM base=198000 size=... bytes banks=...
brcmfmac: CR4 halted; wireless cores held in reset
brcmfmac: firmware download .../...
brcmfmac: firmware reset-vector=... verified
brcmfmac: NVRAM ... bytes written at ... token=...
brcmfmac: CR4 released reset-vector=...
brcmfmac: F2 ready mailbox=... protocol=4
brcmfmac: firmware running; SDPCM transport ready
```

实机曾在 512、256、128-byte CMD53 出现 data CRC error，驱动逐级减半后在
64-byte 稳定。因此当前 transfer limit 是运行时探测结果，不是 firmware 格式的
固定要求。未来完成 block-mode/multi-block 和 host timing 后应重新评估吞吐率。

旧版 `%x` 可能把最高位为 1 的 32-bit 值显示成带负号十六进制，例如
`-47c10e68` 的原始位模式是 `0xb83ef198`。写后校验成功时，这种显示不代表负
地址或 firmware 损坏。

---

## 18. BCDC 与 WPA2 的关键 ABI 陷阱

### BCDC GET 长度

早期实现把所有 GET iovar 的请求长度固定为整个 2048-byte buffer，加上 BCDC
header 后超过本地上限，导致请求在发送前被拒绝。正确长度是：

```text
strlen(iovar_name) + 1 + requested_result_capacity
```

### per-BSS iovar

`wpa_auth`、`sup_wpa` 等配置需要 bsscfg 编码：

```text
"bsscfg:wpa_auth\0" + uint32(bsscfgidx=0) + uint32(value)
"bsscfg:sup_wpa\0"  + uint32(bsscfgidx=0) + uint32(value)
```

### `wsec_pmk_t` 版本

BCM43455 firmware 7.45 使用传统 68-byte ABI：

```text
uint16 key_len
uint16 flags
uint8  key[64]
```

误用带 128-byte SAE 扩展的新结构会得到 `BCME_BADARG (-2)`。部分 firmware
同时不支持 `sup_wpa` 和明文 passphrase 形式的 `SET_WSEC_PMK`，所以当前实现
在主机端使用 `PBKDF2-HMAC-SHA1(passphrase, SSID, 4096)` 生成 32-byte PMK，
并在 firmware offload 不可用时完成 EAPOL 四次握手。

PMK、PTK、GTK 和 Wi-Fi 密码都属于敏感信息，不能打印到串口日志。

---

## 19. 编译、安装与分层排错

普通 `make` 只编译，不会写入 `/Volumes/bootfs`。构建产物为
`kernel/kernel8-xv6_wifi.img`。确认 SD 卡设备号和 FAT32 bootfs 挂载点后，
显式执行安装目标才会复制文件；指定 xv6 分区时还会将 `fs.img` 写入该分区：

```sh
diskutil list
make
make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2 WIFI_FIRMWARE_DIR=firmware
sync
```

`RPI3_BOOTFS`、`RPI3_KERNEL_NAME` 和 `WIFI_FIRMWARE_DIR` 均可通过 Make 变量
覆盖。安装目标会复制并校验：

```text
kernel/kernel8-xv6_wifi.img -> /Volumes/bootfs/kernel8-xv6_wifi.img
fs.img                      -> /dev/rdisk4s2 (when RPI3_XV6_DEV is specified)
config.txt         -> config.txt
BCM43430/43455 BIN/TXT/CLM
```

按层次定位故障：

```text
1. arasan-sdio: probing GPIO34-39        host controller
2. sdio: sdio0:1 ...                    SDIO enumeration
3. ChipCommon / EROM / TCM              F1 backplane
4. firmware running / SDPCM ready       MCU firmware + F2
5. version / CLM / wlan0 MAC            BCDC control plane
6. associated / WPA2 handshake complete wireless security
7. DHCP / ARP / wlan0 IPv4 ready        IP configuration
8. ping google.com                       DNS + routed data path
```

QEMU `raspi3b` 不模拟 BCM43455，只能回归内核启动、总线核心以及“没有 SDIO
设备”的失败路径；firmware、WPA2 和 Internet 数据面必须在真实 RPi3 验证。

---

## 20. 代码导航

| 文件 | 职责 |
|---|---|
| `kernel/drivers/base/device.c`, `kernel/device.h` | bus/device/driver 注册与绑定 |
| `kernel/drivers/mmc/host/sdhost.c` | 外置 SD memory card、bootfs 与 FS.IMG |
| `kernel/drivers/mmc/host/arasan_sdio.c` | 板载 Wi-Fi 的 Arasan SDIO host |
| `kernel/drivers/mmc/core/sdio.c`, `kernel/sdio.h` | MMC/SDIO core、Function、CMD52/CMD53 |
| `kernel/drivers/net/wireless/brcmfmac.c` | backplane、firmware、SDPCM、BCDC、WPA2、收发 |
| `kernel/drivers/net/wireless/wpa_crypto.c` | PBKDF2、PTK、MIC、AES unwrap |
| `kernel/fs/fat/fat32.c` | bootfs firmware 和 FS.IMG 文件访问 |
| `kernel/net/net.c`, `kernel/net.h` | net_device、Ethernet、ARP、IPv4、DHCP、ICMP |
| `kernel/core/workqueue.c`, `kernel/workqueue.h` | 命名 workqueue、有序 delayed work 与 kworker |
| `kernel/core/proc.c`, `kernel/proc.h` | `kthread_create()` 和内核线程调度上下文 |
| `kernel/net/sysnet.c` | `wifi_connect` 系统调用 |
| `user/wifi.c` | `/bin/wifi` 配置与连接命令 |
| `user/ping.c` | Internet 连通性测试 |
| `user/init.c` | 开机自动执行 `/bin/wifi` |
| `Makefile` | 构建、FS.IMG、bootfs 与 firmware 安装 |

---

## 21. 从 Timer 轮询到 SDIO IRQ 下半部的实现演进

本节原来记录在通用 bug 文档中，现集中到 SDIO Wi-Fi 专题文档。

### 21.1 原始 Timer 轮询

最初没有使用 SDIO DAT1 中断，CPU0 每次 Generic Timer IRQ 都执行：

```text
timerintr()                         hard IRQ
  -> netdev_poll_all()
     -> brcmfmac_poll_device()
        -> CMD53 读取 BCM43455 F2
        -> SDPCM/BCDC/Ethernet 解析
        -> net_rx_dev()
```

为了改善 SSH 回显，硬件 Timer 曾从100 ms缩短到10 ms，每十次才增加一次 xv6
逻辑 `ticks`。它能降低等待时间，但无数据时仍检查设备，而且完整接收路径运行在
hard IRQ 中，不能睡眠并会拉长 Timer 中断。

### 21.2 SDIO IRQ + Timer deferred poll 过渡方案

加入 BCM2837 legacy IRQ 62、Arasan `CARD_INT` 和 SDIO CCCR function interrupt
后，芯片能够通过 DAT1 通知 CPU：

```text
BCM43455 F2 frame
  -> SDIO DAT1
  -> Arasan CARD_INT
  -> BCM2837 pending-2 bit 30 (IRQ 62)
  -> arasan_sdio_irq()
       mask CARD_INT
       clear host interrupt
       arasan_card_irq_pending = 1
  -> return from IRQ
```

第一版仍等下一次10 ms Timer 调用 `netdev_poll_all()`。它消除了无条件 CMD53
读取，但耗时处理仍处在 Timer hard IRQ 上下文，因此只是过渡方案。

### 21.3 当前每设备 NAPI/workqueue 方案

当前完整路径如下：

```text
SDIO IRQ 62                         hard IRQ top half
  -> arasan_sdio_irq()
       mask/ack CARD_INT
       pending = 1
       brcmfmac_sdio_irq()
          -> napi_schedule(&bus->napi)
          -> wakeup(brcmf0_wq)
  -> return from IRQ

scheduler
  -> brcmf0_wq kworker              process-context bottom half
       -> brcmf_napi_poll(budget=16)
            -> bounded CMD53 drain
            -> SDPCM/BCDC/EAPOL/Ethernet
            -> net_rx_dev()
            -> drained: napi_complete_done + ack/unmask CARD_INT
            -> budget exhausted: NAPI core requeues poll，IRQ保持屏蔽
       -> sleep(brcmf0_wq)
```

相同NAPI对象在scheduled时不会重复入队；若IRQ在poll运行期间到达，`missed`只
记录一次补跑。因此既不会漏掉burst，也不会为每个中断无限追加队列节点。

`netif_napi_add()`把`bus->napi`绑定到对应`net_device`、`brcmfN_wq`和poll回调，
weight为16；SDIO IRQ准备完成后调用`napi_enable()`，设备释放前调用
`napi_disable()`同步停止poll。Linux用每CPU softirq poll list承载NAPI，本实现用
每设备kworker承载，但保留schedule/complete、budget、missed及生命周期语义。

### 21.4 IRQ 62 和三层中断开关

SDIO 接收能产生 CPU IRQ，需要同时打开三层开关：

1. **SDIO card CCCR**：`sdio_claim_irq()` 用 CMD52 设置 `INT_ENABLE(0x04)` 的
   master bit 和 function 1 bit；remove 时由 `sdio_release_irq()` 清除。
2. **BCM43455 内部**：`SDIO_CORE_HOSTINTMASK` 控制 mailbox/frame 事件是否送到
   DAT1。
3. **Arasan + BCM2837**：`EMMC_IRPT_EN/IRPT_MASK` 打开 `CARD_INT`，legacy
   controller 打开 pending-2 bit 30，也就是 IRQ 62。

顶半部只做 mask、ack、pending 和 work 入队，不执行 CMD52/CMD53，不获取
`brcmf_bus.lock`，也不进入协议栈。

### 21.5 连接 watchdog delayed work

`BRCMF_C_SET_SSID` 发出后，设备私有 `state_work` 每50 ms检查一次连接状态，
10秒到期后设置 `connect_timedout`。watchdog 只有看到 Arasan card IRQ pending
时才补投递 RX，因此不是旧式周期轮询。

`connect_active`、`connect_timedout` 和 `connect_deadline` 由 `bus->lock` 保护。
重新 arm delayed work 与停止路径也通过该锁排序；成功、失败及 remove 均调用
`cancel_delayed_work_sync()`，避免回调在设备释放后执行或停止后再次入队。

### 21.6 workqueue、内核线程和锁

启动后相关队列类似：

```text
workqueue: system_wq worker=0 pid=1
workqueue: net_wq worker=0 pid=5
workqueue: brcmf0_wq worker=0 pid=6
workqueue: brcmf1_wq worker=0 pid=7
```

SDIO Wi-Fi 路径涉及的队列和工作项如下：

| 队列/工作项 | 类型 | 触发源 | 功能 |
|---|---|---|---|
| `brcmf0_wq` | 设备0专用 workqueue，1个worker | `brcmfmac_driver_init()` | 执行板载BCM43455的RX和连接watchdog |
| `brcmf1_wq` | 设备1预留专用 workqueue，1个worker | `brcmfmac_driver_init()` | 支持第二个brcmfmac设备实例，不与设备0共用状态 |
| `bus->napi` | 每设备NAPI，weight=16 | SDIO DAT1/Arasan IRQ 62 | 合并IRQ，按预算CMD53读取F2并解析SDPCM/BCDC/EAPOL/数据帧 |
| `bus->state_work` | 每设备 delayed work | `SET_SSID`连接阶段 | 每50 ms检查pending IRQ和10秒连接deadline |
| `net_wq` | 网络层专用 workqueue，1个worker | `netinit()` | 执行独立于网卡的TCP重传定时任务 |
| `tcp_retransmit_work` | delayed work | TCP存在未确认段 | 按所有连接中最早的`tx_deadline`运行 |
| `system_wq` | 通用队列，4个worker | `schedule_work()` | 通用fallback；当前BCM43455接收路径不使用 |

```mermaid
flowchart TD
    DAT1["BCM43455 DAT1 IRQ"] --> RX["napi_schedule(bus->napi)"]
    RX --> BWQ["brcmf0_wq"]
    BWQ --> SDPCM["CMD53 / SDPCM / BCDC / net_rx_dev"]

    JOIN["SET_SSID"] --> DW["bus->state_work: 50 ms watchdog"]
    DW --> SORT["全局expires有序delayed链表"]
    SORT -->|到期| BWQ

    TCP["最早TCP tx_deadline"] --> SORT
    SORT -->|到期| TW["tcp_retransmit_work"]
    TW --> NWQ["net_wq"]
```

NAPI内嵌的普通work由硬件IRQ立即入设备队列；`state_work`和TCP work先进入
`kernel/core/workqueue.c` 的全局 `delayed_head`。该链表按 `expires` 递增排序，CPU0
每10 ms只移出已经到期的表头项目，并按项目的 `target` 投递到 `brcmf0_wq`、
`brcmf1_wq` 或 `net_wq`。因此时间排序与任务执行相互分离。

| 对象 | 上下文 | 作用 |
|---|---|---|
| Arasan IRQ top half | hard IRQ | mask/ack、记录 pending、调度NAPI |
| `brcmf0_wq.lock` | hard IRQ/进程 | 保护 work FIFO 和 pending/running/rerun |
| `brcmf0_wq` kworker | 内核进程 | CMD53、帧解析、WPA2 和网络栈投递 |
| `bus->lock` | worker/系统调用 | 串行化 SDPCM、BCDC 和连接状态 |
| Generic Timer | hard IRQ | 推进 ordered delayed-work 时钟 |

`kworker` 使用 xv6 进程槽、独立内核栈和 scheduler 上下文，但不返回 EL0；它是
内核线程，不是用户进程。不同设备的私有队列可以在不同 CPU 并发，同一设备的
work 不会并发执行。

### 21.7 构建与真机验证

```sh
make -j4 kernel/kernel8-xv6_wifi.img
make qemu
```

QEMU 只能验证内核、workqueue 和“没有 SDIO 设备”的启动路径。真实 RPi3 应看到：

```text
workqueue: brcmf0_wq worker=0 pid=...
brcmfmac: SDIO DAT1 IRQ receive enabled
brcmfmac: WPA2 four-way handshake complete
dhcp: address=... gateway=... dns=...
brcmfmac: wlan0 IPv4 ready
```

随后应持续运行 `ping` 和 SSH 交互，确认 IRQ 能立即收包，并确认 burst 处理后
`CARD_INT` 被正确重新打开，不会出现运行一段时间后停止接收。

---

## 22. 逐条命令、芯片内部与 DAT1 中断细节

本节把 SDIO 总线上的每一类命令、芯片内部的地址结构和接收中断的时序逐一讲清楚。
行号以 java 分支 `c09ba09` 为准。配套材料：

- 通信细节图：![SDIO F2 收发、CMD52、DAT1 中断](../svg/sdio-f2-cmd52-dat1.png)
- 逐步动画（9 个场景、81 步，可逐帧播放）：生成脚本 `tools/sdio_cmd_anim/sdio_cmd_anim.py`，
  渲染命令写在脚本开头。

### 22.1 三层地址，不要混在一起

| 哪一层 | 谁看得见 | 例子 |
|---|---|---|
| 主机内存地址 | 只有 xv6 内核，不上总线 | `brcmf_io_buf`、`brcmf_nvram_data`、`brcmf_bus.tx/rx/ctl/iovar_buf` |
| SDIO 功能地址（17 位） | CMD52/CMD53 命令里的地址字段 | F0 的 `0x05`；F1 的 `0x1000a`；F1 的 `0x9A00`；F2 的入口 `0x8000` |
| 芯片 backplane 地址（32 位） | 芯片内部 AXI 总线、芯片里的 ARM CR4 | ChipCommon `0x18000000`；TCM RAM `0x198000` |

`tx[]`、`rx[]`、`ctl[]`、`iovar_buf[]` 都是 `struct brcmf_bus_state` 的成员，实例是
`static struct brcmf_bus_state brcmf_devices[]`，位于内核 `.bss`。主机内存地址从不出现在 SDIO
总线上，数据由 CPU 在这些缓冲和 Arasan 的 `EMMC_DATA` 寄存器之间搬运（PIO）。

### 22.2 芯片内部：各核、AXI backplane 与 SDIO 包装

| 核 | EROM id | 作用 |
|---|---|---|
| ChipCommon | `0x800` | chipid、时钟和电源。寄存器固定在 `0x18000000`（`SI_ENUM_BASE`），`+0xfc` 是 EROM 地址 |
| ARM Cortex-R4 | `0x83e` | 跑 FullMAC 固件；带 TCM RAM，backplane 上从 `0x198000` 开始（`BRCMF_RAM_BASE_4345`） |
| D11 802.11 MAC | `0x812` | MAC + PHY/射频，空口收发，用自己的 DMA 读写 TCM RAM；下载固件时保持复位 |
| SDIO device core | `0x829` | 对外是 SDIO 卡；INTSTATUS `+0x20`、HOSTINTMASK `+0x24`、邮箱 `+0x40/+0x48/+0x4c` |

- 所有核挂在同一套 AXI 互连（backplane）上，共用一个 32 位地址空间。每个核有一页寄存器
  （`base`）和一页 wrapper（`wrap`，IOCTL `+0x408`、RESET_CTL `+0x800`），
  `brcmf_core_disable/brcmf_core_reset` 用同一套代码复位任何核。
- 核的地址不写死：`brcmf_scan_cores()`（`brcmfmac.c:602`）先读 `0x18000000+0xfc` 得到 EROM，
  再逐条读描述符，打印 `core[n] id=... base=... wrap=...`。代码里写死的只有 ChipCommon 的
  `0x18000000` 和 TCM RAM 起点 `0x198000`；“各核按 4 KB 排开”只是 Broadcom 常见布局，以启动日志为准。
- SDIO device core 把内部包装成三个 function：
  - **F0**：SDIO 规范的 CCCR/FBR/CIS，由 SDIO core 硬件实现。
  - **F1**：一座桥。SBADDR 三个寄存器是窗口基址，主机对 F1 `0x0000-0x7FFF` 的读写被翻译成对
    `窗口基址 + 偏移` 的 AXI 读写，SDIO core 在这里充当 AXI 主设备。
  - **F2**：FIFO + DMA 引擎。固件在 TCM RAM 里准备缓冲并交给 SDIO core 的 DMA；
    主机读 F2 时 DMA 从 TCM 搬进 FIFO，写 F2 时反向。F2 没有可寻址的存储，地址只是入口。
  - **DAT1**：INTSTATUS 与 HOSTINTMASK 相与不为 0 时，经 CCCR 的中断开关，拉低 DAT1。

一句话：F1 是主机伸进芯片总线的一只手，F2 是固件和主机之间的帧通道，DAT1 是门铃。

### 22.3 6 根线与 CMD52/CMD53

| 线 | GPIO | 作用 |
|---|---|---|
| CLK | 34 | host 输出时钟 |
| CMD | 35 | 双向：48 位命令和 48 位响应都只走这一根 |
| DAT0 | 36 | 数据位 0；卡忙时拉低表示 busy |
| DAT1 | 37 | 数据位 1；DAT 空闲（中断周期）时兼作卡中断，低有效，需上拉（`gpio_init()`） |
| DAT2 | 38 | 数据位 2（规范里另作 Read Wait，本项目未用） |
| DAT3 | 39 | 数据位 3 |

参数格式（`sdio.c:166`、`sdio.c:197`）：

```text
CMD52: W<<31 | FN<<28 | RAW<<27 | addr(17)<<9 | data(8)
CMD53: W<<31 | FN<<28 | BLK<<27 | OP<<26 | addr(17)<<9 | count(9, 0 表示 512)
```

- **CMD52 完全不用 DAT 线**：写的字节在命令的 bit7:0；读到的字节在 R5 响应的 bit7:0，
  也从 CMD 线回来（`*value = response & 0xff`，`response & 0xcb00` 视为错误）。
- **CMD53** 命令和 R5 走 CMD 线，数据块走 DAT[3:0]。本项目只用字节模式单块（`data.blocks = 1`），
  单次 ≤512 字节；`OP=1` 地址自增（F1 读写一段内存），`OP=0` 地址固定（F2 FIFO）。

四个真实例子：

| 操作 | arg |
|---|---|
| CMD52 读 F0 `0x05`（INT_PENDING） | `0x00000A00` |
| CMD52 写 F1 `0x1000a = 0x80`（SBADDRLOW） | `0x92001480` |
| CMD53 写 F1 `0x9A00`，512 字节，OP=1（固件一块） | `0x95340000` |
| CMD53 读 F2 `0x8000`，64 字节，OP=0（收帧头） | `0x21000040` |

### 22.4 按流程逐条命令

**枚举（`sdio_scan_host`，`sdio.c:332-410`）**

1. CMD0：复位，无响应。
2. CMD5 arg=0：R4 返回 OCR，bit30:28 = 功能数（2）。
3. CMD5 arg=OCR，循环直到 R4 bit31（IORDY）= 1。
4. CMD3：R6 高 16 位是 RCA。
5. CMD7 arg=RCA<<16：选中卡，之后才能 CMD52/53。
6. CMD52 读写 F0 `0x07`：切 4 位总线，`set_bus_width(4)`。
7. CMD52 读 FBR（`fn*0x100+0x00` 类别、`+0x09..0x0b` CIS 指针）。
8. CMD52 逐字节读 CIS 元组，得到 MANFID：`0x02d0` / `0xa9bf`（BCM43455），注册 `sdio0:1/sdio0:2`。

**固件下载（`brcmf_probe`，`brcmfmac.c:2107` 起）**

1. `sdio_enable_func(F1)`：CMD52 写 F0 `0x02`，轮询 `0x03`。
2. `brcmf_clock_prepare`：CMD52 写/读 F1 `0x1000e`，等 ALP。
3. 读 chipid：3×CMD52 设窗口 + CMD53 F1 读 4 字节。
4. `brcmf_scan_cores`、`brcmf_get_raminfo`：同样方式读 EROM 和 CR4 bank 寄存器。
5. `brcmf_set_passive`：经 wrapper 停 CR4（CPUHALT）、复位 D11。
6. `brcmf_write_firmware`（`:759`）：每块先 3×CMD52 写 SBADDR，再 1×CMD53 F1 OP=1 写入。
7. `brcmf_write_nvram`（`:856`）：同样方式写到 RAM 末尾。
8. `brcmf_set_active`（`:878`）：复位向量写到 backplane 地址 0，解除 CPUHALT。
9. `brcmf_start_f2`（`:891`）：CMD52 置 FORCE_HT；邮箱写协议版本；`sdio_enable_func(F2)`；
   写 HOSTINTMASK；CMD52 写 WATERMARK/DEVICE_CTL/MESBUSYCTRL；轮询 FWREADY。

**控制面（`brcmf_control_plane_start`，`:1734`）**：`ready=1` 后全是 F2 上的 SDPCM 帧：
`ver`、`cur_etheraddr`、CLM、`UP`；`sdio_claim_irq`（CMD52 写 F0 `0x04` 的 bit0|bit1）；
`arasan_sdio_irq_enable`。

**连接（`brcmfmac_connect`，`:1911`）**：`DOWN/UP`、`event_msgs`、`mpc`、`country`、
`SET_INFRA/AUTH/WSEC`、`wpa_auth`、`sup_wpa`、`SET_WSEC_PMK`、`SET_SSID`、轮询 `GET_BSSID`。
每一条都是一次 22.7 节的 BCDC 请求。

### 22.5 F1 窗口、TCM RAM 布局与 NVRAM

F1 地址空间：

| F1 地址 | 内容 |
|---|---|
| `0x00000-0x07FFF` | 32 KB backplane 窗口（驱动再或上 `0x8000` 表示 4 字节访问） |
| `0x10008` | WATERMARK |
| `0x10009` | DEVICE_CTL |
| `0x1000a/b/c` | SBADDR LOW/MID/HIGH：窗口基址的 bit15:8、23:16、31:24 |
| `0x1000e` | CHIPCLKCSR |
| `0x1000f` | SDIOPULLUP |
| `0x1001d` | MESBUSYCTRL |

TCM RAM 布局：

```text
0x198000              ← rambase
  固件 .BIN（第一个字 = 复位向量）
  固件运行时的堆、栈、帧缓冲
  NVRAM：key=value\0 … \0，补齐到 4 字节
  最后 4 字节：token = (~字数 << 16) | 字数
0x198000 + ramsize    ← ramsize 由 brcmf_get_raminfo 从 CR4 bank 寄存器算出
```

分块下载的例子：写固件偏移 `0x1A00` 那一块，目标 `0x198000 + 0x1A00 = 0x199A00`。

```text
window = 0x199A00 & 0xffff8000 = 0x198000，>>8 = 0x1980
CMD52 W F1 0x1000a = 0x80
CMD52 W F1 0x1000b = 0x19
CMD52 W F1 0x1000c = 0x00
CMD53 W F1 OP=1 addr = (0x199A00 & 0x7fff) | 0x8000 = 0x9A00，len 512
```

块大小初值 `BRCMF_IO_CHUNK = 512`，遇到 CRC 错误会减半重试（`brcmf_transfer_limit`），一块不会
跨 32 KB 窗口。代码每块都重新设窗口，没有缓存当前窗口。

NVRAM 就是 `BCM43455.TXT`：81 条板级参数（`boardtype`、`macaddr`、`xtalfreq=37400`、天线、
`pa2ga*/pa5ga*` 功放校准、`maxp*` 最大功率、`mcsbw*po` 速率功率偏移、蓝牙/LTE 共存）。
`brcmf_prepare_nvram` 去掉注释和空行，每条后加 `\0`，末尾加 token；`brcmf_write_nvram` 写到
`rambase + ramsize - length`；固件启动后读 RAM 最后一个字，校验 token 后找到 NVRAM。

BIN、TXT、CLM 三个文件都在 SD 卡 FAT32 启动分区根目录：`brcmf_load_firmware()` 调用
`fat32openroot()`（`kernel/fs/fat/fat32.c:910`），Makefile 的安装目标把它们复制到
`$(RPI3_BOOTFS)`。CLM 不在启动前写入 RAM，而是固件运行后经 F2 iovar 分段下发。

### 22.6 DAT1 中断：谁拉低，谁清零，为什么先清零再读空

**从 INTSTATUS 到 CPU 中断的链路**

```text
SDIO core INTSTATUS (+0x20)        bit6 = I_HMB_FRAME_IND，bit7 = 邮箱 …
  & HOSTINTMASK (+0x24)            brcmf_start_f2 写入
  ≠ 0 → F1 中断请求 → CCCR INT_PENDING bit1（CMD52 读 F0 0x05 可见）
  & CCCR INT_ENABLE bit0|bit1      sdio_claim_irq
  → 中断周期内拉低 DAT1
  → Arasan EMMC_INTERRUPT.INT_CARD_INT
  & EMMC_IRPT_EN                   arasan_sdio_irq_enable / irq_complete
  → GIC IRQ 62 → arasan_sdio_irq()
```

INTSTATUS bit6 和 DAT1 不是一回事：只清 bit6，若还有别的被放行的位，DAT1 仍为低；
bit6 为 1，若 INT_ENABLE 未开或正在传数据，DAT1 也不会被拉低。

**分工**

- 拉低 DAT1：芯片 SDIO core 硬件。
- 中断处理（GIC 那一列，`arasan_sdio_irq()`）：清主机侧 INT_CARD_INT（写 1），关 IRPT_EN，
  `napi_schedule`。不读数据，不碰 DAT1。
- 清 INTSTATUS（写 1 清零，W1C）：kworker 里的 `brcmf_rx_frame_locked()`（`:1194`），经
  F1 CMD53 读出 INTSTATUS 再原样写回；`brcmf_dcmd` 等回复时也会调用同一函数。清零后若无其他
  放行位，F1 中断请求撤销，卡松开 DAT1，由上拉电阻拉回高电平。
- 读数据：同一函数用 CMD53 读 F2，先读 64 字节帧头取 len，再读剩余部分；`rx_more=1` 后由
  NAPI 循环再次调用，直到某次读到 len=0。

**为什么是“先清零，再读空”**

INTSTATUS bit6 只表示“有货”，不计数。每次调用都是“读 INTSTATUS → 写 1 清零 → 读一次 F2”，
所以一轮的最后一个动作是读，不是清。新帧在不同时刻到达：

| 新帧何时到达 | 结果 |
|---|---|
| 某次清零之前 | 通知被清，但清零后的那次读会读到它 |
| 最后一次清零之后、最后一次读之前 | 最后那次读读到它，len≠0，循环继续 |
| 最后一次读（len=0）之后 | 它的 bit6 在最后一次清零之后置上，没人清；重开 IRPT_EN 时 DAT1 仍低，再来一次中断 |

若改成“读到空 → 最后清零”，在最后一次读和清零之间到达的帧，既没被读，通知又被抹掉，
帧留在 TCM RAM 里没人通知主机（lost wakeup），直到下一帧或某个 dcmd 碰巧轮询 F2。

**其他要点**

- 新帧并不进入主机：它在芯片 TCM RAM 的固件队列里排队。F2 FIFO 只是 SDIO core 里的小窗口，
  主机每读一次 F2，DMA 才从 TCM 取下一段送出。
- DAT1 只是门铃，数据从不经过它；CMD53 传数据时 DAT1 是数据线，卡在下一个中断周期再拉低，
  因为 INTSTATUS 是锁存的。
- 读到“比预想多”是正常的：驱动不按数量读，只按帧头里的长度一帧一帧读到空。连带效果是偶尔
  多一次空中断（CMD52 看到 F1 有中断，读 F2 直接 len=0），以及 NAPI budget 16：读满 16 帧
  未读空时不 complete，中断保持关闭，NAPI 重新调度继续读。
- 伪中断：Arasan 有时在卡没有中断时也报 INT_CARD_INT。先用一次 CMD52 读 INT_PENDING，
  bit1=0 判为 spurious，IRQ 保持屏蔽 1 个 tick（10 ms）再打开。

### 22.7 一条 BCDC 请求与 `brcmf_dcmd` 的等待方式

`brcmf_dcmd()`（`:1530`）：

1. 持 `brcmf_bus.lock`，在 `tx[]` 拼 SDPCM 头 12 B（len、~len、seq、ch0、doff）+ BCDC 头 16 B
   （cmd、len、flags 含请求号、status）+ 参数（iovar 是 `"name\0" + 值`）。
2. `brcmf_f2_transfer(write=1)`：3×CMD52 设窗口到 `cc_base`，CMD53 F2 W OP=0
   `addr=(cc_base&0x7fff)|0x8000`，按传输上限分块。
3. 等回复：调用 `brcmf_rx_dispatch_locked()`，ch0 回复拷进 `ctl[]`，`ctl_id == id` 即完成。

原实现等回复时一直持有 `brcmf_bus.lock`（自旋锁，关中断）每 200 µs 轮询一次，最长 3 秒，
期间本核不响应中断，NAPI 和 xmit 也被挡住。现在的实现：

- `brcmf_bus.lock` 只在每次 SDIO 访问时持有，等待期间释放；前 2 ms 每 100 µs 轮询一次，
  之后释放锁并 `sleep(&ticks)` 一个 tick 再查；3 秒超时不变。启动阶段 probe 在调度器之前
  （`myproc()==0`），退回忙等但同样不持锁。
- 新增 `dcmd_lock`（mutex）保证同一时间只有一个控制请求在等回复，`ctl[]` 不会被覆盖；
  `iovar_lock` 因跨越等待而改为 mutex。锁顺序：`iovar_lock → dcmd_lock → lock`。
- scan/connect 循环中的 250 ms 忙等改为 `brcmf_wait_ticks(25)`。

该修复已提交到 java、hdmi、mpi、rpi3-lock 四个分支。

# MMC host 请求串行化

用户态程序和内核 Wi-Fi worker 的调度优先级不是 SDIO 请求正确性的保证。当前 xv6 中普通用户
进程与新建 kworker 默认都属于 `SCHED_OTHER/nice 0`，而且 BCM43455 的 NAPI、状态 work 和控制
路径可以在不同 CPU 上并发运行。Arasan 控制器只有一套 `ARG1/CMDTM/INTERRUPT/DATA` 寄存器，
所以 CMD52/CMD53 事务必须以 **host** 为粒度串行，而不能只依赖每个 brcmf 设备自己的锁。

`struct mmc_host` 现在包含 `request_lock` sleeplock，MMC core 的 `mmc_request_sync()` 在完整命令/
数据事务外持有它。所有 CMD52 和 CMD53 都通过该入口，作用类似 Linux MMC core 的 host claim：

```text
brcmf control/state work ─┐
brcmf NAPI RX/TX         ├─ mmc_request_sync()
SDIO function control   ─┘       │
                                 └─ request_lock
                                      └─ Arasan ARG/CMD/IRQ/DATA registers
```

这里使用 sleeplock 而不是 spinlock，因为 Arasan request 会等待命令、数据和超时，持有期间不能
禁止调度。此前偶发的 `CMD52 failed status=df0001 irq=100` 中 bit 0 是 `CMD_INHIBIT`，可能由并发
请求覆盖寄存器或前一事务尚未结束造成；host 串行化消除了软件侧请求重叠。单次硬件超时仍可能
发生，届时应继续依据 ERROR/timeout 位诊断，不能归因于同时运行的普通用户程序。

启动探测发生在 `userinit()` 之前，此时 `myproc()` 为空，并且只有启动 CPU 会提交 MMC 请求；
`mmc_request_sync()` 在这个阶段直接执行 host request。进入用户进程、NAPI 和 workqueue 运行阶段
后才获取 sleeplock，避免现有 sleeplock 为记录 owner PID 而解引用空的 `myproc()`。
