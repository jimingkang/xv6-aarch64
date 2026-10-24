# rpi3-lock：内核锁机制与使用规则

本文记录 `rpi3-lock` 分支当前的内核锁对象、等待方式、适用场景，以及启动期
`panic: mutex_lock: no process` 的原因和修复。用户态的 `pthread_mutex`、semaphore 和
`sync_wait/sync_signal` 另见 `readme_process_sync.md`。

## 1. 当前锁对象总览

| 对象 | 等待方式 | 可在持锁期间睡眠 | 主要用途 |
|---|---|---|---|
| `struct spinlock` | CPU 自旋 | 否 | 调度器、短队列、引用计数和锁内部状态 |
| `struct mutex` | FIFO 等待队列，进程 `sleep()` | 是 | FAT32、共享文件偏移、较长的可阻塞临界区 |
| `struct rw_semaphore` | FIFO；连续 reader 批量唤醒，writer 独占 | 是 | inode 内容和元数据的读写并发控制 |
| `struct sleeplock` | `mutex` 兼容包装 | 是 | 尚未迁移到新接口的旧驱动和文件系统代码 |

相关实现：

```text
include/kernel/spinlock.h       kernel/core/spinlock.c
include/kernel/mutex.h          kernel/core/mutex.c
include/kernel/rwsem.h          kernel/core/rwsem.c
include/kernel/sleeplock.h      kernel/core/sleeplock.c
```

## 2. mutex：短自旋保护状态，长等待交给调度器

`struct mutex` 的内部结构为：

```c
struct mutex {
  struct spinlock wait_lock;    // 只保护下面的 owner 和等待队列
  struct proc *owner;           // 正常进程上下文的所有者
  struct cpu *boot_owner;       // userinit/scheduler 前的启动 CPU
  struct mutex_waiter *head;
  struct mutex_waiter *tail;
  char *name;
};
```

正常进程争用 mutex 时，waiter 临时放在该进程的内核栈中：

```text
mutex_lock
  -> acquire(wait_lock)
  -> 锁空闲：owner = myproc()，立即返回
  -> 锁忙：waiter 追加到 FIFO 尾部
  -> sleep(&waiter, &wait_lock)

mutex_unlock
  -> acquire(wait_lock)
  -> 从 FIFO 头取一个 waiter
  -> owner 直接交接给该 waiter
  -> wakeup(waiter)
```

直接交接所有权可防止后来者插队，也避免旧 sleeplock 的 `wakeup()` 惊醒所有等待者后再次竞争。
mutex 不允许递归获取；只有记录的 owner 能解锁。

### 2.1 为什么需要 `boot_owner`

initcall 在 `userinit()` 和 `scheduler()` 之前运行。`sdroot` 挂载 FAT32 bootfs 后，
`cmdline_init()` 会读取 bootfs 文件并进入 `fat32statpath()`、`fat32readnext()`。FAT32 已改用
可睡眠 mutex，但此时：

```c
myproc() == 0
```

第一版 `mutex_lock()` 因而直接执行：

```text
panic: mutex_lock: no process
```

当前实现允许没有当前进程的启动 CPU **无竞争地** 获取 mutex：

```c
struct proc *p = myproc();
struct cpu *boot = p ? 0 : &cpus[cpuid()];

if(m->owner == 0 && m->boot_owner == 0 && m->head == 0){
  m->owner = p;
  m->boot_owner = boot;
  release(&m->wait_lock);
  return;
}
```

启动期锁状态因此是 `owner == 0, boot_owner == &cpus[0]`。解锁、trylock、递归检测和
`mutex_is_owned()` 都同时识别这两种 owner。

无进程上下文没有可供 `sleep()` 的 `struct proc`，所以一旦发生竞争仍然立即报错：

```c
if(p == 0)
  panic("mutex_lock: contention without process");
```

这不是把 mutex 变成 spinlock，也不是允许 IRQ 随意睡眠；它只覆盖单 CPU 启动阶段的无竞争
临界区。正常并发阶段仍使用进程 FIFO 等待。

## 3. rw_semaphore：inode 的共享读与独占写

`struct rw_semaphore` 保存 active reader 数、writer owner 和 FIFO waiter 队列：

```text
队列头是 writer：等 readers 归零后只授予该 writer
队列头是 reader：一次授予队首连续的一批 readers
writer 已排队：后来的 reader 不得越过它
```

