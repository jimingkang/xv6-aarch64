# xv6 AArch64 VFS 中间层

## 目标

原来的 ext2 驱动只能通过 `ext2read`、`ext2readdir` 两个专用系统调用访问，普通的
`open/read/fstat/close` 完全不知道 ext2。现在增加 `kernel/vfs.c`，把非原生文件系统包装成
vnode，并在统一文件描述符层分派操作。

当前挂载布局：

```text
/
├── 原生 xv6 inode filesystem    读写
├── /proc                        procfs，只读
└── /mnt/ext2                    ext2，只读
```

挂载点是内核路径路由，不要求 xv6 根目录中事先创建真实的 `mnt/ext2` inode。

## 数据结构

```text
struct file
  ├── FD_INODE  -> struct inode       原生 xv6 filesystem
  ├── FD_DEVICE -> struct inode       设备
  ├── FD_PIPE   -> struct pipe
  └── FD_VNODE  -> struct vnode       VFS filesystem
                         │
                         └── vnode_ops
                              ├── stat
                              ├── read
                              └── readdir
```

`sys_open()` 先调用 `vfsopen()`：

- 路径不属于 VFS mount 时返回 0，继续执行原来的 `namei()`；
- 路径属于 `/mnt/ext2` 且存在时返回 vnode；
- 路径属于 mount 但不存在或请求写入时返回错误，不会错误地回落到 xv6 root。

`fileread()`、`filestat()` 和 `fileclose()` 根据 `FD_VNODE` 调用对应 VFS 操作。ext2 目录项会
转换成 xv6 的 `struct dirent`，所以原来的 `ls` 可以工作；普通文件内容经内核页缓冲后
`copyout` 到用户地址，所以原来的 `cat` 也可以工作。

## 使用

真实 SD 卡存在兼容的 ext2 分区时：

```text
$ ls /mnt/ext2
$ cat /mnt/ext2/path/to/file
```

也可以继续使用兼容命令：

```text
$ ext2ls /
$ ext2cat /path/to/file
```

## 当前限制

- ext2 后端只读；以写模式打开、创建和截断会失败。
- `chdir()` 和 `exec()` 仍使用原生 inode 路径，暂时不能把 VFS 目录设为 cwd，也不能直接
  执行 ext2 上的程序。
- xv6 原生目录格式的文件名只有 14 字节；ext2 长文件名在通用 `ls` 中会被截断。专用
  `ext2ls` 仍可显示完整文件名。
- ext2 驱动目前只支持 direct block 和 single-indirect block，并拒绝 ext4 extents 等不兼容
  feature。
- 已实现最小只读 `mount(2)`，目前只接受 `procfs` 和 `ext2`；尚未实现 `umount`、可写挂载、
  dentry cache、权限检查和符号链接。

下一步可以把原生 xv6 inode 也包装成 vnode，使 `exec/chdir/link/unlink/mkdir` 全部经过
VFS，然后增加可写 ext2 或 FAT32 filesystem ops。

## procfs

VFS 还提供动态生成的只读 `/proc`：

```text
$ ls /proc
1              1 1001 0
2              1 1002 0

$ ls /proc/2
status         2 2002 128

$ cat /proc/2/status
Name:   sh
Pid:    2
State:  sleeping
VmSize: 16384 bytes
Killed: 0
```

`/proc` 没有对应的磁盘数据块。打开目录时，procfs 扫描 `proc[NPROC]` 并为当前有效进程生成
PID 目录；读取 `status` 时持有目标进程的锁并生成当时的状态快照。进程退出后，对应路径立即
失效。`init` 会创建可见的 `/proc`、`/mnt`、`/mnt/ext2` 挂载点目录，但这些目录下面的
内容由 VFS 后端覆盖，而不是写入 xv6 磁盘文件系统。

procfs 还提供内存诊断文件：

```sh
cat /proc/meminfo   # RAM统计、内核与ramdisk范围、实际MMIO映射
cat /proc/iomem     # Linux风格的物理地址资源树
```

