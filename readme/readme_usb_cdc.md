# RPi3 DWC2 USB Host 与 CDC Ethernet

## 实现范围

`kernel/usbnet.c` 增加了一个适合当前 xv6 教学环境的最小 USB Host：

- 访问 BCM2837 的 DWC2 控制器，基址为 `PERIPHERAL_BASE + 0x980000`；
- 将 DWC2 切换到 Host 模式并启用内部 DMA；
- 复位、供电并枚举 root port；
- 在 QEMU `raspi3b` 的虚拟 USB hub 上供电、复位并枚举下游设备；
- 读取 device/config/interface/endpoint descriptor；
- 从 QEMU `usb-net` 的 RNDIS/CDC 复合设备中选择 CDC-ECM 配置；
- 启用 CDC data interface 的 alternate setting 1；
- 通过 EP2 Bulk-OUT 发送 Ethernet frame，通过 EP2 Bulk-IN 接收；
- 将 USB Ethernet 接到 `kernel/net.c` 的 ARP/IPv4/UDP 协议栈。

QEMU 的设备同时公布 RNDIS 和 CDC 两套配置。当前驱动选择 CDC-ECM，因此不需要 RNDIS 的 encapsulated command、OID 和 packet-message 封装。这是“CDC/RNDIS 复合 USB 网卡的 CDC 模式”，还不是一个可连接任意纯 RNDIS 网卡的完整 RNDIS 协议实现。

## 初始化流程

```text
main()
  -> netinit()
  -> usbnetinit()
       -> DWC2 core reset / force host / DMA enable
       -> root port power + reset
       -> enumerate QEMU root hub as address 1
       -> hub port power + reset
       -> enumerate usb-net child as address 2
       -> scan configurations for CDC class=2, subclass=6
       -> SET_CONFIGURATION(1)
       -> SET_INTERFACE(interface=1, alternate=1)
       -> SET_ETHERNET_PACKET_FILTER
       -> net_set_xmit(usbnet_xmit)
```

`usbnet_xmit()` 使用 host channel 1 同步执行 Bulk-OUT。RX 使用 host channel 2；它不会在 NAK 后立即取消事务，而是保持通道挂起，等待设备收到 Ethernet frame 后由 DWC2 自动重试。timer IRQ 中的 `usbnetpoll()` 只检查完成状态。

收到帧后必须先释放 `usb_lock` 再调用 `net_rx()`。原因是 ARP request 会在 `net_rx()` 中同步生成 ARP reply，reply 又通过 `usbnet_xmit()` 获取同一把锁；持锁调用会造成递归 acquire panic。交付前先把 DMA buffer 复制到独立 buffer，避免重新挂起 RX 后覆盖协议栈正在处理的数据。

## QEMU 启动与测试

```sh
make qemu-net
```

等价的网络关键参数是：

```text
-netdev user,id=net0,ipv4=on,ipv6=off,net=10.0.2.0/24
-device usb-net,netdev=net0,mac=52:54:00:12:34:56
-object filter-dump,id=netdump,netdev=net0,file=packets.pcap
```

启动成功会显示：

```text
usbnet: root device class=9 ...
usbnet: hub port=1 child class=2 vid=525 pid=a4a2 ...
usbnet: CDC-ECM ready addr=2 cfg=1 vid=525 pid=a4a2
```

xv6 中可以运行：

```text
$ nettest
$ netdns
```

宿主机可检查实际经过 USB 网卡后端的帧：

```sh
tcpdump -nn -r packets.pcap
```

已验证抓包能够看到 xv6 发出的 UDP/DNS frame、QEMU 返回的 ARP request、xv6 的 ARP reply，以及 QEMU 返回的 IPv4/ICMP frame，说明 DWC2 Bulk-IN 和 Bulk-OUT 均已连通。

## 真实 Raspberry Pi 3 的限制

这版代码首先针对 QEMU `raspi3b` 验证。真实 Pi 3 板载网络不是 CDC/RNDIS：它通常是 DWC2 后接 LAN951x USB hub，再接 SMSC95xx Ethernet。因此要驱动板载网口，还需要：

- 完整的 USB hub 状态与热插拔处理；
- high-speed hub 后面的 split transaction；
- SMSC95xx 网卡驱动；
- 更完整的超时、错误恢复和 USB 中断处理。

外接纯 CDC-ECM 网卡也可能经过 hub；真实硬件上的 hub split transaction 尚未实现。不要把 QEMU 下的枚举成功直接等同于真实 Pi 3 板载 Ethernet 已可用。