因此它是 writer 公平的，不会因为持续到来的 reader 让 writer 永久饥饿。接口为：

```c
down_read();              up_read();
down_write();             up_write();
down_read_trylock();      down_write_trylock();
```

与 mutex 相同，启动期 writer 用 `boot_writer` 标识；无当前进程时只允许立即成功，不允许排队
睡眠。reader 不记录单个 owner，但仍由 `wait_lock` 原子维护计数和队列。

## 4. inode：从独占 sleeplock 改为 i_rwsem

当前 `struct inode` 使用：

```c
struct rw_semaphore i_rwsem;
```

接口映射为：

```text
ilock / iunlock             -> down_write / up_write
ireadlock / ireadunlock     -> down_read / up_read
```

路径查找、普通读取、`stat`、`pread` 和 `chdir` 类型检查可以并发持有读锁；创建、写入、删除、
截断以及 inode 首次从磁盘加载仍使用写锁。首次加载时若调用者持有读锁，会先释放读锁、取得写锁
完成 `valid/type/size/addrs[]` 初始化，再降回读锁，避免多个 reader 同时填充同一个缓存 inode。

inode cache 的 `itable.lock` 仍是 spinlock：它只保护槽查找、`ref` 和槽复用；`i_rwsem` 保护一个
具体 inode 的内容和长时间 I/O。两者职责不能混用。

## 5. `struct file.pos_lock`

`dup()` 和 `fork()` 会让多个文件描述符共享同一个 `struct file`，因此也共享文件偏移 `f->off`。
如果两个进程同时执行 read/write/lseek，仅靠 inode 锁不能保证“读取当前 offset、完成 I/O、更新
offset”是一个整体。

当前每个 `struct file` 有一个 `pos_lock` mutex，串行化：

```text
read/write: lock -> 使用 f->off -> I/O -> 更新 f->off -> unlock
lseek:      lock -> 计算并写入 f->off              -> unlock
pread/pwrite: 使用显式 offset，不改变 f->off，不需要 pos_lock
```

## 6. FAT32 与兼容 sleeplock

FAT32 的全局锁过去是 spinlock，但 FAT 操作会执行长时间 SD I/O，持有 spinlock 等待设备会关闭
本 CPU 中断并阻塞其它调用者。现在 `fat32_lock` 是 sleepable mutex。

旧 `struct sleeplock` 已变成 `struct mutex` 的兼容包装，原来的
`initsleeplock/acquiresleep/releasesleep/holdingsleep` 调用不必一次全部重写，也自动获得 FIFO 等待
和直接 owner 交接语义。ext2 尚未完成 allocation、journal、目录与 inode 多级锁拆分，目前其旧
sleeplock 也通过该包装运行。

## 7. 使用约束与锁顺序

1. spinlock 临界区内不得调用可能 `sleep()` 的 mutex、rwsem、磁盘或文件系统操作。
2. mutex/rwsem 只能在可调度进程上下文中发生竞争；启动期只允许无竞争获取。
3. 不要递归获取同一 mutex 或写 rwsem。
4. 获得 inode 引用（`iget/idup`）不等于锁定 inode；引用必须用 `iput` 配对，锁必须用对应的
   `iunlock/ireadunlock` 配对。
5. 一般顺序为 `struct file.pos_lock -> inode i_rwsem -> buffer/cache lock`；不得在下层回头获取
   上层锁。
6. 等待队列节点位于睡眠进程的内核栈，unlocker 必须先将节点移出队列再唤醒；当前实现遵守该
   生命周期。

## 8. 当前完成度

已完成：FIFO mutex、writer 公平 rwsem、sleeplock 兼容层、inode i_rwsem、共享文件偏移锁、
FAT32 mutex，以及启动期无进程 owner 支持。

后续仍可改进：

- 为 IRQ/softirq 增加明确的 `in_interrupt()` 上下文检查，禁止中断路径误入 sleepable lock；
- 为实时调度增加 priority inheritance，降低优先级反转；
- 将 ext2 全局锁拆分为 allocation、journal、目录和每 inode 锁，并明确锁序；
- 增加 lockdep 风格的锁类、持锁栈和 ABBA 检测；
- 增加 mutex/rwsem 多核争用、kill/exit 和超时压力测试。

