# 跨机 MPI：xv6（rpi3 / QEMU）与 macOS 同一作业

`user/mpi.c` 现在可以组成一个跨多台机器的 MPI 作业。例如 iMac 上 2 个 rank，
rpi3（或 QEMU raspi3b）上 2 个 rank，一共 4 个 rank 的 `MPI_COMM_WORLD`。
fem 的 `shell_mpi_benchmark` 可以这样联合求解，结果与单机一致。

## 结构

* **节点**：一台机器上的一组 rank。每个节点内仍由 `MPI_Init` fork 出各 rank，
  rank 之间走共享内存环形缓冲（`readme_mpi.md`）。
* **跨节点**：每个 rank 与其它节点上的每个 rank 各有一条 TCP 连接，
  传的仍是同一种 16 字节头加数据的字节流。`mpi.c` 的 `chan_*` 层把
  "环形缓冲"和"套接字"统一成按对端编号的字节通道，消息匹配、意外队列、
  集合通信都不变。
* **建连方向**：节点 k 的 rank 主动连接编号更小的节点上的 rank。节点 j 的第
  r 个 rank 监听 `port_j + r`。最大编号的节点只向外连，不监听，所以它可以在
  NAT 后面（QEMU user 网络）。每条连接先发 hello `{magic, rank, world}`。
* **等待**：有远端对端时，rank 不在门铃信号量上睡眠（套接字数据不会敲门铃）。
  xv6 上改为 `sched_yield` 自旋，因为 xv6 的 `poll` 以 100 ms tick 为粒度；
  macOS 上 `poll(…, 1 ms)`。
* **结束**：`MPI_Finalize` 在 barrier 之后与每个远端互发一个告别消息
  （`TAG_BYE`），收到对方的告别后才关连接。这样不会有未读数据被丢弃，
  也就避免了 macOS 回 RST。
* **中止**：`MPI_Abort` 关闭所有连接，远端读到 EOF 后也中止。
* **上限**：`MPI_MAX_RANKS` 改为 16。xv6 内核的 TCP 连接表是 `NTCP = 16`，
  xv6 节点上的 rank 数乘以远端 rank 数不要超过约 14。

## 启动参数

`MPI_Init` 从 argv 中剥掉这些参数，位置不限：

| 参数 | 含义 |
| --- | --- |
| `-np N` | 本节点 rank 数（可省略，取 `-mpi_nodes` 中本节点的值） |
| `-mpi_nodes a,b,…` | 各节点 rank 数，节点 0 在前；节点 0 的 rank 编号最小 |
| `-mpi_node k` | 本机是第几个节点 |
| `-mpi_hosts ip0:port0,ip1:port1,…` | 各节点的地址和基端口；从不监听的节点写 `-` |

xv6 的 sh 原来最多 10 个参数，现改为 32（`user/sh.c` `MAXARGS`）。
串口一行输入约 128 字符，命令不宜过长。

## macOS 原生构建（`hostmpi/`）

```
brew install eigen        # 已装可跳过
make -C hostmpi           # 输出 build/hostmpi/
```

`hostmpi/Makefile` 用系统 C++ 库和真 Eigen 编译同一份 `user/mpi.c`
（`-DMPI_HOST`，系统调用映射见 `user/mpi_host.h`）、`user/petsc`、
fem 库和程序：`shell_mpi_benchmark`、`shell_serial_benchmark`、
`bar_mpi_trace`、`mpitest`。

* MPI 编成 `libxmpi.dylib`。Mach-O 没有符号别名，`user/mpi_host_alias.c`
  （由 `mpi.h` 生成）提供弱定义的 `MPI_x`，转调 `PMPI_x`。
* `tests/shell_mpi_profile.cpp` 在 macOS 上用 dyld `__interpose` 包装 MPI
  调用，只对动态库导入生效，所以与 fem 的 CMake 构建一样，它被编成
  `libshell_mpi_profile.dylib`。
