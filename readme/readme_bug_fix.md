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

第一次运行旧命令 `/bin/tftp`（现已改名为 `/bin/tftpclient`）时，下载刚开始便出现用户异常：

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
/bin/tftpclient
```

每次都应从`192.168.0.195`下载`kernel8-xv6_wifi.img`到`/boot`，不应再出现
`ELR/FAR=0x0505...`。如果扩大栈后仍复现，说明还存在独立的物理页生命周期或
并发破坏，需要进一步记录异常时用户栈L3 PTE对应物理页是否仍在`kmem.freelist`；
不能把第二次偶然成功当作问题已经消失。

---

## 相机转换期间偶发 `SDIO CMD52 failed status=df0001`

### 现象

OV5647 已经通过 Unicam DMA 得到完整帧，`camshot` 也完成 RAW10 解包，随后在用户态
统计白平衡和亮度时，串口偶发：

```text
camshot: calculating white balance and level...
arasan-sdio: request CMD52 failed status=df0001 irq=100 c1=e0807
```

### 容易产生的误判

不能仅根据两行日志相邻，就判断普通用户程序 `camshot` 抢占了内核 Wi-Fi worker。
当前调度器中：

- `camshot` 默认是 `SCHED_OTHER/nice 0`；
- `brcmf0_wq` kworker 通过 `kthread_create()` 创建后同样默认是
  `SCHED_OTHER/nice 0`；
- 用户态和内核线程使用同一套调度实体，“运行在用户态”不自动代表最低优先级；
- SDIO IRQ 上半部仍可立即打断 `camshot`；
- 当前 `nice` 只改变 `SCHED_OTHER` 时间片长度，不改变其有效优先级。

因此，CPU 调度可能改变并发时序，但不足以解释控制器寄存器为什么处于
`CMD_INHIBIT`。此前为 `camshot` 加 `sched_yield()` 的处理已经撤回；它不能修复 SDIO
host 的事务并发。

### 根因

`sdio_cmd52()` 与 `sdio_cmd53()` 原来直接调用 Arasan host 的 `request()`，没有
host 级互斥。BCM43455 的状态 delayed work、NAPI RX/TX 和 BCDC 控制请求可能在多个
CPU 上同时运行，而 Arasan 只有一套共享寄存器：

```text
ARG1 / CMDTM / INTERRUPT / DATA / BLKSIZECNT
```

一个请求还未结束时，另一个请求可能清中断状态或覆盖参数/命令寄存器。
`status=df0001` 的 bit 0 是 `CMD_INHIBIT`，`irq=100` 只有 SDIO card interrupt、没有
本次 CMD_DONE，符合事务重叠或前一事务尚未结束的表现。

### 修复

- `struct mmc_host` 增加 `request_lock` sleeplock；
- MMC core 新增 `mmc_request_sync()`；
- CMD52 和 CMD53 的完整命令/数据事务都通过该入口串行执行；
- 锁粒度是物理 MMC host，而不是单个 SDIO function 或单个 brcmf 设备；
- 使用 sleeplock 而不是 spinlock，因为 host request 可能等待命令、数据和超时，期间
  不能长期禁止中断或调度；
- 启动枚举发生在 `userinit()` 之前，此时 `myproc()` 为空且只有启动 CPU 提交 MMC
  请求，所以启动阶段直接执行 request；进入进程、NAPI 和 workqueue 上下文后才持有
  sleeplock。

该设计对应 Linux MMC core 的 host claim 思路：

```text
brcmf control/state work ─┐
brcmf NAPI RX/TX         ├─ mmc_request_sync()
SDIO function control   ─┘       │
                                 └─ mmc_host.request_lock
                                      └─ Arasan request
