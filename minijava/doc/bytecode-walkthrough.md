# Counter 的字节码逐条讲解：操作数栈怎么变，synchronized 怎么落地

配图：

- [`incblock-stack.png`](incblock-stack.png)：`Counts.incBlock()` 每条指令执行后的操作数栈、locals 和锁字
- [`mjvm-interp-frame.png`](mjvm-interp-frame.png)：mjvm 的 `invoke()` 帧布局、取指/分派循环和异常路径

完整的 `javap -c -v -p` 输出在 [`javap-Counter.txt`](javap-Counter.txt)，用 `make -C minijava javap C=Counts` 可以重新生成。

源码：`examples/Counter.java`。类文件是 mjavac 生成的，版本 49.0（Java 5 格式，不需要 StackMapTable），`java -Xverify:all` 可以通过校验。

---

## 0. 记号

JVM 是栈机：每个方法帧有一个**局部变量表** `locals[max_locals]` 和一个**操作数栈** `stack[max_stack]`。在 mjvm 里，这两个都是 `invoke()` 里的 C 变长数组：

```c
// mjvm.c invoke()
Slot locals[m->max_locals + 1];
Slot stack[m->max_stack + 2];
Slot *sp = stack;              // 指向下一个空槽
...
for(;;){ ipc = pc; uint8_t op = U1(); switch(op){ ... } }
```

下面的表格写的都是**指令执行之后**的栈，`[a, b]` 中 `b` 是栈顶。

---

## 1. incRacy()：为什么会丢更新

```
void incRacy() { racy = racy + 1; }

stack=3, locals=1, args_size=1
 0: aload_0
 1: aload_0
 2: getfield  #19   // Field racy:I
 5: iconst_1
 6: iadd
 7: putfield  #19   // Field racy:I
10: return
```

| pc | 指令 | 执行后的栈 | mjvm 做了什么 |
|---|---|---|---|
| 0 | aload_0 | [this] | `(sp++)->j = locals[0].j`，先压一个 this，留给 putfield 用 |
| 1 | aload_0 | [this, this] | 再压一个 this，留给 getfield 用 |
| 2 | getfield #19 | [this, v] | `resolve_field` 查到 Field*（第一次解析之后缓存在 `cp[19].resolved`），`o=POPA()`，压入 `FIELDS(o)[0]` |
| 5 | iconst_1 | [this, v, 1] | 栈深到 3，就是 max_stack |
| 6 | iadd | [this, v+1] | `PUSHI(POPI()+POPI())` |
| 7 | putfield #19 | [] | 弹出 v+1，再弹出 this，写回字段 |
| 10 | return | | `depth--`，返回 0 |

pc 2 的读和 pc 7 的写之间隔着 3 条指令。mjvm 的线程是真正的内核线程，另一个 Worker 完全可能在这中间读到同一个旧的 v，两边都写回 v+1，结果就少加了一次。

## 2. incVolatile()：字节码一模一样

字节码和 incRacy **完全相同**，区别只在字段表里 `vol` 的 flags = `0x0041 ACC_PUBLIC, ACC_VOLATILE`。mjvm 在 getfield/putfield 里会看这个标志：

```c
// getfield：volatile 读 = load-acquire (LDAR)
v = (f->flags & ACC_VOLATILE) ? __atomic_load_n(&p->j, __ATOMIC_ACQUIRE) : p->j;
// putfield：volatile 写 = STLR，用 SEQ_CST 是为了和后面的 volatile 读保持顺序
if(f->flags & ACC_VOLATILE) __atomic_store_n(&p->j, v, __ATOMIC_SEQ_CST); else p->j = v;
```

这样每次读写都可见、有序，但"读 → 加 → 写"仍然是三步，结果照样丢更新。

## 3. incSafe()：ACC_SYNCHRONIZED，字节码里看不到锁

```
public synchronized void incSafe();
  flags: (0x0021) ACC_PUBLIC, ACC_SYNCHRONIZED
  stack=3, locals=1     // 字节码和 incRacy 一样，只是字段换成 #25 safe
```

字节码里**没有** monitorenter/monitorexit，锁全由 VM 来做：

```c
// invoke() 入口
if(m->flags & ACC_SYNCHRONIZED){
  syncobj = (m->flags & ACC_STATIC) ? cls->mirror : locals[0].a;   // static 方法锁 Class 对象
  monitor_enter(t, syncobj);
}
// 每个 xreturn
if(syncobj && monitor_exit(t, syncobj) < 0) goto exception_noframe;
// 异常逃出本方法（exception: 标签末尾）
if(syncobj){ ...; monitor_exit(t, syncobj); ... }
```

所以"return 一定解锁"和"抛异常也解锁"这两条，对 synchronized 方法来说是 VM 保证的，不靠 javac 生成代码。

## 4. incBlock()：synchronized 块 = monitorenter + monitorexit + 异常表

对应的图是 **[incblock-stack.png](incblock-stack.png)**。