* 单机运行：`build/hostmpi/shell_mpi_benchmark -np 4 -shell_benchmark`。

## QEMU 联跑

iMac（节点 0，监听 17000、17001；7000 被 AirPlay 占用）：

```
build/hostmpi/shell_mpi_benchmark -np 2 -mpi_nodes 2,2 -mpi_node 0 \
    -mpi_hosts 10.0.2.2:17000,-
```

QEMU 中的 xv6（节点 1，`make qemu-net`，登录 root/xv6）：

```
$ dhcp usb0
$ shell_mpi_benchmark -mpi_nodes 2,2 -mpi_node 1 -mpi_hosts 10.0.2.2:17000,-
```

slirp 把来宾访问的 10.0.2.2 转到宿主机 127.0.0.1，所以两边写同一个地址即可。

## 实板 rpi3

rpi3 通过 Wi-Fi（`/etc/wifi.conf`，开机由 init 连接）拿到局域网地址。
把 `10.0.2.2` 换成 iMac 的局域网 IP，iMac 防火墙需放行 17000 起的端口：

```
# iMac
build/hostmpi/shell_mpi_benchmark -np 2 -mpi_nodes 2,4 -mpi_node 0 -mpi_hosts 192.168.0.195:17000,-
# rpi3
$ shell_mpi_benchmark -mpi_nodes 2,4 -mpi_node 1 -mpi_hosts 192.168.0.195:17000,-
```

实板这一步尚未验证，QEMU 已验证。

## 为此修的内核问题

1. **dwc2：hub 后面的 CDC 设备**。QEMU raspi3b 把 usb-net 挂在内置 hub 下，
   `dwc2_init` 原来只接受 hub 后的 MT7601U 和 HID，现在 class 2 的设备
   交给 `usbnet_attach`。
2. **usbnet 收包丢 64 字节**。批量 IN 在 NAK 后被重新提交时会覆盖已收到的包。
   现在 NAK 不再触发中断（通道会自己重试）。传输被中断时保留已收的整包，
   下一次接在后面收。
3. **usbnet 收包没人轮询**。`netdev_poll_all` 从未被调用，DHCP 之后就收不到包了。
   现在通道 2 完成时由中断调度 kworker（`cdc_rx_work`）收包。
4. **hub 后批量 IN 每 1 ms 帧只走一个 64 字节包**，一个 1.5 KB 帧要约 25 ms。
   现在中断只等首包；首包是满包时，用连续的 split 传输把帧剩余部分拉完（发送方向本来就这样做）。
   跨机带宽从 48 KB/s 升到 3.3 MB/s。
5. **TCP 停等**。原来每次只有一个段在途。slirp 是 BSD 协议栈，单个段的 ACK
   要等 200 ms 的延迟 ACK 定时器。现在最多 4 个 MSS 在途（`TCP_TXWIN`），
   按累计 ACK 释放，超时整窗回退重传（go-back-N）。发送失败只当作丢段，
   不再判连接失败。接收窗口 2 KB 改为 8 KB。

## 测试（QEMU raspi3b + iMac M4）

* `mpitest`（2+2 rank）：全部通过。跨节点 8 字节单程延迟约 0.35 ms，64 KB
  流带宽约 3.3 MB/s；同节点 0.9 us（iMac）。
* `shell_mpi_benchmark`（4×2 网格，iMac 2 rank 加 xv6 2 rank）：u_error 5.8e-16，
  reaction_error 1.0e-11，末端挠度 -0.230292986591，与单机一致。墙钟 2.0 s，
  其中 MPI 1.25 s。
* `shell_mpi_benchmark -shell_benchmark`（16×8）：u_error 1.9e-13，墙钟 18.7 s，
  其中 MPI 12.1 s。瓶颈是 QEMU 的模拟网络，比 xv6 单独 4 rank（8.7 s）还慢。
  跨机在实际网络、单元计算更重的问题上才划算。
