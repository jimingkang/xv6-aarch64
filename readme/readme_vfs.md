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
ext2 /mnt/ext2 ext2 ro 0 0
```

`init` 逐行解析 `fstab` 的 source、target、fstype 和 options 字段并调用新加入的
`mount(source, target, fstype, flags)` 系统调用。当前实现只支持只读 VFS 后端：`procfs`
总能挂载；`ext2` 只有在启动阶段发现兼容分区后才会挂载成功。为了保持接口形状接近 Linux，
系统调用保留了 source 和 flags 参数，但当前后端尚未使用它们，也尚未实现设备名解析与
`umount(2)`。

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

`init` 先使用历史兼容节点 `/console` 完成最早的标准输入输出初始化，随后创建：

```text
/dev                 原生 xv6 目录
/dev/console         字符设备，major=1，minor=0
/dev/tty             当前进程的控制终端，major=2
/dev/ttyS0           Mini UART 具体终端，major=3，minor=0
```

创建完成后，init 关闭文件描述符 0、1、2，用 `/dev/ttyS0` 重新打开 stdin，并复制为 stdout
和 stderr。`/dev/console` 仍是系统控制台，当前也使用同一个 Mini UART 后端；`/console`
暂时保留为旧文件系统镜像和早期启动的回退入口。

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
