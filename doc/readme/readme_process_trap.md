# 进程、异常与栈：fork / exec / wait / 调度 / initcode

本文按 rpi3-dynmodel 分支的源码（`kernel/core/proc.c`、`exec.c`、`trap.c`、`trapasm.S`、`swtch.S`、`entry.S`、`user/initcode.S`）整理，回答下面几类问题：

- trapframe 和 context 各存什么、放在哪里；
- `p->context.x30` 和 `p->context.sp` 的取值，以及它们为什么不负责返回用户态；
- 为什么要有 initcode，它做了什么，能不能不要；
- fork、exec、wait、exit、sleep 的路径；
- 四个异常级别、向量表的 16 个入口，以及常见事件各落到哪一格；
- SP_EL0 和 SP_EL1 的分工。

文中的地址和栈帧大小取自当前编译出来的 `kernel/kernel.asm`，重新编译后可能变化，方法不变。

---

## 1. 两份现场：trapframe 与 context

| | 存什么 | 放在哪里 | 谁写 | 谁读 |
|---|---|---|---|---|
| `struct trapframe`（272 B） | 用户态全部寄存器：x0–x30、elr、spsr、sp_el0 | 内核栈最顶端 272 字节 | `usavereg`（从 EL0 陷入时） | `urestorereg`（`eret` 回 EL0 前） |
| `struct context`（14 个字） | 内核线程的 sp 和 x18–x30 | `struct proc` 里的一个字段（`proc` 本身是单独 kalloc 的一页） | `swtch` 前半段 | `swtch` 后半段 |

- trapframe 回答"**用户程序**从哪继续"：`eret` 时 elr 成为 PC，sp 成为 SP_EL0，spsr 决定回到 EL0t、开中断。
- context 回答"**内核线程**从哪继续"：`swtch` 的最后两条是 `mov sp, x10` 和 `ret`，`ret` 跳到 x30。
- `swtch` 是一次普通函数调用。调用者保存的寄存器 x0–x17 已由编译器处理，所以 context 只需要 sp、x18–x30。

### 1.1 trapframe 就是内核栈顶的那 272 字节

本移植没有 RISC-V 版的 trampoline 页。AArch64 把地址空间分成两半：内核在 TTBR1，一直有映射；切换进程只换 TTBR0。所以从 EL0 陷入时不用换页表，现场直接压在内核栈上。

```c
// allocproc()
sp = (char*)p->kstack + PGSIZE;
sp -= sizeof(*p->trapframe);           // 272
p->trapframe = (struct trapframe*)sp;
p->context.x30 = (uint64)forkret;
p->context.sp  = (uint64)sp;
```

为什么 `usavereg` 压出的帧恰好落在 `p->trapframe` 上：

- 进程在 EL0 运行时，它的内核栈是空的。上一次 `urestorereg` 末尾的 `add sp, sp, #272` 已经把 SP_EL1 退回 `kstack + 4096`。
- 下一次从 EL0 陷入，`sub sp, sp, #272` 后按顺序 `stp`。这个地址正好等于 `p->trapframe`，字段顺序也与 struct 一致：x0..x29，然后 (x30, elr)、(spsr, sp_el0)。

由此得到一条硬约束：**`sizeof(struct trapframe)` 必须是 272，字段顺序必须和 `stp` 顺序一致。**

```
p->kstack + 4096  ← 进程在 EL0 时 SP_EL1 停在这里
  ┌──────────────────────────┐
  │ trapframe (272 B)        │ ← p->trapframe
  │ x0..x30 elr spsr sp_el0  │
  ├──────────────────────────┤ ← p->context.sp 的初值
  │ usertrap → syscall → …   │
  │ 内核调用帧向下生长         │
  │ （系统调用中来 IRQ：       │
  │  el1irq 再压 272 B）       │
  └──────────────────────────┘
p->kstack
  （再往下一页不映射：KSTACK(p) = MAXVA - (p+1)*2*PGSIZE，栈与栈之间隔一页保护页）
```

内核栈只有 4 KiB，其中 272 B 被 trapframe 占用。溢出时会踩进保护页，触发 EL1 同步异常；但 `el1sync` 的 `savereg` 仍要往这个坏栈上压寄存器，于是再次出错、反复陷入，表现是**无声挂死**，不会打印 panic。

另外两处过时的地方：

- `proc.h` 里关于 trampoline 和 sscratch 的注释是 RISC-V 原版遗留的，已经不对。
- `kernel/arch/arm64/uservec.S` 没有编进内核，Makefile 只用 `trapasm.o`。

---

## 2. `context.x30` 与 `context.sp`

### 2.1 新进程：第一次被调度

`allocproc` 伪造了一个"刚刚从 `swtch` 返回"的现场：

- `x30 = forkret`：`swtch` 最后的 `ret` 跳进 forkret。
- `sp = p->trapframe`：forkret 及其调用的函数（第一次运行时还有 `prepare_namespace`）从 trapframe 的**下方**开始用栈，不会覆盖 trapframe。

forkret 本身运行在 EL1，它只是新进程回到用户态的一块跳板：

```
scheduler: swtch(&c->context, &p->context)   ← 用 context：换 sp，ret 到 x30
forkret:   release(&p->lock)
           第一次运行：prepare_namespace()     ← 挂载根文件系统，要 sleep，只能在进程上下文做
           usertrapret(p->trapframe)          ← 从这里起只用 trapframe
trapret:   mov sp, x0 ; msr daifset,#0xf ; urestorereg ; eret
```

### 2.2 老进程：曾经让出过 CPU

进程让出 CPU 的唯一入口是 `sched()`：

```
ffffff800008ed18:  add  x0, x21, #0xb0       // &p->context
ffffff800008ed20:  add  x1, x1, #0x8         // &mycpu()->context
ffffff800008ed24:  bl   swtch                // x30 ← 下一条指令的地址
ffffff800008ed28:  mrs  x0, mpidr_el1        // ← mycpu()->intena = intena;
...
ffffff800008ed40:  ldp  x29, x30, [sp], #48  // 弹出 sched 自己的栈帧
ffffff800008ed44:  ret                       // 回到 yield / sleep / exit
```

- **context.x30**：任何通过 `sched()` 让出过 CPU 的进程，它的值都是 `0xffffff800008ed28`，即 `bl swtch` 的下一条指令。调度器自己的 `c->context.x30` 是 `scheduler()` 里 `bl swtch` 的下一条（`0x…eba8`，接着执行 `switchkvm()`）。两者构成一对往返。
- **context.sp**：调用 `swtch` 那一刻的 sp，也就是 `sched()` 栈帧的底部。它**不再**紧挨着 trapframe，具体有多深取决于进程是从哪条路径进入 sched 的。

用户态被时钟中断抢占时，调用链是 `el0irq → userirq → yield → sched → swtch`，栈帧大小取自各函数序言：

```
p->kstack+4096 ┌──────────────────────────┐
               │ trapframe        272 B   │ ← el0irq usavereg
               ├──────────────────────────┤ ← p->trapframe
               │ userirq 栈帧      32 B   │ stp x29,x30,[sp,#-32]!
               │ yield 栈帧        48 B   │ stp x29,x30,[sp,#-48]!
               │ sched 栈帧        48 B   │ stp x29,x30,[sp,#-48]!
               ├──────────────────────────┤ ← context.sp = 栈顶 − 400 = trapframe − 128
               │          （空）           │
p->kstack      └──────────────────────────┘
```

另外两种情况：在系统调用里睡眠（例如 `wait`）时，链路是 `usertrap → syscall → sys_wait → wait → sleep → sched`，更深。内核态被中断抢占时，栈上还会多一份 el1irq 压的 272 B 内核现场。

**context.x30 只负责把执行流带回 sched 这一层。** 再往上回到 yield、userirq、trapret，靠的是栈上各层函数自己保存的 x29/x30。恢复过程如下：

