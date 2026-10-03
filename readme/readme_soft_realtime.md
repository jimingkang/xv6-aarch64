# 软实时调度：优先级、CPU 亲和性与实时策略

分支：`rpi3-soft-realtime`

本文记录在 xv6-aarch64（Raspberry Pi 3）上加入的软实时调度支持：

1. 进程优先级：`SCHED_OTHER` 的 nice 值，以及 `SCHED_FIFO`/`SCHED_RR` 的实时优先级 1～99；
2. CPU 亲和性：每个进程一个 `cpumask`；
3. 实时调度算法：固定优先级抢占式调度（与 Linux `SCHED_FIFO`/`SCHED_RR` 语义一致），
   通过核间中断（IPI）立即抢占，并带 RT 节流保护。

“软实时”指：高优先级任务被唤醒后会尽快抢占低优先级任务，延迟通常很小，但内核不对
最坏延迟做形式化保证（见第 10 节）。

## 0. 改动文件

| 文件 | 内容 |
|---|---|
| `kernel/sched.h` | 新增。策略常量、优先级范围、`struct sched_info`，内核与用户程序共用 |
| `kernel/proc.h` | `struct proc` 增加调度属性；`struct cpu` 增加 `need_resched`、`cur_prio`、RT 节流计数 |
| `kernel/proc.c` | 调度器核心：`pick_next()`、`make_runnable()`、`resched_hint()`、`sched_tick()`、`sched_set*()`；`ps` 输出增加调度列 |
| `kernel/trap.c` | 每个 10 ms 时钟中断调用 `sched_tick()`；处理 IPI；在中断返回、系统调用返回处检查 `need_resched` |
| `kernel/bcm2837.c` | ARM-local mailbox 0 作为重新调度 IPI：`send_resched_ipi()`、`resched_ipi_ack()` |
| `kernel/memlayout.h` | `IPI_RESCHED_IRQ` 伪中断号 |
| `kernel/syscall.[ch]`、`kernel/sysproc.c` | 7 个新系统调用（第 7 节） |
| `kernel/defs.h` | 新函数声明 |
| `user/chrt.c`、`taskset.c`、`nice.c`、`renice.c` | 与 Linux 同名命令用法一致 |
| `user/spin.c`、`user/rtlat.c` | 测试工具：忙循环、唤醒延迟测量 |
| `user/schedutil.h` | 上述用户程序共用的小函数 |
| `user/ps.c`、`user/user.h`、`user/usys.pl`、`Makefile` | 接入新系统调用与程序 |

## 1. 多核启动

本分支开始前 SMP 已经可用（见 `readme_usb_wifi_MT7601.md` 第 16 节），这里只简述流程：

```text
上电：4 个核都进入 _entry（entry.S）
  ├─ CPU0：清 BSS、建启动页表 → secondary_release=1, SEV → main() 完成全部初始化
  │        → start_secondary_cpus()：把 _entry 物理地址写入 spin-table 0xe0/0xe8/0xf0
  │          （固件 armstub 让 CPU1～3 在这里等待），dc cvac 写回内存，SEV
  └─ CPU1～3：从 spin-table 跳到 _entry → entryothers → 开 MMU → main() 的 else 分支
             kvminithart → trapinithart → gicv3inithart → timerinit → scheduler()
```

真机日志：

```text
PH0hart 3 starting
hart 2 starting
hart 1 starting
```

三个副核同时从 spin-table 出发，谁先抢到 printf 的锁谁先打印，顺序不固定。`PH0` 是
`boot_uart_mark()` 的早期打点，不经过 printf 锁，所以会与其他输出交错。

`readme_rpi3` 和 `readme_exeption_irq_timer` 中“只运行 CPU0”的描述已经过时。

## 2. 调度策略与属性

策略常量的值与 Linux 相同（`kernel/sched.h`）：

| 策略 | 值 | 优先级参数 | 行为 |
|---|---|---|---|
| `SCHED_OTHER` | 0 | 必须为 0；另有 nice -20～19 | 分时。nice 只决定时间片长度 |
| `SCHED_FIFO` | 1 | 1～99，越大越优先 | 一直运行，直到阻塞、`sched_yield()` 或被更高优先级抢占。没有时间片 |
| `SCHED_RR` | 2 | 1～99 | 同 FIFO，但同优先级任务每 100 ms 轮转一次 |