`/proc/iomem` 列出 BCM system timer、legacy interrupt controller、property mailbox、GPIO、
PL011、AUX/Mini UART、Arasan EMMC、DWC2 USB host 和 BCM ARM-local interrupt/timer 区间。
Linux 通常把详细硬件地址资源放在 `/proc/iomem`，而 `/proc/meminfo` 主要显示统计信息；本实现
同时按调试需求在 meminfo 中给出 MMIO 的 PA→内核 VA 映射摘要。

## 启动挂载与 `/etc`

启动过程改成接近嵌入式 Linux 的“内核提供驱动，用户态 init 决定挂载”模式：

```text
kernel main
  -> fat32init()/ext2init()       探测块设备和文件系统驱动
  -> vfsinit()                    初始化空的 mount table
  -> userinit()
       -> /init
            -> 创建 /etc 和挂载点
            -> 读取 /etc/fstab
            -> mount(2)
                 -> vfsmount()
                      -> mount table
            -> /sh
```

路径查找使用最长挂载点匹配，因此 `/proc/2/status` 会得到相对路径 `/2/status` 并交给
procfs，而普通路径继续进入原生 xv6 inode 文件系统。根文件系统可用于启动 `/init`；其他
文件系统由 init 在 shell 出现前按配置挂载，而不是由内核硬编码自动挂载。

`init` 确保以下磁盘目录存在：

```text
/proc
/boot
/mnt
/mnt/ext2
/etc
```

首次启动还会创建：

```text
/etc/hostname
/etc/fstab
```

新建系统默认的 `fstab` 内容为：

```text
proc /proc procfs ro 0 0
bootfs /boot fat32 rw 0 0
ext2 /mnt/ext2 ext2 ro 0 0
```

`init` 逐行解析 `fstab` 的 source、target、fstype 和 options 字段并调用新加入的
`mount(source, target, fstype, flags)` 系统调用。当前实现只支持只读 VFS 后端：`procfs`
总能挂载；`ext2` 只有在启动阶段发现兼容分区后才会挂载成功；FAT32 bootfs 支持只读或有限
的读写挂载。`ro`/`rw` 会被转换为挂载标志，ext2 和 procfs 仍拒绝 `rw`。目前尚未实现设备名
解析与 `umount(2)`。

## FAT32 bootfs 挂载

Raspberry Pi 固件在启动内核前读取 SD 卡上的 FAT32 启动分区。进入 Linux 后，这个分区并
不会自动变成根文件系统的一部分；Linux 通常再次把它挂载在 `/boot` 或
`/boot/firmware`。本项目采用同样的布局：原生 xv6 文件系统仍挂载为 `/`，而启动阶段识别到的 FAT32 分区通过 VFS
覆盖挂载到 `/boot`。

```text
SD 卡
├── p1 FAT32 bootfs ── fat32 驱动 ── VFS mount table ── /boot
├── p2 xv6 raw fs ─── xv6 inode fs ──────────────────── /
├── p3 ext2 ───────── ext2 驱动 ──── VFS mount table ── /mnt/ext2
└── p4 reserved
```

因此 `ls /` 显示的是原生根目录以及作为入口的 `boot` 目录；`ls /boot` 才枚举 FAT32
根目录中的 `config.txt`、内核镜像和固件等文件。`cat /boot/CONFIG.TXT` 通过普通
`open/read/close` 路径读取 FAT 文件，不再需要 FAT32 专用系统调用。

FAT32 VFS 支持根目录的 `stat`、`readdir`、普通文件读取，以及有限的创建、截断、顺序写入和
重命名。文件名查找、新文件创建和目录枚举支持 ASCII 长文件名（LFN）；LFN缺失或校验失败时
才回退显示 FAT 8.3 短名别名。
不支持子目录、非 ASCII LFN、随机位置写入或并发写入。实现不提供 FAT 日志和崩溃恢复，写入期间
断电可能损坏分区；修改启动分区前应备份 SD 卡。FSInfo 的空闲簇计数会在分配/释放簇后标记为
未知，避免保留过时计数。

