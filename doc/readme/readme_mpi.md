# MPI subset in user space

`user/mpi.c` (linked into `libc.so`) and `user/include/mpi.h` give xv6
programs a single-node MPI: one `MPI_COMM_WORLD`, blocking point-to-point
and the common collectives. No kernel changes are needed.

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
* **Matching.** Each rank drains every incoming ring. A header that matches
  the posted receive is copied straight into the user buffer. Any other
  message goes to an unexpected queue (malloc'd copy), kept in arrival order
  so per-source ordering holds. Every wait loop keeps draining, so sends are
  effectively buffered and head-to-head large `MPI_Send`s do not deadlock.
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
`Get_processor_name`, `Type_size`, `MPI_Wtime_us`; `Send/Recv/Sendrecv`,
`Probe`, `Get_count` (with `MPI_ANY_SOURCE`, `MPI_ANY_TAG`, `MPI_PROC_NULL`);
`Barrier`, `Bcast`, `Reduce`, `Allreduce`, `Gather`, `Scatter`, `Allgather`,
`Alltoall` (`MPI_IN_PLACE` supported for Reduce/Allreduce/Gather/Allgather).
The integer types are `CHAR/BYTE/UNSIGNED_CHAR/SHORT/UNSIGNED_SHORT/INT/UNSIGNED/LONG/UNSIGNED_LONG/LONG_LONG`,
and the ops are `SUM PROD MAX MIN LAND LOR BAND BOR BXOR`.

## Limits

* There is no `MPI_FLOAT`/`MPI_DOUBLE` and no `double MPI_Wtime()`. User
  code is built with `-mcpu=cortex-a53+nofp` and the kernel does not save
  FP/SIMD state on a context switch. Enabling FP (CPACR_EL1.FPEN, plus
  saving q0-q31/fpcr/fpsr per task) is a kernel project of its own.
* There are no non-blocking calls (`Isend/Irecv/Wait`) yet. The progress
  engine already has per-source drain state, so they would mostly add a
  request list.
* N ≤ 8 (`MPI_MAX_RANKS`). Each job uses N of the 64 kernel sync objects
  (`NSYNC`). If a rank crashes, the other ranks keep waiting. Use
  `MPI_Abort` or `kill`.
* xv6 programs must call `exit()`. On rank 0, `MPI_Finalize` waits for the
  other ranks to exit.

## Test

`mpitest` checks a token ring, a 200 KB head-to-head exchange, tag/source
matching, probe, self-send, every collective (for each root where it
applies) and an integer-only π reduce. In QEMU raspi3b (TCG) it passes with
1, 2, 3, 4 and 8 ranks.

QEMU numbers are only a rough guide: about 1.5 µs one-way 8 B latency and
about 110–125 MB/s for a 64 KB stream with ≤ 4 ranks. With 8 ranks on 4 CPUs,
latency rises to about 450 µs because ranks yield and sleep.
