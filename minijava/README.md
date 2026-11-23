# MiniJava on xv6-aarch64：从 `synchronized` 看 Java 多线程是怎么实现的

这个目录是一个**自包含**的原型：一个用 C 写的 MiniJava 子集编译器 `mjc`，把 `.java` 直接编译成 AArch64 汇编，再和一个很小的运行时（`runtime.c` + `rt_asm.S`）链接成 xv6 用户程序。重点放在 Java 多线程和 `synchronized` 的实现上：对象头里的锁字、thin lock 到膨胀 monitor 的升级、`wait/notify`、异常安全的解锁、`volatile` 的内存序。

它不修改仓库里任何现有文件：内核、libc、顶层 Makefile 都保持原样，只复用 java 分支已有的 `thread_clone / texit / tjoin`、`sync_create(SYNC_SEM)` 和 `sched_yield`。

```
minijava/
├── mjc.c            编译器：词法 → 递归下降语法分析 → AST → 类布局/vtable → 带类型检查的代码生成 → .S
├── runtime.c        运行时：堆分配、对象/数组、线程、锁字与 ObjectMonitor、wait/notify、异常对象
├── rt_asm.S         必须用汇编写的部分：main 入口、线程入口、mj_throw 栈展开
├── Makefile         独立构建；make install 把程序写进 fs_ext2.img 的 /bin
└── examples/
    ├── Hello.java      类、继承、虚调用、数组、异常
    ├── Counter.java    数据竞争：普通字段 / volatile / synchronized 方法 / synchronized 块
    ├── SyncRules.java  可重入、异常释放锁、return 释放锁、IllegalMonitorStateException、static synchronized
    └── ProdCons.java   有界缓冲区：wait() / notifyAll()
```

---

## 1. Java 这一侧：`synchronized` 在 JVM 里是什么

### 1.1 两种写法，两种字节码

```java
synchronized (lock) { count++; }          // 同步块
synchronized void inc() { count++; }      // 同步方法
```

`javac` 对**同步块**生成显式的 `monitorenter / monitorexit`，并且一定会多生成一个异常处理器，保证抛异常时也能解锁：

```
 0: aload_1
 1: dup
 2: astore_2
 3: monitorenter              // 加锁
 4: ... count++ ...
 9: aload_2
10: monitorexit               // 正常路径解锁
11: goto 19
14: astore_3                  // 异常路径
15: aload_2
16: monitorexit               // 先解锁
17: aload_3
18: athrow                    // 再重新抛出
Exception table:
   from  to  target type
     4   11    14   any
```

**同步方法**不生成这两条指令，只在方法的 access flags 里设置 `ACC_SYNCHRONIZED`。解释器/JIT 在方法入口和出口（包括异常出口）自动对 `this`（静态方法则是 `Class` 对象）加锁和解锁。语义与上面的同步块完全一样。

### 1.2 锁放在哪里：对象头里的 mark word

HotSpot 里每个对象都有一个对象头，第一个字叫 **mark word**（64 位）。它的最低几位是锁状态标记，其余位的含义随状态变化：

| 低 2 位 | 状态 | 其余位存的是 |
|---|---|---|
| `01` | 无锁 | hash、GC 年龄 |
| `00` | 轻量级锁（stack lock / lightweight） | 指向持锁线程栈上 "displaced header" 的指针（JDK 21+ 新的 lightweight locking 改用每线程的 lock-stack） |
| `10` | 重量级锁（已膨胀） | 指向 `ObjectMonitor` 的指针 |
| `11` | GC 标记 | |

还有过一个"偏向锁"（biased locking）状态：第一次加锁时把线程 ID 写进 mark word，之后同一线程再加锁连 CAS 都省了。它在 JDK 15 被默认关闭，JDK 18 起整个移除，因为现代 CPU 上 CAS 已经很便宜，而撤销偏向需要 safepoint，代价很大。

### 1.3 三级加锁路径

