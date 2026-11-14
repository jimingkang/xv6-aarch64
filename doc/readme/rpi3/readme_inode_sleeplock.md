# xv6 inode sleeplock、inode cache 槽复用与 login 死锁

## 1. 这次故障的现象

启动停止在：

```text
init: starting local login on /dev/ttyS0
exec-login: begin
exec-login: inode found
ilock-login: before sleeplock locked=1 owner=1 valid=0
```

这里的 `owner=1` 表示逻辑上的 inode sleeplock 仍记录为 PID 1，也就是 `init` 持有。
login 子进程已经被调度，并且 `namei("/bin/login")` 已经找到 inode，但在
`ilock(ip)` 中等待锁，因此不能继续读取 ELF 文件。

最终定位到 [kernel/file.c](../kernel/file.c) 的普通 inode 读取分支漏掉了
`iunlock(f->ip)`：

```c
} else if(f->type == FD_INODE){
  ilock(f->ip);
  if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
    f->off += r;
  iunlock(f->ip);       // 修复：读取结束必须释放 inode sleeplock
}
```

## 2. inode sleeplock 的作用

xv6 同时存在两类锁：

- `spinlock`：临界区必须很短，等待者持续占用 CPU 自旋，持锁期间不能睡眠；
- `sleeplock`：操作可能持续较久，锁被占用时进程进入 `SLEEPING`，让出 CPU。

inode 操作可能触发磁盘/SD 读写，因此不能在整个操作期间持有普通 spinlock。
每个内存 inode 都有自己的 sleeplock：

```c
struct inode {
  uint dev;
  uint inum;
  int ref;
  struct sleeplock lock;
  int valid;
  short type;
  uint size;
  uint addrs[NDIRECT + 1];
  // ...
};
```

它保护 `valid`、`type`、`size`、`addrs[]` 等 inode 内容，保证同一时刻只有一个执行
上下文能够读取或修改这些字段。例如：

```text
进程 A                              进程 B
   |                                  |
 ilock(ip)                           ilock(ip)
   |                                  |
 修改 size/addrs                锁被占用，sleep(ip->lock)
 写回磁盘                             |
 iunlock(ip) -> wakeup                 |
                                      v
                                 获得锁后继续
```

`sleeplock.pid` 主要用于调试和 `holdingsleep()` 判断；真正的等待通过内部 spinlock、
`sleep()` 和 `wakeup()` 完成。

## 3. 磁盘 inode 与 inode cache 槽不是同一个东西

磁盘中每个文件有一个持久化 inode，例如：

```text
/bin/login       磁盘 inode 号 10
/etc/fstab       运行时创建的磁盘 inode 号 53（本次 QEMU 验证）
```

内核不会让所有磁盘 inode 永久驻留内存，而是维护固定大小的全局缓存：

```c
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;
```

`itable.inode[]` 的每一个数组元素就是一个 **inode cache 槽**。槽本身位于内核全局
内存中，不属于任何进程，也不会在 `fork()` 时复制。

一个槽在不同时间可以代表不同的磁盘 inode：

```text
同一个 itable.inode[k]

时间 T1：dev=1, inum=53  -> /etc/fstab
              ref=1

时间 T2：文件关闭
              ref=0     -> 该槽可以被回收

时间 T3：dev=1, inum=10  -> /bin/login
              ref=1
```

这里的“复用”不是把 `/etc/fstab` 改成了 `/bin/login`，也不是两个文件共用同一个磁盘
inode。它只是说内核把同一块临时内存缓存空间先后用于缓存两个不同的磁盘 inode。

## 4. `iget()` 如何选择和复用槽

`iget(dev, inum)` 在全局 `itable` 中执行两步搜索：

1. 如果已经有 `ref > 0 && dev/inum` 匹配的槽，增加 `ref` 并返回；
2. 否则寻找第一个 `ref == 0` 的空闲槽，写入新的 `dev/inum`，设置 `ref=1`、
   `valid=0`，然后返回。

简化逻辑如下：

```c
for(ip = itable.inode; ip < &itable.inode[NINODE]; ip++) {
  if(ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
    ip->ref++;
    return ip;
  }
  if(empty == 0 && ip->ref == 0)
    empty = ip;
}

ip = empty;
ip->dev = dev;
ip->inum = inum;
ip->ref = 1;
ip->valid = 0;
return ip;
```

标准 xv6 依赖一个重要不变量：

> 当 `ref == 0` 时，任何进程都不应再持有这个 inode 的 sleeplock。

因此 `iget()` 复用槽时不重新初始化 sleeplock。正常调用必须先 `iunlock()`，再通过
`iput()` 降低引用计数。

## 5. 为什么子进程看到“父进程持有锁”

这不是 `fork()` 把一把已锁定的锁复制给了子进程。

inode cache 是全局内核对象：

```text
                         内核全局 itable
                     +---------------------+
init PID 1 ---------->| inode cache slot k |<---------- login PID 2
                     | sleeplock           |
                     | locked=1            |
                     | pid=1               |
                     +---------------------+
```

父进程和子进程进入系统调用后访问的是同一份 `itable`。这次的实际时序是：

