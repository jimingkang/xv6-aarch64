# xv6-aarch64 Raspberry Pi 3 SDIO Wi-Fi 驱动架构

本文以当前工程代码为准，总结 Raspberry Pi 3 板载 BCM43430/BCM43455
Wi-Fi 从硬件控制器、总线枚举、固件装载、网络设备注册，到 WPA2、DHCP 和
Internet 数据收发的完整路径。

相关实现主要位于：

- `kernel/device.c`：bus/device/driver 核心；
- `kernel/arasan_sdio.c`：BCM2837 Arasan SDHCI host；
- `kernel/sdio.c`、`kernel/sdio.h`：最小 MMC/SDIO core；
- `kernel/brcmfmac.c`：Broadcom FullMAC 驱动；
- `kernel/wpa_crypto.c`：WPA2-PSK 密码学；
- `kernel/net.c`、`kernel/net.h`：`net_device`、Ethernet、ARP、IPv4、UDP、ICMP、DHCP；
- `kernel/sysnet.c`：用户态网络系统调用入口；
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

外置 SD 卡走 SD Memory 协议，由 `kernel/sdhost.c` 管理；板载 Wi-Fi 走
SDIO 协议，由 `kernel/arasan_sdio.c` 管理。把两者混到同一个 host 驱动会造成
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

## 11. 接收、发送与轮询模型

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

当前 xv6 采用**CPU 定时轮询**而不是 SDIO DAT1 IRQ：ARM Generic Timer 每约
100 ms 调用 `netdev_poll_all()`。为了防止四个 CPU 同时操作共享的 Arasan/USB
host，硬件网络轮询只在 CPU0 执行。

这与芯片内部固件自己的调度不是一回事：所有 `brcmf_*` C 函数都由 ARM CPU
执行；BCM43455 内部运行的是下载进去的 `.BIN` 固件。

---

## 12. 与 MT7601U USB Wi-Fi 的关系

```mermaid
flowchart TB
    NET["xv6 网络层"]
    BCM["wlan0: BCM43455<br/>SDIO FullMAC<br/>当前可 WPA2 + DHCP + ping"]
    MT["MT7601U<br/>USB SoftMAC<br/>当前到固件/RF/扫描阶段"]

    NET --> BCM
    NET -. "完成关联和 802.11 数据面后注册" .-> MT
```

两块网卡可以同时存在，但当前 MT7601U 尚未注册为可用 `net_device`，避免它
抢占默认路由。BCM 执行 BCDC 连接控制期间会暂停 MT7601U USB 扫描，防止两个
轮询型驱动同时占用较长的硬中断时间；连接完成后恢复 MT7601U 扫描。

---

## 13. 初始化顺序与依赖

```mermaid
flowchart TD
    A["device_init"] --> B["sdio_bus_init"]
    B --> C["netinit"]
    C --> D["arasan_sdio_driver_init"]
    D --> E["fat32init"]
    E --> F["brcmfmac_driver_init"]
    F --> G["下载 firmware/NVRAM/CLM"]
    G --> H["register_netdev wlan0"]
    H --> I["userinit"]
    I --> J["/init -> /bin/wifi"]
```

顺序不能随意交换：

- 没有 `sdio_bus_init()`，SDIO function 无法绑定驱动；
- 没有 `arasan_sdio_driver_init()`，不会枚举出 BCM43455；
- 没有 `fat32init()`，驱动读取不到 bootfs firmware；
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
- CPU0 单核有界轮询；
- 与 USB MT7601U 扫描阶段共存。

### 后续改进

1. SDIO DAT1 interrupt，替代 100 ms CPU polling；
2. CMD53 block-mode/multi-block，完整支持大型控制响应；
3. 更完整的固件 flow-control/credit 管理；
4. 断线重连、漫游、扫描结果用户接口；
5. WPA3、开放网络和更多加密组合；
6. 网络配置工具与多网卡策略路由；
7. MT7601U SoftMAC 认证、关联、WPA2 与 `wlan1` 注册。

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

确认 SD 卡设备号和 FAT32 bootfs 挂载点后执行：

```sh
diskutil list
make install-rpi3 WIFI_FIRMWARE_DIR=firmware
sync
```

`RPI3_BOOTFS`、`RPI3_KERNEL_NAME` 和 `WIFI_FIRMWARE_DIR` 均可通过 Make 变量
覆盖。安装目标会复制并校验：

```text
kernel/kernel8.img -> kernel8-xv6_wifi.img
fs.img             -> FS.IMG
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
| `kernel/device.c`, `kernel/device.h` | bus/device/driver 注册与绑定 |
| `kernel/sdhost.c` | 外置 SD memory card、bootfs 与 FS.IMG |
| `kernel/arasan_sdio.c` | 板载 Wi-Fi 的 Arasan SDIO host |
| `kernel/sdio.c`, `kernel/sdio.h` | MMC/SDIO core、Function、CMD52/CMD53 |
| `kernel/brcmfmac.c` | backplane、firmware、SDPCM、BCDC、WPA2、收发 |
| `kernel/wpa_crypto.c` | PBKDF2、PTK、MIC、AES unwrap |
| `kernel/fat32.c` | bootfs firmware 和 FS.IMG 文件访问 |
| `kernel/net.c`, `kernel/net.h` | net_device、Ethernet、ARP、IPv4、DHCP、ICMP |
| `kernel/sysnet.c` | `wifi_connect` 系统调用 |
| `user/wifi.c` | `/bin/wifi` 配置与连接命令 |
| `user/ping.c` | Internet 连通性测试 |
| `user/init.c` | 开机自动执行 `/bin/wifi` |
| `Makefile` | 构建、FS.IMG、bootfs 与 firmware 安装 |