1. `swtch` 把 sp 设为"栈顶 − 400"，`ret` 到 `0x…ed28`；
2. sched 写回 intena，`ldp x29,x30` 后 `ret` 回 yield；
3. yield 执行 `release(&p->lock)` 后 `ret` 回 userirq；
4. userirq 执行 `intr_off` 后 `ret` 到 el0irq 里 `bl userirq` 的下一条，即 `trapret`；
5. `urestorereg` 从 trapframe 恢复用户寄存器，sp 回到栈顶，`eret`。

新进程的栈上没有这些可以逐层返回的调用帧，所以 forkret 直接调用 `usertrapret(tf)`，一步跳到第 5 步。

---

## 3. 第一个进程与 initcode

### 3.1 userinit

```c
p = allocproc();  initproc = p;
uvminit(p->vm->pagetable, initcode, sizeof(initcode));  // 拷到用户 VA 0
p->vm->sz = PGSIZE;
p->trapframe->elr  = 0;       // PC
p->trapframe->spsr = 0;       // M=0b0000 EL0t，DAIF 全清（用户态开中断）
p->trapframe->sp   = PGSIZE;  // 代码和栈共用这一页，sp 从 0x1000 往下
p->cwd = 0;                   // 由 forkret 里的 prepare_namespace() 设置
make_runnable(p, 0);
```

### 3.2 initcode 做了什么

```
0x00 ldr x0, =init     // 字面量池 0x38 → 0x1c "/init"
0x04 ldr x1, =argv     // 0x40 → 0x28，argv = {0x1c, 0}
0x08 mov x7, #7        // SYS_exec（本移植用 x7 传调用号）
0x0c svc #0
0x10 mov x7, #2        // SYS_exit：只有 exec 失败才会走到这里
0x14 svc #0
0x18 b   0x10
```

完整流程：

1. 第一次调度：swtch → forkret → 挂载根文件系统 → usertrapret → `eret` 到地址 0。
2. 执行 `svc`，el0sync 压出 trapframe：x0 = "/init"，x1 = argv，x7 = 7。
3. syscall 读 tf->x7 → sys_exec → `exec("/init", argv)`。
4. exec 改写 trapframe：elr = /init 的入口，sp = 新用户栈，x1 = argv，返回 argc 写进 x0。
5. `eret` 进入 `/init` 的 `main(1, {"/init"})`。initcode 那一页随旧页表一起被释放。pid 还是 1，`initproc` 指针不变。
6. 如果 exec 失败（例如 ext2 镜像里没有 /init），执行落到 exit 循环，`exit()` 发现 `p == initproc`，`panic("init exiting")`。

### 3.3 为什么需要 initcode

内核刚启动时只有 `main()`，有三件事做不到：

- **不能读文件**：读 /init 要 sleep 等磁盘，sleep 需要进程上下文；根文件系统要等 forkret 才挂载。
- **exec 需要一个进程**：它要改 `p->trapframe`、换 `p->vm->pagetable`。
- **进入 EL0 只能靠 eret**：必须先有一个映射好的用户程序。

initcode 编译进内核镜像，不需要文件系统；它让 pid 1 存在，并让 pid 1 用普通的 `svc → exec` 装入 /init。这样，第一个程序和以后所有程序走同一条路径，内核没有"直接加载第一个程序"的特殊代码。顺带，开机时就把 EL0 → EL1 → EL0 往返、系统调用分派和 trapframe 布局整体验证了一遍。

### 3.4 可以不要 initcode

Linux 的做法是 `kernel_init` 在内核态调用 `kernel_execve()`。本移植也可以在 forkret 第一次运行时直接 exec：

```c
if(first){
  first = 0;
  prepare_namespace();
  blkdev_selftest();
  static char *argv[] = { "/init", 0 };
  int argc = exec("/init", argv);
  if(argc < 0)
    panic("no /init on root filesystem");
  tf->x0 = argc;            // 没经过 syscall()，x0 要手动写
}
usertrapret(tf);
```

同时，`userinit` 删掉 `uvminit`、`sz = PGSIZE` 和设置 elr、sp、spsr 那三行，这些都由 exec 设置。

| | 保留 initcode | forkret 里直接 exec |
|---|---|---|
| 特殊路径 | 无 | forkret 里多一段只运行一次的分支 |
| 需要维护的东西 | `proc.c` 里手抄的 `initcode[]` 字节，必须和 `user/initcode.S` 一致 | 无 |
| 找不到 /init 时 | 报 `panic: init exiting`，提示比较绕 | 直接报 `no /init on root filesystem` |
| 启动自检 | 开机就测一次完整的系统调用往返 | 要到 /init 运行后才发生第一次系统调用 |

即使保留 initcode，也建议让 Makefile 用 `xxd -i` 从 `user/initcode` 自动生成这个数组，避免两边不一致。目前两边逐字节核对是一致的。

---

## 4. fork

调用路径：用户 `svc` → el0sync（usavereg 写入父进程 trapframe）→ usertrap（EC = 0x15）→ syscall → sys_fork → `fork()`。

1. `allocproc()`：分配槽位、内核栈、trapframe 位置、空的 vmspace，并设置 `context = {x30: forkret, sp: tf}`。
2. `uvmcopy`：逐页复制用户内存。这是整页复制，不是写时复制。
3. `*(np->trapframe) = *(p->trapframe)`：子进程得到相同的 elr（`svc` 的下一条）、sp_el0 和全部寄存器。SP_EL0 的值相同，但对应的物理页是新复制的。
4. `np->trapframe->x0 = 0`：这就是"子进程中 fork 返回 0"的全部机制。
5. `filedup` 打开的文件，`idup` cwd，继承调度属性，设置 parent，`make_runnable`。
6. 父进程返回 pid，`syscall()` 写入父进程 tf->x0，然后 `eret`。

子进程从没执行过 fork 的前半段。它第一次被调度时走 swtch → forkret → usertrapret → eret，看起来就像"也从 fork 返回了"，只是 x0 = 0。

---

## 5. exec

1. 检查 vmspace 的 refcount == 1（多线程时拒绝），置 execing。
2. namei、ilock，读 ELF 头，校验魔数。
3. `uvmcreate()` 建一张**新**页表；对每个 PT_LOAD 段 uvmalloc，再 loadseg。
4. 建用户栈：

```
sz_top ┌──────────────┐ ← sp 初值
       │ argv 字符串    │ 16 字节对齐
       │ argv[] 指针数组│ ← x1
       │   …          │ USTACKPAGES = 4 页
       ├──────────────┤ stackbase
       │ 保护页        │ uvmclear 去掉 PTE_U，用户栈溢出 → Data Abort
       ├──────────────┤
       │ text/data/bss│
0      └──────────────┘
```

5. 提交：换 `vm->pagetable` 和 `sz`；设置 `tf->elr = elf.entry`、`tf->sp = sp`、`tf->spsr = 0`、`tf->x1 = argv`。
6. `uvmsync_icache`（ARM 的 I/D cache 不一致，必须做）、`switchuvm`（立即换 TTBR0）、`uvmfree(旧页表)`。
7. `return argc`：由 syscall() 写入 tf->x0。`eret` 后从 `main(argc, argv)` 开始执行。

exec 不建新进程，也不动 context，只把 trapframe 改写成"一个新程序刚开始"的样子。pid、打开的文件、cwd 都保留，shell 的重定向就建立在这一点上。

---

## 6. wait、exit 与 sleep

### 6.1 sys_wait

```c
uint64 sys_wait(void) {
  uint64 p;
  if(argaddr(0, &p) < 0) return -1;   // 用户的 int *status，取自 tf->x0
  return wait(p);                     // 返回值写回 tf->x0
}
```

`p` 是用户虚拟地址，内核不能直接解引用，只能用 `copyout` 通过用户页表写入。

### 6.2 wait()

```c
acquire(&wait_lock);
for(;;){
  扫描进程表，找 parent == p 的子进程：
    若是 ZOMBIE：copyout(xstate → *status)，freeproc，return pid   // 不睡
  if(!havekids || p->killed) return -1;                          // 不睡
  sleep(p, &wait_lock);                                          // 只有这里睡
}
```

