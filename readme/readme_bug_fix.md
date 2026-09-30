# xv6-rpi3 问题修复记录

## SSH 远程终端逐字符回显缓慢

### 现象

SSH 已经能够完成密钥交换、密码认证并建立 PTY shell，但是远程输入时，字符经常延迟约 100～300 ms 才显示。连续输入时表现为字符断续出现，命令输出通常比本地串口慢得多。

这不是 SSH 客户端本地回显问题。SSH 终端默认由服务端 PTY 回显，因此每个按键都必须完成下面的路径：

```text
客户端按键
  -> 无线帧到达 BCM43455/MT7601U
  -> xv6 CPU 轮询网卡
  -> TCP 接收并唤醒 sshd
  -> SSH 解密 CHANNEL_DATA
  -> 写入 PTY master
  -> PTY 行规程产生字符回显
  -> sshd 读取 PTY master
  -> SSH 加密 CHANNEL_DATA
  -> TCP 发送
  -> 客户端显示
```

### 根因一：网络设备只有每 100 ms 才被轮询

原来的 ARM Generic Timer 周期为 100 ms，CPU0 在每次 timer IRQ 中调用：

```c
netdev_poll_all();
mt7601u_poll();
net_tcp_tick();
```

BCM43455 和当前尚未完全 IRQ 化的 USB Wi-Fi 接收路径依赖该轮询。一个刚到达的按键平均需要等待约 50 ms，最坏等待接近 100 ms，返回的回显还可能再等待一个轮询周期。

### 根因二：TCP `write()` 同步等待 ACK

原来的 `net_tcp_write()` 只有一个未确认发送槽。调用者发送一个 TCP 段后，会执行：

```c
while(c->tx_unacked && !c->tx_failed)
  sleep(c, &c->lock);
```

因此 `write()` 不是“数据复制到 socket 发送缓冲后返回”，而是“对端确认后才返回”。PTY 的单字符回显会形成很小的 SSH 包，每个小包都可能支付一次完整网络往返时间。

加密 SSH 包还包含密文和 32 字节 HMAC。即使连续调用两次 `write()`，第二次也会被前一个未确认段阻塞。

## 修复

### 1. 10 ms 设备轮询，保留 100 ms xv6 tick

修改 `kernel/timer.c`：

- ARM Generic Timer 的硬件周期从 100 ms 改为 10 ms；
- 每次 10 ms 中断在 CPU0 执行网络设备轮询；
- 每累计 10 次硬件中断才形成一个 xv6 `ticks`；
- `net_tcp_tick()` 仍每 100 ms 调用一次，因此原来的 500 ms TCP 重传超时没有被意外缩短；
- 只有每第十次 timer IRQ 返回调度时钟事件，保持原有 100 ms 抢占节奏。

```text
10 ms hardware timer IRQ
  +-> netdev_poll_all()          每次调用
  +-> mt7601u_poll()             每次调用（注册 wlan1 前）
  +-> divider == 10 ?
        +-> ticks++              每 100 ms
        +-> net_tcp_tick()       每 100 ms
        +-> scheduler preempt    每 100 ms
```

所以 `sleep(n)`、`uptime()` 和 TCP 重传计时仍采用原来的 100 ms 单位，不会因为降低网络轮询延迟而快十倍。

### 2. 添加 TCP 异步发送队列

修改 `kernel/net.c`，每条 TCP 连接增加 8 KiB `txq`：

```text
user write()
  -> copyin 到 txq
  -> 如果当前没有未确认段，立即启动一个 MSS 段
  -> write() 在数据成功入队后返回

TCP ACK
  -> 释放当前重传槽
  -> 从 txq 启动下一个 MSS 段
  -> 唤醒等待发送队列空间的 writer
```

仍然只允许一个 TCP 段处于未确认状态。这保留了当前简化 TCP 的 stop-and-wait 重传模型，但用户进程不再为每个小包同步等待 ACK。

队列相关行为：

- 阻塞 socket：仅在 8 KiB 队列装满时睡眠；
- 非阻塞 socket：写入当前可用空间后立即返回，队列满时返回 `-1`；
- `EPOLLOUT`：发送队列存在空间时就绪，不再要求网络上完全没有未确认段；
- `close()`：等待队列与当前未确认段全部排空，再发送 FIN；
- 重传最终失败：清空发送队列、设置 `tx_failed` 并唤醒等待者；
- 对端已经发送 FIN 的 `TCP_CLOSE_WAIT` 状态仍允许排空此前入队的数据。

### 修复后的交互延迟

理论上的设备发现延迟由 `0～100 ms` 降为 `0～10 ms`。小型 SSH 回显包入队后立即返回，不再把一次 TCP ACK 往返串行加入每个字符的处理路径。

这不是完整的 Linux TCP 实现。目前仍有以下限制：

- 同一连接最多只有一个在途 TCP 段；
- 没有拥塞窗口、滑动发送窗口和 RTT 自适应 RTO；
- Wi-Fi 数据接收仍是 10 ms CPU 轮询，而不是真正的 SDIO/USB IRQ；
- 大量输出填满 8 KiB 队列后仍会产生背压，这是预期行为。

最终方案仍应为 BCM43455 SDIO IRQ 和 DWC2 host-channel IRQ 驱动接收，同时把 TCP 扩展为滑动窗口发送。

## 验证

代码必须先通过：

```sh
make kernel/kernel8.img
```

安装到 BOOTFS 并启动后：

```sh
ssh root@192.168.0.201
```

检查项目：

1. 单个字符应基本立即回显，不再呈现固定约 100 ms 的阶梯延迟；
2. 快速输入一整行不应丢字、乱序或重复；
3. `ls -l /bin` 等多行输出不应导致连接挂死；
4. 上下箭头历史、退格和回车的 PTY 行规程仍正常；
5. 空闲超过 TCP 重传周期后连接仍可继续交互；
6. 退出 shell 后 SSH channel、TCP FIN 和进程回收正常。