`struct proc` 新增字段（全部受 `p->lock` 保护）：

| 字段 | 含义 |
|---|---|
| `policy` | `SCHED_OTHER` / `SCHED_FIFO` / `SCHED_RR` |
| `rt_priority` | 实时优先级 1～99，`SCHED_OTHER` 为 0 |
| `nice` | -20～19 |
| `cpumask` | 允许运行的 CPU，bit n 代表 CPUn，默认 `0xf` |
| `slice` | 当前时间片剩余的调度节拍数 |
| `rq_seq` | 入队序号，同优先级按它先来先服务 |
| `last_cpu` | 正在或最近一次运行它的 CPU，-1 表示还没运行过 |
| `run_ticks` | 累计运行的调度节拍（每个 10 ms） |

继承规则与 Linux 相同：`fork()` 的子进程继承策略、优先级、nice 和亲和性；`exec()`
不改变它们。所以 `chrt`、`taskset` 都是“先改自己，再 exec 目标程序”。

内核线程（kworker）默认是 `SCHED_OTHER`、全部 CPU。如有需要可以用 `chrt -p` 修改，
例如把 `usbkbd_wq` 提成实时：`chrt -f -p 60 6`（6 是它在 `ps` 中的 PID）。

## 3. 选择下一个进程

xv6 原来的调度器按 `proc[]` 下标顺序轮询，第一个 RUNNABLE 就运行。现在每个 CPU 调用
`pick_next()`，选出：

1. `cpumask` 允许当前 CPU 的 RUNNABLE 进程中，**有效优先级最高**的；
2. 同优先级时 **`rq_seq` 最小**的，也就是最早入队的。

有效优先级（`effective_prio()`）：

```text
PRIO_RT_BASE + rt_priority   = 101..199   SCHED_FIFO / SCHED_RR
PRIO_OTHER                   = 0          SCHED_OTHER
PRIO_THROTTLED               = -1         实时任务，但所在 CPU 本周期 RT 预算已用完（第 6 节）
PRIO_IDLE                    = -1000      仅用于 cpu->cur_prio：CPU 空闲
```

任何实时任务都排在所有普通进程前面。“运行队列”仍然是 `proc[]` 数组加一个序号，
保持 xv6 的风格，没有引入链表或红黑树。`NPROC` 是 64，每次选择扫描一遍的开销可以接受。

`pick_next()` 返回时不持有锁，所以 `scheduler()` 加锁后会再检查一次：如果该进程已被
其他 CPU 取走，或亲和性刚被改掉，就放弃这次选择，重新挑选。

### 入队位置：队首还是队尾

进程变成 RUNNABLE 统一走 `make_runnable(p, at_head)`：

| 场景 | 位置 | 说明 |
|---|---|---|
| 被唤醒、新建、fork | 队尾 | 分配新的 `rq_seq` |
| 时间片用完 | 队尾 | RR 轮转、普通进程轮转都靠这一条 |
| `sched_yield()` | 队尾 | 让给同优先级的其他任务 |
| FIFO/RR 被更高优先级抢占，时间片还有剩余 | **队首** | 保留原 `rq_seq`；与 Linux 相同，被抢占的实时任务不应失去位置 |

## 4. 时间片与调度节拍

每个 CPU 的 Generic Timer 每 10 ms 中断一次（`timer.c`）。现在**每次**中断都是一个调度
节拍，调用 `sched_tick()`：

- 当前进程 `run_ticks++`；
- 不是 FIFO 的进程 `slice--`，减到 0 就设置 `need_resched`；
- 更新 RT 节流计数（第 6 节）。

时间片长度（`timeslice()`）：

| 情况 | 节拍数 | 时间 |
|---|---|---|
| `SCHED_FIFO` | 无限 | — |
| `SCHED_RR` | 10 | 100 ms |
| `SCHED_OTHER`，nice 0 | 10 | 100 ms（与原 xv6 相同） |
| `SCHED_OTHER`，nice -20 | 20 | 200 ms |
| `SCHED_OTHER`，nice 19 | 1 | 10 ms |

公式：`(20 - nice) / 2`，最少 1 个节拍。两个普通进程争用同一个核时，CPU 时间大致与
时间片长度成正比。

`sleep()`、`uptime()` 使用的 xv6 `ticks` 仍然每 100 ms 由 CPU0 加一，原有语义不变。

