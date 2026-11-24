# mjvm 的 thin lock：CAS 的实现代码和对应的 AArch64 指令

文中代码都来自 `minijava/jvm/mjvm.c`。反汇编用的是 `aarch64-linux-gnu-objdump -d -l build/mjvm.o`，编译选项为 `-Os -mcpu=cortex-a53+nofp -mno-outline-atomics`。

## 1. 锁字布局（mjvm.c:444-450）

每个对象头里有两个字段：`Object { Class *cls; volatile int64_t lock; }`，所以 `lock` 位于偏移 +8。锁字只用来存锁状态，不像 HotSpot 那样和 hash、GC age 共用。

```c
#define LW_THIN 1L
#define LW_INFLATED 2L
#define LW_TAG(w) ((w) & 3L)
#define THIN(id, rec) (((int64_t)(id) << 32) | ((int64_t)(rec) << 2) | LW_THIN)
#define THIN_OWNER(w) ((int)((uint64_t)(w) >> 32))
#define THIN_REC(w) ((int)(((w) >> 2) & 0x3fffffff))
#define THIN_REC_MAX 1000
```

```
 63            32 31                 2  1 0
+----------------+--------------------+----+
| owner 线程 id   | 重入次数 rec        | 01 |   thin：有主，无人排队
+----------------+--------------------+----+
| ObjectMonitor* (8 字节对齐)          | 10 |   inflated：有人排队 / wait 过
+-------------------------------------+----+
| 0                                        |   无锁
+------------------------------------------+
```

## 2. 加锁：monitor_enter（mjvm.c:538-566）

```c
static int
monitor_enter(Thread *me, Object *o)
{
  int spins = 0;
  for(;;){
    int64_t w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(w == 0){
      // 快路径：一次 CAS，0 → THIN(me, 1)
      if(__atomic_compare_exchange_n(&o->lock, &w, THIN(me->id, 1), 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
      continue;
    }
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) == me->id){                  // 重入
        if(THIN_REC(w) < THIN_REC_MAX){
          if(__atomic_compare_exchange_n(&o->lock, &w, w + 4, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 0;                               // rec + 1（rec 从第 2 位开始，所以 +4）
        } else inflate(me, o, w, "recursion overflow");
        continue;
      }
      if(spins == 0) st_contended++;                // 被别人持有：先自旋
      if(++spins < SPIN_LIMIT){ if(spins > 8) plat_yield(); continue; }
      inflate(me, o, w, "contention");              // 自旋 64 次还拿不到就膨胀
      continue;
    }
    mon_enter((ObjectMonitor *)(w & ~3L), me);      // 已膨胀：进 ObjectMonitor 排队 + park
    return 0;
  }
}
```

### 编译出来的指令（快路径）

```asm
monitor_enter:
  1090: add   x22, x1, #0x8         ; x22 = &o->lock
  1094: ldar  x2, [x22]             ; w = load-acquire(o->lock)        mjvm.c:542
  1098: cbnz  x2, 10d4              ; w != 0 → 慢路径
  109c: ldrsw x0, [x20]             ; me->id
  10a0: mov   x1, #0x5              ;   (rec=1)<<2 | THIN = 0b101
  10a4: orr   x0, x1, x0, lsl #32   ; x0 = THIN(me->id, 1)
  10a8: ldaxr x1, [x22]             ; ┐ load-acquire exclusive         mjvm.c:545 的 CAS
  10ac: cbnz  x1, 10b8              ; │ 已经不是 0 了 → CAS 失败
  10b0: stxr  w2, x0, [x22]         ; │ store exclusive
  10b4: cbnz  w2, 10a8              ; ┘ 独占被打断（别的核写过这行）→ 重试
  10b8: cmp   x1, #0x0
  10bc: b.ne  1094                  ; 失败 → 回到 for(;;) 重新读
  10c4: mov   w0, #0x0              ; return 0
  10d0: ret
```

Cortex-A53 是 ARMv8.0，没有 LSE 原子指令，所以 `__atomic_compare_exchange_n` 会被展开成一个 **LDAXR/STXR 循环**。

- **LDAXR**：带 acquire 语义的独占读。它把这个地址登记到本核的 exclusive monitor，同时保证临界区里后面的读写不会被重排到这条读之前。
- **STXR**：独占写。只有从 LDAXR 到这里之间没有别的核写过这一行，写才会成功，此时 w2=0；否则 w2=1，回到 LDAXR 重试。两个线程同时 CAS 同一把锁时，最多只有一个能 STXR 成功。
- 成功序设为 `__ATOMIC_ACQUIRE`，所以只需要 LDA**X**R + 普通 STXR，不需要 STLXR。

