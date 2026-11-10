# swtch 换入/换出时 struct context 每个寄存器指向什么（log7 实测 + 反汇编）

`swtch(old, new)`（kernel/arch/arm64/swtch.S）只存/取 13 个值：`sp, x18, x19–x28, x29, x30`，
存进 `old`、从 `new` 装回，然后 `mov sp` + `ret`。x0–x17 不存——对编译器来说 swtch 是一次普通函数调用，
调用者本来就不指望它们保留。

两份 context：
- `p->context`（struct proc +0xb8，log 里 `context=0x…eb40b8` = proc `0x…eb4000` + 0xb8）：**任务这一侧**，在 sched() 里 `swtch(&p->context, &c->context)` 存。
- `c->context`（struct cpu +0x8）：**调度器这一侧**，在 scheduler() 里 `swtch(&c->context, &p->context)` 存。trace 不打印它，下面按 scheduler 反汇编给出。

## 1. 任务侧 p->context（sched() 调 swtch 那一刻）

sched 的序言：`stp x29,x30,[sp,#-48]! ; mov x29,sp ; stp x19,x20,[sp,#16] ; stp x21,x22,[sp,#32]`
——sched 自己用 x19–x22（调用者的 x19–x22 被它存在自己 48 字节的栈帧里），**不碰 x23–x28**。

| 寄存器 | 含义 | log7 实测 |
|---|---|---|
| sp | = sched 栈帧底（序言后 sp 就是 x29） | init[28] `…e49c0`、login[48] `…e2db0`、wifi[42] `…e2e10` |
| x29 (fp) | 同 sp：sched 的帧记录 {调用者 fp, 返回地址}，回溯从这里开始 | 永远 = sp |
| x30 (lr) | `sched+0xb4`：`bl swtch` 的下一条指令。换入时 swtch 的 `ret` 跳到这里，接着从 swtch 返回 | 所有任务都一样 |
| x19 | `&cpus[0]`（0x…cd768）：sched 算 mycpu() = cpus + id×0xa0 的基址 | 所有任务一样 |
| x20 | `0xa0` = sizeof(struct cpu) | 所有任务一样 |
| x21 | `p` 本身（myproc()） | init `…f6e000`、login `…eb4000` |
| x22 | 进 sched 时保存的 `c->intena`（换回后写回 `mycpu()->intena`） | **从定时器中断 yield 来的 = 0**（init[28]、sh[59]）；sleep/exit 路径 = 1 |
| x23–x28 | **sched 的调用者链**（yield/sleep/wait/exit/consoleread/sdsector…）当时放在被调用者保存寄存器里的局部变量；沿途没人用过的寄存器则还是**用户态的值** | 见下 |
| x18 | 平台寄存器，内核代码不用；内容跟着带过来 | login `0x8ad0` = trapframe 里用户 x18 |

x23–x28 实例（这几个最能说明“context 里保存的是调用链的现场”）：
- **login [48] 睡在 consoleread**：x25 = `0x…117a48` = `cons+0x98` = **&cons.r**（consoleread 把 sleep 的 chan 放在 x25）；x23 = `0x20`（read 请求的字节数 n=32）；x24 = 1（user_dst）。x26/x27/x28 = `0x8ad0/0x139b8/0x8b58` 与 trapframe 里用户 x26–x28 **完全相同**——从 el0sync 到 sched 整条路径没人动过它们。
- **init [28] 在 sdsector 忙等时被抢占**：x24 = `&sd_rca`（0x…f2908）、x25 = `0x…3f20203c`、x27 = `0x…3f202050`——SD host 控制器的 MMIO 寄存器地址（外设基址 0x3f202000 + 偏移），是 sdsector 循环里正在轮询的寄存器；x23/x28 = 1。
- **wifi [42] 在 exit 里**：x24 = `&wait_lock`（0x…cda58）——exit 持有/使用的锁；x26–x28 仍是用户值。
- **camera_wq [27] 内核线程**：x23–x28 全 0——从没进过用户态，kworker 也没用这些寄存器。

## 2. 调度器侧 c->context（scheduler() 调 swtch 那一刻）

scheduler 序言存 x19–x28（112 字节帧，在该 CPU 的 stack0 切片上），循环里寄存器分配（`swtch(&c->context, &p->context)` 前）：

| 寄存器 | 含义 |
|---|---|
| sp / x29 | scheduler 的栈帧，在 stack0[cpu]（0x…c8000 + cpu×0x1000）上 |
| x30 | `scheduler+0x120`：swtch 返回后执行 `switchkvm()`，再 `c->proc = 0`、`release(&p->lock)` |
| x19 | 选中的 `p`（pick_next 的结果，也是 acquire/release 的锁所在） |
| x20、x21 | cpu id（`mpidr_el1 & 0xff`），x21 还用来算 `1<<id` 检查 cpumask |
| x22 | `cpus − 0x18` 基址（访问 proc_head 等全局的基址） |
| x23 | `c` = &cpus[id] |
| x24 | `&cpus[0]`（与 x28 组合访问 `c->proc`） |
| x25 | 遍历运行队列的游标 proc 指针 |
| x26 | 0xa0 = sizeof(struct cpu) |
| x27 | `1<<id` 掩码 / 之后改作 `c` |
| x28 | id × 0xa0（c 相对 cpus 的偏移） |

swtch 的实参：x0 = `[sp,#96]` 里预先算好的 `&c->context`（c+8），x1 = `p + 0xb8` = `&p->context`。

## 3. 一次完整的换入 / 换出

换入（scheduler → 任务，例：[47] CPU3 换入 login）：
1. scheduler 的 x19–x30、sp 存进 `cpus[3].context`；
2. 从 `login->context` 装回：sp = `…e2db0`（login 内核栈）、x29 同值、x30 = `sched+0xb4`、x19–x22 = sched 自己的值、x23–x28 = consoleread 链/用户的值；
3. `ret` → sched+0xb4：sched 用 x19/x20 算出**当前** CPU（CPU3，不是睡前的 CPU2）写回 intena，再从自己的栈帧恢复调用者的 x19–x22、`ret` 回 sleep → consoleread……

换出（任务 → scheduler，例：[48] login 再次睡下）：反过来，login 在 sched 里把上表那些值存进 `login->context`，从 `cpus[3].context` 装回 scheduler 的现场，`ret` 到 `scheduler+0x120`。

要点：
- 同一个任务每次换出，x19/x20/x30 都一样，变的是 sp（调用深度）、x21（是谁）、x22（是否从中断来）和 x23–x28（在做什么）。
- 换 CPU 不需要改 context：x19/x20 存的是 cpus 基址和大小，sched 恢复后重新读 mpidr_el1 算出当前 CPU。
- x23–x28 正好暴露了睡前在干什么：login 的 x25 就是它等待的 chan，init 的 x25/x27 是正在轮询的 SD 寄存器。

注：反汇编来自当前 kernel/kernel（11-06 20:05 构建）；sched/scheduler 的偏移（sched+0xb4 等）和 log7 那次构建一致，数据符号（cpus、cons、wait_lock、sd_rca）也与 log7 里的值对得上。