1. **快速路径（无竞争）**：一次 CAS 把 mark word 从"无锁"改为"被我锁住"。没有系统调用，也不进内核。
2. **重入**：同一线程再次进入，只需记一个重入计数（stack lock 用值为 0 的 displaced header 记重入，ObjectMonitor 用 `_recursions`）。
3. **竞争 → 膨胀（inflate）**：别的线程已经持有轻量级锁，当前线程先自旋一会儿，还拿不到就把锁"膨胀"成一个 `ObjectMonitor`，把它的地址 CAS 进 mark word，然后排队睡眠。

`ObjectMonitor` 的核心字段：

```
_owner        当前持有者线程
_recursions   重入次数
_cxq/_EntryList  在 monitorenter 处阻塞的线程队列
_WaitSet      调用了 wait() 的线程队列
```

线程睡眠和唤醒用的是每个线程自己的 `ParkEvent`，在 Linux 上最终落到 `futex(FUTEX_WAIT/FUTEX_WAKE)`。

### 1.4 `wait / notify`

- 调用 `wait()` 必须已经持有该对象的 monitor，否则抛 `IllegalMonitorStateException`。
- `wait()`：把自己放进 `_WaitSet`，**完全释放** monitor（重入计数也要先保存起来），然后睡眠；被唤醒后要重新竞争 monitor，拿到后恢复原来的重入计数，才从 `wait()` 返回。
- `notify()`：把 `_WaitSet` 里的一个线程挪到入口队列。它不会立刻运行，要等通知者退出同步块、把锁交出来之后才轮到它。所以 `wait` 一定要写在 `while (条件不满足)` 循环里。
- 因为需要 wait set，只要对一个轻量级锁调用 `wait/notify`，HotSpot 会先把它膨胀。

### 1.5 `Thread.start / join` 和 Java 内存模型

- HotSpot 里 Java 线程和 OS 线程是 1:1 的：`start()` 最终调用 `pthread_create`（在 Linux 上就是带 `CLONE_VM|CLONE_THREAD|...` 的 `clone`）。
- `join()` 在 JDK 里就是普通 Java 代码：`while (isAlive()) wait(0);`。线程结束时 JVM 对这个 `Thread` 对象执行 `notifyAll()`。
- Java 内存模型（JMM）里的几条 happens-before：
  - 对同一个 monitor 的**解锁** happens-before 之后的**加锁**；
  - **volatile 写** happens-before 之后对同一变量的**读**；
  - `t.start()` happens-before `t` 里的所有动作；`t` 的所有动作 happens-before 别的线程从 `t.join()` 返回。

### 1.6 落到 AArch64 指令上

| Java 语义 | AArch64（ARMv8.0，比如 Cortex-A53） | ARMv8.1+ LSE |
|---|---|---|
| 加锁 CAS（acquire） | `LDAXR` + `STXR` 循环 | `CASA` |
| 解锁（release） | `STLR` 或 `LDXR` + `STLXR` | `CASL` / `STLR` |
| volatile 读 | `LDAR` | 同左 |
| volatile 写 | `STLR` | 同左 |
| 完整屏障 | `DMB ISH` | 同左 |

ARMv8 的 `LDAR/STLR` 是 RCsc 的：一个 `STLR` 后面跟一个 `LDAR`，硬件也不会重排，所以 volatile 不需要再额外加 `DMB`。树莓派 3 的 Cortex-A53 没有 LSE，所以这里全部用的是 LL/SC（`LDAXR/STXR`）。

---

## 2. 映射到 xv6-aarch64：内核要提供什么

| JVM 需要的东西 | Linux 上的做法 | 本仓库 java 分支已有的 | 这个原型怎么用 |
|---|---|---|---|
| 共享地址空间的线程 | `clone(CLONE_VM\|CLONE_THREAD…)` | `thread_clone(fn, arg, stack)`、Linux 风格的 `clone()` 及 `CLONE_*` flags | `mj_thread_start` 用 `thread_clone` |
| 线程退出与回收 | `exit` / `pthread_join` | `texit`、`tjoin` | 线程跑完 `run()` 后 `texit(0)`，`main` 结束时 `tjoin` 回收 |
| 让线程睡眠/唤醒（park/unpark） | `futex` | 内核同步对象 `sync_create(SYNC_SEM, 0)` + `sync_wait / sync_signal` | 每个 Java 线程一个信号量，相当于 HotSpot 的 `ParkEvent` |
| 让出 CPU（自旋退避） | `sched_yield` | `sched_yield` | 自旋超过 8 次后让出 |
| 线程寄存器（TLS） | `TPIDR_EL0` | 没有用 | 编译出来的代码把 **x28** 固定为当前线程指针（HotSpot 在 AArch64 上也用 r28 = rthread） |

