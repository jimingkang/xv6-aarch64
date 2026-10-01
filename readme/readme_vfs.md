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

默认 `fstab` 内容为：

```text
proc /proc procfs ro 0 0
bootfs /boot fat32 ro 0 0
ext2 /mnt/ext2 ext2 ro 0 0
```

`init` 逐行解析 `fstab` 的 source、target、fstype 和 options 字段并调用新加入的
`mount(source, target, fstype, flags)` 系统调用。当前实现只支持只读 VFS 后端：`procfs`
总能挂载；`ext2` 只有在启动阶段发现兼容分区后才会挂载成功。为了保持接口形状接近 Linux，
系统调用保留了 source 和 flags 参数，但当前后端尚未使用它们，也尚未实现设备名解析与
`umount(2)`。

## FAT32 bootfs 挂载

Raspberry Pi 固件在启动内核前读取 SD 卡上的 FAT32 启动分区。进入 Linux 后，这个分区并
不会自动变成根文件系统的一部分；Linux 通常再次把它挂载在 `/boot` 或
`/boot/firmware`。本项目采用同样的布局：原生 xv6 文件系统仍挂载为 `/`，而启动阶段
识别到的 FAT32 分区通过 VFS 覆盖挂载到 `/boot`。

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

当前 FAT32 VFS 后端有意保持只读，以免尚未具备日志和崩溃恢复能力的代码损坏可启动分区。
它已支持根目录的 `stat`、`readdir` 与普通文件读取，并使用 FAT 8.3 短文件名；长文件名
条目和 `/boot/overlays` 等子目录遍历仍是后续工作。由于 QEMU 当前直接把 `fs.img` 作为
SD 介质而没有 FAT 分区，QEMU 中 `/boot` 是空的原生挂载点；真实 Raspberry Pi 的分区表
被探测后才会打印 `vfs: mounted fat32 at /boot read-only` 并显示 bootfs 内容。

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
bootfs /boot fat32 ro 0 0
```

`mount_fstab()` 逐行解析配置并调用 `mount(2)`。内核 `vfsmount()` 看到类型为 `fat32` 时，
先检查 `fat32ready()`，再把 `/boot -> fat32_ops` 放入 mount table。挂载点的原生 inode
仍然存在，但访问该路径时会被挂载的 FAT32 后端覆盖。

```text
/init
  -> mkdir("/boot")
  -> mount_fstab()
       -> mount("bootfs", "/boot", "fat32", read-only)
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
};
```

- `fat_vstat()` 调用 `fat32statpath()`，把 FAT 属性转换成 xv6 的 `struct stat`。
- `fat_vreaddir()` 调用 `fat32readdirroot()`，把一个 FAT 32-byte directory entry 转换为
  xv6 `struct dirent`，供普通 `ls` 使用。
- `fat_vread()` 把路径转换为 11-byte FAT 8.3 名称，通过 `fat32openroot()` 找到首簇和大小，
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

当前 `vfsopen()` 对 FAT32 只接受 `O_RDONLY`。写入、创建、删除、重命名和 truncate 都不会
落到 FAT32 驱动，这既体现了 fstab 的 `ro` 配置，也避免未完成一致性保护前修改启动介质。

#### 7. 验证结果

构建使用：

```sh
make -j4 kernel/kernel8.img user/_init fs.img
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

在真实 SD 卡上，`fat32init()` 能识别第一分区，因此还应出现：

```text
vfs: mounted fat32 at /boot read-only
```

随后 `ls /boot` 显示的是 FAT32 根目录的 8.3 短文件名，而不再是下面被覆盖的空 xv6 目录。

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

内核现在提供最小 TTY 层。进程结构新增 `sid`、`pgid` 和 `ctty`；login 调用
`tty_attach(0)` 成为新 session 和 process-group leader，并把 `ttyS0` 设为 controlling
TTY。fork 出来的 shell 和命令继承这些字段。访问 `/dev/tty` 时，TTY 驱动读取当前进程的
`ctty`，再动态转发到 `/dev/ttyS0`，因此它不是 console 的静态别名：没有 controlling TTY
的进程访问 `/dev/tty` 会失败。

当前是单串口、单会话的基础实现，尚未实现 termios、前台进程组检查、`tcsetpgrp()`、信号和
多个终端。具体关系为：

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