| 情况 | 结果 |
|---|---|
| 已有 ZOMBIE 子进程 | 立即回收并返回其 pid，不睡 |
| 有子进程但都在运行 | `sleep → sched → swtch` 让出 CPU；被唤醒后回到 for 循环开头重新扫描 |
| 没有子进程，或自己被 kill | 立即返回 -1 |

### 6.3 为什么需要 ZOMBIE，为什么要由父进程回收

exit 能当场关闭文件、放掉 cwd、把孩子过继给 init，但有两样东西不能当场处理：

- **内核栈**：exit 正运行在这一页上，直到最后的 `sched → swtch` 为止；
- **退出码 xstate**：父进程还没来取。

所以 exit 只能把自己设为 ZOMBIE 并永久切走。父进程在**自己的**内核栈上执行 `wait → freeproc`：`vmspace_put` 释放用户内存，`free_kstack` 释放内核栈（trapframe 也在这一页里），然后 `state = UNUSED`。父进程不调用 wait，子进程就一直是僵尸（演示程序：`user/zombie.c`）。

### 6.4 睡眠与唤醒的配对

- wait 睡在 `chan = p`（父进程的 proc 指针）上。
- exit 里调用 `wakeup(p->parent)`；`reparent` 把孤儿交给 init 时调用 `wakeup(initproc)`。
- **不会丢失唤醒**：父进程从扫描到 sleep 一直持有 wait_lock，sleep 先拿 p->lock 再放 wait_lock；子进程在 exit 里也必须先拿到 wait_lock 才能 wakeup。

### 6.5 谁在调用 wait

- **sh**：执行每条命令都是 `fork → 子进程 exec → 父进程 wait`，所以要等命令结束才出现下一个 `$`。
- **init**：在循环里 wait。wait 返回 login/sh 的 pid 就重新 fork 一个；返回其他 pid 说明回收了一个被过继来的孤儿。init 绝不能退出。

### 6.6 sleep 不是异常

sleep → sched → swtch 都是普通的 `bl`/`ret` 函数调用，全部在 EL1 内完成，不经过向量表，ESR、ELR、SPSR 都不碰。一次 `wait()` 的完整时间线：

```
EL0  svc #0 (x7 = SYS_wait)
      │  ← 唯一一次异常：来源 EL0，同步异常 → +0x400 el0sync
EL1  usavereg → usertrap → syscall → sys_wait → wait → sleep → sched → bl swtch
      ……CPU 运行别的进程；子进程 exit → wakeup(父) → 调度器 swtch 回来……
     swtch ret → sched → sleep → wait 返回 pid → usertrap 返回
     urestorereg ; eret       ← 异常返回
EL0  svc 的下一条指令，x0 = 子进程 pid
```

所以说"在系统调用里睡眠属于 el0sync"，指的是它所在的那次系统调用是从 el0sync 进来的。这次 el0sync 处理可能持续几秒，中间夹着很多别的进程的运行，但对硬件来说它一直没有结束。

---

## 7. 调度

- `scheduler()`：每个 CPU 一个，永不返回。循环里先 `intr_on()`；`pick_next` 按有效优先级选，同优先级取 rq_seq 最小的；`switchuvm`（TTBR0 = 用户页表）→ swtch → `switchkvm`（TTBR0 = 0）。
- `sched()`：断言持有 p->lock、`noff == 1`、`state != RUNNING`、中断已关，然后 `swtch(&p->context, &mycpu()->context)`。
- **锁的交接**：调度器 acquire p->lock 后切到进程，进程在 forkret 或 sched 返回后 release；反方向同理。这样，在 state 已改、context 还没存好的窗口里，别的 CPU 拿不到这个进程。
- **进入 sched 的三条路**：
  - 时间片：定时器 IRQ → `sched_tick` 把 slice 减到 0 → need_resched → userirq/kernelirq 里 yield；
  - `sleep`；
  - `exit`。

---

## 8. 异常级别与向量表

### 8.1 四个 EL 在本系统中的用途

| EL | 一般用途 | 本移植 |
|---|---|---|
| EL3 | 安全监控 | 只在启动时经过（使用自定义 armstub 时） |
| EL2 | Hypervisor | 只在启动时经过（固件 armstub、QEMU 从这里起步） |
| EL1 | OS 内核 | xv6 内核，始终是 EL1h |
| EL0 | 用户程序 | initcode、init、sh…，EL0t、AArch64 |

`entry.S` 读 CurrentEL，从 EL3 或 EL2 都用 `eret` 降到 EL1h：设置 `HCR_EL2.RW = 1`、SPSR = `0x3c5`（M = 0b0101 EL1h，DAIF 全屏蔽），ELR 指向标号 3。之后不再使用 EL2/EL3，也没有安装 VBAR_EL2/EL3。

进入 EL0 用的是同一个技巧：`tf->spsr = 0`，然后 `urestorereg; eret`。

### 8.2 栈指针：EL1t / EL1h 与 SP_EL0

| PSTATE.M | 名称 | 指令里的 `sp` |
|---|---|---|
| 0b0000 | EL0t | SP_EL0（EL0 只能用它） |
| 0b0100 | EL1t | SP_EL0 |
| 0b0101 | EL1h | SP_EL1 |

- **用户态（EL0t）**：sp 就是 SP_EL0，值是 exec 设置的用户栈虚拟地址，经 TTBR0 翻译。
- **陷入时**：硬件把 sp 切到 SP_EL1（内核栈），SP_EL0 保持用户的值不动；`usavereg` 用 `mrs x11, sp_el0` 存进 tf->sp。
- **返回时**：`msr sp_el0, x11` 写回，然后 `eret`。
- 用户无法读写 SP_EL1（`msr sp_el1` 是特权指令），所以用户把自己的栈搞坏了，陷入内核后用的仍是可靠的内核栈。

EL1t 的用途：有些内核让线程在 EL1t 下以 SP_EL0 作为线程栈，SP_EL1 作为每个 CPU 的专用异常栈（类似 Cortex-M 的 PSP/MSP），线程栈坏了异常处理仍然可靠。xv6 不这样用，因为在本系统里 SP_EL0 专门保存用户栈指针。

### 8.3 VBAR_EL1 = alltraps 的 16 个入口

表必须 2 KiB 对齐，每格 0x80 字节。

| 偏移 | 来源 | Sync | IRQ | FIQ | SError |
|---|---|---|---|---|---|
| 0x000 | 当前 EL，用 SP_EL0（EL1t） | `b .` | `b .` | `b .` | `b .` |
| 0x200 | 当前 EL，用 SP_ELx（EL1h） | **el1sync → kerneltrap → panic** | **el1irq → kernelirq** | `b .` | `b .` |
| 0x400 | 低一级 EL，AArch64 | **el0sync → usertrap** | **el0irq → userirq** | `b .` | `b .` |
| 0x600 | 低一级 EL，AArch32 | `b .` | `b .` | `b .` | `b .` |

- **格子由"异常发生那一刻 CPU 在哪一级、用哪个 SP"决定。** 处理程序本身总在 EL1h、使用 SP_EL1。`el0sync` 的意思是"来自 EL0 的同步异常"，它自己的代码运行在 EL1。
- **低一级 EL 分为 AArch64 和 AArch32 两组**，因为 32 位用户程序的寄存器视图（r0–r14 映射到 x0–x14 的低 32 位）、系统调用 EC（0x11 SVC32 对比 0x15 SVC64）和 ABI 都不同。Linux 的 CONFIG_COMPAT 用 0x600 组运行 armhf 程序。本系统只装 64 位 ELF，`spsr = 0` 中 M[4] = 0，而执行状态只能在 `eret` 时由内核切换，所以 0x600 组不可能被触发。
- 对本系统而言，**用户态的一切异常都落在 0x400 组**。
- 进入任何一格前，硬件自动完成：ELR_EL1 ← PC，SPSR_EL1 ← PSTATE；同步异常写 ESR_EL1（访存错误还写 FAR_EL1）；DAIF 全屏蔽；切到 EL1、SP_EL1；PC = VBAR + 偏移。
- `b .` 的格子会导致无声挂死。遇到"板子不动了、没有 panic"，可以怀疑 SError（访问了不存在的外设地址）、误执行了 `msr spsel, #0`，或内核栈溢出。

