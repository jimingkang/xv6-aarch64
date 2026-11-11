#include "feappv/model.hpp"
#include <mpi.h>
#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>

// Benchmark-only counters for the calling thread. Unknown or mixed request
// groups remain in the total without assigning their time to either class.
namespace {
enum class Kind { other, reduction, point };
thread_local fem::PetscMpiCalls calls;
thread_local std::unordered_map<MPI_Request, Kind> requests;
thread_local bool inside_call = false;

template<class Call>
int blocking(Kind kind, int operations, Call call) {
    if (inside_call) {
        return call();
    }
    inside_call = true;
    const double start = PMPI_Wtime();
    const int result = call();
    const double elapsed = PMPI_Wtime()-start;
    inside_call = false;
    calls.seconds += elapsed;
    if (kind == Kind::reduction) {
        calls.reduction_seconds += elapsed;
        calls.reductions += operations;
    } else if (kind == Kind::point) {
        calls.point_seconds += elapsed;
        calls.point_operations += operations;
    }
    return result;
}

template<class Call>
int post(Kind kind, MPI_Request* request, bool persistent, Call call) {
    const bool outer = !inside_call;
    const int result = blocking(persistent ? Kind::other : kind, 1, call);
    if (outer && result == MPI_SUCCESS) {
        if (*request != MPI_REQUEST_NULL) {
            requests[*request] = kind;
        }
        if (kind == Kind::reduction) {
            ++calls.async_reductions;
        }
    }
    return result;
}

Kind request_kind(int n, const MPI_Request* q) {
    Kind kind = Kind::other;
    for (int i = 0; i < n; ++i) {
        if (q[i] == MPI_REQUEST_NULL) {
            continue;
        }
        const auto entry = requests.find(q[i]);
        if (entry == requests.end() || (kind != Kind::other && kind != entry->second)) {
            return Kind::other;
        }
        kind = entry->second;
    }
    return kind;
}

template<class Call>
int start_requests(int n, MPI_Request* q, Call call) {
    const auto kind = request_kind(n, q);
    return blocking(kind, n, call);
}

template<class Call>
int complete(int n, MPI_Request* q, Call call) {
    if (inside_call || n == 0 || requests.empty()) {
        return blocking(Kind::other, 0, call);
    }
    const auto kind = request_kind(n, q);
    // Most completions contain a handful of requests. Keep the old handles
    // so completed nonpersistent requests can be removed after MPI nulls them.
    std::array<MPI_Request, 16> local;
    std::vector<MPI_Request> large;
    auto* old = local.data();
    if (n > static_cast<int>(local.size())) {
        large.resize(n);
        old = large.data();
    }
    std::copy_n(q, n, old);
    const int result = blocking(kind, 0, call);
    for (int i = 0; i < n; ++i) {
        if (q[i] == MPI_REQUEST_NULL && old[i] != MPI_REQUEST_NULL) {
            requests.erase(old[i]);
        }
    }
    return result;
}
}

fem::PetscMpiCalls shell_mpi_calls() { return calls; }

// macOS uses two-level namespaces, so interpose the dynamic-library imports.
#ifdef __APPLE__
#define MPI_PROFILE(name, parameters, ...)                                   \
    static int profile_##name parameters {                                   \
        return __VA_ARGS__;                                                  \
    }                                                                        \
    __attribute__((used, section("__DATA,__interpose")))                      \
    static const struct { const void* replacement; const void* original; }    \
        interpose_##name = {reinterpret_cast<const void*>(&profile_##name),   \
                            reinterpret_cast<const void*>(&MPI_##name)};
#else
#define MPI_PROFILE(name, parameters, ...)                                   \
    extern "C" int MPI_##name parameters {                                    \
        return __VA_ARGS__;                                                  \
    }
#endif

MPI_PROFILE(Allreduce, (const void* s, void* r, int n, MPI_Datatype t, MPI_Op op, MPI_Comm c),
            blocking(Kind::reduction, 1, [&] { return PMPI_Allreduce(s, r, n, t, op, c); }))
MPI_PROFILE(Iallreduce, (const void* s, void* r, int n, MPI_Datatype t, MPI_Op op, MPI_Comm c, MPI_Request* q),
            post(Kind::reduction, q, false, [&] { return PMPI_Iallreduce(s, r, n, t, op, c, q); }))
MPI_PROFILE(Reduce, (const void* s, void* r, int n, MPI_Datatype t, MPI_Op op, int root, MPI_Comm c),
            blocking(Kind::reduction, 1, [&] { return PMPI_Reduce(s, r, n, t, op, root, c); }))
MPI_PROFILE(Bcast, (void* b, int n, MPI_Datatype t, int root, MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Bcast(b, n, t, root, c); }))
MPI_PROFILE(Barrier, (MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Barrier(c); }))
MPI_PROFILE(Allgather, (const void* s, int ns, MPI_Datatype ts, void* r, int nr, MPI_Datatype tr, MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Allgather(s, ns, ts, r, nr, tr, c); }))
MPI_PROFILE(Allgatherv, (const void* s, int ns, MPI_Datatype ts, void* r, const int* nr,
                       const int* d, MPI_Datatype tr, MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Allgatherv(s, ns, ts, r, nr, d, tr, c); }))
