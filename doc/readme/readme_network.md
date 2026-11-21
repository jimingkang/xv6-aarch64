# xv6-aarch64 网络层

本实现参考 MIT xv6 networking lab 的分层方式，并扩展为包含网卡抽象、路由、ARP、
IPv4、ICMP、UDP、TCP、DHCP、NAPI 和 socket 文件描述符接口的小型网络子系统。

当前没有独立的 `net_device.c`。相关代码分布为：

- `kernel/net/net.c`：`net_device` 注册/注销、NAPI、ARP/IPv4/ICMP/UDP/TCP、路由和 DHCP；
- `include/kernel/net.h`：`struct net_device`、`struct net_device_ops` 和协议报文结构；
- `kernel/net/sysnet.c`：UDP、ICMP、TCP、Wi-Fi 和 DHCP 系统调用入口；
- `kernel/fs/sysfile.c`：socket 文件描述符与 VFS `file_operations` 适配；
- `user/libc_syscall.c`：用户态 libc 系统调用包装。

`net_device` 是二层设备抽象，不实现 UDP 或 TCP。UDP/TCP 位于它的上层，最终把完整的
Ethernet frame 交给 `net_device->ops->start_xmit()`。

## UDP 用户接口

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

`udp_recv()` 在端口队列为空时睡眠。网卡驱动调用 `net_rx_dev()` 交付完整 Ethernet frame，
协议层解析 Ethernet/IP/UDP 报文，按目的端口入队并唤醒等待进程。

这里的 `udp_send()` 是 libc/系统调用接口，不是 `struct net_device` 的成员。调用关系是：

```text
user udp_send()
  -> sys_udp_send()
  -> net_udp_send()
  -> net_udp_send_copy() 构造 UDP/IP/Ethernet headers
  -> netdev_xmit_on(dev, frame, len)
  -> dev->ops->start_xmit()
  -> 具体网卡驱动
```

## TCP 与 socket 接口

当前已经实现 TCP 主动连接、监听、accept、收发、关闭、重传、接收窗口、少量乱序段合并，
并通过 socket 文件描述符接入 `read/write/poll/epoll`。用户程序可以使用：

```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
connect(fd, ...);
read(fd, buf, len);
write(fd, buf, len);
close(fd);
```

`camserver`、`wget`、`sshd`、`tcpd` 和跨节点 `mpi` 都使用这条 TCP/socket 路径；同一节点
的 MPI rank 则可以使用共享内存传输。

## net_device 设备边界

当前网卡接口定义为：

```c
struct net_device_ops {
  int  (*start_xmit)(struct net_device *dev, void *frame, int length);
  void (*poll)(struct net_device *dev);
  int  (*open)(struct net_device *dev);
  void (*stop)(struct net_device *dev);
};
```

驱动通过 `register_netdev()` 注册设备；发送走 `netdev_xmit_on()`，接收走
`net_rx_dev()`。设备层只处理 Ethernet frame，不区分其内部是 TCP、UDP 还是 ICMP。

## UDP 数据路径

```text
user udp_send
  -> sys_udp_send
  -> net_udp_send
  -> 构造 Ethernet + IPv4 + UDP frame
  -> netdev_xmit_on
  -> net_device->ops->start_xmit
  -> 网卡驱动

网卡接收
  -> net_rx_dev
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

当前已接入多种 `net_device`：QEMU CDC-ECM、板载 Broadcom SDIO Wi-Fi 和 MT7601U USB
Wi-Fi。网络层已经实现 ARP cache、路由、DHCP、ICMP、UDP、TCP 和 socket 文件描述符。
仍未实现的主要能力包括 IPv4 fragmentation/reassembly、IPv6、完整 TCP 拥塞控制以及
Linux `cfg80211/nl80211` 管理接口。

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
