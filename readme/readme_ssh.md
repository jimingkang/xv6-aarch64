# xv6-aarch64 SDIO Wi-Fi SSH 远程登录移植

## 当前阶段：TCP socket、epoll 与 PTY 基础

真正 SSH 必须运行在 TCP 字节流上。当前第一阶段已经加入：

- IPv4 protocol 6 接收分发；
- TCP pseudo-header checksum；
- 被动监听和三次握手；
- 顺序数据接收、ACK、PSH 和 FIN；
- 未确认段缓存、500 ms RTO 和最多 5 次重传；
- 重复 segment ACK 和一个乱序 segment 的重组；
- 阻塞式 `tcp_accept()`、`tcp_read()`；
- `tcp_write()`、`tcp_close()`；
- 用户态 `/bin/tcpd` 字节流回显测试；
- listener/accepted connection 使用不同文件描述符；
- listener backlog 与最多 16 个 TCP control block；
- `fcntl(O_NONBLOCK)` 和最多 16 个 watch 的 `epoll`；
- 双向 PTY master/slave 文件描述符以及 `/bin/ptytest`。
- `/bin/sshd` 的 SSH-2 identification、明文 binary packet 与 KEXINIT bring-up。

内核 TCP control block 仍由小整数 handle 索引，但 handle 已封装在
`struct file(FD_SOCKET)` 之后，用户程序只看到普通文件描述符。连接表最多 16 项，
单连接接收窗口为 2048 字节，适合验证 wlan0 上的 TCP 服务端和早期 SSH 握手。

## 实机验证

先确认板载 SDIO Wi-Fi 已经得到 IPv4 地址：

```text
brcmfmac: wlan0 IPv4 ready
dhcp: address=192.168.0.x ...
```

在 xv6 中启动：

```sh
/bin/tcpd 2222
```

应显示：

```text
tcp: listening on 2222 handle=1
tcpd: waiting on port 2222
```

在同一局域网的 macOS/Linux 主机连接：

```sh
nc 192.168.0.x 2222
```

三次握手完成后 xv6 打印 `tcpd: client connected`，客户端首先收到
`xv6 socket ready`，随后输入的内容会被原样返回。

## socket 文件描述符层

TCP 已接入 xv6 全局文件表：`struct file` 新增 `FD_SOCKET` 和 TCP connection
handle。用户态通过 `socket_listen(port, backlog)` 得到普通文件描述符，调用
`socket_accept(fd)` 完成握手。连接建立后直接使用统一文件接口：

```c
int listener = socket_listen(2222, 10);
int connection = socket_accept(listener);
read(connection, buffer, sizeof(buffer));
write(connection, buffer, length);
close(connection);
```

`fileread()`、`filewrite()` 和 `fileclose()` 分别路由到 TCP read、write 和 close，
因此后续 sshd 可以像操作管道和 TTY 一样操作网络连接。`dup()` 与 `fork()` 沿用
`struct file` 引用计数。当前 TCP control block 仍记录创建进程 pid，因此 sshd 暂时
由父进程持有网络 fd，shell 子进程只持有 PTY slave；不要把 accepted socket 直接
交给 fork 后的子进程。

listener 与 accepted child socket 已分离。SYN 完成后 child 进入 listener 的 accept
backlog，`socket_accept()` 为 child 创建新的 fd；listener 保持监听，可继续接受连接。

## PTY：网络会话与 shell 的边界

`pty_open(int fd[2])` 返回一对全双工文件描述符：

```text
sshd / SSH channel
       |
       | read/write + epoll
       v
  fd[0] PTY master
       | master 写入 -> input queue  -> slave 读取（shell stdin）
       | master 读取 <- output queue <- slave 写入（shell stdout/stderr）
       v
  fd[1] PTY slave -> dup 到子进程 fd 0、1、2 -> /bin/sh
```

两条队列各 512 字节，分别有独立的读写游标；队列空时 read 睡眠，队列满时 write
睡眠，对端关闭后唤醒等待者并产生 EOF/错误。PTY fd 支持 `dup()`、`fork()`、
`exec()` 和引用计数关闭。`epoll` 可以观察 PTY master 的 `EPOLLIN/EPOLLOUT/EPOLLERR`，
因此 sshd 可用一个事件循环同时处理多个 TCP channel 和 shell 输出。

在板上先运行：

```sh
/bin/ptytest
```

预期输出 `ptytest: ok`。测试覆盖双向数据路径、canonical 行提交、输入回显、ONLCR 和
上箭头历史召回；当前仍没有可配置 termios、窗口大小、信号字符、job control，也尚未
让 `/dev/tty` 动态指向 PTY slave。

## TCP 重传与流重组