MPI_PROFILE(Alltoall, (const void* s, int ns, MPI_Datatype ts, void* r, int nr, MPI_Datatype tr, MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Alltoall(s, ns, ts, r, nr, tr, c); }))
MPI_PROFILE(Alltoallv, (const void* s, const int* ns, const int* ds, MPI_Datatype ts,
                        void* r, const int* nr, const int* dr, MPI_Datatype tr, MPI_Comm c),
            blocking(Kind::other, 0, [&] { return PMPI_Alltoallv(s, ns, ds, ts, r, nr, dr, tr, c); }))
MPI_PROFILE(Isend, (const void* b, int n, MPI_Datatype t, int dst, int tag, MPI_Comm c, MPI_Request* q),
            post(Kind::point, q, false, [&] { return PMPI_Isend(b, n, t, dst, tag, c, q); }))
MPI_PROFILE(Irecv, (void* b, int n, MPI_Datatype t, int src, int tag, MPI_Comm c, MPI_Request* q),
            post(Kind::point, q, false, [&] { return PMPI_Irecv(b, n, t, src, tag, c, q); }))
MPI_PROFILE(Startall, (int n, MPI_Request* q),
            start_requests(n, q, [&] { return PMPI_Startall(n, q); }))
MPI_PROFILE(Wait, (MPI_Request* q, MPI_Status* s),
            complete(1, q, [&] { return PMPI_Wait(q, s); }))
MPI_PROFILE(Waitall, (int n, MPI_Request* q, MPI_Status* s),
            complete(n, q, [&] { return PMPI_Waitall(n, q, s); }))
MPI_PROFILE(Waitany, (int n, MPI_Request* q, int* i, MPI_Status* s),
            complete(n, q, [&] { return PMPI_Waitany(n, q, i, s); }))
MPI_PROFILE(Test, (MPI_Request* q, int* flag, MPI_Status* s),
            complete(1, q, [&] { return PMPI_Test(q, flag, s); }))
MPI_PROFILE(Issend, (const void* b, int n, MPI_Datatype t, int dst, int tag, MPI_Comm c, MPI_Request* q),
            post(Kind::point, q, false, [&] { return PMPI_Issend(b, n, t, dst, tag, c, q); }))
MPI_PROFILE(Ibarrier, (MPI_Comm c, MPI_Request* q),
            blocking(Kind::other, 0, [&] { return PMPI_Ibarrier(c, q); }))
MPI_PROFILE(Ineighbor_alltoallv, (const void* s, const int* ns, const int* ds, MPI_Datatype ts,
                                 void* r, const int* nr, const int* dr, MPI_Datatype tr,
                                 MPI_Comm c, MPI_Request* q),
            blocking(Kind::other, 0, [&] { return PMPI_Ineighbor_alltoallv(s, ns, ds, ts, r, nr, dr, tr, c, q); }))
MPI_PROFILE(Recv, (void* b, int n, MPI_Datatype t, int src, int tag, MPI_Comm c, MPI_Status* s),
            blocking(Kind::point, 1, [&] { return PMPI_Recv(b, n, t, src, tag, c, s); }))
MPI_PROFILE(Send, (const void* b, int n, MPI_Datatype t, int dst, int tag, MPI_Comm c),
            blocking(Kind::point, 1, [&] { return PMPI_Send(b, n, t, dst, tag, c); }))
MPI_PROFILE(Sendrecv, (const void* a, int na, MPI_Datatype ta, int dst, int stag,
                       void* b, int nb, MPI_Datatype tb, int src, int rtag, MPI_Comm c, MPI_Status* s),
            blocking(Kind::point, 1, [&] { return PMPI_Sendrecv(a, na, ta, dst, stag, b, nb, tb, src, rtag, c, s); }))
MPI_PROFILE(Start, (MPI_Request* q),
            start_requests(1, q, [&] { return PMPI_Start(q); }))
MPI_PROFILE(Waitsome, (int n, MPI_Request* q, int* count, int* i, MPI_Status* s),
            complete(n, q, [&] { return PMPI_Waitsome(n, q, count, i, s); }))
MPI_PROFILE(Testall, (int n, MPI_Request* q, int* flag, MPI_Status* s),
            complete(n, q, [&] { return PMPI_Testall(n, q, flag, s); }))

MPI_PROFILE(Send_init, (const void* b, int n, MPI_Datatype t, int dst, int tag, MPI_Comm c, MPI_Request* q),
            post(Kind::point, q, true, [&] { return PMPI_Send_init(b, n, t, dst, tag, c, q); }))
MPI_PROFILE(Recv_init, (void* b, int n, MPI_Datatype t, int src, int tag, MPI_Comm c, MPI_Request* q),
            post(Kind::point, q, true, [&] { return PMPI_Recv_init(b, n, t, src, tag, c, q); }))
MPI_PROFILE(Request_free, (MPI_Request* q), ([&] {
    const auto old = *q;
    const int result = blocking(Kind::other, 0, [&] { return PMPI_Request_free(q); });
    if (!inside_call && result == MPI_SUCCESS) {
        requests.erase(old);
    }
    return result;
})())