```
void incBlock() { synchronized (lock) { blockSafe = blockSafe + 1; } }

stack=3, locals=3, args_size=1
 0: aload_0
 1: getfield      #15   // Field lock:Ljava/lang/Object;
 4: dup
 5: astore_1
 6: monitorenter
 7: aload_0
 8: aload_0
 9: getfield      #28   // Field blockSafe:I
12: iconst_1
13: iadd
14: putfield      #28   // Field blockSafe:I
17: aload_1
18: monitorexit
19: goto          27
22: astore_2
23: aload_1
24: monitorexit
25: aload_2
26: athrow
27: return
Exception table:
   from    to  target type
      7    19    22   any
     22    25    22   any
```

### 正常路径

下表中，锁字是 lock 对象头里的 `lock` 字段。T2 表示 2 号 VM 线程（main 是 1 号）。

| pc | 指令 | 执行后的栈 | locals[1] | 锁字 | 说明 |
|---|---|---|---|---|---|
| 0 | aload_0 | [this] | | 0 | |
| 1 | getfield #15 | [lock] | | 0 | 求出 `synchronized(...)` 括号里的表达式 |
| 4 | dup | [lock, lock] | | 0 | 一份给 monitorenter 用，一份存起来 |
| 5 | astore_1 | [lock] | lock | 0 | **把锁对象存进局部变量**：即使块里改了 `this.lock`，解锁的也还是当初锁住的那个对象 |
| 6 | **monitorenter** | [] | lock | `T2 rec=1 \|01` | `monitor_enter`：锁字是 0，一次 CAS 就拿到 thin 锁（LDAXR/STXR，acquire 语义） |
| 7 | aload_0 | [this] | lock | T2 | **受保护区间 [7,19) 从这里开始** |
| 8 | aload_0 | [this, this] | lock | T2 | |
| 9 | getfield #28 | [this, v] | lock | T2 | |
| 12 | iconst_1 | [this, v, 1] | lock | T2 | 栈深 3 = max_stack |
| 13 | iadd | [this, v+1] | lock | T2 | |
| 14 | putfield #28 | [] | lock | T2 | 写 blockSafe。此时还持有锁，所以别的线程插不进来 |
| 17 | aload_1 | [lock] | lock | T2 | 取回第 5 条存的锁对象 |
| 18 | **monitorexit** | [] | lock | 0 | `monitor_exit`：CAS 把锁字改回 0，用 release 语义（STLXR），保证临界区里的写先于"解锁"被别的线程看见 |
| 19 | goto 27 | [] | lock | 0 | `pc = ipc + 8`，跳过 handler |
| 27 | return | | | 0 | 这不是 synchronized 方法，syncobj == 0，直接返回 |

如果有别的线程在第 6 条时已经持有这把锁，`monitor_enter` 会先自旋 64 次，然后 `inflate()`：锁字变成 `ObjectMonitor* | 10`，当前线程进入 entry 队列，再 `park()`（底层是 `sync_wait` 信号量）。持有者在第 18 条 `monitor_exit` 时会把 owner 直接交给队首线程，并 `unpark` 它。用 `mjvm -v Counter` 可以看到 `inflate … (contention)` 和 `blocked on …` 这两类日志。

### 异常路径

假设 7..18 之间某条指令抛了异常，比如字段访问遇到空指针，或者被调方法返回 -1：

1. `THROW()` 或 athrow 会设置 `t->exc = e`，然后 `goto exception`。
2. 执行 `find_handler(t, m, ipc, e)`。它按顺序扫异常表：ipc ∈ [7,19)，type 是 `any`（0），命中，handler = 22。
3. handler 入口：`t->exc = 0; sp = stack; PUSHA(e); pc = 22;`。**操作数栈先被清空，然后只放异常对象**，这是 JVMS 规定的。

| pc | 指令 | 执行后的栈 | locals[1] | locals[2] | 锁字 |
|---|---|---|---|---|---|
| (进入 handler) | | [e] | lock | | T2 rec=1 |
| 22 | astore_2 | [] | lock | e | T2 |
| 23 | aload_1 | [lock] | lock | e | T2 |
| 24 | **monitorexit** | [] | lock | e | **0** |
| 25 | aload_2 | [e] | lock | e | 0 |
| 26 | athrow | | | | 0 |

在第 26 条，ipc = 26 不在任何区间里，所以本方法里找不到 handler。这时 `record_frame` 把这一帧记进栈轨迹，`invoke()` 返回 -1。调用者 Worker.run 的 invokevirtual 看到 -1，就在**它自己的**异常表里接着找，异常就这样一层层往外传。

**为什么还有第二条 `[22,25) → 22`？** 它罩住的是 handler 自己的 astore_2、aload_1 和 monitorexit。如果第 24 条 monitorexit 本身抛异常（比如 IllegalMonitorStateException），控制流会回到 22 再试一次解锁。这是 javac 的固定写法，mjavac 的 `gen_sync()` 照着生成了同样的形状。to 是开区间，所以 25 和 26 不在里面，第 26 条的 athrow 不会被自己抓回来。

## 5. return 写在 synchronized 块里：SyncRules 的 Account.depth()

