# Git 提交记录：612a165

## 提交信息

- 分支：`rpi3-vfs`
- 提交：`612a165d65c33391b02e745e956bb78fe0de9e79`
- 日期：2026-10-01 12:42:07 -0500
- 提交说明：`epoll改为非阻塞`

该提交的主要工作是把 epoll 从“全局广播唤醒后扫描所有 watch”改为“按对象通知、实例级 ready 队列”，并将 `epollserver` 示例改造成使用非阻塞 socket 的事件循环。提交还调整了树莓派镜像安装行为，详见下文。

## epoll 内核实现

### 旧行为

内核维护一个全局 `epoll_generation` 和一个公共等待通道。TCP 或 PTY 状态变化时，`epollnotify()` 增加 generation 并唤醒该通道上的等待者。被唤醒后，每个 epoll waiter 都要遍历自己的 watch 数组，并逐个调用 `file_ready()` 检查 socket 或 PTY 是否就绪。

这种方式实现直接，但一次对象状态变化可能使无关 epoll 实例也醒来并扫描。每个实例最多只有 16 个 watch，因此当时线性扫描尚能工作，但无关唤醒会随实例和等待者增加而变多。

### 实例级 ready 队列

`kernel/epoll.c` 为每个 epoll 实例增加 ready 队列，队列项保存 watch 的 slot 和版本号。watch 本身新增 `queued` 状态，避免同一个 watch 在队列中重复出现。

- `EPOLL_CTL_ADD` 和 `EPOLL_CTL_MOD` 将 watch 放入队列，供 `epoll_wait` 立即重新检查其当前状态。
- `EPOLL_CTL_DEL` 移除该 watch 的待处理项；版本号递增，使并发操作遗留的旧队列项失效。
- `epoll_wait` 从队列取出最多 `maxevents` 个 watch，暂时释放 epoll 锁，再查询其真实 readiness。这样只检查被通知的对象，而不是每次扫描整个 watch 数组。
- 查询后重新取得 epoll 锁，并核对 watch 版本，避免 `MOD` 或 `DEL` 后使用过期的 watch 状态。
- LT watch 若仍然就绪，会再次入队；ET watch 通过 `last_ready` 与本次 readiness 比较，只报告新的就绪边沿。
- 没有待处理项且使用无限超时时，等待者睡在各自 epoll 实例的通道上；有新队列项时只唤醒该实例的等待者。

ready 队列的最大容量与 watch 数相同（16）。由于队列按 watch 去重，最多一个 watch 对应一个待处理项。

### 按 socket / PTY 对象通知

原来的无参数全局通知接口被替换为：

- `epollnotify_socket(handle)`：通知指定 TCP socket 的 watch。
- `epollnotify_pty(pty, master)`：通知指定 PTY 端点的 watch。

通知代码在 epoll 实例表和每个实例的 watch 表中寻找匹配对象，将匹配项排入对应实例的队列。TCP 的状态变化点改为传入连接 handle 通知，包括接收数据、FIN、ACK、发送队列状态变化、连接关闭和重传失败等。`accept` 从监听队列取走连接、TCP 读取释放接收缓冲区、TCP 写入改变发送队列后，也会通知监听 socket 或连接 socket，使 readiness 能随状态变化重新计算。

PTY 的读写或关闭会影响对端可读/可写状态，因此 PTY 通知辅助函数会同时通知 master 和 slave 两端。

### TCP 半关闭状态

`net_tcp_poll()` 现在也会在 TCP `CLOSE_WAIT` 状态报告可写；`net_tcp_write()` 在该状态仍接受待发送数据。这样客户端关闭发送方向后，服务端还能把已经收到、尚未回显的数据写回去，而不是仅因读到 EOF 就丢弃缓冲内容。

## `user/epollserver.c` 非阻塞事件循环

示例服务器现在将监听 socket 和每个 accepted socket 分别设置为 `O_NONBLOCK`。

- 监听 socket 收到可读事件后，循环调用 `socket_accept()`，直到暂时没有待处理连接。
- 每个客户端有独立的 4 KiB 输出缓冲区；可读时循环读取数据，直到缓冲区满或 `read()` 暂时无法继续。
- `write()` 若只写出部分内容，会保留剩余字节供之后续写。
- 客户端缓冲区有待发送内容时才注册 `EPOLLOUT`；缓冲区满时暂停 `EPOLLIN`，避免继续读取导致溢出。
- 读取到 EOF 后停止关注 `EPOLLIN`，等已缓冲数据写完再关闭连接。

由于 epoll 每实例最多观察 16 个 watch，而示例把监听 socket 也作为一个 watch，客户端表容量为 15。

## 文档与安装流程

`readme/readme_ssh_protocol.md` 更新了 epoll 和非阻塞示例的说明，并指出 sshd 的协议读写仍然是阻塞式 I/O；不能只给 sshd socket/PTY 设置 `O_NONBLOCK`，还需要把同步 packet 读写改造成可恢复的状态机。

`Makefile` 不再在未指定 `RPI3_XV6_DEV` 时把 `fs.img` 复制到 bootfs 的 `FS.IMG`。此时安装目标会跳过根文件系统并打印警告；只有明确指定原始 xv6 分区时才写入 `fs.img`。`readme/readme_raw_xv6_partition.md` 同步说明了此行为，目的是避免把根文件系统镜像放进 bootfs，也避免误写入错误位置。

## 提交文件范围

主要实现和文档文件：

- `kernel/defs.h`
- `kernel/epoll.c`
- `kernel/net.c`
- `kernel/pty.c`
- `user/epollserver.c`
- `readme/readme_ssh_protocol.md`
- `readme/readme_raw_xv6_partition.md`
- `Makefile`

提交还包含 `.idea/modules.xml`、`.idea/xv6-aarch64.iml` 和 `.DS_Store`。这些是 IDE / macOS 辅助文件，不参与 epoll 或内核运行逻辑。

## 当前限制

- 每个 epoll 实例仍最多注册 16 个 watch；TCP 连接表和 epoll 实例表也有固定容量。
- readiness 变化时需要遍历 epoll 实例和实例内 watch 来寻找订阅者。事件等待阶段不再全量扫描 readiness，但注册查找仍随实例数和 watch 数线性增长。
- 有限超时的 `epoll_wait` 仍使用 tick 通道等待超时；按对象的实例级唤醒用于无超时等待路径。
- `sshd` 仍使用阻塞式 socket/PTY I/O，本提交只将 `epollserver` 示例改为非阻塞事件循环。