**缺什么**：xv6 这里没有 `futex`。HotSpot 的 park 本质上是"按地址睡眠/唤醒"，信号量能替代，但有限制：内核里同步对象总数 `NSYNC = 64`，并且是全系统共享的，所以原型最多支持 48 个 Java 线程。如果要往下做，最值得加的内核功能就是一个最小的 `futex(addr, WAIT/WAKE)`：用户态地址加内核里的哈希等待队列，不需要预先分配对象。

---

## 3. 原型的实现

### 3.1 对象布局

```
对象:   [0] vtable 指针    [8] 锁字 (mark word)    [16..] 字段 (每个 8 字节)
数组:   [0] vtable         [8] 锁字                [16] length   [24..] 元素
vtable: [0] Class*         [8] 方法槽 1 ...        (Thread.run 固定在槽 1)
Class:  [0] 0  [8] 锁字  [16] 名字  [24] 父类  [32] 实例大小  [40] vtable
        └── Class 对象本身也有锁字，static synchronized 锁的就是它
```

### 3.2 锁字的三种状态（`runtime.c`）

```
0                                      无锁
owner_id << 32 | rec << 2 | 0b01       thin lock（轻量级锁），rec = 重入次数
ObjectMonitor* | 0b10                  已膨胀（重量级锁）
```

状态转换：

```
          CAS 0 → thin(me,1)                  别的线程自旋 64 次仍拿不到
 无锁 ───────────────────────▶ thin ─────────────────────────────▶ inflated
  ▲     STLXR thin(me,1) → 0     │   或：wait/notify 需要 wait set          │
  └──────────────────────────────┘   或：重入次数超过 1000                  │
                                                                           ▼
                              ObjectMonitor { owner, rec, entry 队列, wait 队列 }
```

膨胀时有一个微妙的竞争：膨胀者 CAS 锁字的时候，原持有者仍在临界区里。做法和 HotSpot 一样：膨胀者把"当前 owner 和重入次数"抄进新的 `ObjectMonitor`，再 CAS 进锁字。原持有者之后解锁时，快速路径的 CAS 会失败（锁字已经不是 `thin(me,1)` 了），于是走进 `mj_monitor_exit_slow`，在那里发现是 monitor，就按 monitor 的方式解锁并唤醒排队者。所有对 thin 状态的修改都通过 CAS 完成，所以两边不会互相覆盖。

### 3.3 编译器怎么降级 `synchronized`（`mjc.c` 里的 `gen_sync`）

真实生成的代码（`make -C minijava asm S=examples/Counter.java` 可以看到全部）：

```asm
	// ---- monitorenter: thin-lock fast path  (JVM bytecode: monitorenter)
	add x1, x0, #8              // &obj->lockword
	ldr w2, [x28]               // x28 = current MJThread, [0] = thread id
	lsl x2, x2, #32
	add x2, x2, #5              // owner<<32 | rec 1<<2 | THIN
.L8:
	ldaxr x3, [x1]              // load-acquire exclusive
	cbnz x3, .L9                // locked/inflated -> runtime
	stxr w4, x2, [x1]           // try to claim
	cbnz w4, .L8                // lost the reservation, retry
	b .L10
.L9:
	clrex
	bl mj_monitor_enter_slow     // recursion, contention, inflation
.L10:
	// install exception handler record (prev, fp, sp, pc)   ← 相当于 Exception table 的 "any"
	...
	<方法体>
	...
	// ---- monitorexit: thin-lock fast path
.L11:
	ldxr x3, [x1]
	cmp x3, x2
	b.ne .L12
	stlxr w4, xzr, [x1]         // store-release 0: unlock
	cbnz w4, .L11
	...
.L5:                            // exception inside synchronized: unlock + rethrow
	<monitorexit>
	bl mj_throw
```