`/boot` 的读写性由 `/etc/fstab` 决定。首次创建的配置使用 `rw`；已有系统的配置文件不会被
自动改写，如果其中仍为 `bootfs /boot fat32 ro 0 0`，请先改成 `rw` 并重启。启动日志应显示
`vfs: mounted fat32 at /boot read-write`。如果启动时 FAT32 未挂载，`/boot` 仍只是 init
创建的原生空目录，所以 `cd /boot` 成功而 `ls` 只有 `.` 和 `..` 并不能证明 FAT32 已挂载。

QEMU 当前直接把 `fs.img` 作为 SD 介质，没有 FAT32 分区，因此 QEMU 下 `/boot` 是空的原生挂载点；
真实 Raspberry Pi 的分区表探测到 FAT32 bootfs 后，启动日志才会显示挂载结果并枚举其中的文件。

### TFTP 下载启动镜像

`tftp` 从 IPv4 TFTP 服务器以 octet 模式下载文件，并在 `/boot` 下沿用远端文件名。
不带参数时默认从 `192.168.0.195` 下载 `kernel8-xv6_wifi.img`：

```sh
tftp
tftp 192.168.1.20
tftp 192.168.1.20 kernel8.img
```

完整命令格式为 `tftp [server-ip [remote-filename]]`。只给服务器地址时仍使用默认文件名；
第二个参数可指定服务器上的其他 basename，例如
`tftp 192.168.1.20 kernel8-xv6_wifi.img` 会保存为 `/boot/kernel8-xv6_wifi.img`。
客户端直接创建或截断目标文件，因此传输失败或按 Ctrl+C 中止时，目标路径会留下部分文件；
再次下载相同文件会先截断它。客户端使用固定本地 UDP 端口 49152，需确保该端口未被其他进程占用。
传输中每累计收到 16 KiB 会显示累计字节数和平均速度（KB/s）；完成时显示耗时与平均速度。
TFTP 不协商文件总长度，因此进度以已接收字节数和速度显示，不显示百分比。串口终端按
Ctrl+C 可向当前前台命令进程组发送 SIGINT 并终止下载。下载使用远端basename作为FAT长文件名，
因此接收 `kernel8-xv6_wifi.img` 后，`ls /boot` 和后续 `open()` 都使用同一个名称。

为了把这个名称经普通 `struct dirent` 返回给 `ls`，原生 xv6 的 `DIRSIZ` 从14扩大为30；
`struct dirent` 总长由16变为32字节，仍可整除1 KiB文件系统块。该修改改变了原生xv6磁盘目录
格式，升级后必须重新生成并烧录 `fs.img`，不能仅替换内核后继续使用旧根文件系统。

早期版本在每次 `udp_tryrecv()` 暂时无数据时调用 `sleep(1)`。xv6 的一个逻辑 tick 是
100 ms，而标准 TFTP 每块只有512字节，因此形成 `512 B / 100 ms ≈ 5 KB/s` 的固定上限。
现在新增 `udp_recv_timeout()`：进程睡在对应 UDP port 的 wait channel 上，Wi-Fi IRQ/NAPI
收到数据并完成 UDP 入队后立即 `wakeup()`；逻辑 tick 只唤醒等待者检查超时期限，以便丢包
时重发 RRQ 或 ACK。正常传输不再等待下一个100 ms tick，同时仍保留 TFTP 的超时重试行为。

`mv old-path new-path` 调用内核 `rename()`。当前只支持同一个可读写 FAT32 挂载中的根目录
文件重命名或替换；不支持跨挂载点移动、目录移动或原生 xv6 文件系统中的重命名。
例如：`mv /boot/OLD.IMG /boot/NEW.IMG`。运行普通 `make` 会把 `mv` 编入 `fs.img` 的
`/bin/mv`；将更新后的 `fs.img` 安装到 xv6 分区后即可使用。

