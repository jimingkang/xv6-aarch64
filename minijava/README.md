# Java on xv6-aarch64：javac → .class → JVM，从字节码看 `synchronized` 和线程

```
 examples/*.java ──mjavac (宿主机)──▶ *.class ──拷进 fs_ext2.img──▶ mjvm (xv6 用户程序) 解释执行
                └──javac --release 8──▶ *.class ──────────────────────┘   （两种 class 都能跑）
```

这个目录现在走的是和真 Java 一样的两段式流程：

1. **`javac/mjavac.c`**：一个小 javac。把 Java 子集源码编译成**标准 class 文件**，包括常量池、字节码、异常表和 `ACC_SYNCHRONIZED`。生成的 class 能通过 HotSpot 的 `java -Xverify:all` 校验，也能用 `javap -c -v` 反汇编。
2. **`jvm/mjvm.c`**：一个跑在 xv6 上的小 JVM。负责 class 文件解析、加载、链接（vtable）、类初始化，用字节码解释器执行。它也有自己的线程、对象头锁字、ObjectMonitor、`wait/notify` 和按异常表分派的异常处理。

mjvm 不只能跑 mjavac 的输出，用真 `javac --release 8` 编出来的 class 也能直接跑，这是校验 mjvm 是否忠实实现了 JVM 规范的最好办法。第一版原型（mjc：Java 直接编译成 AArch64 汇编，没有 JVM）保留在 `aot/` 里，可以对比两种实现。

```
minijava/
├── javac/mjavac.c      编译器：词法 → 语法 → AST → 类型检查 + 字节码生成 → class 文件写出
├── jvm/mjvm.c          JVM：class 解析、链接、初始化、解释器、对象/字符串/数组、线程、监视器、异常
├── jvm/plat.h          JVM 需要的操作系统服务（内存、文件、线程、信号量、时间）
├── jvm/plat_xv6.c      ……在 xv6 上的实现（sbrk、thread_clone、tjoin、sync_create(SYNC_SEM) 等）
├── jvm/plat_host.c     ……在 Linux/macOS 上的实现（pthread），方便在开发机上对照 HotSpot 测试
├── examples/           4 个标准 Java 程序（javac 和 mjavac 都能编译）
├── aot/                第一版：mjc + 运行时，直接生成 AArch64 汇编
└── Makefile
```

仓库里现有的任何文件都没有改，只复用了 java 分支已有的系统调用。

---

## 1. 快速上手

```sh
make -C minijava                    # build/mjavac, build/classes/*.class, build/_mjvm
make -C minijava javac-classes      # 另外用真 javac --release 8 编一份到 build/jclasses/
make -C minijava host-test          # 在开发机上：同一批 class 先跑 HotSpot，再跑 mjvm(host)
make -C minijava javap C=Counts     # 看 mjavac 生成的字节码
make -C minijava qemu               # 写进 fs_ext2.img（/bin/mjvm，/root/classes）并启动 QEMU
```

在 xv6 里：

```
$ mjvm -cp /root/classes Counter
4 threads x 20000 increments, expected 80000
racy             = 68751   <-- lost 11249 updates
volatile         = 68482   <-- lost 11518 (volatile is not atomic)
synchronized m() = 80000
synchronized {}  = 80000

$ mjvm -cp /root/jclasses Counter        # 同一个程序，真 javac 编出来的 class
4 threads x 20000 increments, expected 80000
racy             = 68762   <-- lost 11238 updates
volatile         = 69222   <-- lost 10778 (volatile is not atomic)
synchronized m() = 80000
synchronized {}  = 80000

$ mjvm -cp /root/classes SyncRules
1. reentrant depositTwice -> balance=10
   nested synchronized depth = 50
2. synchronized method threw: insufficient funds
   synchronized block threw: thrown inside synchronized block
   ...another thread could still lock it: balance=1010
3. withdraw(3) returned 1007
4. notify() without the lock -> java.lang.IllegalMonitorStateException: current thread is not owner
   wait() without the lock   -> IllegalMonitorStateException
5. static synchronized register() count = 3000 (expected 3000)
   balance = 3007 (expected 3007)

$ mjvm -cp /root/classes ProdCons
Thread-2 took 400 items, sum=236924
Thread-3 took 600 items, sum=513576
total=750500  OK
```