### 8.4 常见事件对照

| 事件 | 是否异常 | 来源 | 入口 | 结果 |
|---|---|---|---|---|
| 用户执行 `svc` | 同步 | EL0 | 0x400 el0sync | EC 0x15 → syscall；ELR 指向 svc 的**下一条** |
| 系统调用里 sleep | **不是异常** | — | 不经过向量表 | 普通函数调用 sleep → sched → swtch |
| 用户态时钟中断 | IRQ | EL0 | 0x480 el0irq | userirq → devintr → 时间片用完则 yield；ELR 指向**被打断的那条**，返回后重新执行它 |
| 系统调用执行中来了中断 | IRQ | EL1 | 0x280 el1irq | 在当前内核栈再压 272 B；kernelirq → 可能 yield |
| 内核访存出错、执行到坏指令 | 同步 | EL1 | 0x200 el1sync | kerneltrap 打印 esr/elr/far → panic |
| 用户空指针、野指针、用户栈溢出 | 同步 | EL0 | 0x400 | EC 0x24 Data Abort，FAR = 出错地址 → killed → exit(-1) |
| 用户跳到未映射地址 | 同步 | EL0 | 0x400 | EC 0x20 Instruction Abort → killed |
| 用户执行未定义指令（如 `udf`） | 同步 | EL0 | 0x400 | EC 0x00 → killed |
| 用户执行浮点/SIMD 指令 | 同步 | EL0 | 0x400 | EC 0x07（CPACR_EL1.FPEN 未打开）→ killed |
| sp 或 PC 未对齐 | 同步 | EL0 | 0x400 | EC 0x26 / 0x22 → killed |
| `brk` | 同步 | EL0 | 0x400 | EC 0x3C → killed |
| **用户整数除以 0** | **不是异常** | — | — | ARMv8 的 sdiv/udiv 除以 0 结果为 0，程序继续运行 |
| FIQ / SError | — | 任意 | `b .` | 无声挂死 |

关于除以 0：

- AArch64 和 x86 不同，x86 的整数除零会产生 `#DE`。要像 Linux 那样报 SIGFPE，只能靠编译器插入检查（`-fsanitize=integer-divide-by-zero`）。
- 浮点也到不了这一步：Makefile 用 `-mcpu=cortex-a53+nofp`，用户程序不会生成浮点指令。即使打开 FP，1.0/0.0 的结果也是 inf，Cortex-A53 不支持 FPCR 的浮点异常陷入。

---

## 9. 一张总图

```
main: userinit ─► scheduler ─swtch─► forkret(第一次：挂载根) ─usertrapret─eret─►
EL0 initcode: svc exec("/init") ─► el0sync → usertrap → sys_exec → exec(改 trapframe) ─eret─►
EL0 /init: fork() ─svc─► fork：复制页表与 trapframe，子进程 x0 = 0
   ├─ 父：x0 = pid ─eret─► wait() ─svc─► wait → sleep → sched ─swtch─► scheduler
   └─ 子：scheduler ─swtch─► forkret ─eret─► fork 返回 0 ─► exec("/bin/login") ─► …
定时器 IRQ ─► el0irq → userirq → sched_tick 时间片用完 → yield → sched ─► 换下一个进程
login/sh exit ─► ZOMBIE → wakeup(init) ─► init 的 wait → freeproc 回收内核栈
```

---

# 第二部分：共享物理页——引用计数、写时复制、页缓存、mmap 与 ld.so

以下内容对应三步改造（分支 `wip-cow-mmap-ldso`，基于 `rpi3-dynmodel` 的 c0aeb0e）：

| 步骤 | 内容 | 主要文件 |
|---|---|---|
| 第一步 | 共享 libc.so 的代码页 | `exec.c` |
| 第二步 | 物理页引用计数 + fork 写时复制 | `kalloc.c`、`vm.c`、`trap.c`、`proc.c` |
| 第三步 | 页缓存 + 通用 mmap + 用户态 `/lib/ld.so` | `pcache.c`、`mmap.c`、`exec.c`、`user/ldso.c` |

## 10. 第一步：共享 libc 代码页

libc.so 是位置无关的，没有 `DT_TEXTREL`，所有重定位都落在 RW 段。所以它的只读页在任何基址下都逐字节相同，可以让所有进程共用一份物理内存；RW 页（GOT、.data、.bss）仍然每个进程一份。

```
LOAD  vaddr 0x0     memsz 0x9272   R-X   第 0..9 页，可以共享
LOAD  vaddr 0xa278  memsz 0x390    RW-   0xa000 这一页，每个进程一份
重定位：71 条 R_AARCH64_JUMP_SLOT，目标全部在 RW 段
```

- **第一版**：`exec()` 里专门缓存 `/lib/libc.so` 的代码页，第一次读盘，以后直接映射；用软件位 `PTE_SHARED` 标记"这一页不归我释放"。
- **踩到的坑**：`growproc()` 缩小内存时有一个自己写的释放循环，绕过了 `PTE_SHARED` 检查。usertests 的 `sbrkbugs` 让子进程 `sbrk(-sz)` 释放全部内存，结果把所有进程共用的 libc 代码页还给了空闲链表。父进程、sh、init 随后在 libc 的同一地址 0x1617c 遇到未定义指令（EC 0，kfree 用垃圾字节填满了页），最后 `panic: init exiting`。
- **现在**：用引用计数取代特判，代码页改由通用页缓存提供（见第 12 节）。`login` 和 `sh` 的 libc 基址不同（0x2000 和 0x3000，由主镜像大小决定）不影响共享：同一组物理页只是被映射到不同的虚拟地址。

另外，`copyout()` 是通过内核直接映射写用户内存的，**不受用户页表项 AP 位约束**。必须让它自己检查只读页，否则 `read(fd, libc_text, n)` 能让一个进程改写所有进程共用的代码。

## 11. 第二步：物理页引用计数与写时复制

### 11.1 引用计数（kalloc.c）

```c
static uint16 pageref[PHYSTOP / PGSIZE];   // 受 kmem.lock 保护，以物理页号为下标
kalloc()       // 取出空闲页时置为 1
kref_inc(pa)   // 多一个映射：COW、页缓存、共享库
kref_get(pa)
kfree(va)      // 减一，减到 0 才放回空闲链表
```

"释放"从此意味着"放弃我这一份"：`uvmunmap`、`growproc` 缩小、进程退出一律调用 `kfree`。相机 DMA 缓冲、模块区等不归 kalloc 管的页不计数。

### 11.2 fork 不再复制（vm.c `uvmcopy_range`）

```c
acquire(&pte_lock);
for(va = start; va < end; va += PGSIZE){
  pte = walk(old, va, 0);  pa = PTE2PA(*pte);
  if((*pte & PTE_U) && (*pte & PTE_RO) == 0)              // 用户可写的页
    *pte = (*pte & ~PTE_AP(3)) | PTE_URO | PTE_COW;        // 父进程也改成只读 + COW
  mappages(new, va, PGSIZE, pa, PTE_FLAGS(*pte) | (*pte & PTE_COW));
  kref_inc(pa);
}
release(&pte_lock);
flush_tlb();     // 其他 CPU 上同一地址空间的线程可能缓存着可写的翻译
```

`PTE_COW` 是描述符的 bit 55。ARMv8 把 [58:55] 留给软件，MMU 不看这几位。

### 11.3 写保护异常（trap.c + vm.c `uvm_cowfault`）

```c
} else if(ec == 0x24 && ((esr >> 6) & 1) && (esr & 0x3c) == 0x0c &&
          (cow = uvm_cowfault(p->vm->pagetable, r_far_el1())) != -1){
  if(cow == -2){ printf("pid %d %s: out of memory for a copy-on-write fault at %p\n", ...); p->killed = 1; }
}
```