### 实现过程和代码调用链

这次改造没有把 FAT32 合并进 xv6 inode 文件系统，而是让 FAT32 成为一个独立 VFS 后端。
这样同一个绝对路径接口可以根据挂载点选择不同文件系统，同时保持根文件系统格式不变。

#### 1. 内核启动阶段探测 FAT32

主核在 `kernel/main.c` 中调用 `fat32init()`。该函数通过 SD 块设备读取 MBR，识别 FAT32
分区并解析 BPB，保存分区起始 LBA、FAT 起始 LBA、数据区起始 LBA、每簇扇区数和根目录簇。
探测成功后 `diskmap.fat` 被置位；新增的 `fat32ready()` 将这个状态提供给 VFS。这里仅仅是
识别并准备文件系统，还没有把它放入用户可见的目录树。

```text
main()
  -> fat32init()
       -> sdsector(0)                 读取 MBR
       -> 选择 FAT32 分区
       -> sdsector(partition_lba)     读取 BPB
       -> 保存 FAT/data/root 布局
       -> diskmap.fat = 1
  -> vfsinit()                        初始化空 mount table
```

#### 2. init 创建挂载点并读取 fstab

进入用户态后，`user/init.c` 先调用 `mkdir("boot")` 创建原生 xv6 目录 `/boot`，然后在首次
生成的 `/etc/fstab` 中写入：

```text
bootfs /boot fat32 rw 0 0
```

`mount_fstab()` 逐行解析配置并调用 `mount(2)`。内核 `vfsmount()` 看到类型为 `fat32` 时，
先检查 `fat32ready()`，再把 `/boot -> fat32_ops` 放入 mount table。挂载点的原生 inode
仍然存在，但访问该路径时会被挂载的 FAT32 后端覆盖。

```text
/init
  -> mkdir("/boot")
  -> mount_fstab()
       -> mount("bootfs", "/boot", "fat32", read-write)
            -> sys_mount()
                 -> vfsmount()
                      -> fat32ready()
                      -> mountops("/boot", &fat32_ops)
```

#### 3. VFS 按最长挂载点匹配路由路径

普通 `open()` 最终先进入 `vfsopen()`。`findmount()` 遍历 mount table，并采用最长挂载点
匹配：`/boot` 和 `/boot/CONFIG.TXT` 都匹配 `/boot`，但传给 FAT32 后端的相对路径分别是
`/` 和 `/CONFIG.TXT`。没有匹配到挂载点时，`vfsopen()` 返回 0，调用者继续走原生 xv6
inode 路径。

```text
open("/boot/CONFIG.TXT", O_RDONLY)
  -> vfsopen()
       -> findmount()
            absolute = /boot/CONFIG.TXT
            mount    = /boot
            relative = /CONFIG.TXT
       -> fat32_ops.stat("/CONFIG.TXT")
```

这个返回值约定很重要：`1` 表示 VFS 已成功打开，`-1` 表示路径属于某个挂载文件系统但打开
失败，`0` 表示不属于任何 VFS 挂载点，应继续尝试原生根文件系统。

#### 4. FAT32 vnode 操作

`kernel/vfs.c` 新增 `fat32_ops`，与 ext2、procfs 使用相同的 `vnode_ops` 接口：

```c
static struct vnode_ops fat32_ops = {
  .stat = fat_vstat,
  .read = fat_vread,
  .readdir = fat_vreaddir,
  .write = fat_vwrite,
  .create = fat_vcreate,
  .truncate = fat_vtruncate,
  .rename = fat_vrename,
};
```

- `fat_vstat()` 调用 `fat32statpath()`，把 FAT 属性转换成 xv6 的 `struct stat`。
- `fat_vreaddir()` 调用 `fat32readdirroot()`，把一个 FAT 32-byte directory entry 转换为
  xv6 `struct dirent`，供普通 `ls` 使用。
