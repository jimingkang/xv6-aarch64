# xv6-aarch64 SSH-2 协议通信流程

本文聚焦 `rpi3-vfs` 分支中 `/bin/sshd` 与 OpenSSH 客户端之间的线上通信，不重复
介绍 Wi-Fi、TCP 和 VFS 的完整实现。主要代码位于：

- `user/sshd.c`：SSH 状态机、报文编解码、认证、channel 和 PTY relay；
- `user/ssh_crypto.c`、`user/tweetnacl.c`：SHA-256、HMAC、AES-CTR、X25519、Ed25519；
- `kernel/pty.c`：PTY master/slave 与最小终端行规程；
- `kernel/net.c`：TCP 字节流；
- `kernel/file.c`：socket、PTY 与普通文件描述符的统一读写入口。

实现参考 SSH Transport、Authentication 和 Connection 三层规范：
[RFC 4253](https://www.rfc-editor.org/rfc/rfc4253)、
[RFC 4252](https://www.rfc-editor.org/rfc/rfc4252)、
[RFC 4254](https://www.rfc-editor.org/rfc/rfc4254)，Curve25519 交换格式参考
[RFC 8731](https://www.rfc-editor.org/rfc/rfc8731)。

## 一、整体数据路径

```text
Mac OpenSSH client
        |
        | TCP/22 over Wi-Fi
        v
brcmfmac -> Ethernet -> IPv4 -> TCP -> FD_SOCKET
                                      |
                                      v
                                  /bin/sshd
                           decrypt / verify / channel
                                      |
                             PTY master fd[0]
                                      |
                       input queue <-> output queue
                                      |
                              PTY slave fd[1]
                               dup -> 0/1/2
                                      |
                                  /bin/sh
```

TCP 只提供可靠有序的字节流，不保留 SSH packet 边界。`packet_read()` 必须先读取包头，
得到 `packet_length`，然后继续读取剩余密文和 MAC；一次 `read()` 不等于一个 SSH 包。

## 二、完整握手时序

```mermaid
sequenceDiagram
    participant C as OpenSSH client
    participant S as xv6 /bin/sshd
    participant P as /etc/passwd
    participant T as PTY + /bin/sh

    C->>S: TCP three-way handshake
    S->>C: SSH-2.0-xv6-aarch64_0.1 CRLF
    C->>S: SSH-2.0-OpenSSH_... CRLF
    S->>C: SSH_MSG_KEXINIT (20)
    C->>S: SSH_MSG_KEXINIT (20)
    C->>S: SSH_MSG_KEX_ECDH_INIT (30), Q_C
    S->>S: X25519 -> K; SHA-256 -> H
    S->>S: Ed25519 host key signs H
    S->>C: SSH_MSG_KEX_ECDH_REPLY (31), K_S/Q_S/signature
    S->>C: SSH_MSG_NEWKEYS (21)
    C->>S: SSH_MSG_NEWKEYS (21)
    Note over C,S: 后续 packet 使用 AES-128-CTR + HMAC-SHA2-256
    C->>S: SERVICE_REQUEST ssh-userauth (5)
    S->>C: SERVICE_ACCEPT ssh-userauth (6)
    C->>S: USERAUTH_REQUEST none (50)
    S->>C: USERAUTH_FAILURE password (51)
    C->>S: USERAUTH_REQUEST password (50)
    S->>P: 验证 name/password
    P-->>S: home=/root, shell=/bin/sh
    S->>C: USERAUTH_SUCCESS (52)
    C->>S: CHANNEL_OPEN session (90)
    S->>C: CHANNEL_OPEN_CONFIRMATION (91)
    C->>S: CHANNEL_REQUEST pty-req (98)
    S->>C: CHANNEL_SUCCESS (99)
    C->>S: CHANNEL_REQUEST shell (98)
    S->>T: pty_open + fork + exec /bin/sh
    S->>C: CHANNEL_SUCCESS (99)
    C->>S: CHANNEL_DATA (94), keyboard bytes
    S->>T: write PTY master
    T->>S: shell output via PTY master
    S->>C: CHANNEL_DATA (94), terminal bytes
    C->>S: CHANNEL_EOF/CLOSE (96/97)
    S->>C: CHANNEL_EOF/CLOSE (96/97)
```

## 三、Identification 与明文 Binary Packet

双方先交换一行 identification：

```text
SSH-2.0-xv6-aarch64_0.1\r\n
SSH-2.0-OpenSSH_10.x\r\n
```

identification 不是 SSH binary packet，也不包含四字节长度。服务端允许客户端在
`SSH-` 行前发送有限数量的说明行，但拒绝 NUL、过长行和非 SSH-2.0 客户端。

未启用加密时，一个 packet 为：

```text
uint32 packet_length
byte   padding_length
byte[] payload
byte[] random_padding, 至少 4 字节
```

`packet_length` 不包括自身四字节，但包括 `padding_length` 字段、payload 和 padding。
初始阶段按 8 字节块对齐；AES-CTR 启用后按 16 字节块对齐。单包上限由当前教学实现
限制为 4096 字节。

## 四、KEXINIT 与算法协商

xv6 当前只公布一套可执行组合：

| 功能 | 算法 |
|---|---|
| Key exchange | `curve25519-sha256` |
| Server host key | `ssh-ed25519` |
| 双向加密 | `aes128-ctr` |
| 双向完整性 | `hmac-sha2-256` |
| 压缩 | `none` |

`SSH_MSG_KEXINIT` 包含 16 字节 cookie、10 个 name-list、
`first_kex_packet_follows` 和 reserved 字段。当前代码读取客户端 KEX 与 host-key
name-list 用于诊断，但没有实现通用“取双方第一个交集”的算法框架，因此客户端必须
接受上述组合。

## 五、Curve25519、交换哈希与主机身份

客户端发送 type 30：

```text
byte   SSH_MSG_KEX_ECDH_INIT = 30
string Q_C                       // 32-byte X25519 public key
```

服务端为每条连接从 BCM2837 RNG 取得临时私钥，生成 `Q_S` 并计算共享密钥 `K`。交换
哈希严格覆盖：

```text
H = SHA256(
      string V_C || string V_S ||
      string I_C || string I_S ||
      string K_S || string Q_C || string Q_S || mpint K)
```

其中 `I_C/I_S` 是包含 type 20 在内的完整 KEXINIT payload，`K_S` 是
`ssh-ed25519` host-key blob。服务端使用 `/etc/ssh_host_ed25519_key` 对 `H` 签名，
返回 type 31：

```text
byte   SSH_MSG_KEX_ECDH_REPLY = 31
string K_S
string Q_S
string signature_blob
```

客户端第一次连接会把主机公钥写入 `~/.ssh/known_hosts`。重新制作 `FS.IMG` 后如果
服务端重新生成主机密钥，OpenSSH 会报告 host key changed。确认目标确实是该开发板后，
应在客户端执行：

```sh
ssh-keygen -R 192.168.0.201
```

生产系统应把主机密钥保存在持久化、受保护的存储中，而不是随根镜像更新而丢失。

## 六、NEWKEYS、密钥派生和加密 Packet

第一次交换的 `H` 同时作为 `session_id`。按 RFC 4253 的字符 A--F 派生：

```text
A: client -> server IV
B: server -> client IV
C: client -> server AES key
D: server -> client AES key
E: client -> server MAC key
F: server -> client MAC key
```

服务端先以明文发送 `SSH_MSG_NEWKEYS`，发送完成后立即启用 outbound cipher；收到对端
明文 NEWKEYS 后启用 inbound cipher。两个方向拥有独立的 AES-CTR 状态和 MAC key。

加密阶段发送过程：

```text
plaintext_packet = packet_length || padding_length || payload || padding
mac = HMAC-SHA256(mac_key, sequence_number || plaintext_packet)
ciphertext = AES128-CTR(plaintext_packet)
wire = ciphertext || mac
```

接收端先连续解密 packet，再验证 MAC，验证通过后才把 payload 交给上层。MAC 本身不
加密。收发 sequence number 独立，从第一个 binary packet 开始递增，NEWKEYS 时不能
清零。

## 七、ssh-userauth 密码认证

客户端请求 `ssh-userauth` 服务后，OpenSSH 通常先用 `none` 方法探测：

```text
SSH_MSG_USERAUTH_REQUEST (50)
  string user
  string "ssh-connection"
  string "none"
```

xv6 返回 type 51，并声明当前只支持 `password`。密码请求增加：

```text
boolean FALSE        // 不是修改密码请求
string  password
```

服务端从 `/etc/passwd` 读取：

```text
name:password:uid:gid:gecos:home:shell
```

默认开发账号为 `root/xv6`。认证成功后保存 home 与 shell，发送 type 52。密码位于已经
加密且经过 HMAC 认证的 SSH transport 中，不会以 TCP 明文传输；但当前 `/etc/passwd`
本身仍保存教学用明文密码，不适合生产环境。

## 八、Session Channel 与流量窗口

客户端以 type 90 打开 `session`：

```text
string "session"
uint32 sender_channel
uint32 initial_window_size
uint32 maximum_packet_size
```

服务端分配 local channel 0，通过 type 91 返回自己的 channel 编号、64 KiB receive
window 和 2048 字节 maximum packet。此后 packet 中的 `recipient_channel` 必须使用
对端分配给自己的编号，不能混淆 local/remote channel。

收到客户端 type 94 数据后，服务端将 data 写入 PTY master，并通过 type 93
`CHANNEL_WINDOW_ADJUST` 归还已消费的窗口。shell 输出只有在客户端公布的 remote
window 足够时才能封装成 type 94；发送后相应扣减窗口。

## 九、pty-req、shell 与交互数据

`pty-req` 包含 terminal type、字符行列、像素尺寸和 terminal-mode blob。当前服务端
校验结构并回复成功，但尚未把 mode blob 转换为完整 termios。

收到 `shell` 请求后：

1. `pty_open()` 创建 master/slave；
2. `fork()` 创建 shell 子进程；
3. 子进程关闭 TCP 和 PTY master；
4. slave 依次 `dup()` 到 fd 0、1、2；
5. 切换到 passwd 指定的 home；
6. `exec("/bin/sh")`；
7. 父进程用 `epoll` 同时等待 TCP socket 与 PTY master。

键盘方向：

```text
OpenSSH -> encrypted CHANNEL_DATA -> sshd -> PTY master -> shell stdin
```

输出方向：

```text
shell stdout/stderr -> PTY slave -> PTY master -> sshd
                    -> encrypted CHANNEL_DATA -> OpenSSH terminal
```

PTY 最小行规程负责输入回显、Backspace、CR/LF、LF 到 CRLF 转换，以及最近 8 条命令
的上下箭头历史。历史属于单个 PTY，断开 SSH 后释放。

## 十、关闭过程

客户端可以发送：

- type 96 `SSH_MSG_CHANNEL_EOF`：不再发送数据；
- type 97 `SSH_MSG_CHANNEL_CLOSE`：关闭 channel。

服务端收到 EOF 后向 PTY 注入终止输入；收到 CLOSE 后回复 CLOSE，停止 relay，回收
epoll fd、PTY master 和 shell 子进程。shell 自行退出时，服务端先发 EOF 再发 CLOSE。

当前尚未完整返回 `exit-status`，所以部分客户端退出时可能提示远端没有提供退出码。

## 十一、实机运行与日志对应

xv6：

```sh
/bin/sshd
```

客户端：

```sh
ssh -vvv root@192.168.0.201
```

服务端成功日志与状态机对应关系：

```text
sshd: sent SSH_MSG_KEX_ECDH_REPLY type=31       # KEX reply
sshd: sent SSH_MSG_NEWKEYS; outbound ...        # S -> C 加密启用
sshd: received SSH_MSG_NEWKEYS; inbound ...     # C -> S 解密启用
sshd: encrypted transport ready; service ...    # ssh-userauth service
sshd: password authentication accepted ...      # type 52
sshd: session channel opened ...                # type 91
sshd: PTY request accepted                      # pty-req success
sshd: PTY shell started pid=...                 # /bin/sh 已运行
```

## 十二、当前边界与后续工作

当前实现可以完成 OpenSSH 密码登录和交互 shell，但仍是教学子集：

- 只支持一套 KEX/cipher/MAC/host-key 组合；
- 不支持 rekey、public-key authentication 和 keyboard-interactive；
- `/etc/passwd` 尚无 salted password hash；
- host key 还需要独立持久化；
- channel 暂不支持 `exec`、subsystem/SFTP、signal、完整 exit-status；
- PTY 没有完整 termios、window-change、controlling terminal 和 job control；
- sshd 串行服务连接，尚未实现成熟的每连接进程/线程模型；
- TCP 实现仍缺完整拥塞控制、SACK、window scaling 和多段乱序队列；
- 密码学实现和协议状态机尚未经过安全审计，不能作为生产 SSH 服务端。

## 附录 A：SSH 下层的 TCP、socket 与 epoll

SSH 依赖可靠的 TCP 字节流。当前内核已经实现 IPv4 protocol 6 分发、TCP
pseudo-header checksum、被动监听和三次握手，以及 ACK、PSH、FIN 和顺序数据接收。
TCP control block 最多 16 个，单连接接收缓冲为 2048 字节。

### A.1 socket 文件描述符与 backlog

内核仍用小整数 handle 索引 TCP control block，但 handle 已封装进
`struct file(FD_SOCKET)`，所以用户程序看到的是普通文件描述符：

```c
int listener = socket_listen(2222, 10);
int connection = socket_accept(listener);
read(connection, buffer, sizeof(buffer));
write(connection, buffer, length);
close(connection);
```

`fileread()`、`filewrite()` 和 `fileclose()` 分别转发到 TCP read、write 和 close；
`dup()` 与 `fork()` 沿用 `struct file` 的引用计数。监听 socket 和连接 socket 是两个
不同 fd：SYN 握手完成后，child connection 进入 listener 的 accept backlog，
`socket_accept()` 再为它分配 fd，listener 本身继续监听。

当前 TCP control block 仍记录创建者 pid。因此 sshd 父进程保留 accepted socket，
shell 子进程只继承 PTY slave，不应把网络 fd 直接交给 fork 后的 shell。

`fcntl(O_NONBLOCK)` 已可用于 socket；`epoll` 当前使用固定的 16 项 watch 数组，足以让
sshd 同时观察 TCP socket 和 PTY master，但还不是 Linux 那种红黑树加 ready-list 的
可扩展实现。

### A.2 重传与最小流重组

每个连接缓存一个尚未确认的 TX segment，包括 sequence、flags 和 payload。CPU0 的
timer 每 100 ms 调用 `net_tcp_tick()`；某段超过 500 ms 未收到推进 `snd_nxt` 的 ACK
时，用原 sequence number 重传，最多 5 次。`tcp_write()` 等待 ACK 后才提交下一段，
所以当前是可靠但吞吐较低的 stop-and-wait。

RX 对 `seq == rcv_nxt` 的数据立即追加到流缓冲；高于 `rcv_nxt` 的最近一段进入单个
out-of-order buffer。缺失段到达后再拼接，重复或乱序段都会触发当前 cumulative ACK。
这能支撑 SSH bring-up，但仍缺拥塞控制、RTT/RTO 自适应、TCP options、发送窗口和
多段乱序队列。

### A.3 用 tcpd 独立验证网络字节流

在调试 SSH 前，可以把加密和 PTY 排除在外：

```sh
# xv6
/bin/tcpd 2222

# 同一局域网中的 macOS/Linux
nc 192.168.0.201 2222
```

xv6 应打印 `tcpd: client connected`；客户端收到 `xv6 socket ready`，随后输入内容会
被原样回显。`tcpd` 只是 TCP 诊断程序，不是 Telnet 或 SSH 服务端。

## 附录 B：PTY 实现与独立测试

`pty_open(int fd[2])` 返回全双工 master/slave：

```text
sshd / SSH channel
       |
       | read/write + epoll
       v
  fd[0] PTY master
       | master 写 -> input queue  -> slave 读（shell stdin）
       | master 读 <- output queue <- slave 写（shell stdout/stderr）
       v
  fd[1] PTY slave -> dup 到 fd 0/1/2 -> /bin/sh
```

输入和输出队列各 512 字节，各有独立读写游标。队列空时 read 睡眠，队列满时 write
睡眠；任一端关闭会唤醒等待者并形成 EOF 或错误。PTY fd 支持 `dup()`、`fork()`、
`exec()` 和引用计数关闭，epoll 可报告 master 的 `EPOLLIN/EPOLLOUT/EPOLLERR`。

在板上运行以下命令可脱离 SSH 单独验证 PTY：

```sh
/bin/ptytest
```

预期输出为 `ptytest: ok`。测试覆盖双向数据、canonical 行提交、输入回显、ONLCR 和
上下箭头历史。当前仍没有完整 termios、窗口尺寸、信号字符、controlling terminal
和 job control，也没有让 `/dev/tty` 动态指向 PTY slave。

## 附录 C：Ed25519 与单页用户栈故障

初版把 identification、双方临时密钥、共享密钥、主机密钥和签名数组都放在
`serve_connection()` 栈帧中，该函数自身约占 `0x4c0` 字节。TweetNaCl 的 Ed25519
点加法还会在深层调用中申请约 `0x490` 字节，叠加曲线调用后超过 xv6 的单页用户栈，
写入 guard page 时产生 EL0 data-abort：

```text
esr=0x9200004f ec=0x24
elr=... add() in tweetnacl.c
far=sp=0x5ee0
```

正确修复不是开放 guard page，而是把每连接的 `struct ssh_kex_state` 移到堆上；返回前
清零临时私钥、共享密钥和 Ed25519 secret key，再释放内存。修复后主调用帧由约 1216
字节降至约 272 字节，guard page 仍能捕捉真正的栈越界。

加入 userauth/channel 后又出现一次同类故障，现场是 `ELR=add+0x18`、
`FAR=SP=0x7fe0`。原因不是密钥状态回到栈上，而是 `-Os` 把
`serve_connection()`、userauth 和 channel setup 内联进 `main()`：`main` 固定帧达到
992 字节，再叠加 TweetNaCl `add()` 的 1168 字节帧及上层曲线函数便越过 guard page。

现在对三个协议层边界使用 `noinline`，实测固定帧为 `main=32`、
`serve_connection=320`、`userauth=784`、`channel_session=320` 字节；relay 的 2048
字节终端缓冲也已移到堆上。这里的 `noinline` 不是性能优化，而是单页用户栈条件下
明确的栈深度隔离边界。