## 5. 抢占与重新调度 IPI

### 5.1 谁需要被抢占

一个进程变成 RUNNABLE 时，`make_runnable()` 调用 `resched_hint(p)`：

```text
for 每个 p 允许运行、且已经上线的 CPU：
    如果 cpus[i].cur_prio == PRIO_IDLE → 返回（那个空闲核自己会挑中 p）
    记下 cur_prio 最低的 CPU
如果 p 的优先级 > 那个最低的 cur_prio → kick_cpu(那个 CPU)
```

`kick_cpu(cpu)` 设置 `cpus[cpu].need_resched = 1`。如果目标不是当前 CPU，再发一个 IPI。

`cur_prio` 由各 CPU 在切换进程时写入，其他 CPU 不加锁读取，只作为提示。读到旧值的
最坏结果是多一次不必要的重新调度，或者晚一个时钟节拍才抢占，不会出错。

### 5.2 IPI：ARM-local mailbox 0

BCM2836/2837 的 ARM-local 外设（物理 `0x40000000`）给每个核提供 4 个 mailbox。
Linux 在这颗芯片上也用 mailbox 0 做 IPI，这里同样如此：

| 偏移 | 寄存器 | 用法 |
|---|---|---|
| `0x50 + 4*c` | Core c mailbox 中断控制 | 写 1：开启 mailbox 0 的 IRQ（`gicv3inithart()`） |
| `0x60 + 4*c` | Core c IRQ source | bit 3 = 虚拟定时器，bit 4 = mailbox 0 |
| `0x80 + 0x10*c` | Core c mailbox 0 write-set | 写 1：向 core c 发 IPI（`send_resched_ipi()`） |
| `0xc0 + 0x10*c` | Core c mailbox 0 read/clear | 写全 1：清除（`resched_ipi_ack()`） |

`gic_iar()` 发现 mailbox 0 置位时返回伪中断号 `IPI_RESCHED_IRQ`（1000），`devintr()`
只负责清除它。`need_resched` 在发送 IPI 之前就已经设好（发送前有 `dsb sy`）。

### 5.3 在哪里让出 CPU

原来只有“逻辑节拍（100 ms）的时钟中断”才会 `yield()`。现在三处都检查
`resched_pending()`：

| 位置 | 作用 |
|---|---|
| `userirq()`：用户态被中断后 | 时间片用完或收到 IPI，立即切换 |
| `usertrap()`：系统调用返回前 | 系统调用自己唤醒了更高优先级的进程（例如往管道写数据） |
| `kernelirq()`：内核态被中断后 | 内核态抢占：被唤醒的实时任务不必等一个很长的系统调用结束 |

内核只有在不持有自旋锁时才开中断，所以 `kernelirq()` 里 `yield()` 是安全的（原来就有
这条路径，只是触发条件变了）。

### 5.4 一次完整的抢占

以 `rtlat` 为例：CPU3 上跑着普通进程 hog，读者（FIFO 80，只允许 CPU3）阻塞在管道上，
写者在 CPU1 上：

```text
CPU1：write(pipe)
        └─ wakeup(&pi->nread) → make_runnable(reader)
              └─ resched_hint：CPU3 的 cur_prio = 0 < 180
                    └─ kick_cpu(3)：need_resched=1，写 0x40000080+0x30 = 1
CPU3：（正在运行 hog 的用户态）
        IRQ → userirq → devintr：IPI → resched_ipi_ack()
        resched_pending() → yield()：hog 时间片还有剩余，但它是 SCHED_OTHER → 放到队尾
        scheduler → pick_next → reader → swtch
        reader 从 read() 返回 → clock_us() 计算延迟
```

## 6. RT 节流

为了防止一个失控的 `SCHED_FIFO` 死循环把整个系统卡死，采用和 Linux 默认值相同的配额：

```text
sched_rt_period  = RT_PERIOD  = 100 个节拍 = 1 s
sched_rt_runtime = RT_RUNTIME =  95 个节拍 = 950 ms
```

每个 CPU 独立计数。实时任务在一个周期里用满 950 ms 后，这个 CPU 进入 `rt_throttled`：
实时任务的有效优先级变成 -1，低于普通进程，于是 shell、kworker 能在剩下的 50 ms 里
运行。周期结束时解除节流，并设置 `need_resched` 让实时任务立即恢复。