这 4 个程序在 HotSpot、宿主机上的 mjvm 和 QEMU 里的 mjvm 上输出一致（丢失更新的次数每次不同，属于正常现象）。

`mjvm -v` 会打印类加载、线程启动、锁膨胀、park 的过程；`mjvm -stats` 只在退出时打印统计：

```
$ mjvm -v -cp /root/jclasses SyncRules
[mjvm] loaded SyncRules from /root/jclasses/SyncRules.class (2 methods, 0 fields, cp 127)
[mjvm] loaded Account from /root/jclasses/Account.class (6 methods, 1 fields, cp 43)
...
[mjvm] main: start Thread-0 (VM thread T2, kernel tid 19)
[mjvm] main: inflate Bumper@9d240 (wait), thin owner=main rec=1 -> ObjectMonitor 0x195e50
...
[mjvm] exit: 67064 bytecodes, 4 threads, 2085 KB heap, contended-thin=0 inflations=1 parks=1 waits=1, 1148 ms
```

`inflate Bumper@… (wait)` 是 `main` 调用 `t.join()` 时发生的：`join()` 内部要在 Thread 对象上 `wait()`，wait 需要 wait set，所以把这个对象的锁膨胀成了 ObjectMonitor。注意 `-v` 打印本身会改变线程时序，很多时候线程就不再重叠了，想看数据竞争请用不带 `-v` 的运行。

---

## 2. 编译阶段：`synchronized` 变成了什么字节码

`make -C minijava javap C=Counts` 看 `incBlock()`，mjavac 的输出和 javac 逐条一致：

```java
void incBlock() { synchronized (lock) { blockSafe = blockSafe + 1; } }
```
```
 0: aload_0
 1: getfield      #15   // Field lock:Ljava/lang/Object;
 4: dup
 5: astore_1            // 锁对象存进局部变量，解锁时要用同一个对象
 6: monitorenter
 7: aload_0             // ┐
 8: aload_0             // │ 方法体
 9: getfield      #28   // │
12: iconst_1            // │
13: iadd                // │
14: putfield      #28   // ┘
17: aload_1
18: monitorexit         // 正常路径解锁
19: goto          27
22: astore_2            // 异常路径：先解锁，再重新抛出
23: aload_1
24: monitorexit
25: aload_2
26: athrow
27: return
Exception table:
   from    to  target type
     7    19    22   any       ← 方法体里任何异常都跳到 22
    22    25    22   any       ← 解锁本身出异常也回到 22（javac 的固定写法）
```

块里有 `return` 时，编译器先把返回值存进临时变量，`monitorexit` 之后再取出来返回（`SyncRules.depth()`）：

```
 9: iconst_0
10: istore        4      // 暂存返回值
12: aload_2
13: monitorexit         // 先解锁
14: iload         4
16: ireturn
```

**同步方法**在字节码里什么都看不到，只有方法的 access flags 多了一位：

```
public synchronized void deposit(int);
  flags: (0x0021) ACC_PUBLIC, ACC_SYNCHRONIZED
```

加锁和解锁由 JVM 在调用和返回时（包括异常返回时）完成。

---

## 3. 运行阶段：mjvm 怎么实现这些

### 3.1 类加载、链接、初始化