- `fat_vread()` 通过 `fat32openpath()` 解析 FAT 短名或长文件名并找到首簇和大小，
  再由 `fat32pread()` 沿 FAT cluster chain 读取数据，供普通 `cat` 使用。

FAT 空文件的 first cluster 合法值为 0，而 xv6 `ls` 把 inode 0 当作未使用目录项。为避免
空文件被隐藏，VFS 在这种情况下为目录输出合成一个非零 inode 编号。

#### 5. 根目录枚举过程

`fat32readdirroot(index, de)` 从 BPB 获得根目录首簇，遍历该簇的所有扇区和 32-byte 目录项，
必要时通过 `fat_next()` 沿 FAT 链进入下一簇。它跳过删除项、卷标和 LFN 项，仅把普通文件
及目录的 8.3 项返回给 VFS。

```text
ls /boot
  -> read(directory fd)
       -> vfsread()
            -> fat_vreaddir(path="/", index=N)
                 -> fat32readdirroot(N)
                      -> cluster_lba(root_cluster)
                      -> sdsector()
                      -> 过滤 deleted/LFN/volume label
                      -> 填充 name, cluster, size, directory
            -> 转换为 xv6 struct dirent
            -> copyout 到 ls
```

#### 6. 普通文件读取过程

```text
cat /boot/CONFIG.TXT
  -> open()
       -> vfsopen() -> fat_vstat()
  -> read()
       -> vfsread() -> fat_vread()
            -> fat32openroot("CONFIG  TXT")
            -> fat32pread(offset, length)
                 -> 定位文件簇
                 -> sdsector(cluster_lba + sector)
                 -> 沿 FAT 链继续读取
            -> copyout 到用户缓冲区
```

写打开只允许 `O_WRONLY`；可创建新文件、用 `O_TRUNC` 清空文件，然后顺序追加数据。为避免
假装支持随机写，非空文件未使用 `O_TRUNC` 打开会失败。长文件名可创建、查找、截断和顺序写入；
`rename()` 目前仍只支持 8.3 名称的同一 FAT32 挂载根目录文件重命名/替换。删除、子目录写入和
随机偏移写仍不支持。

#### 7. 验证结果

构建使用：

```sh
make -j4 kernel/kernel8-xv6_wifi.img user/_init fs.img
```

QEMU 使用的是不含 MBR/FAT32 的裸 `fs.img`，因此验证结果是根目录出现原生挂载点，而
`/boot` 暂时为空：

```text
$ ls /
boot           1 44 32

$ ls /boot
.              1 44 32
..             1 1 1024
```

只有在 FAT32 分区成功探测且 `/etc/fstab` 配置为 `rw` 时，启动日志才应出现：

```text
vfs: mounted fat32 at /boot read-write
```

随后 `ls /boot` 显示的是 FAT32 根目录的 8.3 短文件名，而不再是下面被覆盖的空 xv6 目录。
如果启动日志没有这条 FAT32 挂载消息，`/boot` 仍是普通 xv6 空目录；进入该目录成功并不代表
挂载成功。旧系统上已存在的 `/etc/fstab` 不会被自动改写，需将 `bootfs /boot fat32 ro 0 0`
手动改为 `rw` 后重启。

例如，可以删除或注释 `proc /proc procfs ...` 这一行来阻止下一次启动挂载 `/proc`；也可以
修改 target，把 procfs 挂载到另一个已经创建的绝对路径。当前 mount table 没有卸载和重复
挂载支持，配置中的目标路径应保持唯一。

shell 启动时还读取 `/etc/rc`：

```sh
export PATH=/bin:/usr/bin:/:.
```