每条连接保存一个尚未确认的 TX segment，包括 sequence、flags 和 payload。
`net_tcp_tick()` 每 100 ms 由 CPU0 timer 调用；超过 500 ms 未收到推进
`snd_nxt` 的 ACK 时，使用原始 sequence number 重发，最多重试 5 次。阻塞式
`tcp_write()` 只有在 ACK 到达后才提交下一段，因此当前是简单可靠的 stop-and-wait，
吞吐不高，但足以保证早期 SSH 握手报文不会因一个 Wi-Fi 丢包直接破坏字节流。

RX 对 `seq == rcv_nxt` 的数据立即追加到流缓冲。高于 `rcv_nxt` 的最近一段暂存在
out-of-order buffer；缺失段到达后自动拼接。重复或乱序段都会回复当前 cumulative
ACK，促使对端重传缺口。

## 当前 TCP 限制

本阶段是 SSH bring-up 用的最小服务端，不是完整 TCP/IP 实现。目前还缺少：

- 发送窗口、拥塞控制和 RTT/RTO；
- 多段 out-of-order queue（当前只保留一段）；
- TCP options（MSS、window scale、SACK、timestamp）；
- TCP control block 的所有权仍绑定创建进程 pid，尚未完全改成 open-file-description
  生命周期；
- PTY 尚缺 termios、窗口尺寸、信号字符和 controlling-terminal/job-control 语义。

因此先用 `nc` 验证基本路径。丢包环境下可能需要重新连接；在进入 SSH 大报文交换
前必须至少补齐发送重传、MSS 和多段流重组。

## 到真正 sshd 的后续层级

```text
wlan0 / brcmfmac
  -> Ethernet / ARP / IPv4
  -> TCP（当前阶段）
  -> socket fd、epoll 与 PTY（已完成基础层）
  -> SSH-2 identification/binary packet/KEXINIT（已完成 bring-up）
  -> X25519 + SHA-256 exchange hash + Ed25519 type 31（已完成）
  -> NEWKEYS + AES-128-CTR + HMAC-SHA2-256（已完成）
  -> password authentication（已完成）
  -> channel/session/pty-req/shell（已完成基础交互路径）
  -> /bin/sh（经 PTY master/slave 转发）
```

`tcpd` 只是 TCP 回显诊断程序，不是 Telnet，也没有冒充 SSH。标准 `ssh` 客户端现在
可以连接 22 端口，完成 identification、KEXINIT、type 31 和 NEWKEYS，并验证第一条
加密的 `SSH_MSG_SERVICE_REQUEST`。服务端随后用加密包返回
`SSH_MSG_SERVICE_ACCEPT("ssh-userauth")`。

## SSH-2 transport bring-up

`/bin/sshd` 当前实现 RFC 4253 的可测试前半段：

1. 在 TCP 22 端口监听，listener 与 accepted socket 分离；
2. 发送 `SSH-2.0-xv6-aarch64_0.1` identification；
3. 接受最长 255 字节的客户端 identification，并拒绝 NUL 和非 SSH-2.0；
4. 编解码尚未加密的 SSH binary packet；
5. 发送并解析 `SSH_MSG_KEXINIT`；
6. 输出客户端的 KEX 和 host-key name-list；
7. 接收 `SSH_MSG_KEX_ECDH_INIT`，严格检查 Curve25519 临时公钥为 32 字节；
8. 从 BCM2837 硬件 RNG 生成每连接 X25519 临时私钥，并拒绝全零共享密钥；
9. 首次启动生成 `/etc/ssh_host_ed25519_key`（32 字节原始 seed），以后复用；
10. 严格按 `V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K` 计算 SHA-256 `H`；
11. 用 Ed25519 主机密钥签名 `H`，返回 `SSH_MSG_KEX_ECDH_REPLY`（type 31）。

启动：

```sh
/bin/sshd
```

从另一台机器测试：

```sh
ssh -vvv -p 22 root@192.168.0.201
```

xv6 应看到类似：

```text
sshd: client SSH-2.0-OpenSSH_...
sshd: KEXINIT received
sshd: client kex=sntrup761x25519-sha512,...
sshd: client hostkey=ssh-ed25519,...
sshd: Curve25519 client public key received (32 bytes)
sshd: sent SSH_MSG_KEX_ECDH_REPLY type=31
```

密码原语来自 public-domain TweetNaCl；`user/sshcryptotest.c` 使用 RFC/NIST 向量验证
SHA-256、HMAC-SHA2-256、AES-128-CTR、X25519 和 Ed25519。测试程序为避免挤满
1000-block 教学文件系统，不默认打进 `fs.img`。

type 31 完成后，以首次交换的 `H` 作为 `session_id`，按 RFC 4253 的 A--F 标识分别
派生 client/server IV、AES key 和 MAC key。发送 NEWKEYS 后立即启用 server-to-client
加密；收到对端 NEWKEYS 后启用 client-to-server 解密。两个方向保持独立 AES-CTR
计数器和从第一个 binary packet 开始、NEWKEYS 时不清零的 `uint32` sequence number。
MAC 输入为 `sequence_number || plaintext_packet`，验证通过后才向上交付 payload。

