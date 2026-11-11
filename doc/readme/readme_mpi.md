# MPI subset in user space

`user/mpi.c` (linked into `libc.so`) and `user/include/mpi.h` give xv6
programs a single-node MPI: one `MPI_COMM_WORLD`, blocking and non-blocking
point-to-point, the common collectives, and integer and floating-point
reductions. The library itself needs no kernel changes. Floating point needs
the per-task FP/SIMD context described below.

```
$ mpirun -np 4 mpitest      # or: mpitest -np 4
```

## How it works

* **Launch.** `MPI_Init` strips `-np N` from `argv`, maps one
  `MAP_SHARED|MAP_ANONYMOUS` region, creates one `SYNC_SEM` doorbell per rank
  and forks ranks 1..N-1. `dup_mmap()` maps shared pages into the child
  instead of COW-copying them, so all ranks see the same memory. Each rank
  is pinned to CPU `rank % NCPU` with `sched_setaffinity`. `mpirun` only
  inserts `-np N` and execs the program: memory set up before `exec` would
  not survive it.
* **Transport.** `ring[src][dst]` is a 16 KB single-producer/single-consumer
  byte ring. A message is a 16-byte header (tag, length) followed by the
  payload, streamed in pieces, so any size works. Indices are published with
  `stlr` and read with `ldar` (`__atomic_*`). No pipes are used:
  `PIPESIZE` is 512 bytes and `NOFILE` is 16.
* **Requests.** A send or receive is a `struct mpi_req`. Blocking calls keep
  it on the stack and wait on it. `MPI_Isend`/`MPI_Irecv` malloc it and
  return it as an `MPI_Request`, and `Wait`/`Test` free it. Sends queue
  FIFO per destination and stream from the caller's buffer as ring space
  appears, so the buffer must stay untouched until the request completes.
* **Matching.** Each rank drains every incoming ring. A header is matched
  against the posted receives in post order and copied straight into the
  first match's buffer. Any other message goes to an unexpected queue
  (malloc'd copy), kept in arrival order so per-source ordering holds. A
  later receive that matches an unexpected message which is still arriving
  completes when the message does. Every wait loop drives both directions,
  so head-to-head large `MPI_Send`s do not deadlock. Progress happens only
  inside MPI calls.
* **Waiting.** A rank spins (with `sched_yield` when ranks > CPUs). After
  2000 idle rounds it sets its `sleeping` flag, re-checks, and blocks in
  `sync_wait` on its doorbell. A peer that publishes data, or frees ring
  space, checks the flag and calls `sync_signal`. The seq_cst store/load pair
  on both sides makes a lost wake-up impossible.
* **Collectives.** These are binomial trees (`Bcast`, `Reduce`) or linear
  sends (`Gather`, `Scatter`, `Alltoall`) over point-to-point, using
  negative internal tags that `MPI_ANY_TAG` never matches. `Barrier` is a
  zero-byte reduce plus a zero-byte bcast.

## API

`MPI_Init/Initialized/Finalize/Abort`, `Comm_rank/size`,
`Get_processor_name`, `Type_size`, `MPI_Wtime/Wtick/Wtime_us`;
`Send/Recv/Sendrecv`, `Probe/Iprobe`, `Get_count` (with `MPI_ANY_SOURCE`,
`MPI_ANY_TAG`, `MPI_PROC_NULL`); `Isend/Irecv`, `Wait/Waitall/Waitany`,
`Test/Testall` (`MPI_REQUEST_NULL`, `MPI_STATUSES_IGNORE`);
`Barrier`, `Bcast`, `Reduce`, `Allreduce`, `Gather`, `Scatter`, `Allgather`,
`Alltoall` (`MPI_IN_PLACE` supported for Reduce/Allreduce/Gather/Allgather).
The integer types are `CHAR/BYTE/UNSIGNED_CHAR/SHORT/UNSIGNED_SHORT/INT/UNSIGNED/LONG/UNSIGNED_LONG/LONG_LONG`,
with ops `SUM PROD MAX MIN LAND LOR BAND BOR BXOR`. `FLOAT` and `DOUBLE`
take `SUM PROD MAX MIN`.

## FP/SIMD context (kernel)

User code used to be built `-mcpu=cortex-a53+nofp` because the kernel never
saved v0-v31. Now:

* `entry.S` clears `CPTR_EL3.TFP` and `CPTR_EL2.TFP` on the way down, and
  sets `CPACR_EL1.FPEN=0b11` on every core.
* `struct proc` has a `struct fpsimd_state` (q0-q31, fpsr, fpcr; 528
  bytes). `scheduler()` calls `fpsimd_load()` right before `swtch()` to a
  task and `fpsimd_save()` right after `swtch()` returns. The kernel is still
  built `+nofp`, so between a task's trap and that point the registers still
  hold the task's values, and no trap path has to save them.
* `allocproc()` zeroes the state. `do_clone()` copies the caller's live
  registers into the child (fork and threads). `exec()` zeroes and loads a
  clean state.
* `USER_CFLAGS` appends `-mcpu=cortex-a53`, which overrides `+nofp` for
  user code, libc and ld.so only.

The cost is two 528-byte copies per context switch, done eagerly. A lazy
scheme (trap on first FP use) would avoid them for tasks that never touch
FP.

`fptest` fills q0-q31 and FPCR.RMode with a per-task pattern in more tasks
than CPUs, spins, and checks every register. It also checks that `fork()`
hands the live registers to the child, and that double arithmetic agrees
across tasks. With the scheduler's save/load commented out, it fails.

## Limits

* Collectives are blocking only. There is no `MPI_Cancel`, no persistent
  requests, and no derived datatypes.
* `printf` has no `%f`. Print doubles scaled to integers.
* N ≤ 8 (`MPI_MAX_RANKS`). Each job uses N of the 64 kernel sync objects
  (`NSYNC`). If a rank crashes, the other ranks keep waiting. Use
  `MPI_Abort` or `kill`.
* xv6 programs must call `exit()`. On rank 0, `MPI_Finalize` waits for the
  other ranks to exit.

## Test

`mpitest` checks:

* a token ring and a 200 KB head-to-head exchange;
* tag/source matching, probe and self-send;
* every collective, for each root where it applies;
* an integer π reduce and a double π `Allreduce`;
* non-blocking calls: a ring shift with `Waitall`, post-order matching
  with `Testall`, `Waitany`, `Test` polling, and `Iprobe` followed by a
  receive of a message still arriving;
* float/double `MAX/MIN/SUM/PROD`.

In QEMU raspi3b (TCG) it passes with 1, 2, 4 and 8 ranks. `fptest`,
`threadtest`, `cowtest` and `posixtest` also pass.

QEMU numbers are only a rough guide: about 1.5 µs one-way 8 B latency and
about 110–125 MB/s for a 64 KB stream with ≤ 4 ranks. With 8 ranks on 4 CPUs,
latency rises to about 450 µs because ranks yield and sleep.
