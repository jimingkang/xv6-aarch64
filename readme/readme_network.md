# xv6-aarch64 网络层

本实现参考 MIT xv6 networking lab 的分层方式，加入 Ethernet、ARP reply、IPv4、UDP 和
阻塞式 UDP 接收队列。协议层位于 `kernel/net.c`，报文结构位于 `kernel/net.h`，系统调用
封装位于 `kernel/sysnet.c`。

## 用户接口

```c
int udp_bind(int port);
int udp_unbind(int port);
int udp_send(uint32 dst, int sport, int dport, const void *buf, int len);
int udp_recv(int dport, uint32 *src, uint16 *sport, void *buf, int maxlen);
```

地址参数使用 host byte order，例如：

```c
#define IP_LOOPBACK 0x7f000001U
udp_send(IP_LOOPBACK, 1000, 2000, "hello", 6);
```

`udp_recv()` 在端口队列为空时通过 `sleep(port, &port->lock)` 阻塞；数据到达后 `net_rx()`
解析 Ethernet/IP/UDP 报文，按目的端口入队并调用 `wakeup(port)`。每个端口最多排队 16
个 datagram，超过限制的新报文会被丢弃，防止网络输入耗尽内核页面。

## 当前数据路径

```text
user udp_send
  -> sys_udp_send
  -> 构造 Ethernet + IPv4 + UDP frame
  -> loopback: net_rx
  -> 校验 Ethernet/IP/UDP header
  -> UDP port queue
  -> wakeup receiver
  -> udp_recv copyout 到用户缓冲区
```

测试：

```sh
nettest
```

预期输出：

```text
nettest: src=10.0.2.15:1000 len=19 data='hello from xv6 UDP'
nettest: ok
```

## 与 MIT xv6-network 的硬件差异

MIT lab 使用 PCI E1000 网卡，RPi3 没有这块硬件。RPi3 Model B/B+ 的有线网络位于 USB
复合设备之后，因此真机外网还需要 DWC2 USB Host、hub 枚举以及对应 USB Ethernet
驱动。QEMU `raspi3b` 也不能直接复用 E1000 PCI 驱动。

协议层预留了以下网卡边界：

```c
void net_set_xmit(int (*xmit)(void *packet, int len));
void net_rx(void *packet, int len);
```

未来网卡驱动初始化时调用 `net_set_xmit()` 注册发送函数；接收中断把完整 Ethernet frame
交给 `net_rx()`。这样接入 LAN951x/USB 网卡时不需要修改 UDP/IP 层。

当前已实现 UDP loopback、ARP reply、ICMP Echo 和 DWC2 CDC-ECM 网卡。尚未实现通用
ARP cache、IP fragmentation、TCP、socket 文件描述符接口和动态网络配置。

## 通过宿主机 Wi-Fi 的 QEMU NAT

运行：

```sh
make qemu-net
```

该目标添加：

```text
-netdev user,id=net0,ipv4=on,ipv6=off,net=10.0.2.0/24
-device usb-net,netdev=net0,mac=52:54:00:12:34:56
```

QEMU SLIRP 使用 macOS 当前默认路由，因此 Mac 通过 Wi-Fi 上网时，NAT 流量也通过 Wi-Fi；
它不是把物理 Wi-Fi 芯片直通给 xv6。虚拟网络为：

```text
xv6 10.0.2.15 -> QEMU gateway 10.0.2.2 -> macOS Wi-Fi -> Internet
```

原始报文会记录到 `packets.pcap`。可使用：

```sh
tcpdump -n -r packets.pcap
```

`raspi3b` 没有 PCI/virtio 总线，QEMU 的 `virtio-net-device` 会报告
`No 'virtio-bus' bus found`，所以这里使用 DWC2 USB Host 和 CDC-ECM NIC。`netdns` 验证
UDP/DNS，`ping` 验证 DNS 解析和 ICMP Echo：

```text
$ ping 10.0.2.2
$ ping google.com
```

内核提供 `icmp_send()` 和 `icmp_recv()` 两个简单 syscall。接收路径验证 IPv4 与 ICMP
checksum，按 identifier 匹配 Echo Reply 并唤醒等待进程。`ping` 对域名先向 QEMU DNS
`10.0.2.3` 查询 A 记录，然后连续发送四个 Echo Request。timer tick 为 100 ms，因此当前
显示的 RTT 精度也是 100 ms。