`init` 为 `/bin` 创建根目录用户程序的硬链接。shell 对不带 `/` 的命令依次搜索 PATH，
因此进入 `/etc`、`/proc` 或其他目录后仍能直接运行 `ls`、`cat` 等命令。命令行中执行
`export PATH=...` 也会更新当前 shell 的搜索路径。即使旧卡上的 `/etc/rc` 还没有 `/`，
shell 在所有 PATH 项失败后也会尝试根目录中的原始程序，作为启动和存储异常时的恢复路径。

## `/dev` 设备目录

内核早期日志由console子系统直接写Mini UART，不依赖任何文件系统设备节点。
根文件系统可用并进入用户态后，`init`首先创建：

```text
/dev                 原生 xv6 目录
/dev/console         字符设备，major=1，minor=0
/dev/tty             当前进程的控制终端，major=2
/dev/ttyS0           Mini UART 具体终端，major=3，minor=0
```

创建完成后，init用`/dev/ttyS0`打开stdin并复制为stdout和stderr；若具体TTY打开
失败，则回退到`/dev/console`。根目录历史节点`/console`及相关启动回退已经删除。
`/dev/console`仍是系统控制台的标准用户态入口，当前使用同一个Mini UART后端。

## 本地登录

`init` 不再直接启动 shell，而是在 `/dev/ttyS0` 上反复启动 `/bin/login`。首次启动会创建：

```text
/etc/passwd
root:xv6:0:0:root:/root:/bin/sh
```

默认开发账号是 `root`，密码是 `xv6`。认证成功后切换到 passwd 中指定的 home，并执行指定
shell；登录进程退出后 init 会重新显示登录提示。当前 xv6 尚无 uid/gid 和权限检查，因此这层
认证是进入 shell 的入口控制，并不构成 Linux 那样的多用户权限隔离。console 驱动也还没有
termios echo 开关，所以当前输入密码时字符仍会回显。

交互 shell 把 `exit` 和 `logout` 实现为内建命令：它们直接结束当前 shell，而不是 fork 一个
只能结束自身的子进程。login退出后由init回收，并重新启动新的login提示。执行外部命令时，shell
把该命令的进程组设为TTY前台进程组；Mini UART收到 `Ctrl+C`（ASCII ETX, `0x03`）后清空当前
输入行并向前台进程组投递SIGINT，唤醒阻塞进程并将其终止，随后shell恢复为前台进程。这样
TFTP、ping以及管线中的整个前台进程组都可以被 `Ctrl+C` 中止，而不会杀死login或init。

内核现在提供最小 TTY 层。进程结构新增 `sid`、`pgid` 和 `ctty`；login 调用
`tty_attach(0)` 成为新 session 和 process-group leader，并把 `ttyS0` 设为 controlling
TTY。fork 出来的 shell 和命令继承这些字段。访问 `/dev/tty` 时，TTY 驱动读取当前进程的
`ctty`，再动态转发到 `/dev/ttyS0`，因此它不是 console 的静态别名：没有 controlling TTY
的进程访问 `/dev/tty` 会失败。

当前是单串口、单会话的基础实现，尚未实现 termios 和多个终端。shell 在执行前台命令时
设置其进程组；Ctrl+C 会向该前台组发送默认动作即终止的 SIGINT。shell 提示符期间没有前台
进程组，Ctrl+C 只清除当前输入行，不会退出 shell。尚未实现用户态信号处理器或完整的
POSIX job control。具体关系为：

```text
Mini UART/console input queue
       ├── /dev/console        系统全局控制台
       └── /dev/ttyS0          具体串口 TTY
              ↑
              └── /dev/tty     按调用进程的 ctty 动态路由
```

真正 SSH 登录也不能由 `/dev/tty` 单独提供。当前网络栈只有 Ethernet/ARP/IPv4/ICMP/UDP，
尚无 TCP；SSH 还依赖 TCP 流、随机数、密钥存储、加密算法、SSH 握手和 `/dev/pts` 伪终端。
因此现阶段没有把明文 UDP/Telnet shell 冒充成 SSH。合理的实现顺序是 TCP -> socket API ->
pty/会话 -> 密钥与密码学 -> sshd。
