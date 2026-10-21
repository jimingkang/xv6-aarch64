# 进程、异常与栈：fork / exec / wait / 调度 / initcode

本文按 rpi3-dynmodel 分支的源码（`kernel/core/proc.c`、`kernel/fs/exec.c`、`kernel/core/trap.c`、`kernel/arch/arm64/trapasm.S`、`kernel/arch/arm64/swtch.S`、`kernel/arch/arm64/entry.S`、`user/initcode.S`）整理，回答下面几类问题：

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