条件含义：EC 0x24 是来自 EL0 的数据中止；WnR=1 表示写；DFSC 0b0011xx 是权限错误。

`uvm_cowfault` 的处理：
- 引用计数为 1：别的使用者都已复制或退出，直接把页表项改回可写。
- 否则：`kalloc` 一页，复制内容；如果是可执行页，还要做 I-cache 同步；然后改页表项，再 `kfree` 掉旧页的那一份引用。
- 改完后 `flush_tlb()`，然后返回用户态，那条 `str` 重新执行。

### 11.4 copyout 与 pte_lock

- `copyout()` 在 `pte_lock` 下逐页处理：COW 页先复制，只读页返回 -1，并且**持锁完成这一页的拷贝**，防止另一个线程的 `fork()` 恰好在检查和写入之间把这一页重新共享出去。
- `pte_lock` 串行化所有把页变成 COW、把 COW 页改回可写的操作：fork 标记、写保护异常、`copyout`、区间撤销映射（`uvm_unmap_range`）。

### 11.5 测试与副作用

`cowtest` 的结果：
```
cowtest: 8 MiB parent: fork used 52 KiB, child's writes 8192 KiB
cowtest: 50 forks of 8 MiB: free memory changed by 0 KiB
cowtest: OK
```

副作用有两个：
- **内存不足从 fork 时推迟到写入时。** usertests 的 `countfree()` 子进程先吃光内存，再写一个仍与父进程共享的页，于是被杀，并打印 `out of memory for a copy-on-write fault`。这和 Linux 的 OOM 行为一样。
- **fork 太便宜，进程数不再受内存限制。** 动态增长的进程表加上线性扫描的调度器，承受不了几万个进程。因此在 `param.h` 加了 `NPROC_MAX = 1024`，相当于 Linux 的 `threads-max`。

## 12. 第三步（上）：页缓存与 mmap

### 12.1 页缓存（kernel/pcache.c）

```c
struct pcfile { struct inode *ip; uint64 *pa; uint64 lastuse; };   // 持有 inode 引用
uint64 pcache_getpage(struct inode *ip, uint64 pgidx);  // 返回 pa，并为调用者 kref_inc
void   pcache_invalidate(struct inode *ip);             // 文件被写或 truncate 后调用
void   pcache_reap(void);                                // 锁外 iput 被淘汰的 inode
```

- **容量**：最多 8 个文件（`NINODE` 只有 50），每个文件最多 512 页（2 MiB），满了按最近使用淘汰。文件末尾之后的字节读作 0；页读入后统一做一次 I-cache 同步。
- **一致性**：`filewrite`、`ftruncate`、`open(O_TRUNC)` 之后作废该文件的缓存页。已有的映射保留旧页（相当于 MAP_PRIVATE 快照），新的映射看到新内容。
- **加锁顺序**：先 `ilock(ip)`，再页缓存的睡眠锁。被淘汰的 inode 不能在持锁时 `iput`，先挂起，由 `pcache_reap()` 在锁外自开事务释放。

### 12.2 地址空间布局与 VMA

```
[0, sz)                      程序、内核加载的 libc 或 ld.so、栈、sbrk 堆（sbrk 不许越过 MMAP_BASE）
[0x10_0000_0000, 0x3f_0000_0000)  mmap 区：ld.so 映射的 libc.so、文件与匿名映射
0x3f_f000_0000               相机 DMA 窗口（驱动自己映射）
```

每个 `struct vmspace` 有一张 32 项的 VMA 表：`struct vma { start, end, prot, flags }`。mmap 时整段立即映射（没有按需调页），所以 VMA 只需要记录区间。

### 12.3 mmap 的语义（kernel/mmap.c）

| 形式 | 实现 |
|---|---|
| `MAP_ANONYMOUS` | 新分配的清零页 |
| 文件 + `MAP_PRIVATE` | 页缓存页；只读映射直接共享，可写映射以只读 + COW 共享，写时复制 |
| 文件 + `MAP_SHARED` | 只允许只读（没有回写） |
| `MAP_FIXED` | 先撤销该区间原有的映射 |
| `munmap` | 可以只撤销一部分，必要时把一个 VMA 切成两个 |

- `PROT_EXEC` 决定 UXN 位；内核永远不执行用户页（PXN）；`PROT_NONE` 映射成 AP=00，EL0 无法访问。
- fork 时 `vma_copy()` 对每个区间调用 `uvmcopy_range`（同样写时复制）；exit 和 exec 时 `vma_unmap_all()` 撤销全部区间。
- 字符设备（`/dev/video0`）仍交给 `chrdev_mmap()`。

`mmaptest` 覆盖了以下情况，全部通过：只读映射与文件内容一致；私有写不影响其他映射和文件；写 `PROT_READ` 映射的进程被杀；fork 后子进程的写入不影响父进程；匿名页为零；munmap 掉的中间页不可再访问；`MAP_FIXED` 覆盖已有映射；文件被改写后新映射看到新内容。

## 13. 第三步（下）：PT_INTERP 与用户态 /lib/ld.so

### 13.1 链接与安装

```make
USER_LD_MODE = -pie -dynamic-linker /lib/ld.so --hash-style=sysv
$U/ld.so: $U/ldso_start.o $U/ldso.o
	$(LD) $(LDFLAGS) -shared -Bsymbolic --no-undefined -z text --hash-style=sysv -e _start -soname ld.so -o $@ $^
```

- 每个程序都带 `PT_INTERP /lib/ld.so` 和 `PT_PHDR`。
- 模式戳改名为 `.dynamic-ldso`，这样旧的二进制会被重新链接。
- `mkfs` 支持多个 `-L`；fs.img 和 ext2 根镜像同时安装 `/lib/libc.so` 与 `/lib/ld.so`。

### 13.2 exec 的工作

1. 主程序照旧装到 VA 0。
2. 发现 `PT_INTERP`：读出路径，把 ld.so 装在主镜像之后。ld.so 的只读页也取自页缓存，所有进程共用一份。
3. 从 ld.so 的入口开始运行。寄存器约定：`x0 = argc, x1 = argv, x2 = auxv`。auxv 的标签取 Linux 的值：`AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY, AT_NULL`，定义在 `include/uapi/auxv.h`。
4. 没有 `PT_INTERP` 的程序仍走内核内置的加载器（第 10 节），作为兼容路径；它的 libc 代码同样来自页缓存，日志显示 `text shared`。

### 13.3 ld.so 的工作（user/ldso.c）

ld.so 是独立的：不依赖 libc，用原始 `svc` 做系统调用，全部符号 hidden。它依次执行：

1. **自重定位**：根据 `AT_BASE` 处理自己 `_DYNAMIC` 里的 RELATIVE 重定位。实际构建出的 ld.so 没有任何重定位项。
2. **找到程序**：用 `AT_PHDR` 和 `PT_PHDR` 算出主程序基址，找到它的 `PT_DYNAMIC`。
3. **装载依赖**：对每个 `DT_NEEDED`（不带路径的名字到 `/lib` 下找）：
   - 用 `mmap` 映射第 0 页，读出 ELF 头和程序头；
   - 用 `mmap(NULL, span)` 把整个镜像映射成文件内容，这一步决定了基址；
   - 其余段用 `MAP_FIXED` 按各自的地址和权限重新映射；
   - `.bss` 清零，不足的部分补匿名页。
   依赖按广度优先装载，可以有多个库。
4. **重定位**：先库后程序，处理 RELATIVE、ABS64、GLOB_DAT、JUMP_SLOT。符号用 SysV 哈希在全局作用域"程序 → 各库"中查找，因此程序可以覆盖（interpose）libc 的符号。
5. **运行**：以 `(argc, argv)` 调用 `AT_ENTRY`，用返回值调用 `exit`。