- **快速路径内联**：没有竞争时，加锁和解锁各只有一个 LL/SC 循环，不调用任何函数，也不进内核。
- **acquire/release 语义**：加锁用 `LDAXR`（acquire），解锁用 `STLXR`（release）。这正好就是 JMM 里"解锁 happens-before 之后的加锁"在硬件上的实现。
- **异常安全**：进入同步块后，编译器在栈帧里挂一条"异常处理记录"（prev, fp, sp, pc），链在当前线程 `[x28, #8]` 上。`mj_throw` 沿着这条链展开栈，处理器先 `monitorexit`，再重新抛出。这就是 javac 的 `any` 异常表项。
- **`return` 穿过同步块**：编译器在 `return` 前按由内到外的顺序摘掉处理记录并 `monitorexit`。
- **同步方法**：`ACC_SYNCHRONIZED` 在编译时被改写成 `synchronized (this) { 方法体 }`；静态方法则锁 `MJC_类名` 这个 Class 对象。
- **volatile 字段**：读用 `LDAR`，写用 `STLR`。

### 3.4 慢路径（`runtime.c`）

- `mj_monitor_enter_slow`：处理重入（CAS 让 rec + 1）、短暂自旋（超过 8 次以后每次 `sched_yield`），自旋 64 次以后膨胀，然后进入 `monitor_enter`。
- `monitor_enter / monitor_exit`：`ObjectMonitor` 用一个小自旋锁保护。拿不到锁的线程把自己挂到 entry 队列上，然后 `park()`（`sync_wait` 自己的信号量）。解锁的线程**直接把所有权交给**队首线程（`owner = next`），然后 `unpark()` 它。
- `mj_wait`：把 rec 存进 `saved_rec`，加入 wait 队列，把 monitor 交给下一个排队者，然后 park。等到被 notify 挪回 entry 队列、并且有人把锁交给它时才会醒来，此时 rec 已经恢复。
- `mj_notify / mj_notify_all`：把线程从 wait 队列挪到 entry 队列，不直接唤醒。
- `mj_thread_join`：和 JDK 一样写成 `synchronized(t) { while (alive) t.wait(); }`；线程结束时 `mj_thread_finish` 对 Thread 对象执行 `notifyAll`。

---

## 4. 构建和运行

先按平常的方式构建顶层（生成 `user/libc.so`、`fs.img`、`fs_ext2.img` 和内核镜像），然后：

```sh
make -C minijava                                         # 编译器 + 4 个示例 (minijava/build/_mj*)
make -C minijava install                                 # 写入 fs_ext2.img 的 /bin，源码放进 /root
make -C minijava qemu                                    # install + 重建 SD 镜像 + 启动 QEMU
make -C minijava asm S=examples/Counter.java | less      # 只看生成的汇编
```

注意顶层的 `make qemu` 会重新生成 `fs_ext2.img`，刚才 install 进去的程序会被覆盖掉，所以用 `make -C minijava qemu` 启动。上真机的话，把 `minijava/build/_mj*` 拷进 ext2 根分区的 `/bin` 即可。在 xv6 shell 里：

```
$ mjCounter
4 threads x 100000 increments, expected 400000
racy             = 310957   <-- lost 89043 updates
volatile         = 334308   <-- lost 65692 (volatile is not atomic)
4 threads x 20000 locked increments, expected 80000
synchronized m() = 80000
synchronized {}  = 80000
```

每次运行丢的数量都不一样，偶尔 `volatile` 那一行会碰巧不丢。`volatile` 只保证可见性和顺序，`x = x + 1` 仍然是"读、加、写"三步，不是原子的。

```
$ mjSyncRules
1. reentrant depositTwice -> balance=10
   nested synchronized depth = 50
2. withdraw threw RuntimeException@0xd140 ...
   ...and another thread could still lock it: balance=1010
3. withdraw(3) returned 1007
4. notify() without the lock -> IllegalMonitorStateException
   wait() without the lock   -> IllegalMonitorStateException
5. static synchronized register() count = 3000 (expected 3000)
   balance = 3007 (expected 3007)

$ mjProdCons
consumer 1 took 400 items, sum=407219
consumer 2 took 600 items, sum=343281
total=750500 expected=750500  OK
```

