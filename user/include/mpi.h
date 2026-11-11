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
//   * Only MPI_COMM_WORLD.  Point-to-point is blocking, non-blocking
//     (Isend/Irecv + Wait/Test family) or persistent (Send_init/Recv_init +
//     Start).  Collectives are blocking; Iallreduce and Ibarrier finish
//     inside the call and return a completed request.
//   * Sends are effectively buffered: every wait loop also drains incoming
//     rings into an unexpected-message queue, so the classic "both ranks
//     Send first" pattern does not hang.
//   * Communication progresses only inside MPI calls (no progress thread).
//   * xv6 programs must call exit(); MPI_Finalize on rank 0 waits for the
//     other ranks to exit before it returns.
#ifndef XV6_MPI_H
#define XV6_MPI_H

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;
typedef struct mpi_req *MPI_Request;

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
#define MPI_STATUSES_IGNORE ((MPI_Status *)0)
#define MPI_REQUEST_NULL  ((MPI_Request)0)
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
#define MPI_FLOAT           0x0b04
#define MPI_DOUBLE          0x0c08

#define MPI_SUM   1
#define MPI_PROD  2
#define MPI_MAX   3
#define MPI_MIN   4
#define MPI_LAND  5
#define MPI_LOR   6
#define MPI_BAND  7
#define MPI_BOR   8
#define MPI_BXOR  9

#ifdef __cplusplus
extern "C" {
#endif

// Every function exists as MPI_x and as PMPI_x (the MPI profiling
// interface).  libc.so defines PMPI_x and makes MPI_x an alias, and the
// library itself only calls PMPI_x, so a program may define its own MPI_x
// wrappers that call PMPI_x.
#define _MPI_FN(ret, name, params) ret MPI_##name params; ret PMPI_##name params;
_MPI_FN(int, Init, (int *argc, char ***argv))
_MPI_FN(int, Initialized, (int *flag))
_MPI_FN(int, Finalize, (void))
_MPI_FN(int, Abort, (MPI_Comm comm, int errorcode))
_MPI_FN(int, Comm_rank, (MPI_Comm comm, int *rank))
_MPI_FN(int, Comm_size, (MPI_Comm comm, int *size))
_MPI_FN(int, Get_processor_name, (char *name, int *resultlen))
_MPI_FN(int, Type_size, (MPI_Datatype type, int *size))
_MPI_FN(double, Wtime, (void))
_MPI_FN(double, Wtick, (void))
_MPI_FN(unsigned long, Wtime_us, (void))
_MPI_FN(int, Send, (const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm))
_MPI_FN(int, Recv, (void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Status *status))
_MPI_FN(int, Sendrecv, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, int recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Status *status))
_MPI_FN(int, Probe, (int source, int tag, MPI_Comm comm, MPI_Status *status))
_MPI_FN(int, Iprobe, (int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status))
_MPI_FN(int, Get_count, (const MPI_Status *status, MPI_Datatype type, int *count))
_MPI_FN(int, Isend, (const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Irecv, (void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Wait, (MPI_Request *request, MPI_Status *status))
_MPI_FN(int, Test, (MPI_Request *request, int *flag, MPI_Status *status))
_MPI_FN(int, Waitall, (int count, MPI_Request *requests, MPI_Status *statuses))
_MPI_FN(int, Testall, (int count, MPI_Request *requests, int *flag, MPI_Status *statuses))
_MPI_FN(int, Waitany, (int count, MPI_Request *requests, int *index, MPI_Status *status))
_MPI_FN(int, Barrier, (MPI_Comm comm))
_MPI_FN(int, Bcast, (void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm))
_MPI_FN(int, Reduce, (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm))
_MPI_FN(int, Allreduce, (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm))
_MPI_FN(int, Gather, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm))
_MPI_FN(int, Scatter, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm))
_MPI_FN(int, Allgather, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm))
_MPI_FN(int, Alltoall, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm))
_MPI_FN(int, Allgatherv, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *displs, MPI_Datatype recvtype, MPI_Comm comm))
_MPI_FN(int, Alltoallv, (const void *sendbuf, const int *sendcounts, const int *sdispls, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm))
_MPI_FN(int, Gatherv, (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *displs, MPI_Datatype recvtype, int root, MPI_Comm comm))
_MPI_FN(int, Iallreduce, (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Ibarrier, (MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Issend, (const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Ssend, (const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm))
_MPI_FN(int, Send_init, (const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Recv_init, (void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Request *request))
_MPI_FN(int, Start, (MPI_Request *request))
_MPI_FN(int, Startall, (int count, MPI_Request *requests))
_MPI_FN(int, Request_free, (MPI_Request *request))
_MPI_FN(int, Waitsome, (int incount, MPI_Request *requests, int *outcount, int *indices, MPI_Status *statuses))
_MPI_FN(int, Ineighbor_alltoallv, (const void *sendbuf, const int *sendcounts, const int *sdispls, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request))
#undef _MPI_FN

#ifdef __cplusplus
}
#endif

#endif