```
int depth(int n) { synchronized (this) { if (n == 0) return 0; return 1 + depth(n - 1); } }

 0: aload_0       1: dup        2: astore_2     3: monitorenter
 4: iload_1       5: iconst_0   6: if_icmpne 17
 9: iconst_0     10: istore 4  12: aload_2     13: monitorexit   14: iload 4   16: ireturn
17: iconst_1     18: aload_0   19: iload_1     20: iconst_1      21: isub
22: invokevirtual #30 depth:(I)I
25: iadd         26: istore 5  28: aload_2     29: monitorexit   30: iload 5   32: ireturn
33: astore_3     34: aload_2   35: monitorexit 36: aload_3       37: athrow
Exception table:  4–33 → 33 any;  33–36 → 33 any
```

关键在 9..16 这段：mjavac 先 `istore 4` 把返回值存进局部变量，然后**先** monitorexit，**再** `iload 4` 把返回值取回来，最后 ireturn。

真 javac（`build/jclasses/Account.class`）的写法更省事：

```
 8: iconst_0     9: aload_2    10: monitorexit   11: ireturn      ← 返回值 0 一直压在锁对象下面
20: iadd        21: aload_2    22: monitorexit   23: ireturn
```

monitorexit 只弹出栈顶的锁对象，压在它下面的返回值不会被动到，所以根本不用借局部变量。两种写法都合法，语义也一样：**解锁一定发生在 ireturn 之前**。javac 还把 `n == 0` 编成了 `iload_1; ifne`，mjavac 编成的是 `iconst_0; if_icmpne`。

递归调用时（mjavac 版的第 22 条），同一个线程重入同一把锁，thin 锁字里的 rec 会 +1（`CAS w → w+4`）。递归层数超过 `THIN_REC_MAX` 才会膨胀成 ObjectMonitor。`depth(50)` 只到 rec=51，一直都是 thin 锁。

## 6. 调用的过程：Worker.run 里的 `c.incBlock()`

```
24: aload_0
25: getfield      #15   // Field c:LCounts;      栈: [c]
28: invokevirtual #31   // Method Counts.incBlock:()V
31: iinc          1, 1
34: goto          9
```

第 28 条在 mjvm 里的执行过程（对应图 mjvm-interp-frame.png 左半边）：

1. `rm = resolve_method(t, cls, 31)` 拿到 `Counts.incBlock`，解析结果缓存在 `Worker.cp[31].resolved`。
2. `args0 = sp - rm->argslots`，这里 argslots = 1，只有 this。`recv = args0[0].a`，然后做 NULLCHK。
3. 虚分派：`target = recv->cls->vtable[rm->vindex]`。如果 c 是 Counts 的子类并且覆盖了 incBlock，这里就会取到子类的方法。
4. `sp = args0`，调用者的栈退回去，然后 `invoke(t, target, args0, &r)`。**在 C 里递归一层**：新的 `invoke()` 建立自己的 locals/stack 两个变长数组，`memcpy(locals, args0, 8*argslots)`，于是被调者的 locals[0] = c。
5. 返回 0 时，如果有返回值就 `*sp++ = r`。返回 -1 就 `goto exception`，在 run 自己的异常表里找 handler。

一个 Java 线程的全部 Java 调用链，就是它那块 512KB clone 栈上一串嵌套的 `invoke()` C 帧。递归深度上限是 600 层，超过就抛 StackOverflowError。

## 7. main 里的字符串拼接（顺带一提）

```
22: getstatic     #30   // System.out                     [out]
25: new           #48   // StringBuilder                  [out, sb]
28: dup                                                    [out, sb, sb]
29: invokespecial #49   // StringBuilder.<init>()V         [out, sb]
32: ldc           #51   // "4 threads x "                  [out, sb, "…"]
34: invokevirtual #55   // append(String)                  [out, sb]     (返回 this)
37: iload_1                                                [out, sb, n]
38: invokevirtual #58   // append(I)                       [out, sb]
 …
50: invokevirtual #64   // toString()                      [out, str]
53: invokevirtual #38   // println(String)                 []
```

`new` 只负责分配对象，`dup` 留下一份引用，`invokespecial <init>` 会消耗掉一份，所以执行完以后栈上还剩一份已经初始化好的对象。在 mjvm 里，StringBuilder 和 PrintStream 的方法都是 native（`m->native`），不建 Java 帧。

---

### 和 javac 的差别

- `diff <(javap -c -cp build/jclasses Counts) <(javap -c -cp build/classes Counts)` 显示：**incRacy、incVolatile、incSafe、incBlock 四个方法的指令序列和异常表与 javac 逐条相同**，只有常量池编号不一样。另外 mjavac 给所有成员都标了 `public`，而 javac 对没写修饰符的成员给的是包私有。这两点对运行都没有影响。
- 有差别的是第 5 节 depth() 那种在同步块里 return 的写法：mjavac 借了一个局部变量来暂存返回值，javac 直接把返回值留在操作数栈上。
- 想自己对比，可以运行 `make -C minijava JAVAC=javac`，编出 `build/jclasses/`。