`-mno-outline-atomics` 的作用：不加这个选项时，GCC 会把 CAS 编译成调用 `__aarch64_cas8_acq`，由运行时检测有没有 LSE。xv6 的 libc 里没有这个函数，所以要让编译器直接内联 LL/SC 循环。

重入路径（mjvm.c:553）用的是 `RELAXED`，编出来是不带 acquire 的 **LDXR/STXR**：

```asm
  10f0: cmp   w2, #0xf9f            ; THIN_REC(w) < 1000 ?（编译器把移位和比较合成了一条）
  10f8: add   x0, x2, #0x4          ; w + 4
  10fc: ldxr  x1, [x22]
  1100: cmp   x1, x2
  1104: b.ne  1110
  1108: stxr  w3, x0, [x22]
  110c: cbnz  w3, 10fc
```

重入时锁本来就在自己手里，第一次加锁时的 acquire 已经建立了 happens-before，所以这里不需要再加屏障。

## 3. 解锁：monitor_exit（mjvm.c:569-596）

```c
    int64_t w = __atomic_load_n(&o->lock, __ATOMIC_ACQUIRE);
    if(LW_TAG(w) == LW_THIN){
      if(THIN_OWNER(w) != me->id) break;            // 不是 owner → IllegalMonitorStateException
      int64_t nw = THIN_REC(w) > 1 ? w - 4 : 0;     // 重入就 rec-1，否则清零
      if(__atomic_compare_exchange_n(&o->lock, &w, nw, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return 0;
      continue;
    }
```

```asm
  24e0: ldar  x19, [x1]             ; w
  24f4: lsr   x0, x19, #32          ; THIN_OWNER(w) == me->id ?
  2504: sub   x0, x19, #0x4         ; w - 4
  2508: tst   x2, #0x3ffffffe       ; rec > 1 ?
  250c: csel  x0, x0, xzr, ne       ; nw = rec>1 ? w-4 : 0      （无分支）
  2510: ldxr  x2, [x1]              ; ┐
  2514: cmp   x2, x19               ; │ 锁字被改过（别人把它膨胀了）→ 失败
  2518: b.ne  2524                  ; │
  251c: stlxr w3, x0, [x1]          ; │ ★ store-release exclusive
  2520: cbnz  w3, 2510              ; ┘
```

关键是 **STLXR**，也就是 release 语义的独占写。它保证临界区里所有的写（比如 `blockSafe = v+1`）都先于"锁字变 0"对其他核可见。下一个线程用 LDAXR 读到 0，就一定能看到这些写。**JMM 里"解锁 happens-before 随后的加锁"这条规则，靠的就是 acquire/release 这一对指令。**

解锁也要用 CAS，不能直接写 0。原因是持有期间，别的线程可能已经把锁字膨胀成 `ObjectMonitor*|10`。这时 CAS 会失败，代码回到循环顶部，改走 inflated 分支：在 spin_lock 保护下 `--rec`，如果 rec 到 0，就把 owner 交给 entry 队首，再 `unpark()` 它。

## 4. 膨胀本身也是一次 CAS（mjvm.c:484-501）

```c
  ObjectMonitor *m = vm_alloc(sizeof(ObjectMonitor));
  m->owner = threads[THIN_OWNER(w)]; m->rec = THIN_REC(w);   // 把 thin 锁的 owner/rec 搬过去
  __atomic_compare_exchange_n(&o->lock, &expect, (int64_t)m | LW_INFLATED, 0,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
```

竞争线程会用 CAS 把锁字从 `THIN(owner, rec)` 换成 `m|10`，这一步不需要 owner 配合。如果恰好 owner 在同一时刻解锁，锁字已经变成 0，膨胀的 CAS 就会失败，竞争线程回到 `w == 0` 的快路径直接拿锁。这时分配出来的 m 就浪费了，因为 mjvm 没有 GC。owner 那边的解锁 CAS 如果遇到锁字已经被膨胀，同样会失败，然后改走 inflated 分支。两个 CAS 不管谁先成功，状态都是一致的。

## 5. park/unpark 用的 xv6 内核信号量（kernel/core/sync.c）

协调员之前的推测是对的：**每个信号量对象一把自旋锁，加上 xv6 自己的 sleep/wakeup**。

