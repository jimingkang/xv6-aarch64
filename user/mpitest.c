// mpitest: functional tests and micro-benchmarks for the xv6 MPI subset.
//   mpirun -np 4 mpitest
#include <uapi/types.h>
#include "user/user.h"
#include "user/include/mpi.h"

static int rank, size, failures;

#define CHECK(cond, ...)                                     \
  do {                                                       \
    if(!(cond)){                                             \
      printf("rank %d: FAIL %s:%d: ", rank, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
      failures++;                                            \
    }                                                        \
  } while(0)

// Every rank contributes its failure count; rank 0 reports the test.
static void
report(const char *name)
{
  int total = 0;
  MPI_Allreduce(&failures, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  if(rank == 0)
    printf("mpitest: %s %s\n", name, total ? "FAILED" : "ok");
}

static void
test_ring(void)
{
  int token;
  if(size == 1){
    report("ring");
    return;
  }
  if(rank == 0){
    token = 1000;
    MPI_Send(&token, 1, MPI_INT, 1, 7, MPI_COMM_WORLD);
    MPI_Recv(&token, 1, MPI_INT, size - 1, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    CHECK(token == 1000 + size * (size - 1) / 2, "token %d", token);
  } else {
    MPI_Recv(&token, 1, MPI_INT, rank - 1, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    token += rank;
    MPI_Send(&token, 1, MPI_INT, (rank + 1) % size, 7, MPI_COMM_WORLD);
  }
  report("ring");
}

// Both partners send a message larger than the ring before receiving.
static void
test_large_exchange(void)
{
  int n = 50000;                         // 200 KB, ~12x the ring size
  int *out = malloc(n * sizeof(int)), *in = malloc(n * sizeof(int));
  int peer = rank ^ 1;
  MPI_Status st;
  int count;

  if(peer < size){
    for(int i = 0; i < n; i++)
      out[i] = rank * 1000003 + i;
    MPI_Send(out, n, MPI_INT, peer, 3, MPI_COMM_WORLD);
    MPI_Recv(in, n, MPI_INT, peer, 3, MPI_COMM_WORLD, &st);
    MPI_Get_count(&st, MPI_INT, &count);
    CHECK(count == n && st.MPI_SOURCE == peer && st.MPI_TAG == 3,
          "count %d src %d tag %d", count, st.MPI_SOURCE, st.MPI_TAG);
    for(int i = 0; i < n; i++)
      if(in[i] != peer * 1000003 + i){
        CHECK(0, "word %d = %d", i, in[i]);
        break;
      }
  }
  free(out);
  free(in);
  report("large head-to-head exchange");
}

static void
test_matching(void)
{
  if(rank == 0){
    int seen = 0, v;
    MPI_Status st;
    // ANY_SOURCE: one message from each other rank, in any order.
    for(int i = 1; i < size; i++){
      MPI_Recv(&v, 1, MPI_INT, MPI_ANY_SOURCE, 11, MPI_COMM_WORLD, &st);
      CHECK(v == st.MPI_SOURCE * 10, "v %d from %d", v, st.MPI_SOURCE);
      seen |= 1 << st.MPI_SOURCE;
    }
    CHECK(seen == ((1 << size) - 2), "seen %x", seen);
    // Tags received out of send order.
    if(size > 1){
      int a, b;
      MPI_Recv(&a, 1, MPI_INT, 1, 22, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&b, 1, MPI_INT, 1, 21, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      CHECK(a == 222 && b == 211, "a %d b %d", a, b);
      // Probe tells the size before the receive.
      MPI_Probe(1, 30, MPI_COMM_WORLD, &st);
      int cnt;
      MPI_Get_count(&st, MPI_CHAR, &cnt);
      CHECK(cnt == 6, "probe count %d", cnt);
      char s[16];
      MPI_Recv(s, sizeof(s), MPI_CHAR, 1, 30, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      CHECK(strcmp(s, "hello") == 0, "probe msg %s", s);
    }
  } else {
    int v = rank * 10;
    MPI_Send(&v, 1, MPI_INT, 0, 11, MPI_COMM_WORLD);
    if(rank == 1){
      int a = 211, b = 222;
      MPI_Send(&a, 1, MPI_INT, 0, 21, MPI_COMM_WORLD);
      MPI_Send(&b, 1, MPI_INT, 0, 22, MPI_COMM_WORLD);
      MPI_Send("hello", 6, MPI_CHAR, 0, 30, MPI_COMM_WORLD);
    }
  }
  // Self-send through the unexpected queue.
  int x = rank + 5, y = 0;
  MPI_Sendrecv(&x, 1, MPI_INT, rank, 40, &y, 1, MPI_INT, rank, 40,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  CHECK(y == rank + 5, "self y %d", y);
  report("tag/source matching, probe, self-send");
}

static void
test_collectives(void)
{
  int v[4], r[4], i;
  int *all = malloc(size * 4 * sizeof(int));

  for(int root = 0; root < size; root++){
    for(i = 0; i < 4; i++)
      v[i] = rank == root ? 100 * root + i : -1;
    MPI_Bcast(v, 4, MPI_INT, root, MPI_COMM_WORLD);
    for(i = 0; i < 4; i++)
      CHECK(v[i] == 100 * root + i, "bcast root %d v[%d]=%d", root, i, v[i]);
  }

  for(i = 0; i < 4; i++)
    v[i] = rank + i;
  MPI_Reduce(v, r, 4, MPI_INT, MPI_SUM, size - 1, MPI_COMM_WORLD);
  if(rank == size - 1)
    for(i = 0; i < 4; i++)
      CHECK(r[i] == size * (size - 1) / 2 + size * i, "reduce r[%d]=%d", i, r[i]);

  long lv = rank * 7, lmax = 0, lmin = 0;
  MPI_Allreduce(&lv, &lmax, 1, MPI_LONG, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&lv, &lmin, 1, MPI_LONG, MPI_MIN, MPI_COMM_WORLD);
  CHECK(lmax == (size - 1) * 7 && lmin == 0, "max %d min %d", (int)lmax, (int)lmin);

  int ip = rank + 1;
  MPI_Allreduce(MPI_IN_PLACE, &ip, 1, MPI_INT, MPI_PROD, MPI_COMM_WORLD);
  int fact = 1;
  for(i = 2; i <= size; i++)
    fact *= i;
  CHECK(ip == fact, "prod %d", ip);

  for(i = 0; i < 4; i++)
    v[i] = rank * 4 + i;
  MPI_Gather(v, 4, MPI_INT, all, 4, MPI_INT, 0, MPI_COMM_WORLD);
  if(rank == 0)
    for(i = 0; i < size * 4; i++)
      CHECK(all[i] == i, "gather all[%d]=%d", i, all[i]);

  if(rank == 1 % size)
    for(i = 0; i < size * 4; i++)
      all[i] = 1000 + i;
  MPI_Scatter(all, 4, MPI_INT, r, 4, MPI_INT, 1 % size, MPI_COMM_WORLD);
  for(i = 0; i < 4; i++)
    CHECK(r[i] == 1000 + rank * 4 + i, "scatter r[%d]=%d", i, r[i]);

  memset(all, 0, size * 4 * sizeof(int));
  MPI_Allgather(v, 4, MPI_INT, all, 4, MPI_INT, MPI_COMM_WORLD);
  for(i = 0; i < size * 4; i++)
    CHECK(all[i] == i, "allgather all[%d]=%d", i, all[i]);

  int *s2 = malloc(size * sizeof(int)), *r2 = malloc(size * sizeof(int));
  for(i = 0; i < size; i++)
    s2[i] = rank * 100 + i;
  MPI_Alltoall(s2, 1, MPI_INT, r2, 1, MPI_INT, MPI_COMM_WORLD);
  for(i = 0; i < size; i++)
    CHECK(r2[i] == i * 100 + rank, "alltoall r2[%d]=%d", i, r2[i]);
  free(s2);
  free(r2);

  for(i = 0; i < 100; i++)
    MPI_Barrier(MPI_COMM_WORLD);
  free(all);
  report("collectives");
}

static void
test_nonblocking(void)
{
  int left = (rank - 1 + size) % size, right = (rank + 1) % size;
  MPI_Request rq[8];
  MPI_Status st[8];
  int i, flag;

  // Ring shift with both requests outstanding, large enough to stream.
  int n = 40000;
  int *out = malloc(n * sizeof(int)), *in = malloc(n * sizeof(int));
  for(i = 0; i < n; i++)
    out[i] = rank * 7919 + i;
  MPI_Irecv(in, n, MPI_INT, left, 50, MPI_COMM_WORLD, &rq[0]);
  MPI_Isend(out, n, MPI_INT, right, 50, MPI_COMM_WORLD, &rq[1]);
  MPI_Waitall(2, rq, st);
  CHECK(rq[0] == MPI_REQUEST_NULL && rq[1] == MPI_REQUEST_NULL, "not nulled");
  CHECK(st[0].MPI_SOURCE == left && st[0].MPI_TAG == 50, "st src %d tag %d",
        st[0].MPI_SOURCE, st[0].MPI_TAG);
  for(i = 0; i < n; i++)
    if(in[i] != left * 7919 + i){
      CHECK(0, "shift word %d = %d", i, in[i]);
      break;
    }
  free(out);
  free(in);

  // Posted receives match in post order, whatever order messages arrive in.
  if(size > 1){
    if(rank == 0){
      int a = 0, b = 0, c = 0, d = 0;
      MPI_Irecv(&a, 1, MPI_INT, 1, 61, MPI_COMM_WORLD, &rq[0]);
      MPI_Irecv(&b, 1, MPI_INT, 1, 62, MPI_COMM_WORLD, &rq[1]);
      MPI_Irecv(&c, 1, MPI_INT, 1, MPI_ANY_TAG, MPI_COMM_WORLD, &rq[2]);
      MPI_Irecv(&d, 1, MPI_INT, MPI_ANY_SOURCE, 63, MPI_COMM_WORLD, &rq[3]);
      int go = 1;
      MPI_Send(&go, 1, MPI_INT, 1, 60, MPI_COMM_WORLD);   // receives are up
      do {
        MPI_Testall(4, rq, &flag, st);
      } while(!flag);
      // rank 1 sends tags 62, 63, 61, 63.  The first 63 goes to c (the
      // earlier-posted ANY_TAG receive), not d; the second 63 goes to d.
      CHECK(a == 610 && b == 620 && c == 630 && d == 631,
            "a %d b %d c %d d %d", a, b, c, d);
    } else if(rank == 1){
      int go, v;
      MPI_Recv(&go, 1, MPI_INT, 0, 60, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      v = 620; MPI_Send(&v, 1, MPI_INT, 0, 62, MPI_COMM_WORLD);
      v = 630; MPI_Send(&v, 1, MPI_INT, 0, 63, MPI_COMM_WORLD);
      v = 610; MPI_Send(&v, 1, MPI_INT, 0, 61, MPI_COMM_WORLD);
      v = 631; MPI_Send(&v, 1, MPI_INT, 0, 63, MPI_COMM_WORLD);
    }
  }

  // Waitany collects one message from every other rank.
  if(rank == 0){
    int vals[MPI_MAX_RANKS], idx, seen = 0;
    for(i = 1; i < size; i++)
      MPI_Irecv(&vals[i], 1, MPI_INT, i, 70, MPI_COMM_WORLD, &rq[i - 1]);
    for(i = 1; i < size; i++){
      MPI_Waitany(size - 1, rq, &idx, &st[0]);
      CHECK(idx >= 0 && st[0].MPI_SOURCE == idx + 1 &&
            vals[idx + 1] == 700 + idx + 1, "waitany idx %d", idx);
      seen |= 1 << (idx + 1);
    }
    CHECK(seen == (1 << size) - 2, "waitany seen %x", seen);
    MPI_Waitany(size - 1, rq, &idx, MPI_STATUS_IGNORE);
    CHECK(idx == MPI_UNDEFINED, "waitany on all-null gave %d", idx);
  } else {
    int v = 700 + rank;
    MPI_Isend(&v, 1, MPI_INT, 0, 70, MPI_COMM_WORLD, &rq[0]);
    MPI_Wait(&rq[0], MPI_STATUS_IGNORE);
  }

  // MPI_Test polling, self-send, and a receive matching an unexpected
  // message that is still arriving.
  int x = rank * 3, y = -1;
  MPI_Irecv(&y, 1, MPI_INT, rank, 80, MPI_COMM_WORLD, &rq[0]);
  MPI_Isend(&x, 1, MPI_INT, rank, 80, MPI_COMM_WORLD, &rq[1]);
  do {
    MPI_Test(&rq[0], &flag, MPI_STATUS_IGNORE);
  } while(!flag);
  MPI_Wait(&rq[1], MPI_STATUS_IGNORE);
  CHECK(y == rank * 3, "self irecv y %d", y);

  if(size > 1){
    int big = 30000;
    int *buf = malloc(big * sizeof(int));
    if(rank == 1){
      for(i = 0; i < big; i++)
        buf[i] = i ^ 0x5a5a;
      MPI_Send(buf, big, MPI_INT, 0, 90, MPI_COMM_WORLD);
    } else if(rank == 0){
      int probed = 0;
      MPI_Status ps;
      while(!probed)                 // header in, payload maybe not yet
        MPI_Iprobe(1, 90, MPI_COMM_WORLD, &probed, &ps);
      memset(buf, 0, big * sizeof(int));
      MPI_Irecv(buf, big, MPI_INT, 1, 90, MPI_COMM_WORLD, &rq[0]);
      MPI_Wait(&rq[0], &st[0]);
      int cnt;
      MPI_Get_count(&st[0], MPI_INT, &cnt);
      CHECK(cnt == big, "count %d", cnt);
      for(i = 0; i < big; i++)
        if(buf[i] != (i ^ 0x5a5a)){
          CHECK(0, "um word %d = %d", i, buf[i]);
          break;
        }
    }
    free(buf);
  }
  report("non-blocking Isend/Irecv/Wait/Test/Waitany");
}

static long
dabs_scaled(double x)
{
  if(x < 0)
    x = -x;
  return (long)(x * 1e12);
}

static void
test_double(void)
{
  const int n = 1000000;
  double h = 1.0 / n, part = 0, pi = 0;
  double t0 = MPI_Wtime();

  for(int i = rank; i < n; i += size){
    double x = h * (i + 0.5);
    part += 4.0 / (1.0 + x * x);
  }
  part *= h;
  MPI_Allreduce(&part, &pi, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  CHECK(dabs_scaled(pi - 3.14159265358979323846) < 1000,
        "pi err %d e-12", (int)dabs_scaled(pi - 3.14159265358979323846));

  double v[3] = { rank + 0.25, -rank * 1.5, 1.0 + rank }, mx[3], mn[3];
  MPI_Allreduce(v, mx, 3, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(v, mn, 3, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
  CHECK(mx[0] == size - 1 + 0.25 && mx[1] == 0 && mn[1] == -(size - 1) * 1.5,
        "double max/min");
  float f = 0.5f * (rank + 1), fs = 0;
  MPI_Reduce(&f, &fs, 1, MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD);
  if(rank == 0)
    CHECK(fs == 0.25f * size * (size + 1), "float sum");
  double prod = 0;
  double two = 2.0;
  MPI_Allreduce(&two, &prod, 1, MPI_DOUBLE, MPI_PROD, MPI_COMM_WORLD);
  CHECK(prod == (double)(1 << size), "double prod");
  if(rank == 0){
    long frac = (long)((pi - 3) * 1e12);
    printf("mpitest: double pi ~= 3.%d%d (%d ranks, %d us)\n",
           (int)(frac / 1000000), (int)(frac % 1000000), size,
           (int)((MPI_Wtime() - t0) * 1e6));
  }
  report("MPI_DOUBLE/MPI_FLOAT reductions, MPI_Wtime");
}

// pi = integral_0^1 4/(1+x^2), midpoint rule in fixed point (no FP here):
// pi * 1e12 ~= sum_i 16e12 * n / (4n^2 + (2i+1)^2)
static void
test_pi(void)
{
  const unsigned long n = 100000, scale = 16000000000000UL;
  unsigned long part = 0, pi = 0;
  unsigned long t0 = MPI_Wtime_us();

  for(unsigned long i = rank; i < n; i += size)
    part += scale * n / (4 * n * n + (2 * i + 1) * (2 * i + 1));
  MPI_Reduce(&part, &pi, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
  if(rank == 0){
    unsigned long frac = pi % 1000000000000UL;
    char digits[13];
    for(int d = 11; d >= 0; d--){
      digits[d] = '0' + frac % 10;
      frac /= 10;
    }
    digits[12] = 0;
    printf("mpitest: pi ~= %d.%s (%d ranks, %d us)\n",
           (int)(pi / 1000000000000UL), digits, size,
           (int)(MPI_Wtime_us() - t0));
    CHECK(pi / 1000000 == 3141592, "pi %d", (int)(pi / 1000000));
  }
  report("integer pi reduce");
}

static void
bench(void)
{
  enum { PINGS = 2000, BIG = 65536, REPS = 50 };
  char small[8];
  char *big;
  unsigned long t0, t1;

  if(size < 2)
    return;
  big = malloc(BIG);
  memset(big, rank, BIG);
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank < 2){
    int peer = 1 - rank;
    t0 = MPI_Wtime_us();
    for(int i = 0; i < PINGS; i++){
      if(rank == 0){
        MPI_Send(small, 8, MPI_BYTE, peer, 1, MPI_COMM_WORLD);
        MPI_Recv(small, 8, MPI_BYTE, peer, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      } else {
        MPI_Recv(small, 8, MPI_BYTE, peer, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(small, 8, MPI_BYTE, peer, 1, MPI_COMM_WORLD);
      }
    }
    t1 = MPI_Wtime_us();
    if(rank == 0)
      printf("mpitest: 8B ping-pong one-way latency %d.%d us\n",
             (int)((t1 - t0) * 10 / (2 * PINGS)) / 10,
             (int)((t1 - t0) * 10 / (2 * PINGS)) % 10);

    t0 = MPI_Wtime_us();
    for(int i = 0; i < REPS; i++){
      if(rank == 0)
        MPI_Send(big, BIG, MPI_BYTE, peer, 2, MPI_COMM_WORLD);
      else
        MPI_Recv(big, BIG, MPI_BYTE, peer, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Sendrecv(small, 1, MPI_BYTE, peer, 3, small, 1, MPI_BYTE, peer, 3,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    t1 = MPI_Wtime_us();
    if(rank == 0 && t1 > t0)
      printf("mpitest: 64KB stream bandwidth %d KB/s\n",
             (int)((unsigned long)BIG * REPS / 1024 * 1000000 / (t1 - t0)));
  }
  MPI_Barrier(MPI_COMM_WORLD);
  free(big);
}

int
main(int argc, char *argv[])
{
  int total = 0;

  if(MPI_Init(&argc, &argv) != MPI_SUCCESS)
    exit(1);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  printf("mpitest: rank %d of %d, pid %d, cpu %d\n", rank, size, getpid(),
         getcpu());

  test_ring();
  test_large_exchange();
  test_matching();
  test_collectives();
  test_pi();
  test_nonblocking();
  test_double();
  bench();

  MPI_Allreduce(&failures, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  MPI_Finalize();
  if(rank == 0)
    printf("mpitest: %s\n", total ? "SOME TESTS FAILED" : "all tests passed");
  exit(total ? 1 : 0);
}
