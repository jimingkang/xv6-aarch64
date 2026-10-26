# MMIO、DMA、PCIe BAR 与 IOMMU

## 1. 两条方向不同的访问路径

MMIO 和 DMA 使用目的不同：

```text
CPU -> PCIe 设备：BAR / MMIO（入站访问）
PCIe 设备 -> 主机 DDR：DMA（出站访问）
```

CPU 访问设备时，操作系统给 PCIe BAR 分配一段 CPU 物理地址。CPU 对该地址执行
load/store，Root Complex 把访问转换为 PCIe 请求，并送到设备寄存器或设备内存窗口。

设备访问主机内存时，设备内部 DMA 引擎主动发出 PCIe Memory Read/Write 请求。驱动
把 DMA 地址写进设备的描述符或 DMA 寄存器，CPU 不负责逐字节搬运数据。

## 2. 四类地址不能混用

| 地址 | 使用者 | 作用 |
|---|---|---|
| 设备内部地址 | 设备内部逻辑 | 定位设备片内 SRAM、FIFO、寄存器 |
| BAR/MMIO 地址 | CPU | 访问设备寄存器或设备公开的内存窗口 |
| DMA/IOVA 地址 | PCIe 设备 | 发起 DMA 时写进 PCIe 请求的地址 |
| 主机物理地址 | 内存控制器/CPU | 数据在主机 DDR 中的实际位置 |

例如，同一批数据可能位于设备内部 SRAM 的 `0x2000`，CPU 通过 BAR 窗口
`0xf0020000` 访问它；设备向主机传输时使用 IOVA `0x40000000`，IOMMU 再把它翻译
到 DDR 物理地址 `0x81234000`。这些数值属于不同地址空间。

## 3. IOMMU 不负责复制数据

IOMMU 是硬件地址转换与权限检查单元：

```text
设备给出的 IOVA -> IOMMU 页表 -> 主机 DDR 物理地址
```

真正搬运数据的是设备 DMA 引擎。IOMMU 不把设备内部 SRAM 复制到 DDR，也不替代 DMA。
它主要提供：

- 设备访问隔离，防止 DMA 越界破坏内核内存；
- 把连续 IOVA 映射到离散物理页；
- 虚拟机设备直通与每设备独立地址空间；
- 让地址位宽较小的设备访问高地址物理内存。

没有 IOMMU 时，DMA 地址通常直接对应设备可达的主机物理地址；有 IOMMU 时，DMA 地址
是 IOVA。两种情况下都由设备 DMA 引擎发起传输。

## 4. PCIe DMA 的数据源在哪里

DMA 数据源由传输方向决定，并不固定为 PCIe 设备的内部 SRAM。

### 设备写入主机 DDR

```text
设备 SRAM / RX FIFO / 硬件流水线 / 外部输入
        -> 设备 DMA 引擎
        -> PCIe Memory Write TLP
        -> IOMMU（可选：IOVA -> 主机 PA）
        -> 主机 DDR
```

- 网卡：PHY/MAC 收到的网络包，通常先经过 RX FIFO 或片内缓冲；
- NVMe：NAND Flash 经闪存控制器读出的数据；
- 摄像采集卡：传感器产生的实时像素流；
- GPU/加速器：显存、缓存或计算流水线的输出。

设备 SRAM 可以作为数据源或中转缓冲，但不是必需的完整帧缓存。设备也可以边接收、边从
FIFO 经 DMA 写入 DDR。

### 设备读取主机 DDR

```text
主机 DDR
        -> IOMMU（可选）
        -> PCIe Memory Read completion
        -> 设备 DMA 引擎
        -> 设备 SRAM / TX FIFO / 处理单元
```

网卡发送就是典型例子：驱动在 DDR 准备数据包和描述符，通知网卡；网卡 DMA 引擎读取
DDR，数据进入 TX FIFO，再由 MAC/PHY 发出。

## 5. 与 Raspberry Pi 3 DMA 地址的联系

本项目里的 DWC2 和 Unicam 不是 PCIe 设备，但原理相同：它们都是总线主设备，可以绕过
CPU 直接访问 DDR。区别在于 BCM2837 使用 VideoCore 总线别名，而不是 PCIe IOVA：

```c
bus_addr = 0xc0000000U | cpu_physical_address;
```

例如相机缓冲的 CPU 物理地址 `0x07200000` 写进 Unicam DMA 寄存器时使用
`0xc7200000`。高位 `0xC` 选择 SDRAM 的总线别名；它不是另一块内存。PCIe + IOMMU 系统
中的 IOVA 也由设备使用，但其转换由可编程 IOMMU 页表完成，两者不应当简单视为同一种
编码机制。

## 6. 结论

- BAR/MMIO 解决 CPU 如何访问设备；
- DMA 解决设备如何高效访问主机内存；
- IOMMU 解决 DMA 地址如何翻译和限制访问权限；
- DMA 数据源既可能是设备 SRAM，也可能是 FIFO、硬件流水线、外部输入或主机 DDR；
- IOMMU 只翻译和检查地址，实际数据始终由 DMA 引擎传输。