```c
struct ksync { struct spinlock lock; int used, generation, type, value, waiters, owner; };
static struct { struct spinlock lock; struct ksync object[NSYNC]; } synctable;   // NSYNC = 64

int ksync_wait(int handle) {            // sync_wait 系统调用
  struct ksync *o = getobject(handle);  // 校验 handle 的 index/generation，返回时已持有 o->lock
  o->waiters++;
  while(o->value == 0){
    if(myproc()->killed){ ...; return -1; }
    sleep(o, &o->lock);                 // 原子地释放 o->lock 并睡在 chan=o 上，醒来时重新持有
  }
  if(o->type != SYNC_EVENT) o->value--; // SEM：消费一个 permit
  o->waiters--;
  release(&o->lock);
  return 0;
}

int ksync_signal(int handle, int count) {
  ...
  } else if(o->type == SYNC_SEM){
    o->value += count;                  // 先加 permit
  }
  wakeup(o);                            // 唤醒所有睡在 chan=o 上的线程；没拿到 permit 的会在 while 里接着睡
  release(&o->lock);
}
```

mjvm 里每个 Java 线程有一个 `t->park = sync_create(SYNC_SEM, 0)`：`park()` 就是 `sync_wait`，`unpark()` 就是 `sync_signal(h, 1)`（mjvm.c:420-427）。

因为它是**计数信号量**，有一个关键性质：如果 `unpark` 跑在 `park` 前面（mon_enter 释放了 m->spin，但还没来得及 park，owner 就已经交接并 unpark 了），这个 permit 会留在 `value` 里，后面的 park 会立刻返回，**不会丢唤醒**。这和 HotSpot 的 ParkEvent/Parker 是同一种设计。

代价是：每次 park/unpark 都要进内核，还要拿 `o->lock`。内核的 `acquire` 本身也是 LDAXR/STXR 自旋，并且会关中断。所以 mjvm 只在自旋 64 次失败之后才走这条路。

## 6. 和 HotSpot 的对比

| | mjvm | HotSpot |
|---|---|---|
| 锁状态存在哪 | 对象头里单独一个 `lock` 字 | 对象头的 **mark word**，和 identity hash、GC age 共用，低 2 位是标签 |
| 无竞争加锁 | CAS `0 → (tid<<32)\|(rec<<2)\|01` | JDK ≤ 20 的 **stack locking**：把原 mark word（displaced header）存进当前栈帧的 BasicLock，再 CAS 把 mark word 改成指向它的指针，标签 `00`。JDK 21+ 的 **lightweight locking**（JDK 23 起默认）：CAS 只把标签 `01 → 00`，hash 留在原地，"谁持有"记在线程自己的 lock-stack 里 |
| 重入 | 锁字里的 rec 加 1 | stack locking 在栈上再压一个 displaced header 为 0 的 BasicLock。lightweight locking 在 lock-stack 里重复压同一个对象，**不用 CAS** |
| 偏向锁 | 没有 | JDK 15 起默认关闭，JDK 18 删除 |
| AArch64 指令 | ARMv8.0：LDAXR/STXR、LDXR/STLXR 循环（通过 `-mno-outline-atomics` 内联） | `MacroAssembler::cmpxchg` 在 `UseLSE` 开启时（ARMv8.1+）用单条 **CASAL**，否则同样是 LDAXR/STLXR 循环。在 A53 上两者走的是同一条路 |
| 快路径在哪 | 一个 C 函数调用 | 模板解释器、C1、C2 直接**内联生成**这几条指令，没有函数调用 |
| 竞争时 | 固定自旋 64 次（第 8 次以后 yield），然后膨胀 | 自适应自旋（`ObjectMonitor::TrySpin`，根据上次是否成功调整次数），然后膨胀 |
| 膨胀后 | `ObjectMonitor{owner, rec, entry, wait}`，一把 spin 锁保护 | `ObjectMonitor{_owner, _recursions, _cxq, _EntryList, _WaitSet}`，入队用 CAS 推进 cxq，不加锁 |
| 睡眠/唤醒 | 每个线程一个内核计数信号量（sync_wait/sync_signal → sleep/wakeup） | 每个线程一个 ParkEvent（Linux 上是 pthread mutex+cond，底层是 futex） |
| 降级（deflation） | 不做，膨胀之后一直是 ObjectMonitor | 有 MonitorDeflationThread，会把空闲的 monitor 回收、恢复成无锁状态 |

一句话总结：两者的语义骨架一致，都是 acquire 加锁、release 解锁、CAS 抢锁字、竞争时膨胀、用 park/unpark 睡和醒。mjvm 用一个独立的 64 位字和一个 C 函数把这些做了出来；HotSpot 在此之上省掉了单独的锁字（与 hash 共用 mark word），把快路径内联进 JIT 代码，用无锁队列和自适应自旋，并且会回收空闲的 monitor。