**文本重定位的规则**：用户程序以 `-fno-pie` 编译、`-pie` 链接，只读段里的指针表需要重定位，所以带 `DT_TEXTREL`（usertests 就是这样）。程序是 exec 装进来的私有可写副本，改写无妨；库的代码页是所有进程共用的只读页。所以 ld.so **只对库**拒绝 `DT_TEXTREL`，libc 本身也用 `-z text` 链接来保证这一点。

### 13.4 vmmap 验证（QEMU，gcc 13 构建，libc 代码段 12 页）

- libc 映射在 `0x10_0000_1000` 起；12 个代码页在 init、sh、vmmap 三个进程里是同一组物理页，每页 "shared by 4 mappings"（页缓存加 3 个进程）。
- 文件第 0xb000 页既是代码段的尾巴（只读可执行），又是 .data 的开头（只读 + COW），被引用 7 次。
- 被重定位写过的 GOT 页已经各自复制成私有 RW 页。
- ld.so 的代码页只有一份物理页（0x6f60000）。

## 14. 现状与待办

- 代码在分支 `wip-cow-mmap-ldso`（commit ac9c288），还没有合入 `rpi3-dynmodel`。
- 已通过：`cowtest`、`mmaptest`、`forktest`、`usertests sbrkbugs`、旧方式链接程序的兼容路径、usertests 中已跑完的 14 项。
- **待解决**：
  - `usertests forkforkfork` 在 QEMU 里有时跑不完。现象是约 900 个进程同时在 `open()` 上争抢日志，并非死锁。需要和改动前的内核对比耗时，必要时把 `NPROC_MAX` 调低。
  - 完整的 usertests 还没跑完。
- 与 Linux 相比还缺：环境变量与 `AT_EXECFN`、`dlopen/dlsym`、延迟绑定、IFUNC、TLS、ASLR、按需调页、可回写的 `MAP_SHARED`；页缓存不服务普通 `read()`。

## 15. 实例：fork 快照里的虚拟地址与物理页

用 `procvmtrace("process-create:fork")` 在 fork 完成时打印子进程的 proc、内核栈、trapframe、context 和用户页表。下面两张图都来自开机后的同一段日志：

- **pid 16**：init 刚 fork 出、即将 exec `/bin/sh` 的子进程；
- **pid 17**：在 sh 里输入 `ls` 后，sh fork 出、即将 exec `/bin/ls` 的子进程。

两者都是 `state=1`（USED），已经建好，还没有被调度过。

### 15.1 一个进程：从虚拟地址到物理页（pid 17）

![pid 17 的虚拟地址视角与物理页](images/proc-vm-phys-pid17.png)

这里选 pid 17：它就是后面执行 `/bin/ls` 的那个进程。fork 刚完成时它还是 sh 的拷贝，名字也还是 `sh`。上半部分是虚拟地址视角，下半部分把物理内存 `0x6e9e000`–`0x6ecffff` 按页横向排开，每列一页。图中 10 条箭头分别从 struct proc、`p->context`、`p->vm`、内核栈、trapframe、页表根、L2、L3、用户 sp 和 elr 连到各自的物理页。

**内核这一侧**

| 对象 | 内核虚拟地址 | 物理地址 | 换算方式 |
|---|---|---|---|
| struct proc | `0xffffff8006eb7000` | `0x6eb7000` | 直接映射：PA = VA − `0xffffff8000000000` |
| `p->context` | `0xffffff8006eb70b0` | `0x6eb70b0` | 嵌在 proc 内，偏移 `+0xb0` |
| vmspace | `0xffffff8006eb5000` | `0x6eb5000` | 直接映射 |
| 内核栈（槽 15） | `0xffffffbffffe0000`–`e1000` | `0x6ec0000` | `KSTACK(slot) = MAXVA − (slot+1)·0x2000`，经内核页表映射 |
| trapframe | `0xffffffbffffe0ef0` | `0x6ec0ef0` | 内核栈顶 − 272（`0x110`） |
| 页表根、L2、L3 | — | `0x6eb8000`、`0x6ecf000`、`0x6ecc000` | `TTBR0 → L1[0] → L2[0] → L3` |

这几页并不相邻：它们大多是 pid 16 exec sh 时刚释放、又被 kalloc 发出来的页，见 §15.2。

**trapframe 里的值都能对上**

- `elr = 0xe5bc`、`x17 = 0xe59c`：sh 的 libc 基址 `0xa000` 加 `0x459c` 正是 `__syscall`，`elr` 指向其中 `svc #0` 的下一条指令（`+0x20`）。
- `x16 = 0x143e0`：libc 的 GOT 起点（`0xa000 + 0xa3e0`），这是 PLT 存根刚用过的寄存器。
- `x27 = 0x14650`：libc 数据段结尾。
- `x0 = 0`：子进程中 `fork()` 的返回值。
- `spsr = 0x20000000`：返回 EL0t，中断打开，C 标志置位。

**用户地址换算**：查 L3 叶子，再加页内偏移。

- `sp 0x9830` 在 VA 页 `0x9000`，由 L3[9] 映射到 `0x6ea9000`，物理地址为 `0x6ea9830`；
- `elr 0xe5bc` 在 VA 页 `0xe000`，由 L3[14] 映射到 `0x6ea4000`，物理地址为 `0x6ea45bc`。

sh 比 init 大，所以 ld-xv6 装在 `0x3000`，保护页在 `0x5000`，栈是 `0x6000`–`0xa000`，libc 从 `0xa000` 开始。用户页在物理上不连续，大致按倒序分配，这是 kalloc 空闲链表后进先出的结果。

**保护页**：VA `0x5000` 没有出现在 dump 里。exec 为它分配了物理页，但清掉了 `PTE_U`，而 dump 只打印用户可访问的页。按页号空缺推测，它的物理页是 `0x6ec7000`，图中标为 “?”。

**context.sp 什么时候变**

- **fork 刚完成**：`context.sp` 等于 trapframe 的地址，`x30 = forkret`，`x18`–`x29` 都是 0。
- **第一次被调度**：`swtch` 载入 context，`ret` 到 `forkret`，经 `usertrapret` 从 trapframe 恢复寄存器，`eret` 回到 EL0。此后 SP_EL1 停在内核栈顶。
- **每次 svc 或中断**：`el0sync` 从栈顶执行 `sub sp, sp, #272`，所以 trapframe **总在同一位置**被重写；`context.sp` 不会因此改变。
- **在系统调用里睡眠**（`sleep → sched → swtch`），或者被时钟抢占（`yield → sched`）：`swtch` 把当时的内核 sp 存进 `context.sp`。这个值低于 trapframe，因为 `usertrap → syscall → sys_xxx → sleep → sched` 的调用帧压在 trapframe 下面。
- **再次被调度**：`swtch` 恢复这个 sp，从 `sched` 返回，沿调用链回到 `usertrap`，再经 trapframe 回到 EL0。

不睡眠的系统调用（如 `getpid`）全程不会改变 `context.sp`。

### 15.2 两个进程：同一批物理页在两次快照之间的用途

![物理内存逐页：pid 16 与 pid 17](images/phys-pages.svg)

这张图把物理地址 `0x6e9e000`–`0x6ed3fff` 共 54 页按行列出：

- 中间是物理地址；
- 左格是 pid 16 快照时（t1）这一页的用途，右格是 pid 17 快照时（t2）的用途；
- 外侧标签是两个进程的指针，箭头落在页内的相应偏移处；
- 绿色虚线弧表示页表遍历：根 → L1[0] → L2 → L2[0] → L3。

两份快照的 `root-pa` 都是 `0x6eb8000`，但这**不是共享页表**。在两次快照之间，pid 16 exec 了 sh，它旧的 init 映像被释放；随后 sh fork 出 pid 17，kalloc 后进先出，最先发出的正是这些刚释放的页：

| 物理页 | t1：pid 16 中的用途 | t2：pid 17 中的用途 |
|---|---|---|
| `0x6eb8000` | 页表根 | 页表根 |
| `0x6eb5000` | L2 页表 | struct vmspace |
| `0x6ec0000` | L3 页表 | 内核栈 |
| `0x6eb7000` | 用户页 VA `0x13000` | struct proc |

