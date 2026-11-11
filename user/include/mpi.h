// mpi.h -- a small MPI subset for xv6-aarch64 user programs.
//
// Single node only: MPI_Init forks the ranks of MPI_COMM_WORLD from the
// calling process and they talk through one MAP_SHARED|MAP_ANONYMOUS region
// (one single-producer/single-consumer byte ring per ordered rank pair).
// A rank that has nothing to do spins briefly, then sleeps on a per-rank
// kernel semaphore (sync_create(SYNC_SEM)) that its peers signal.
//
// Start a program with "mpirun -np N prog args..." or "prog -np N args...":
// MPI_Init strips "-np N" from argv.  Without it the job has one rank.
//
// Differences from full MPI that a caller should know about:
//   * Only MPI_COMM_WORLD.  Blocking point-to-point and collectives only.
//   * Sends are buffered: MPI_Send returns once the message is in the ring
//     (the receiver drains rings into an unexpected-message queue while it
//     waits), so the classic "both ranks Send first" pattern does not hang.
//   * The kernel builds user code with +nofp and does not save FP/SIMD
//     state, so there is no MPI_FLOAT/MPI_DOUBLE and no double MPI_Wtime().
//     MPI_Wtime_us() returns microseconds instead.
//   * xv6 programs must call exit(); MPI_Finalize on rank 0 waits for the
//     other ranks to exit before it returns.
#ifndef XV6_MPI_H
#define XV6_MPI_H

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;

typedef struct {
  int MPI_SOURCE;
  int MPI_TAG;
  int MPI_ERROR;
  int _bytes;               // received payload size, read via MPI_Get_count
} MPI_Status;

#define MPI_COMM_WORLD      0

#define MPI_SUCCESS         0
#define MPI_ERR_COMM        1
#define MPI_ERR_COUNT       2
#define MPI_ERR_TYPE        3
#define MPI_ERR_TAG         4
#define MPI_ERR_RANK        5
#define MPI_ERR_TRUNCATE    6
#define MPI_ERR_OP          7
#define MPI_ERR_ARG         8
#define MPI_ERR_BUFFER      9
#define MPI_ERR_OTHER      10

#define MPI_ANY_SOURCE    (-1)
#define MPI_ANY_TAG       (-1)
#define MPI_PROC_NULL     (-2)
#define MPI_UNDEFINED     (-32766)
#define MPI_TAG_UB        0x3fffffff
#define MPI_STATUS_IGNORE ((MPI_Status *)0)
#define MPI_IN_PLACE      ((void *)1)
#define MPI_MAX_PROCESSOR_NAME 32
#define MPI_MAX_RANKS     8

// Datatype handle = (id << 8) | element size in bytes.
#define MPI_CHAR            0x0101
#define MPI_BYTE            0x0201
#define MPI_UNSIGNED_CHAR   0x0301
#define MPI_SHORT           0x0402
#define MPI_UNSIGNED_SHORT  0x0502
#define MPI_INT             0x0604
#define MPI_UNSIGNED        0x0704
#define MPI_LONG            0x0808
#define MPI_UNSIGNED_LONG   0x0908
#define MPI_LONG_LONG       0x0a08

#define MPI_SUM   1
#define MPI_PROD  2
#define MPI_MAX   3
#define MPI_MIN   4
#define MPI_LAND  5
#define MPI_LOR   6
#define MPI_BAND  7
#define MPI_BOR   8
#define MPI_BXOR  9

int MPI_Init(int *argc, char ***argv);
int MPI_Initialized(int *flag);
int MPI_Finalize(void);
int MPI_Abort(MPI_Comm comm, int errorcode);
int MPI_Comm_rank(MPI_Comm comm, int *rank);
int MPI_Comm_size(MPI_Comm comm, int *size);
int MPI_Get_processor_name(char *name, int *resultlen);
int MPI_Type_size(MPI_Datatype type, int *size);
unsigned long MPI_Wtime_us(void);

int MPI_Send(const void *buf, int count, MPI_Datatype type, int dest, int tag,
             MPI_Comm comm);
int MPI_Recv(void *buf, int count, MPI_Datatype type, int source, int tag,
             MPI_Comm comm, MPI_Status *status);
int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int source, int recvtag, MPI_Comm comm, MPI_Status *status);
int MPI_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status);
int MPI_Get_count(const MPI_Status *status, MPI_Datatype type, int *count);

int MPI_Barrier(MPI_Comm comm);
int MPI_Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm);
int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm);
int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype type, MPI_Op op, MPI_Comm comm);
int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm);
int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm);
int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm);
int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm);

#endif
