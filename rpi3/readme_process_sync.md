# xv6 AArch64 进程同步与互斥

本实现添加了 64 个内核同步对象槽位，用户进程通过带 generation 的整数句柄访问。
对象在 `fork()` 前创建后，父子进程持有相同句柄，因此可以跨独立地址空间同步。

## 用户 API

包含头文件：

```c
#include "user/sync.h"
```

互斥锁：

```c
mutex_t m;
mutex_init(&m);
mutex_lock(&m);
// critical section
mutex_unlock(&m);
mutex_destroy(&m);
```

mutex 记录获得锁的 PID，只有所有者可以 unlock。它不支持递归获得；持锁进程退出时也不会
自动恢复，这与 Linux 的普通非 robust mutex 类似。

计数信号量：

```c
semaphore_t sem;
sem_init(&sem, 0);
sem_wait(&sem);       // 计数为 0 时阻塞
sem_post(&sem);       // 增加一个 token
sem_post_n(&sem, n);
sem_destroy(&sem);
```

手动复位事件（wait/notify）：

```c
event_t ready;
event_init(&ready, 0);
event_wait(&ready);
event_notify(&ready); // 唤醒等待者并保持 signaled
event_reset(&ready);  // 回到未触发状态
event_destroy(&ready);
```

`event_signal()` 是 `event_notify()` 的同义接口。event 是广播语义；semaphore 的 token
数量决定实际能通过 `sem_wait()` 的进程数量。底层 xv6 `wakeup()` 会扫描并唤醒同一
channel 上的进程，因此等待方始终用 while 循环重新检查条件。

跨进程 atomic：

```c
katomic_t counter;
katomic_init(&counter, 0);
katomic_fetch_add(&counter, 1);
int value = katomic_load(&counter);
katomic_store(&counter, 10);
int old = katomic_compare_exchange(&counter, 10, 20);
katomic_destroy(&counter);
```

compare-exchange 返回操作前的旧值；旧值等于 expected 表示交换成功。

`atomic_int`、`atomic_load/store/fetch_add/compare_exchange` 是用户地址上的 AArch64
原子操作。当前 xv6 没有线程或共享内存，`fork()` 后地址空间被复制，所以这种本地 atomic
不能用于父子进程共享计数；跨进程请使用 `katomic_t`。

## 内核流程

系统调用 25--30 分别是 create、wait、signal、reset、atomic、destroy。核心代码位于
`kernel/sync.c`。

阻塞路径：

```text
sync_wait(handle)
  -> 获得对象 spinlock
  -> 条件不满足
  -> sleep(object, &object->lock)
  -> 进程 SLEEPING，释放 CPU
```

通知路径：

```text
sync_signal(handle, count)
  -> 修改 token/signaled 状态
  -> wakeup(object)
  -> SLEEPING 进程变为 RUNNABLE
```

对象锁将“检查条件、进入睡眠、修改条件、唤醒”串行化，从而避免 lost wakeup。句柄中的
generation 防止对象销毁并重新分配后，旧句柄意外访问新对象。有进程仍阻塞在对象上时，
destroy 会返回 -1。

## 测试

构建并启动后运行：

```sh
syncdemo
```

预期输出：

```text
syncdemo: notify children
syncdemo: atomic counter=3 (expected 3)
syncdemo: ok
```

## 用户态生产者—消费者

`user/prodcons.c` 展示了容量为 4 的有界环形队列：

```text
producer -> wait(empty) -> lock -> write -> unlock -> post(full)
consumer -> wait(full)  -> lock -> read  -> unlock -> post(empty)
```

运行：

```sh
prodcons
```

普通用户数组在 `fork()` 后不是共享内存，所以示例中的槽位、读索引和写索引使用
`katomic_t` 内核对象保存。两个 semaphore 分别计数空槽和已有数据，mutex 保护环形队列
状态。若以后加入 `mmap(MAP_SHARED)`，数据缓冲区便可直接换成共享用户内存，atomic 只需
用于索引或无锁算法。
