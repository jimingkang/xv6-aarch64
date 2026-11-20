// mpi_host_alias.c -- generated from user/include/mpi.h for host builds of
// user/mpi.c.  Mach-O has no symbol aliases, so each MPI_x is a weak
// function that calls PMPI_x; a program that defines MPI_x (a profiling
// wrapper) overrides it.
#include "include/mpi.h"

__attribute__((weak)) int MPI_Init(int *argc, char ***argv) { return PMPI_Init(argc, argv); }
__attribute__((weak)) int MPI_Initialized(int *flag) { return PMPI_Initialized(flag); }
__attribute__((weak)) int MPI_Finalize(void) { return PMPI_Finalize(); }
__attribute__((weak)) int MPI_Abort(MPI_Comm comm, int errorcode) { return PMPI_Abort(comm, errorcode); }
__attribute__((weak)) int MPI_Comm_rank(MPI_Comm comm, int *rank) { return PMPI_Comm_rank(comm, rank); }
__attribute__((weak)) int MPI_Comm_size(MPI_Comm comm, int *size) { return PMPI_Comm_size(comm, size); }
__attribute__((weak)) int MPI_Get_processor_name(char *name, int *resultlen) { return PMPI_Get_processor_name(name, resultlen); }
__attribute__((weak)) int MPI_Type_size(MPI_Datatype type, int *size) { return PMPI_Type_size(type, size); }
__attribute__((weak)) double MPI_Wtime(void) { return PMPI_Wtime(); }
__attribute__((weak)) double MPI_Wtick(void) { return PMPI_Wtick(); }
__attribute__((weak)) unsigned long MPI_Wtime_us(void) { return PMPI_Wtime_us(); }
__attribute__((weak)) int MPI_Send(const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm) { return PMPI_Send(buf, count, type, dest, tag, comm); }
__attribute__((weak)) int MPI_Recv(void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Status *status) { return PMPI_Recv(buf, count, type, source, tag, comm, status); }
__attribute__((weak)) int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, int recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Status *status) { return PMPI_Sendrecv(sendbuf, sendcount, sendtype, dest, sendtag, recvbuf, recvcount, recvtype, source, recvtag, comm, status); }
__attribute__((weak)) int MPI_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status) { return PMPI_Probe(source, tag, comm, status); }
__attribute__((weak)) int MPI_Iprobe(int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status) { return PMPI_Iprobe(source, tag, comm, flag, status); }
__attribute__((weak)) int MPI_Get_count(const MPI_Status *status, MPI_Datatype type, int *count) { return PMPI_Get_count(status, type, count); }
__attribute__((weak)) int MPI_Isend(const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request) { return PMPI_Isend(buf, count, type, dest, tag, comm, request); }
__attribute__((weak)) int MPI_Irecv(void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Request *request) { return PMPI_Irecv(buf, count, type, source, tag, comm, request); }
__attribute__((weak)) int MPI_Wait(MPI_Request *request, MPI_Status *status) { return PMPI_Wait(request, status); }
__attribute__((weak)) int MPI_Test(MPI_Request *request, int *flag, MPI_Status *status) { return PMPI_Test(request, flag, status); }
__attribute__((weak)) int MPI_Waitall(int count, MPI_Request *requests, MPI_Status *statuses) { return PMPI_Waitall(count, requests, statuses); }
__attribute__((weak)) int MPI_Testall(int count, MPI_Request *requests, int *flag, MPI_Status *statuses) { return PMPI_Testall(count, requests, flag, statuses); }
__attribute__((weak)) int MPI_Waitany(int count, MPI_Request *requests, int *index, MPI_Status *status) { return PMPI_Waitany(count, requests, index, status); }
__attribute__((weak)) int MPI_Barrier(MPI_Comm comm) { return PMPI_Barrier(comm); }
__attribute__((weak)) int MPI_Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm) { return PMPI_Bcast(buf, count, type, root, comm); }
__attribute__((weak)) int MPI_Reduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm) { return PMPI_Reduce(sendbuf, recvbuf, count, type, op, root, comm); }
__attribute__((weak)) int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm) { return PMPI_Allreduce(sendbuf, recvbuf, count, type, op, comm); }
__attribute__((weak)) int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm) { return PMPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm); }
__attribute__((weak)) int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm) { return PMPI_Scatter(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm); }
__attribute__((weak)) int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm) { return PMPI_Allgather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm); }
__attribute__((weak)) int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm) { return PMPI_Alltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm); }
__attribute__((weak)) int MPI_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *displs, MPI_Datatype recvtype, MPI_Comm comm) { return PMPI_Allgatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, comm); }
__attribute__((weak)) int MPI_Alltoallv(const void *sendbuf, const int *sendcounts, const int *sdispls, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm) { return PMPI_Alltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm); }
__attribute__((weak)) int MPI_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *displs, MPI_Datatype recvtype, int root, MPI_Comm comm) { return PMPI_Gatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, root, comm); }
__attribute__((weak)) int MPI_Iallreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm, MPI_Request *request) { return PMPI_Iallreduce(sendbuf, recvbuf, count, type, op, comm, request); }
__attribute__((weak)) int MPI_Ibarrier(MPI_Comm comm, MPI_Request *request) { return PMPI_Ibarrier(comm, request); }
__attribute__((weak)) int MPI_Issend(const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request) { return PMPI_Issend(buf, count, type, dest, tag, comm, request); }
__attribute__((weak)) int MPI_Ssend(const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm) { return PMPI_Ssend(buf, count, type, dest, tag, comm); }
__attribute__((weak)) int MPI_Send_init(const void *buf, int count, MPI_Datatype type, int dest, int tag, MPI_Comm comm, MPI_Request *request) { return PMPI_Send_init(buf, count, type, dest, tag, comm, request); }
__attribute__((weak)) int MPI_Recv_init(void *buf, int count, MPI_Datatype type, int source, int tag, MPI_Comm comm, MPI_Request *request) { return PMPI_Recv_init(buf, count, type, source, tag, comm, request); }
__attribute__((weak)) int MPI_Start(MPI_Request *request) { return PMPI_Start(request); }
__attribute__((weak)) int MPI_Startall(int count, MPI_Request *requests) { return PMPI_Startall(count, requests); }
__attribute__((weak)) int MPI_Request_free(MPI_Request *request) { return PMPI_Request_free(request); }
__attribute__((weak)) int MPI_Waitsome(int incount, MPI_Request *requests, int *outcount, int *indices, MPI_Status *statuses) { return PMPI_Waitsome(incount, requests, outcount, indices, statuses); }
__attribute__((weak)) int MPI_Ineighbor_alltoallv(const void *sendbuf, const int *sendcounts, const int *sdispls, MPI_Datatype sendtype, void *recvbuf, const int *recvcounts, const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request) { return PMPI_Ineighbor_alltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm, request); }