同一个物理页在很短时间内换了用途。这也说明，`kfree` 之后还保留旧指针（use-after-free）的 bug，在这里会立刻表现为另一个进程的数据被改坏。

从 pid 17 的寄存器还能看出 sh 比 init 大：ld-xv6 被装在 `0x3000`，libc 基址是 `0xa000`（`elr = 0xe5bc`、`x16 = 0x143e0`），后面的区域整体后移了一页。

### 15.3 同一个进程：exec 之后与第一个系统调用（pid 17 执行 ls）

![pid 17：exec 换上新映像（A）与第一个系统调用 open 返回（B）](images/exec-syscall-pid17.png)

接着 §15.1，pid 17 执行 `exec("/bin/ls")`。打开 `VM_TRACE` 后会在两个时刻打印快照：

- **A：`exec-image:first`**：在 `exec()` 中，刚 `switchuvm` 换上新页表，还没 `return`（`kernel/fs/exec.c`）；
- **B：`first-syscall:return`**：exec 之后的第一个系统调用执行完、即将返回用户态（`kernel/core/syscall.c`）。这个调用是 ld-xv6 的 `open("/lib/libc.so")`（系统调用号 15），返回 fd 3。

图的左、中两列分别是 A 和 B，内核栈页按比例画（一页 4096 字节），右列按步骤说明两者之间发生了什么。下方是涉及的物理页，实线箭头来自 A，虚线箭头来自 B；内核栈页、栈顶页和 ld-xv6 入口页这三列加宽，画出了页内偏移。

**内核栈：context.sp 只在睡眠时改变**

| | A：exec-image:first | B：first-syscall:return |
|---|---|---|
| 当前内核 sp | `+0x940`：还在 `sys_exec` 的深层调用链里，已用 1728 B | `+0xe80`：只剩 usertrap、syscall 两帧，272 + 112 = 384 B |
| context.sp | `+0x530`：exec 读 ELF 时等磁盘睡眠，`swtch` 保存 | `+0x690`：open 在 `namei`/`bread` 中睡眠时保存 |
| trapframe | `+0xef0` | `+0xef0`：每次 `el0sync` 都在原地重写 |

两次的 context.sp 都低于 trapframe，深度取决于睡眠时的调用链。

**exec 只换用户页表**

- proc（`0x6eb7000`）、vmspace（`0x6eb5000`）、内核栈（`0x6ec0000`）都沿用 fork 时的页；
- 页表根从 `0x6eb8000` 换成 `0x6e8c000`，L2 是 `0x6e8a000`，L3 是 `0x6e89000`；
- 新映像只有 8 个用户页：`/bin/ls` 在 `0x0`–`0x2000`，ld-xv6 在 `0x2000`–`0x4000`，保护页在 `0x4000`，栈在 `0x5000`–`0x9000`，sz = `0x9000`；
- 物理页按分配顺序递减：根 `0x6e8c` → VA 0 页 `0x6e8b` → 走页表时分配的 L2 `0x6e8a`、L3 `0x6e89` → 后续用户页 `0x6e88` 到 `0x6e81`，`0x6e85` 推测为保护页。

**trapframe 的变化**

| 寄存器 | A | B | 说明 |
|---|---|---|---|
| `elr` | `0x2750` | `0x2b9c` | A：ld-xv6 入口 `ldso_start`（`0x2000 + 0x750`）；B：ld-xv6 自带的 `__syscall`（`0xb7c`）+ `0x20` |
| `sp` | `0x8f60` | `0x8a30` | A：exec 刚压好的 argv 表（物理 `0x6e81f60`）；B：ld-xv6 的栈帧（物理 `0x6e81a30`） |
| `x0` | `0x97d0` | `0x3` | A：旧值，`return argc` 之后才由 syscall 写入；B：open 返回的 fd |
| `spsr` | `0x0` | `0x60000000` | 都返回 EL0t、中断打开，B 的 Z、C 标志置位 |
| `x16`/`x17` | `0x143e0`/`0xe59c` | 同 A | sh 留下的旧值 |

**两个细节**

- **exec 不清零通用寄存器。** `x16`/`x17` 等仍是 sh 的值。Linux 的 exec 会把寄存器清零；这里没有清，算是一个小的信息泄露点。
- **`p->context` 的偏移变了。** 这次的构建在 struct proc 里加了 `vm_trace_exec_printed` 等追踪字段，所以 `p->context` 从 §15.1 的 `proc + 0xb0` 移到了 `proc + 0xb8`。

在 B 这一刻，libc 还没有装入，sz 仍是 `0x9000`。接下来 ld-xv6 会用 `pread` 读 libc 的程序头，再用 `sbrk` 把它装到 `0x9000`，过程见 `readme_dynamic_libc.md` §12.2。

### 15.4 读图：用户地址怎样落到物理页

以 §15.3 的 pid 17（ls）为例，8 个用户页的对应关系如下：

| VA 页 | 内容 | L3 项 | 物理页 |
|---|---|---|---|
| `0x0000` | /bin/ls | L3[0] | `0x6e8b000` |
| `0x1000` | /bin/ls | L3[1] | `0x6e88000` |
| `0x2000` | ld-xv6（入口页） | L3[2] | `0x6e87000` |
| `0x3000` | ld-xv6 | L3[3] | `0x6e86000` |
| `0x4000` | 保护页 | L3[4]（清了 `PTE_U`，dump 不打印） | `0x6e85000`（推测） |
| `0x5000`–`0x8000` | 栈 | L3[5]–L3[8] | `0x6e84000`–`0x6e81000` |

几个容易误解的地方：

- **表里每一行是一个 4 KB 页，“VA 0x2000”指的是起始地址，不是大小。** `0x6e87000` 映射到 VA `0x2000`–`0x2fff`。ld-xv6 实际占 `0x2000`–`0x3d20`，即两页；`0x2000` 同时也是它的装载基址（`AT_BASE`），等于 `PGROUNDUP(ls 的结尾 0x1ac8)`。
- **ls 的 VA `0x0`–`0x2000` 是两页，物理上分在 `0x6e8b000` 和 `0x6e88000`。** 相邻物理页号相减得到的 `0x1000` 和程序大小无关。两页不挨着，是因为 `uvmalloc` 每次循环都是先 `kalloc` 一个数据页，再调 `mappages`；第一次 `mappages` 时 `walk()` 发现 L2、L3 都不存在，当场 `kalloc` 了 `0x6e8a`、`0x6e89` 两页，于是 VA `0x1000` 只能拿到 `0x6e88`。kalloc 是单页分配器，物理连续本来就不保证，程序看到的只是连续的虚拟地址。
- **条带按物理地址从左到右递增，和 VA 顺序正好相反**：最左边是栈顶页（VA `0x8000`），最右边是程序。
- **`0x6e87750` 就是 ls 执行的第一条用户指令。** 它是 ld-xv6.so 文件偏移 `0x750`，装到基址 `0x2000` 后是 VA `0x2750`，经 L3[2] 得到物理地址 `0x6e87000 + 0x750`：

  ```
  0000000000000750 <ldso_start>:
   750:  d114c3ff   sub  sp, sp, #0x530     ← 0x6e87750
   754:  d2800002   mov  x2, #0x0
  ```

  第一条指令就是 `sub sp, sp, #0x530`，所以 B 时刻的用户 sp `0x8a30` 正好等于 A 时刻的 `0x8f60 − 0x530`。B 的 elr `0x2b9c`（物理 `0x6e87b9c`）则是同一页里 `__syscall` 中 `svc #0` 的下一条 `ret`。

### 15.5 内核这一侧：proc、vmspace、内核栈在哪个地址空间

物理条带里，用户页和 struct proc 等内核对象挨在一起，这只说明它们都来自同一个 `kalloc()` 页池，不表示 proc 在用户空间里。谁能访问一页，取决于谁的页表、以什么权限映射它：