与 Linux 的一个区别：被节流的 CPU 上如果**没有**可运行的普通进程，实时任务会继续运行，
而不是让 CPU 空转。

## 7. 系统调用

| 编号 | 用户接口 | 说明 |
|---|---|---|
| 59 | `int sched_setscheduler(int pid, int policy, int priority)` | 设置策略和实时优先级；OTHER 必须传 0 |
| 60 | `int sched_getinfo(int pid, struct sched_info *si)` | 读取策略、优先级、nice、cpumask、CPU、运行时间 |
| 61 | `int setnice(int pid, int nice)` | -20～19 |
| 62 | `int sched_setaffinity(int pid, uint mask)` | mask 不能为 0；超出 NCPU 的位被忽略 |
| 63 | `int sched_yield(void)` | 排到同优先级的末尾 |
| 64 | `uint64 clock_us(void)` | 开机以来的微秒数，来自 `CNTVCT_EL0`，各核共享，可以跨核比较 |
| 65 | `int getcpu(void)` | 当前运行在哪个 CPU |

`pid` 为 0 表示调用者自己。修改正在运行的进程时，会让运行它的 CPU 重新调度：
例如亲和性去掉了它当前所在的 CPU，它会在下一次中断或系统调用返回时离开这个核。

本版本没有权限检查：任何进程都可以修改任何进程的调度属性。

## 8. 用户工具

```text
chrt -p PID                         查看策略与优先级
chrt [-f|-r|-o] -p PRIO PID         修改（-f FIFO，-r RR（默认），-o OTHER）
chrt [-f|-r|-o] PRIO CMD [ARG...]   以指定策略运行命令

taskset -p PID                      查看亲和性
taskset -p MASK PID                 修改，例如 0x8 = 只在 CPU3
taskset MASK CMD [ARG...]           以指定亲和性运行命令

nice [-n N] CMD [ARG...]            nice 增加 N（默认 10）
renice N PID                        把 PID 的 nice 设为 N

spin [SECONDS]                      忙循环；结束时打印策略、CPU、得到的 CPU 时间、起止时间
rtlat [-p fifo|rr|other] [-P PRIO] [-c CPU] [-l HOGS] [-n SAMPLES]
                                    唤醒延迟测试（第 9 节）
```

两个命令嵌套时注意顺序：`chrt -r 30 taskset 0x8 spin 2` 先变成实时，再绑到 CPU3；
反过来写成 `taskset 0x8 chrt -r 30 ...`，中间的 `chrt` 会先以普通进程身份被绑到 CPU3。
如果 CPU3 上正有实时任务在跑，它要等到 RT 节流留出的 50 ms 窗口才能执行 `chrt`。

`ps` 输出：

```text
  PID CPU POLICY PRI  NI MASK     TIME STATE    NAME
    6   2  OTHER   0   0    f     0.01 sleeping usbkbd_wq
   14   3   FIFO  50   0    f     1.29 running  spin
```

`CPU` 是正在运行它或最近一次运行它的核；`TIME` 是累计 CPU 时间，单位秒。

## 9. 测试

### 9.1 QEMU 功能测试

`make qemu` 后登录（root / xv6）：

| 命令 | 结果 | 说明 |
|---|---|---|
| 4 个 `chrt -f 50 spin 4 &`，然后 `ps`、`echo` | shell 正常响应；每个 spin 得到 3.6～3.8 s / 4 s | RT 节流生效 |
| 两个 `chrt -r 30 taskset 0x8 spin 2 &` | 时间段重叠（6719..8790、6808..8838 ms），各得约 950 ms | RR 轮转 |
| 两个 `chrt -f 30 taskset 0x8 spin 2 &` | 一个结束另一个才开始（13073..15074、15135..17135 ms） | FIFO 不轮转 |
| `chrt -f 10 ...spin 2 &` 后 `chrt -f 20 ...spin 1 &` | prio 20 得到 960 ms / 1 s；prio 10 只有 850 ms / 2 s | 高优先级立即抢占 |
| `taskset 0x8 spin 3 &` 和 `taskset 0x8 nice -n 10 spin 3 &` | 1700 ms 对 960 ms | 时间片 100 ms 对 50 ms |
| `rtlat -p fifo` / `rtlat -p other` | 平均 8 ms / 169 ms，最大 20 ms / 393 ms | 实时读者明显更快 |