## 密码认证、SSH channel 与 PTY shell

NEWKEYS 后，`sshd` 已继续实现 RFC 4252/RFC 4254 的交互式登录主路径：

1. 严格接收 `SSH_MSG_SERVICE_REQUEST("ssh-userauth")` 并返回 service accept；
2. 处理 OpenSSH 首先发送的 `none` 探测，以 type 51 返回仅支持 `password`；
3. 解析 type 50 中的 username、`ssh-connection`、method、change-password 标志和密码；
4. 使用 `/etc/passwd` 的 `name:password:uid:gid:gecos:home:shell` 记录认证，最多尝试
   6 次；日志只记录成功用户名或失败，不输出密码；
5. 成功时发送 type 52，然后接受 type 90 的 `session` channel；
6. 返回 channel-open-confirmation，同时声明 64 KiB receive window 和 2048 字节
   maximum packet；
7. 解析 `pty-req`；当前接受终端名、行列数和 mode blob；PTY 使用固定的最小行规程，
   尚未把 mode blob 转换为可配置 termios；
8. 对 `shell` 请求创建 PTY pair 并 fork：shell 子进程将 slave `dup` 到 fd 0/1/2，
   SSH 父进程保留 master 和加密 TCP socket；
9. 父进程用 epoll 同时等待 TCP 与 PTY master，把 type 94 channel data 写入 PTY，
   把 shell 输出封装为 type 94，并处理 window-adjust、EOF 与 close；
10. OpenSSH 常发出的 `env` 请求会得到 channel-failure，但不会导致 session 中断。

默认开发账号来自 `init` 创建的配置：

```text
root:xv6:0:0:root:/root:/bin/sh
```

启动服务端后，可从另一台机器交互登录：

```sh
/bin/sshd
ssh -p 22 root@192.168.0.201
```

当前密码数据库是教学用明文格式，不适合生产用途；尚缺 salted password hash、用户
UID/GID/权限切换、public-key authentication、termios/window-change、signal、exit-status、
多 SSH 会话并发和完整 disconnect reason 处理。

### 最小 PTY 行规程

`kernel/pty.c` 对 SSH 写入 master 的字节实现一个固定 canonical 模式：

- 普通字符进入每个 PTY 独立的 128 字节编辑缓冲，并回显到 master output；
- Backspace/DEL 删除编辑缓冲最后一个字符，并回显 `BS SPACE BS`；
- CR 或 LF 提交整行，统一向 slave 交付 LF；紧随 CR 的 LF 会被折叠；
- slave 输出中的裸 LF 转换为 CRLF，已有 CRLF 不重复插入 CR；
- 每个 PTY 保存最近 8 条非空命令，相邻重复命令不再次保存；
- `ESC [ A`/`ESC [ B`（上/下箭头）在内核编辑缓冲中切换历史并重绘当前行。

历史放在 PTY 而不是全局 shell 中，因此不同 SSH 会话互不共享，关闭 PTY 后历史即
释放。Linux 通常由处于 raw/noncanonical 模式的 readline/shell 完成历史编辑；这里把
它放在 PTY 是为了在尚无完整 termios/readline 时提供可用的教学实现。

## Ed25519 单页用户栈故障

初版把 identification、双方临时密钥、共享密钥、主机密钥和签名数组全部声明在
`serve_connection()` 栈帧中，使该函数自身占用约 `0x4c0` 字节。TweetNaCl 的
Ed25519 点加法还会在深层调用中申请约 `0x490` 字节，叠加其他曲线函数后超过 xv6
单页用户栈，第一次写入下面的 guard page 时产生 EL0 data-abort：

```text
esr=0x9200004f ec=0x24
elr=... add() in tweetnacl.c
far=sp=0x5ee0
```

修复不是开放 guard page，而是把每连接的 `struct ssh_kex_state` 放到堆中；函数返回前
清零其中的临时私钥、共享密钥和 Ed25519 secret key，再释放内存。修复后主调用帧从
约 1216 字节下降到约 272 字节，guard page 继续负责捕捉真正的栈越界。

加入 userauth/channel 后曾再次出现同类故障，现场为 `ELR=add+0x18`、
`FAR=SP=0x7fe0`。这次并非密钥状态重新回到栈上，而是 `-Os` 将
`serve_connection()`、userauth 和 channel setup 内联进 `main()`：stack-usage 报告显示
`main` 固定帧达到 992 字节，再调用 TweetNaCl `add()` 的 1168 字节帧及其上层曲线函数
后越过 guard page。现在对这三个协议层边界使用 `noinline`，实测固定帧分别为
`main=32`、`serve_connection=320`、`userauth=784`、`channel_session=320` 字节；同时把
relay 的 2048 字节终端缓冲移到堆上，使其即使被内联也不再携带大数组。这里的
`noinline` 不是性能优化，而是当前单页用户栈下明确的栈深度隔离边界。
