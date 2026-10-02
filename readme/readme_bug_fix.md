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
make kernel/kernel8-xv6_wifi.img
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

---

## SSH 中执行 `ps`，结果却输出到串口

### 现象

通过 SSH 登录后执行：

```sh
ps
```

SSH 客户端看不到进程列表，而连接在 Mini UART 上的 minicom 能看到输出。普通
`ls`、`cat` 等命令仍能在 SSH 客户端显示。

### fd、PTY 与 console 的关系

进程的输出位置由文件描述符指向的文件对象决定，而不是由“这个进程是否由
sshd 创建”决定。

SSH shell 创建时，`sshd`把子进程的三个标准fd复制到PTY slave：

```text
shell fd 0/1/2
  -> PTY slave
  -> PTY master
  -> sshd
  -> SSH_MSG_CHANNEL_DATA
  -> SSH客户端
```

因此普通用户态`printf()`最终调用`write(1, ...)`，输出会经过PTY送到SSH客户端。

串口登录进程的标准fd则绑定到`/dev/ttyS0`或`/dev/console`：

```text
login/sh fd 0/1/2
  -> /dev/ttyS0 或 /dev/console
  -> console字符设备
  -> Mini UART
  -> minicom
```

`/dev/tty`表示调用进程的控制终端：串口会话通常指向`ttyS0`，SSH会话指向该
会话的PTY slave。`/dev/console`则是系统console；当前平台最终连接Mini UART。

### 根因

旧`/bin/ps`没有通过标准输出打印，而是调用专用系统调用：

```text
/bin/ps
  -> ps() syscall
  -> sys_ps()
  -> procdump()
  -> kernel printf()
  -> console/Mini UART
```

内核`printf()`直接写系统console，不查看调用进程的fd 1。因此它绕过PTY和SSH
channel。即使`ps`是从SSH shell启动，输出也必然出现在串口。

### 修复

`sys_ps()`不再调用`procdump()`；它把进程快照格式化到内核临时缓冲区，再复制到
用户缓冲区。`/bin/ps`随后使用`write(1, ...)`输出：

```text
/bin/ps
  -> ps(buffer, size) syscall
  -> proclist()格式化快照
  -> copyout到用户缓冲区
  -> write(fd 1)
  -> fd 1
  -> PTY slave/master
  -> sshd channel
  -> SSH客户端
```

`procdump()`仍独立保留为内核诊断接口；串口输入`Ctrl-P`仍可直接打印进程表，
即使文件系统、PTY或用户进程已经异常也能使用。

### 验证

重新生成并安装包含新版`/bin/ps`的xv6文件系统后：

```sh
ssh root@192.168.0.201
ps
```

SSH客户端应看到类似：

```text
PID STATE NAME
1 sleeping init
2 sleeping kworker
```

同时串口不应出现这次`ps`的列表。串口按`Ctrl-P`时，内核诊断列表仍应只在
console上显示。

---

## 启动时 `panic: balloc: out of blocks`

### 现象与根因

系统完成设备和VFS初始化后，在`init`创建`/dev`、`/etc`配置或SSH主机密钥时
panic。旧配置的`FSSIZE=1000`，即原生xv6文件系统只有约1 MiB；新增用户程序
打包后已经占用约986个块，剩余空间不足以完成首次启动写入。

SD卡分区即使有400 MiB也不能解决该问题，因为可分配块数来自xv6 superblock的
`size`字段，而不是MBR分区长度。只更新内核也不会修改旧superblock。

### 修复

- `FSSIZE`从1000扩大到32768个1024字节块，即32 MiB；
- `fs.img`构建目标改为32 MiB；
- FAT32兼容容器的cluster映射上限同步提高；
- 重新运行`mkfs`生成包含新superblock和位图的完整镜像。

必须把新`fs.img`整体烧录到xv6原始分区：

```sh
make fs.img
make install-rpi3-rawfs RPI3_XV6_DEV=/dev/rdisk4s2
```

这会覆盖原xv6分区内容。若仍需旧系统中的用户文件，应先备份；不能只复制
`kernel8-xv6_wifi.img`，否则磁盘仍使用旧的1000块superblock。

---

## TFTP 偶发跳转到 `0x0505050505050505`

### 现象

第一次运行 `/bin/tftp` 时，下载刚开始便出现用户异常：

```text
elr=0x0505050505050505 far=0x0505050505050505
sz=0x4000 sp=0x4010
```

进程退出后再次运行可能又能完成下载，因此这不是稳定的服务器无响应或普通
TFTP 丢包。

### 分析

`0x05` 是本内核 `kalloc()` 为新分配物理页填写的调试模式。ELR、FAR、字符串
以及用户SP同时异常，说明用户控制流/栈内容遭到破坏，而不是收到了一条TFTP
ERROR报文。

原始`exec()`只分配两页：一页guard和一页4 KiB用户栈。`tftp`的`download()`
单个编译后栈帧已为`0x480`（1152）字节，其中包含516字节数据包、256字节RRQ
和128字节路径；它还会嵌套调用`printf()`、FAT32 `write()`及UDP系统调用。
这对已经扩展为网络/加密环境的用户程序缺少足够安全余量。

### 修复

- `param.h`增加`USTACKPAGES=4`；
- `exec()`现在分配一页不可访问guard加四页用户栈；
- `stackbase`覆盖完整四页栈，参数压栈仍执行下界检查；
- TFTP的packet、RRQ和目标路径缓冲移到进程私有BSS，`download()`不再把大块
  网络缓冲放在调用栈中。

新的进程映像布局为：

```text
ELF/heap | guard 4 KiB | user stack 16 KiB
```

### 验证

连续多次运行：

```sh
/bin/tftp
```

每次都应从`192.168.0.195`下载`kernel8-xv6_wifi.img`到`/boot`，不应再出现
`ELR/FAR=0x0505...`。如果扩大栈后仍复现，说明还存在独立的物理页生命周期或
并发破坏，需要进一步记录异常时用户栈L3 PTE对应物理页是否仍在`kmem.freelist`；
不能把第二次偶然成功当作问题已经消失。