```text
PID 1 init
  |
  | open/read("/etc/fstab")
  v
fileread(FD_INODE)
  |
  | ilock(fstab inode cache slot)
  | readi(...)
  | 原代码漏掉 iunlock()
  v
close(fd)
  |
  | iput() 使 ref 变成 0
  | 但 slot.locked 仍然是 1，owner 仍为 PID 1
  v
fork login child
  |
  +---- PID 1: wait()
  |
  +---- PID 2: exec("/bin/login")
                  |
                  | namei() 调用 iget()
                  | 看到旧槽 ref==0，于是复用
                  | dev/inum 改为 /bin/login，valid=0
                  | sleeplock 状态仍错误地保留 locked=1, pid=1
                  v
                ilock(login inode)
                  |
                  +--> sleep，等待 PID 1 释放一把 PID 1 已经不会再释放的锁
```

所以日志中同时出现：

```text
locked=1 owner=1 valid=0
```

`valid=0` 表示槽已经被重新分配给 login、其磁盘内容尚未加载；`owner=1` 则是上一位
使用者 `/etc/fstab` 遗留的错误锁状态。

## 6. 为什么 `close()` 没有自动解决

文件描述符关闭最终调用 `iput()`，它负责降低 inode 引用计数；但 `iput()` 不是
`iunlock()` 的替代品。

这两个动作表达不同概念：

```text
ilock/iunlock   控制是否正在独占访问 inode 内容
idup/iput       控制有多少长期引用指向 inode cache 槽
```

正常顺序是：

```c
ilock(ip);
readi(ip, ...);
iunlock(ip);
// 文件仍可由 struct file 长期引用
```

关闭文件时才执行：

```c
iput(ip);       // ref--
```

如果遗漏 `iunlock()`，单纯 `iput()` 仍会让 `ref` 下降，但不会清除 sleeplock，最终形成
“`ref==0` 但 `locked==1`”的非法状态。

## 7. 正确修复与错误修复

### 正确修复

在取得锁的同一控制路径上保证释放：

```c
ilock(f->ip);
r = readi(f->ip, 1, addr, f->off, n);
if(r > 0)
  f->off += r;
iunlock(f->ip);
```

即使 `readi()` 返回错误或 0，也必须执行 `iunlock()`。

### 不应保留的临时方案

调试时曾在 `iget()` 发现 `ref==0 && locked==1` 后强制把 `locked` 清零。该方案能够让
系统继续启动，但会掩盖真正的锁配对错误，而且如果引用计数也存在错误，强制解锁
可能造成并发访问。因此定位到 `fileread()` 后已经删除该临时恢复逻辑。

## 8. 验证结果

修复后使用 QEMU 验证完整启动和登录：

```text
vfs: mounted procfs at /proc read-only
init: starting local login on /dev/ttyS0

xv6-rpi3 login: root
Password:
Welcome root
$
```

这证明以下路径全部正常：

```text
读取 /etc/fstab
  -> inode 解锁
  -> fork login
  -> exec /bin/login
  -> ELF 加载
  -> tty_attach
  -> 用户登录
```

## 9. 调试这类问题的方法

遇到 inode 死锁时，应同时打印：

```text
dev, inum, ref, valid, lock.locked, lock.pid, 当前 pid
```

重点检查下列不变量：

- `ref == 0` 时必须没有锁持有者；
- 每次成功 `ilock()` 都必须在所有返回路径上执行 `iunlock()` 或 `iunlockput()`；
- `iput()` 不能代替 `iunlock()`；
- 不要在持有 inode sleeplock 时执行无限等待另一个 inode 的操作；
- 错误返回路径与正常返回路径必须具有相同的锁释放语义。

可进一步在调试版本中加入断言：当 `iget()` 准备复用 `ref==0` 的槽时，如果发现
`lock.locked != 0`，立即 panic 并打印旧 `dev/inum/owner`。这比静默强制解锁更容易
发现真正的锁配对缺陷。

## 10. 为什么以前没有出现

这次问题不是 xv6 原始 inode 实现一直存在、直到现在才偶然触发。检查 Git 基线可见，
原来的 `fileread(FD_INODE)` 是正确配对的：

```c
ilock(f->ip);
if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
  f->off += r;
iunlock(f->ip);
```

近期为了建立统一文件描述符层，`file.c` 连续加入或调整了：

```text
FD_DEVICE
FD_VNODE
FD_SOCKET
FD_EPOLL
FD_PTY
```

工作区中尚未提交的分支合并/编辑使 `FD_INODE` 分支末尾的 `iunlock()` 丢失，形成了
回归。因此旧内核没有问题，而使用这一工作区重新构建的新内核才出现死锁。当前修复
实际上是恢复 Git 基线本来就具备的锁配对，同时保留新增的 socket、epoll 和 PTY
分支。

此外，启动流程的演进改变了缺陷出现的位置：

```text
较早启动流程：直接启动 shell，启动期不读取 /etc/fstab

增加 VFS/fstab 后：
  init 读取 /etc/fstab
    -> 漏解锁立即制造 stale inode cache slot
    -> 下一个需要新 inode cache 槽的 exec/open 会死锁
```

启用自动 Wi-Fi 时，下一个程序可能是 `/bin/wifi`，表现为：

```text
init: connecting Wi-Fi from /etc/wifi.conf
```

之后停止。关闭自动 Wi-Fi 后，下一个程序变成 `/bin/login`，于是表现为：

```text
init: starting local login on /dev/ttyS0
```

之后停止。两种表象可能来自同一个 `/etc/fstab` inode 漏解锁，并不表示 Wi-Fi 或
TTY 本身有问题。

加入更多用户程序会改变 mkfs 分配的 inode 号，创建 `/etc` 文件也会改变 inode cache
槽的复用顺序。因此有时修改程序列表后，死锁对象或日志位置会变化；这只能改变症状，
不能消除漏掉 `iunlock()` 的根因。