- `load_class`：按 `-cp 目录/类名.class` 读文件，`parse_class` 解析常量池、字段、方法和 Code 属性（字节码与异常表）。字符串全部 intern，之后用指针比较名字。
- `link_class`：先加载父类，给实例字段分配槽位（继承父类的槽位），给静态字段分配存储，构造 **vtable**：复制父类的 vtable，同名同描述符的方法覆盖原槽位，否则追加新槽位。每个类还有一个 `java.lang.Class` 镜像对象，它也有锁字，`static synchronized` 锁的就是它。
- `init_class`：按 JVMS §5.5 执行 `<clinit>`。规范要求类初始化本身是同步的，这里就直接用镜像对象的 monitor 加 `wait/notifyAll`：别的线程正在初始化这个类时，当前线程会一直 wait，直到对方完成。
- 内置类（`Object`、`String`、`StringBuilder`、`Thread`、`System`、`PrintStream`、`Math`、`Throwable` 及常用异常）在 C 里定义，方法是 native 函数。用户类可以直接继承它们，比如 `extends Thread`、`extends RuntimeException`。

### 3.2 解释器

`invoke()` 为每次 Java 调用建一个 C 栈帧，帧里有局部变量数组和操作数栈，然后进入一个按 opcode 分派的大 `switch`。long 和 JVM 规范一样占两个槽。`invokevirtual` 用 `接收者->cls->vtable[m->vindex]` 分派，`invokeinterface` 按名字和描述符查找，`invokespecial` 和 `invokestatic` 直接调用常量池里解析好的方法。

### 3.3 异常

一条指令抛异常（或者被调用的方法带着异常返回）时，`exception:` 分支在**当前方法的异常表**里找一条满足 `start <= pc < end` 且类型匹配的记录（类型为 0 表示 `any`）。找到就清空操作数栈、压入异常对象、跳到 handler；找不到就从这一帧返回 -1，由调用者继续找。javac 为 `synchronized` 块生成的 `any` 条目就是在这里起作用的。对 `ACC_SYNCHRONIZED` 方法，帧在因异常退出前由 VM 自己执行 `monitor_exit`。

### 3.4 对象头和锁

和 `aot/` 版本一样，每个对象头有一个 64 位锁字（HotSpot 叫 mark word）：

```
0                                   无锁
owner_id<<32 | rec<<2 | 0b01        thin lock（轻量级锁），只需一次 CAS，不进内核
ObjectMonitor* | 0b10               膨胀后的重量级锁：owner、rec、entry 队列、wait 队列
```

- `monitorenter`、`monitorexit` 和 `ACC_SYNCHRONIZED` 都调用 `monitor_enter()` / `monitor_exit()`。
- 快速路径是一次 `__atomic_compare_exchange`（acquire）。Cortex-A53 是 ARMv8.0，没有 LSE，所以 gcc 生成 `LDAXR/STXR` 循环；解锁用 release 语义（`STLXR`）。
- 有竞争时先自旋，超过 8 次后每次 `sched_yield()`，64 次以后膨胀：分配一个 ObjectMonitor，把当前持有者和重入次数抄进去，再 CAS 进锁字。原持有者解锁时 CAS 会失败，于是改走 monitor 路径。
- 阻塞的线程挂在 entry 队列上，在**自己的信号量**上睡眠（`sync_create(SYNC_SEM, 0)`，相当于 HotSpot 的 ParkEvent）。解锁时把所有权直接交给队首线程并唤醒它。
- `wait()`：保存重入次数，进入 wait 队列，完全释放锁，然后 park。`notify()`：把一个线程从 wait 队列挪到 entry 队列；`notifyAll()` 挪全部。
- `volatile` 字段：`getfield/getstatic` 用 load-acquire（`LDAR`），`putfield/putstatic` 用 seq-cst store（`STLR`）。

### 3.5 线程

- `Thread.start()`（native）：建一个 VM 线程结构和 512 KB 的 C 栈，调用 `thread_clone(thread_main, …)`。新的内核线程和其他线程共享地址空间，在里面调用对象的 `run()`。xv6 进程自带的栈只有 16 KB，所以 Java 的 `main` 也跑在一个这样 clone 出来的线程上。
- `Thread.join()`：和 JDK 的实现一样，就是 `synchronized(t) { while (t.isAlive()) t.wait(); }`。线程结束时 VM 对 Thread 对象执行 `notifyAll()`。
- 未捕获的异常打印成 `Exception in thread "Thread-0" java.lang.RuntimeException: …`，后面跟 `at 类.方法 (bytecode pc N)` 形式的栈信息。
- 进程退出前，启动线程（相当于 JVM 的 launcher）会 `tjoin` 所有 Java 线程。
- `System.out` 每个线程先写进自己的行缓冲，再用一次加锁的 `write()` 输出，所以多线程打印不会交错。