```

### 验证

更新内核后，同时保持 Wi-Fi 联网并反复运行：

```sh
/bin/camshot -t 1 -s
ping 192.168.0.1
```

相机用户态处理不应再诱发可复现的 CMD52/CMD53 重叠错误。如果仍偶发 CMD52 超时，
应继续检查 Arasan 的 ERROR 位、命令恢复和 BCM43455 状态，而不能再次仅凭日志时间关系
归因于 `camshot` 的优先级。SDIO 总线、NAPI 和 workqueue 的完整结构见
`readme_sdio_wifi.md`。

---

## 多核下用户程序执行到非访存指令却发生随机地址 Data Abort

### 现象

更新 `/bin/camshot` 后，程序可能在打印第一行之前异常：

```text
usertrap(): unexpected esr=0x92000004 ec=0x24
elr=0x0000000000000b40 far=0xf3910003fda9a57b
```

用当前 ELF 反汇编，`ELR=0xb40` 对应 `strncpy()` 中的纯寄存器 `sub`，该指令不访问
内存，不可能产生 Data Abort。`FAR` 又是明显的随机指针。这说明 CPU 实际执行的指令流
与磁盘上当前 ELF 的反汇编不一致。

### 根因

ELF 由内核高地址 direct map 写入物理页，用户程序通过低 VA 执行。旧的
`uvmsync_icache()` 已经执行 D-cache clean，但最后使用：

```asm
ic iallu
```

`IALLU` 只失效执行该指令的本地 PE 的 instruction cache。启用四核和进程迁移后，exec
通常在一个 CPU 上装入新映像，进程随后可能在另一个 Cortex-A53 上运行。另一个 CPU 仍可
命中相同低用户 VA 对应的旧 I-cache 指令，于是实际执行旧程序字节，却使用新程序的数据、
栈和页表，最终访问随机地址。

### 修复

保持逐页、逐 cache line 的 `dc cvau` 和前置 `dsb ish`，把本地 I-cache 失效改为
inner-shareable 广播：

```asm
dc cvau, kernel_alias       // 对所有装载页逐 cache line
dsb ish
ic ialluis                  // 广播到 inner-shareable 域内所有 CPU
dsb ish
isb
```

`exec()`、`uvminit()` 和 `uvmcopy()` 都调用 `uvmsync_icache()`，因此新用户映像及 fork
得到的可执行页在迁移到任意 CPU 前都不会命中旧指令。这里不能用高地址执行
`ic ivau` 来代替：用户取指使用的是低 VA alias；也不能继续使用 local-only `ic iallu`。

### 验证

反复启动不同的、代码尺寸相近且都链接到 VA 0 的用户程序，并允许其在所有核之间迁移：

```sh
/bin/camshot -t 1 -s
/bin/ls
/bin/camshot
```

### `camshot` 在 fixed WB/level 后看似停止

若日志停在：

```text
camshot: sensor test pattern; fixed WB/level
```

此时像素转换尚未开始。程序下一步先以 `O_TRUNC` 打开 `/boot/camera.bmp`。旧的 FAT32
链释放代码对每个 cluster 分别执行 FAT 读取以及两份 FAT 的读改写；覆盖一个已有的
320x240 BMP 时可能产生五百多次 SD 扇区事务，因此表现为长时间没有串口输出。

修复后 `fat_free_chain()` 缓存当前 FAT 扇区，在其中批量清除属于旧文件的链项，再把整个
扇区同步写入各 FAT 副本。`camshot` 同时在打开输出文件、写 BMP 头和开始转换前增加阶段
日志。新用户程序的版本标志是：

```text
camshot: converter fixed-pattern-v2 fat-progress batch16
```

### OV5647 软件 SCCB 偶发错误 ID 与 BUS-STUCK

GPIO SCCB 曾出现 address ACK 后读出 `chip-id=3631`，随后诊断扫描显示 BUS-STUCK。`0x3631`
不是有效 OV5647 ID，说明事务收到 ACK 但数据位采样不稳定。旧实现还会在每个软件事务前无条件
发送 9 个恢复时钟，并在错误 ID 后立即扫描整个 7-bit 地址空间，给边缘状态的模块增加额外扰动。

`gpio-sccb-v7` 将软件时钟降至约 25 kHz；总线空闲时不再发送恢复时钟，只有 SDA 被保持为低
才产生最多 9 个 SCL 脉冲，之后统一发送 STOP。非 `0x5647` 的完整读数会被拒绝并重新读取；
已经完成过 0x36 寄存器事务但 ID 不稳定时直接进入下一次断电/上电重试，不再全地址扫描。
电源低电平和上电稳定等待分别延长到 20 ms 与 100 ms。新版启动标志为：

```text
camera: driver gpio-sccb-v8 unicam-poll-v2
```

v7 实机仍可能出现 `0x36` address probe 为 ACK、但三次芯片 ID 读取均失败。这是因为每次重启仍
先执行多次已知会失败的 BSC0 事务，再切换 GPIO SCCB；BSC 状态机的半截事务可能让传感器进入
不稳定状态。v8 从第一次 `0x300a/0x300b` 读取开始就使用已验证的 25 kHz GPIO SCCB，不再先用
BSC0 扰动模块。如果 `0x36` 已 ACK 但寄存器读仍失败，本轮立即断电并交给 `camera_bind()` 做
下一次完整电源周期，不再探测其它地址或扫描整个 7-bit 总线。正常的新日志开头为：

```text
ov5647: probing with 25 kHz GPIO SCCB; BSC0 bypassed
ov5647: software SCCB power-on chip-id=5647
```

### 创建 `/boot/camNN.bmp` 停在 directory-entry begin

#### 现象

相机已经完成采集和 RAW10 转换准备，但在 FAT32 bootfs 中创建输出文件时停止：

```text
camshot: frame received 640x480, 384040 bytes
camshot: unpacking RAW10...
camshot: sensor test pattern; fixed WB/level
camshot: preparing output /boot/cavfs: boot create begin path=/boot/cam02.bmp sub=/cam02.bmp mode=601
m02.bmp (230454 bytes)...
vfs: boot create stat complete exists=0
vfs: boot create directory-entry begin
```

其中 `/boot/cavfs: ... m02.bmp` 是 `camshot` 用户进程与 VFS 内核日志同时写串口造成的字符交错；
实际传给 VFS 的路径仍是 `/boot/cam02.bmp`，不是文件名或用户缓冲区被破坏。

`exists=0` 证明 `fat32stat()` 已经返回且目标文件尚不存在；日志停在
`directory-entry begin` 与 `directory-entry complete` 之间，因此阻塞点位于
`fat32createfile()`，而不是 CSI/Unicam 采集、RAW10 转换、mount 选择或文件数据写入。

#### 根因

第一轮只优化 cluster allocator 后，实机仍然停在同一行，并且连新增的
`fat32: extending root directory...` 都没有打印。这否定了“已经进入 cluster 扫描，只是扫描很慢”
这一单一判断；实际首先发生的是 FAT32 create 调用链的内核栈耗尽。

每个进程的 xv6 内核栈只有一个 4 KiB page。旧 `fat32createfile()` 在栈上同时声明了
`fat_path_match`、22 个 `fat_dir_slot` 和 256-byte 文件名；它再嵌套调用 `fat_find_path()`，后者又
在栈上声明 256-byte 文件名、520-byte LFN 字符数组和 20 个 `fat_dir_slot`。仅这些显式数组就
接近 3.5 KiB，加上 VFS、syscall、函数保存寄存器和其它局部变量后会越过 4 KiB。由于越界首先
破坏相邻内核内存，不一定马上产生 Data Abort，表现可以只是停在调用 `ops->create()` 之后。

外层 `ops->stat()` 能成功不与此矛盾：stat 与 `fat_find_path()` 的大栈帧不是嵌套在
`fat32createfile()` 的大栈帧中；create 才会同时保留两层大型临时数组。

修复栈问题后，新建 FAT32 文件仍可能需要在目录中保留短文件名/LFN 所需的目录项，并保留后续的
目录结束标记。若根目录当前 cluster 尾部没有足够的连续空槽，`fat_reserve_root_slots()` 会调用
`fat_alloc_cluster()` 扩展目录 cluster 链，这才会进入第二个性能问题。

旧 allocator 的 `next_alloc_cluster` 固定从 cluster 2 开始，而且逐个候选调用 `fat_next()`。
FAT32 的一个 512-byte FAT 扇区包含 128 个 32-bit 表项，这种写法会把同一个 FAT 扇区重复读取
最多 128 次。bootfs 前部已有 Raspberry Pi 固件、内核与 Wi-Fi firmware 时，找到尾部空 cluster
可能产生数千次 polled SD 事务，在串口上表现为长时间没有新日志。

这与“覆盖一个已经存在的 BMP 时停在 truncate”不是同一个问题：

- `exists=1` 且停在 `truncate begin`：走 `fat_free_chain()`，需要释放原文件的 cluster 链；
- `exists=0` 且停在 `directory-entry begin`：走 `fat_reserve_root_slots()`，可能需要扩展目录并分配
  新 cluster。

#### 修复

首先将路径解析、LFN 字符、LFN 槽位、create match 和 create 槽位数组移入 FAT32 私有
`fat_work` 工作区。所有 FAT32 操作已经由 `fat32_lock` 串行化，因此工作区不会被两个调用者同时
修改，也不需要扩大每个进程的内核栈。修改后反汇编显示两个关键函数的栈帧分别下降为：

```text
fat32createfile: 464 bytes
fat_find_path:   176 bytes
```

create 路径增加了更靠前的诊断点：

```text
fat32: create lookup begin path=/cam03.bmp
fat32: create lookup complete exists=0
fat32: create reserve begin slots=2
fat32: create reserve complete
```

这样即使后续仍有故障，也可以直接区分栈损坏、路径查找、目录槽位保留和 cluster 分配。

`setup_fat32()` 现在读取 FSInfo 扇区并校验 lead signature `0x41615252` 和 structure signature
`0x61417272`，然后采用 offset 492 的 next-free cluster hint。hint 只是建议值：越界、签名错误或
指向已占用 cluster 时，allocator 仍会环绕执行完整扫描。

`fat_alloc_cluster()` 改为按 FAT 扇区缓存扫描：一次读取 128 个候选表项，只在跨越 FAT 扇区时
重新读盘。找到空 cluster 后仍会更新两个 FAT 副本、清零新 cluster，并推进内存中的
`next_alloc_cluster`。`fat_reserve_root_slots()` 同时增加目录扩展前后的诊断日志：

```text
fat32: allocator next-free hint=...
fat32: extending root directory after cluster ...
fat32: root directory extended with cluster ...
```

VFS 的分阶段日志保留用于区分 create、truncate、verify-stat 和 openwrite，正常创建应继续出现：

```text
fat32: create lookup begin path=/cam03.bmp
fat32: create lookup complete exists=0
fat32: create reserve begin slots=2
fat32: create reserve complete
vfs: boot create directory-entry complete
vfs: boot create verify-stat begin
vfs: boot create verify-stat complete
vfs: boot create openwrite begin
vfs: boot create openwrite complete
camshot: output opened
```

#### 验证

先用小文件验证 FAT32 目录项创建，再运行相机输出：

```sh
/bin/touch /boot/fatprobe
/bin/camshot -t 1 -s -o /boot/cam02.bmp
```

若仍停在 `directory-entry begin`，记录紧随其后的 `fat32: extending...` 日志；若连该日志都没有，
问题位于现有目录槽位扫描；若有 `extending` 但没有 `extended`，问题仍在 FAT cluster 分配或 SD
写入路径。完成后还应通过 `ls /boot` 确认 `cam02.bmp` 存在且大小为 230454 bytes。

真机使用 `gpio-sccb-v8` 和移出内核栈的 FAT 工作区验证通过。关键日志为：

```text
ov5647: software SCCB power-on chip-id=5647
unicam: frame captured by polling; CSI1 IRQ 39 intentionally masked
fat32: create lookup complete exists=0
fat32: create reserve begin slots=2
fat32: create reserve complete
vfs: boot create directory-entry complete
vfs: boot create truncate complete
vfs: boot create openwrite complete
camshot: output opened; writing BMP header...
camshot: 320x240 frame 31, WB R x1.0 B x1.0, level x1.0 -> /boot/cam02.bmp
```

这证明先前停顿并非 CSI 帧未到达，也不是 `/boot/...` 串口交错造成路径损坏；修复大型 FAT 临时
数组导致的 4 KiB 内核栈溢出后，目录项创建和后续写盘均能完成。捕获输入为 640x480 RAW10，
`camshot` 输出阶段按当前转换器设置生成 320x240、230454-byte 的 24-bit BMP，因此最后一行显示
320x240 是预期的输出尺寸，不是 Unicam 丢失了一半分辨率。

异常时 `ELR` 对应非访存指令、`FAR` 为随机地址的情况应消失。若仍出现 Data Abort，再用
实际安装进 SD 卡的用户 ELF 对 `ELR` 执行 `addr2line/objdump`，不要用不同构建版本的符号
文件判断。