QEMU 中的 `rtlat` 绝对数值**不能代表真机**。调试计数显示发往 CPU3 的 14 个 IPI 全部
收到了，机制正常；但测试主机只有 2 个物理核，却要模拟 4 个 vCPU，被唤醒的 vCPU 经常
根本没有在执行。即使 CPU3 上没有任何负载（`rtlat -l 0`），平均延迟也有 7.5 ms。

### 9.2 真机延迟测试

```text
rtlat -p fifo          # 实时读者
rtlat -p other         # 普通读者，对照组
rtlat -p fifo -l 0     # 无负载基线
```

`rtlat` 的结构：

```text
CPU3: 3 个 SCHED_OTHER 忙循环（hog） + 读者（被测策略），都只允许 CPU3
其他 CPU: 写者，每 100 ms 向管道写一次 clock_us()
读者: read() 返回后计算 clock_us() - 写入时间
```

预期：FIFO 读者的延迟主要来自 IPI、中断入口和一次上下文切换，应远低于 1 ms；
OTHER 读者要排在 3 个 hog 后面，每个 hog 最多 100 ms，延迟可达几百毫秒。

如果 FIFO 的最大值偶尔达到几毫秒，最可能的原因是第 10 节中的关中断临界区，
特别是内核 `printf` 正在输出串口。

## 10. 已知限制与后续工作

- **关中断临界区决定最坏延迟**：持有自旋锁期间不响应中断（包括 IPI）。内核 `printf`
  每行在 115200 波特率下需要数毫秒，实时测试时应避免内核日志。`dwc2.c` 的注释也提到，
  printf 会错过 125 µs 的 USB 微帧窗口。
- **没有优先级继承**：低优先级进程持有 sleeplock 时，等待这把锁的高优先级进程会被
  中等优先级的进程间接阻塞（优先级反转）。
- **没有 `SCHED_DEADLINE`（EDF）**。
- **全局扫描**：每次调度扫描整个 `proc[]`，并逐个获取 `p->lock`；空闲 CPU 一直在扫描
  （xv6 原来就是这样，没有 WFI）。进程数多时可以改为每 CPU 一个按优先级组织的运行队列。
- **调度节拍固定 10 ms**：RR 时间片和 RT 节流以节拍为粒度；没有高精度定时器，`sleep()`
  仍以 100 ms 为单位，所以还不能写“每 5 ms 运行一次”的周期任务。
- **没有权限检查**：普通用户也能把进程设成实时。
- **从 RT 降级的被动迁移**：节流或亲和性变化时，任务通过 `resched_hint()` 推给其他 CPU，
  没有主动的负载均衡。

## 附：启动时显示内核编译时间

内核启动横幅会显示镜像的生成时间与 git 版本：

```text
xv6 kernel is booting
build: bcm43430-sdio-v2
kernel image built: 2026-10-03 11:57:03 CDT (rpi3-soft-realtime 6942a16+dirty)
```

实现：Makefile 的 `$K/kernel` 规则在**每次链接**前生成 `kernel/buildinfo.c`：

```c
const char kernel_build_time[] = "2026-10-03 11:57:03 CDT";
const char kernel_build_rev[] = "rpi3-soft-realtime 6942a16+dirty";
```

然后编译并链接进内核，`main()` 打印这两个字符串。没有在 `main.c` 里用
`__DATE__`/`__TIME__`，因为那只是 `main.c` 最后一次被编译的时间：只改了别的文件时，
`main.o` 不会重新编译，显示的时间就是错的。

- 时间是编译机的本地时间（`date '+%Y-%m-%d %H:%M:%S %Z'`）；
- 版本来自 `git describe --always --dirty=+dirty`：没有 tag 时是短提交号，有 tag 时形如
  `v1.0-3-g6942a16`；`+dirty` 表示编译时已跟踪的文件有未提交的修改；
- git 命令都带 `GIT_OPTIONAL_LOCKS=0`，不会在编译时写 `.git/index.lock`；
- 不在 git 仓库中编译时显示 `unknown -`；
- `kernel/buildinfo.c` 是生成文件，`make clean` 会删除它，也不应提交。

用途：TFTP 或 SD 卡上的镜像是否真的更新了，看这一行就能确认。