### 3.6 映射到 xv6 的系统调用（`jvm/plat_xv6.c`）

| JVM 需要 | xv6 java 分支提供的 |
|---|---|
| 堆内存 | `sbrk`（mjvm 的 bump allocator 每次要 1 MB） |
| 读 class 文件 | `open / fstat / read` |
| Java 线程 | `thread_clone(fn, arg, stack)`，`texit`，`tjoin` |
| park / unpark | `sync_create(SYNC_SEM, 0)`，`sync_wait`，`sync_signal` |
| 自旋退避、`Thread.yield()` | `sched_yield` |
| `Thread.sleep`、`currentTimeMillis` | `usleep`，`clock_us` |

---

## 4. mjavac 支持的 Java 子集

- 类、单继承、构造函数（每个类一个，可以写 `super(...)`）、字段初始化器、`static` / `volatile` 字段
- 实例方法和静态方法（不支持重载），`synchronized` 方法，`throws` 子句（解析后忽略，不检查受检异常）
- 类型：`int`、`boolean`、`long`（只用于 `Thread.sleep` 之类的参数）、`String`、类引用、一维数组（`int[]`、`Foo[]`）
- 语句：`if/else`、`while`、`for`、`break`、`continue`、`return`、`synchronized`、`try/catch`（可以多个 catch）、`throw`、`=`、`+=`、`-=`、`*=`、`/=`、`++`、`--`
- 表达式：四则运算和 `%`、比较、`&& || !`、`?:`、字符串 `+`（编译成 StringBuilder）、`new`、`new T[n]`、`a[i]`、`a.length`、`null`、`this`
- 库：`System.out.print/println`，`Object.wait/notify/notifyAll/toString/hashCode/equals`，`String.length/charAt`，`Thread`（`start`、`run`、`join`、`getName`、`isAlive`、`currentThread`、`yield`、`sleep`），`Throwable.getMessage`，以及常用异常类

不支持：接口（mjavac 不支持 `implements`，但 mjvm 能运行 javac 编出来的 `new Thread(runnable)`）、`finally`、`switch`、`float/double`、泛型、lambda、内部类。

mjvm 能执行的字节码比 mjavac 能生成的多：`tableswitch/lookupswitch`、`long` 运算、`wide`、`checkcast/instanceof`、对象数组等都实现了。用 `javac --release 8` 编译时，只要别用到上面不支持的特性（尤其是 Java 9 以后改用 `invokedynamic` 的字符串拼接，`--release 8` 正好避开了它），一般都能跑。

## 5. 和 HotSpot 的差别，以及下一步

1. **没有 GC**。这是最值得做的下一步，而且和多线程直接相关：GC 要先让所有线程停在 **safepoint**。对解释器来说，可以在 `goto` 回跳和方法调用处检查一个全局标志；还需要知道每个栈帧里哪些槽是引用。最简单的做法是保守扫描，或者用 class 文件里的类型信息推导。
2. **锁交接是 FIFO 直接交接**，公平但容易形成 convoy。HotSpot 解锁时只唤醒继任者，让它和新来的线程一起竞争，再加上自适应自旋。
3. **没有 deflation，也没有偏向锁**。
4. **内核 futex**。现在每个 Java 线程占一个内核同步对象，而 `NSYNC = 64`，所以最多 48 个线程。加一个 `futex(addr, WAIT/WAKE)` 系统调用就能去掉这个限制。
5. **`wait(ms)`、`interrupt()`**。需要带超时的 park 和中断标志。现在 `wait(long)` 会忽略超时。
6. **JIT**。`aot/` 里的 mjc 已经演示了怎么把 `monitorenter` 内联成 `LDAXR/STXR`，可以把它和解释器结合成一个最简单的模板 JIT。