加 `-v` 可以看到锁的升级过程：

```
$ mjCounter -v
[mj] T1 started T2 (kernel tid 27)
[mj] T1 inflate 0xd170 (wait): thin owner=main rec=1 -> ObjectMonitor 0x4d6f0
[mj] T1 wait() on 0xd170, releases monitor
[mj] T4 inflate 0xd1d0 (notifyAll): thin owner=Thread-2 rec=1 -> ObjectMonitor 0x4d730
[mj] T2 notifyAll on 0xd170 moved 1 waiter(s) to the entry list
[mj] T1 returns from wait() on 0xd170
...
[mj] stats: contended-thin=3 inflations=8 parks=3 waits=1
```

第一行 inflate 是 `main` 调 `w0.join()`：`join` 内部要 `wait()`，所以把 `w0` 这个 Thread 对象的锁膨胀了。

---

## 5. 支持的 Java 子集

- `class A extends B`，单继承，虚方法（vtable），每个类一个构造函数（不支持 `super(...)`，父类字段请在子类构造函数里赋值）
- 字段：`int`、`boolean`、`int[]`、对象引用；`static` 和 `volatile` 字段
- 语句：`if/else`、`while`、`for`、`return`、`x = e`、`+=`、`-=`、`++`、`--`、`synchronized`、`try/catch`（可以多个 catch）、`throw`
- 表达式：`+ - * / %`（`int` 是 32 位，带除零检查）、比较、`&& || !`、`new`、`new int[n]`、`a[i]`（带越界检查）、`a.length`、`null`、`this`
- `System.out.print/println(...)`，在其中可以写 `"a" + x + "b"` 拼接；每行一次 `write()`，所以多线程输出不会交错
- 内置类：`Object`、`Thread`（`start`、`join`、`run`、`Thread.yield()`、`Thread.sleep(ms)`）、`Exception`、`RuntimeException`、`InterruptedException`、`IllegalMonitorStateException`、`IllegalThreadStateException`、`ArithmeticException`、`NullPointerException`、`ArrayIndexOutOfBoundsException`
- 任意对象上都可以调用 `wait()`、`notify()`、`notifyAll()`

不支持：`finally`、`break/continue`、`?:`、接口、方法重载、`String` 变量（字符串只能在 print 里用）、泛型、lambda、GC。

## 6. 和 HotSpot 的差别，以及可以继续做的方向

1. **没有 GC**。真正的难点在于 GC 和线程的配合：GC 要先让所有 Java 线程停在 safepoint，才能安全地扫描它们的栈。编译器要在循环回边和方法调用处插入 safepoint 轮询，并为每个调用点生成栈映射（哪些槽是引用）。
2. **FIFO 直接交接 vs 竞争式交接**。这里解锁时直接把所有权交给队首线程，公平但容易形成 lock convoy（每次加锁都要睡一次）。HotSpot 解锁时只唤醒继任者，让它和新来的线程一起竞争，并配合自适应自旋，吞吐量高得多。可以改成这种方式对比一下。
3. **不做 deflation**。HotSpot 会在后台把不再竞争的 ObjectMonitor 缩回去（JDK 15+ 是异步 deflation）。
4. **没有偏向锁**。可以在锁字里加一个"偏向"状态做实验，体会撤销偏向为什么需要 safepoint。
5. **内核 futex**。用 `futex(addr, WAIT/WAKE)` 代替每线程信号量，就能去掉 48 个线程的上限，也更接近 Linux 上的真实行为。
6. **`wait(ms)`、`interrupt()`**：需要带超时的 park 和线程中断标志。
7. **在 xv6 里自举**：`mjc.c` 只用了标准 C，可以用仓库里的 `tcc` 移植到 xv6 上，但还缺一个能在 xv6 里运行的 AArch64 汇编器/链接器。
