# xv6-rpi3 运行时故障与修复记录

本文件只记录系统实际运行时遇到的问题，并按“现象 → 复现/日志 → 根因 → 修复 →
验证”的顺序整理。设备架构、协议原理和移植过程不放在这里，而归入相应专题
文档。尚未经过真机验证的判断必须明确标记，不能写成已经解决。

## SSH 远程终端逐字符回显缓慢

### 现象

SSH 已经能够完成密钥交换、密码认证并建立 PTY shell，但是远程输入时，字符经常延迟约 100～300 ms 才显示。连续输入时表现为字符断续出现，命令输出通常比本地串口慢得多。

这不是 SSH 客户端本地回显问题。SSH 终端默认由服务端 PTY 回显，因此每个按键都必须完成下面的路径：

```text
客户端按键
  -> 无线帧到达网络设备
  -> xv6 CPU 处理网卡接收
  -> TCP 接收并唤醒 sshd
  -> SSH 解密 CHANNEL_DATA
  -> 写入 PTY master
  -> PTY 行规程产生字符回显
  -> sshd 读取 PTY master
  -> SSH 加密 CHANNEL_DATA
  -> TCP 发送
  -> 客户端显示
```

### 根因一：早期网络设备只有每 100 ms 才被轮询

早期 ARM Generic Timer 周期为 100 ms，CPU0 在每次 timer IRQ 中调用：

```c
netdev_poll_all();
net_tcp_tick();
```

当时的 Wi-Fi 接收路径依赖该轮询。一个刚到达的按键平均需要等待约50 ms，最坏
等待接近100 ms，返回的回显还可能再等待一个轮询周期。

### 根因二：TCP `write()` 同步等待 ACK

原来的 `net_tcp_write()` 只有一个未确认发送槽。调用者发送一个 TCP 段后，会执行：

```c
while(c->tx_unacked && !c->tx_failed)
  sleep(c, &c->lock);
```

因此 `write()` 不是“数据复制到 socket 发送缓冲后返回”，而是“对端确认后才返回”。PTY 的单字符回显会形成很小的 SSH 包，每个小包都可能支付一次完整网络往返时间。

加密 SSH 包还包含密文和 32 字节 HMAC。即使连续调用两次 `write()`，第二次也会被前一个未确认段阻塞。

### 修复

#### 1. Timer 保留 100 ms xv6 tick，网络维护改用 delayed work

硬件 Timer 以 10 ms 为 workqueue 时基，但每累计 10 次才形成一个 xv6 `ticks`：

```text
10 ms hardware timer IRQ
  +-> workqueue_timer_tick()   推进 delayed-work 时钟
  +-> divider == 10 ?
        +-> ticks++            每 100 ms
        +-> scheduler preempt  每 100 ms
```

所以 `sleep(n)`、`uptime()` 和进程抢占仍采用原来的 100 ms 语义。TCP 重传使用
`net_wq` 的 ordered delayed work，Timer hard IRQ 不再直接运行 TCP 或设备驱动。

#### 2. 添加 TCP 异步发送队列

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

仍然只允许一个 TCP 段处于未确认状态。这保留了当前简化 TCP 的 stop-and-wait
重传模型，但用户进程不再为每个小包同步等待 ACK。

队列相关行为：

- 阻塞 socket：仅在 8 KiB 队列装满时睡眠；
- 非阻塞 socket：写入当前可用空间后立即返回，队列满时返回 `-1`；
- `EPOLLOUT`：发送队列存在空间时就绪，不再要求网络上完全没有未确认段；
- `close()`：等待队列与当前未确认段全部排空，再发送 FIN；
- 重传最终失败：清空发送队列、设置 `tx_failed` 并唤醒等待者；
- 对端已经发送 FIN 的 `TCP_CLOSE_WAIT` 状态仍允许排空此前入队的数据。

#### 修复后的交互延迟

BCM43455 的接收现在由 SDIO DAT1 IRQ 立即触发，TCP 小包写入发送队列后立即
返回，不再把固定轮询周期和一次 TCP ACK 往返串行加入每个字符的处理路径。

目前 TCP 仍有以下限制：

- 同一连接最多只有一个在途 TCP 段；
- 没有拥塞窗口、滑动发送窗口和 RTT 自适应 RTO；
- 大量输出填满 8 KiB 队列后仍会产生背压，这是预期行为。

### 验证

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