| 对象 | 内核虚拟地址 | 物理地址 | 怎么换算 |
|---|---|---|---|
| struct proc（`p`） | `0xffffff8006eb7000` | `0x6eb7000` | 直接映射：VA = KERNBASE + PA，即 `P2V`/`V2P` |
| `&p->context` | `0xffffff8006eb70b8` | `0x6eb70b8` | proc 内偏移 |
| struct vmspace（`p->vm`） | `0xffffff8006eb5000` | `0x6eb5000` | 直接映射 |
| 页表根（`vm->pagetable`） | `0xffffff8006e8c000` | `0x6e8c000` | 直接映射 |
| 内核栈（`p->kstack`） | `0xffffffbffffe0000` | `0x6ec0000` | `KSTACK(15)`，查内核页表 |

- 这些地址都在高半区，由 TTBR1 翻译，PTE 不给 EL0 权限；ls 的用户页表（TTBR0）里也没有任何一项指向它们，所以用户态无论用什么地址都碰不到自己的 struct proc。
- **`struct vmspace` 是内核数据结构**，用来描述一个用户地址空间。最关键的两个字段是 `pagetable`（L1 根，`switchuvm` 把 `V2P(pagetable)` 写进 TTBR0）和 `sz`（用户空间上界）。它从 proc 中单独拆出来，是为了支持线程：`clone` 只让 `refcount++` 并共享同一个 vmspace（见 §15.7）。
- **内核栈不在直接映射区**，而在 `KSTACK(slot) = MAXVA − (slot+1)·0x2000` 的槽位里。每个槽 2 页，只映射 1 页，另一页留空作保护，内核栈溢出会立即缺页。同一个物理页在直接映射区还有别名 `0xffffff8006ec0000`，但内核只通过 `p->kstack` 使用它。
- 同一个物理页可以先后有完全不同的身份：`0x6eb7000` 在 pid 16 的快照里是用户页（VA `0x13000`），到 pid 17 时变成了 struct proc。物理页本身没有“内核”和“用户”之分。

### 15.6 MMU 的三级翻译

![MMU 三级页表翻译：VA 0x89d0 → PA 0x6ece9d0](images/mmu-walk.png)

以 pid 16 的用户栈地址 `0x89d0` 为例，用快照里的真实 PTE 走一遍：

1. **拆分 VA**（`TCR_EL1.T0SZ = 25` 表示 39 位 VA，`TG0` 为 4 KB 页）：`[63:39]` 全 0，选 TTBR0；L1 索引 0，L2 索引 0，L3 索引 8，页内偏移 `0x9d0`。
2. **L1**：`TTBR0 = 0x6eb8000`，读 `0x6eb8000 + 0×8` 得 `0x6eb5003`。低两位 `11` 是表描述符，下一级表在 `0x6eb5000`。
3. **L2**：读 `0x6eb5000` 得 `0x6ec0003`，L3 表在 `0x6ec0000`。
4. **L3**：读 `0x6ec0000 + 8×8 = 0x6ec0040` 得 `0x6ece74b`。这是页描述符，页框为 `0x6ece000`。
5. **拼接**：PA = `0x6ece000 | 0x9d0 = 0x6ece9d0`。

L3 描述符的低 12 位是 `0x74b`：V=1，页描述符，AttrIndx=2（`MAIR_EL1` 第 2 项是 `0xff`，即普通可缓存内存），AP=01（EL1 和 EL0 都可读写，`PTE_U` 就是第 6 位），SH=11（内部共享），AF=1；UXN 和 PXN 都是 0，所以这一页可执行。

MMU 拼出物理地址的同时检查权限：

- 某一级 PTE 的 V=0：translation fault；
- AP 不允许 EL0 访问：permission fault。保护页就是这种情况：`uvmclear` 清了 `PTE_U`，AP 变成 00，ESR 的 DFSC 为 `0x0F`；
- `VA[63:39]` 既不全 0 也不全 1：直接 translation fault，不走表。

每次走表要额外读 3 次内存，TLB 会缓存翻译结果；`switchuvm` 写 TTBR0 之后，以及修改 PTE 之后，都要刷新 TLB。内核地址走同一套硬件流程，只是表来自 TTBR1：struct proc `0xffffff8006ed3000` 的 L1/L2/L3 索引是 0/`0x37`/`0xd3`，内核栈 `0xffffffbffffe2000` 的索引是 `0xff`/`0x1ff`/`0x1e2`。

### 15.7 threadtest：exec 之后与 clone 出的两个线程

![threadtest：exec 快照与线程结构](images/threadtest.png)

**exec 与第一个系统调用（pid 20，有快照）**

- 与 ls 几乎一样：`elr` 依次是 `0x2750`（`ldso_start`）和 `0x2b9c`（`__syscall` 之后）；用户 sp 从 `0x8f60` 变成 `0x8a30`；`open` 返回 fd 3。
- struct proc（`0x6eb7000`）、vmspace（`0x6eb5000`）、内核栈（槽 15，`0x6ec0000`）和 ls 时完全相同：ls 退出后这些页被释放，又分给了 pid 20。
- A 时刻 context.sp 是 `+0x760`（ls 是 `+0x530`），因为 exec 读盘睡眠时所在的调用深度不同；B 时刻同为 `+0x690`。
- 新页表的物理页这次是**递增**的：根 `0x6e99`、VA 0 页 `0x6e9a`、L2 `0x6e9b`、L3 `0x6e9c`、其余用户页 `0x6e9d`–`0x6ea4`。分配顺序与 ls 相同，地址方向相反，因为 ls 退出时按顺序把页还回了空闲链表，kalloc 后进先出，取出来就倒过来了。

**clone 之后的线程结构（按代码推出，tid 和线程栈地址为示意）**

`thread_create()`（`user/thread.c`）先用 malloc 在堆里分配 16 KB+16 的线程栈和一个 start 记录，再调用 `clone(thread_boot, start, 栈顶)`。内核里的 `threadclone()`：

- `allocproc()`：新线程有自己的 struct proc、内核栈（新的 KSTACK 槽）、trapframe 和 context（`context.sp = trapframe`，`x30 = forkret`）；
- `vm->refcount++`，`np->vm` 指向同一个 vmspace，三个线程共用同一张页表（根 `0x6e99000`）；`is_thread = 1`，`tgid = 20`；
- 复制主线程的 trapframe，再改写：`elr = thread_boot`（libc 中，`0x9000 + 0x535c = 0xe35c`），`sp = 线程栈顶`，`x0 = start`，`x30 = 0`；
- 复制文件描述符引用后 `make_runnable`。

线程第一次被调度时走 `swtch → forkret → usertrapret → eret`，进入 `thread_boot → worker`。worker 循环 1000 次：`sync_wait`、`counter++`、`sync_signal`，每 128 次调一次 `sched_yield`。`counter`（`0x1a04`）和 `counter_lock`（`0x1a00`）在程序的 .bss 中，三个线程共享。线程用 `texit` 退出，只拆掉自己，`refcount−−`；main 用 `thread_join` 等两个线程结束，最后检查 counter 是否等于 2000。

| | 共享（同一份） | 各有一份 |
|---|---|---|
| 地址空间 | vmspace、页表、sz、全部用户页 | — |
| 代码与全局变量 | 程序、libc、counter、counter_lock | — |
| 用户栈 | — | 主线程 `0x5000`–`0x9000`；线程是堆中的 16 KB |
| 内核侧 | — | struct proc、内核栈、trapframe、context |
| 身份 | tgid = 20（`getpid`） | pid / tid（`gettid`） |
| 文件 | 打开的文件对象 | 描述符表（`filedup` 复制引用） |

注意：线程栈只是堆里的一块普通内存，下方没有保护页，溢出时会悄悄覆盖相邻的堆数据。如果想用实测值替换图中的示意部分，可以在 `threadclone()` 末尾加一次 `procvmtrace(np, "thread-create")`。
