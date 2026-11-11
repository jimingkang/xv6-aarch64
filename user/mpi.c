// mpi.c -- single-node MPI subset over shared memory (see user/include/mpi.h).
//
// Layout of the region MPI_Init maps with MAP_SHARED|MAP_ANONYMOUS before it
// forks the ranks (fork maps shared pages again instead of COW-copying them):
//
//   page 0      struct shared: rank count, pids, per-rank doorbell handles
//               and "sleeping" flags (each on its own cache line)
//   page 1..    struct ring[n][n]; ring[s][d] carries bytes from s to d
//
// A ring is a free-running SPSC byte queue.  The producer owns head, the
// consumer owns tail; each publishes its index with a release store and
// reads the other's with an acquire load.  A message is a 16-byte header
// followed by its payload, streamed through the ring in as many pieces as
// space allows, so messages larger than the ring work.
//
// Each rank keeps, per source, the message it is currently draining.  An
// arriving header is matched against the posted receives in post order; if
// none matches, the payload is copied into a malloc'd buffer on the
// unexpected queue.  Sends wait on a per-destination FIFO and are streamed
// from the caller's buffer as ring space appears.  Every wait loop drives
// both directions, so a blocked sender never stops its own receiving side,
// and two ranks that send to each other first cannot hang.  Blocking calls
// are a request on the stack plus a wait; MPI_Isend/MPI_Irecv return the
// same request malloc'd.
//
// Waiting: spin on has_work(); after SPIN_LIMIT idle rounds set my sleeping
// flag (seq_cst store), re-check, and sleep on my doorbell semaphore.  A
// peer that publishes into my ring (or frees space in a ring I fill) does a
// seq_cst load of that flag and signals the doorbell if set.  stlr followed
// by ldar is ordered on ARMv8, so one side always sees the other: either the
// sleeper's re-check sees the data or the producer sees the flag.  Extra
// semaphore counts only cause a spurious wake-up.

#include <uapi/types.h>
#include <uapi/mman.h>
#include <uapi/limits.h>
#include "user/user.h"
#include "user/sync.h"
#include "user/include/mpi.h"

#define RING_SIZE    16384U             // power of two
#define RING_MASK    (RING_SIZE - 1)
#define HDR_SIZE     16
#define SPIN_LIMIT   2000
#define PAGE         4096

// Internal tags for collectives; MPI_ANY_TAG never matches a negative tag.
#define TAG_BARRIER  (-101)
#define TAG_BCAST    (-102)
#define TAG_REDUCE   (-103)
#define TAG_GATHER   (-104)
#define TAG_SCATTER  (-105)
#define TAG_ALLTOALL (-106)
#define TAG_ALLGATHERV (-107)
#define TAG_ALLTOALLV  (-108)
#define TAG_GATHERV    (-109)

struct ring {
  uint head;                  // written by the producer only
  char pad0[60];
  uint tail;                  // written by the consumer only
  char pad1[60];
  char buf[RING_SIZE];
};

struct shared {
  int nranks;
  int aborted;
  int pids[MPI_MAX_RANKS];
  int bell[MPI_MAX_RANKS];
  struct {
    int sleeping;
    char pad[60];
  } sl[MPI_MAX_RANKS];
};

struct msghdr {
  int tag;
  int len;
  int comm;
  int seq;
};

// A message being drained from a ring or parked on the unexpected queue.
struct inmsg {
  struct inmsg *next;
  int src, tag, len, got;
  char *buf;                  // user buffer (bound) or malloc'd copy
  int cap;                    // bytes of buf that may be written
  int done;
  struct mpi_req *bound;      // the posted receive it is delivered into
};

#define REQ_SEND 1
#define REQ_RECV 2

// One send or receive.  Blocking calls use one on the stack; MPI_Isend and
// MPI_Irecv malloc it and MPI_Wait/MPI_Test free it.
struct mpi_req {
  int kind;
  int done;
  int heap;                   // malloc'd by Isend/Irecv
  struct mpi_persist *pers;   // set for Send_init/Recv_init requests
  MPI_Status st;
  struct mpi_req *next;       // send queue of its destination, or posted list
  // send: header + payload streamed from the caller's buffer
  int dst;
  struct msghdr h;
  const char *sbuf;
  int off, total;
  // receive
  int src, tag;
  char *buf;
  int cap;
  struct inmsg *um;           // matched unexpected message still arriving
};

// Arguments of a persistent request, replayed by MPI_Start.  The request
// is inactive (done, nothing queued) between a completion and the next Start.
struct mpi_persist {
  int kind;
  void *buf;
  int len, peer, tag;
  int active;
};

static struct shared *shm;
static uint shm_size;
static struct ring *rings;
static int me = 0, nranks = 1, initialized, finalized;
static int seqno;
static struct inmsg *incur[MPI_MAX_RANKS];   // per-source message in flight
static struct inmsg *uq_head, *uq_tail;      // unexpected queue, arrival order
static struct mpi_req *posted_head, *posted_tail;  // receives, post order
static struct mpi_req *sq_head[MPI_MAX_RANKS], *sq_tail[MPI_MAX_RANKS];

// ---------------------------------------------------------------------------
// helpers

static void
fastcopy(void *dst, const void *src, uint n)
{
  char *d = dst;
  const char *s = src;
  if((((uint64)d | (uint64)s) & 7) == 0){
    while(n >= 8){
      *(uint64 *)d = *(const uint64 *)s;
      d += 8; s += 8; n -= 8;
    }
  }
  while(n-- > 0)
    *d++ = *s++;
}

static int
mpi_err(const char *fn, int code)
{
  fprintf(2, "mpi: rank %d: %s failed (error %d)\n", me, fn, code);
  return code;
}

static int
typesize(MPI_Datatype t)
{
  int id = (t >> 8) & 0xff, sz = t & 0xff;
  if(id < 1 || id > 0x0c || (sz != 1 && sz != 2 && sz != 4 && sz != 8) ||
     (t & ~0xffff))
    return -1;
  return sz;
}

static struct ring *
ring_of(int src, int dst)
{
  return &rings[src * nranks + dst];
}

static void
wake(int r)
{
  if(__atomic_load_n(&shm->sl[r].sleeping, __ATOMIC_SEQ_CST))
    sync_signal(shm->bell[r], 1);
}

static uint
ring_used(struct ring *r)
{
  return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - r->tail;
}

static uint
ring_space(struct ring *r)
{
  return RING_SIZE - (r->head - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE));
}

// Producer side: copy up to n bytes in, return how many fit.
static uint
ring_put(struct ring *r, const char *p, uint n)
{
  uint head = r->head, space = ring_space(r);
  if(n > space)
    n = space;
  if(n == 0)
    return 0;
  uint off = head & RING_MASK, first = RING_SIZE - off;
  if(first > n)
    first = n;
  fastcopy(r->buf + off, p, first);
  if(n > first)
    fastcopy(r->buf, p + first, n - first);
  __atomic_store_n(&r->head, head + n, __ATOMIC_SEQ_CST);
  return n;
}

// Consumer side: take up to n bytes out (into p, or dropped if p is 0).
static uint
ring_get(struct ring *r, char *p, uint n)
{
  uint tail = r->tail, used = ring_used(r);
  if(n > used)
    n = used;
  if(n == 0)
    return 0;
  if(p){
    uint off = tail & RING_MASK, first = RING_SIZE - off;
    if(first > n)
      first = n;
    fastcopy(p, r->buf + off, first);
    if(n > first)
      fastcopy(p + first, r->buf, n - first);
  }
  __atomic_store_n(&r->tail, tail + n, __ATOMIC_SEQ_CST);
  return n;
}

static int
tag_match(int want, int tag)
{
  return want == MPI_ANY_TAG ? tag >= 0 : want == tag;
}

static int
req_match(struct mpi_req *q, int src, int tag)
{
  return (q->src == MPI_ANY_SOURCE || q->src == src) && tag_match(q->tag, tag);
}

// First posted receive (in post order) that takes this message, unlinked.
static struct mpi_req *
posted_take(int src, int tag)
{
  struct mpi_req **pp = &posted_head, *prev = 0;
  for(; *pp; prev = *pp, pp = &(*pp)->next){
    struct mpi_req *q = *pp;
    if(req_match(q, src, tag)){
      *pp = q->next;
      if(posted_tail == q)
        posted_tail = prev;
      q->next = 0;
      return q;
    }
  }
  return 0;
}

static void
uq_append(struct inmsg *m)
{
  m->next = 0;
  if(uq_tail)
    uq_tail->next = m;
  else
    uq_head = m;
  uq_tail = m;
}

static void
uq_remove(struct inmsg *m)
{
  struct inmsg **pp = &uq_head, *prev = 0;
  while(*pp && *pp != m){
    prev = *pp;
    pp = &(*pp)->next;
  }
  if(*pp == 0)
    return;
  *pp = m->next;
  if(uq_tail == m)
    uq_tail = prev;
  m->next = 0;
}

static struct inmsg *
uq_find(int src, int tag)
{
  for(struct inmsg *m = uq_head; m; m = m->next)
    if((src == MPI_ANY_SOURCE || src == m->src) && tag_match(tag, m->tag))
      return m;
  return 0;
}

static void
fill_status(MPI_Status *st, int src, int tag, int bytes, int err)
{
  if(st == MPI_STATUS_IGNORE)
    return;
  st->MPI_SOURCE = src;
  st->MPI_TAG = tag;
  st->MPI_ERROR = err;
  st->_bytes = bytes;
}

// ---------------------------------------------------------------------------
// progress engine

static void
check_abort(void)
{
  if(__atomic_load_n(&shm->aborted, __ATOMIC_ACQUIRE))
    exit(1);
}

// Drain whatever has arrived from src.  Returns 1 if anything moved.
static int
progress_src(int s)
{
  struct ring *r = ring_of(s, me);
  int moved = 0;

  for(;;){
    struct inmsg *m = incur[s];
    if(m == 0){
      struct msghdr h;
      if(ring_used(r) < HDR_SIZE)
        break;
      ring_get(r, (char *)&h, HDR_SIZE);
      moved = 1;
      m = malloc(sizeof(*m));
      if(m == 0){
        fprintf(2, "mpi: rank %d: out of memory\n", me);
        PMPI_Abort(MPI_COMM_WORLD, 1);
      }
      m->next = 0;
      m->src = s;
      m->tag = h.tag;
      m->len = h.len;
      m->got = 0;
      m->done = 0;
      m->bound = 0;
      struct mpi_req *q = posted_take(s, h.tag);
      if(q){
        m->bound = q;
        m->buf = q->buf;
        m->cap = q->cap;
      } else {
        m->buf = malloc(h.len > 0 ? h.len : 1);
        if(m->buf == 0){
          fprintf(2, "mpi: rank %d: out of memory\n", me);
          PMPI_Abort(MPI_COMM_WORLD, 1);
        }
        m->cap = h.len;
        uq_append(m);
      }
      incur[s] = m;
    }

    while(m->got < m->len){
      uint n;
      if(m->got < m->cap){
        uint want = m->cap - m->got;
        if(want > (uint)(m->len - m->got))
          want = m->len - m->got;
        n = ring_get(r, m->buf + m->got, want);
      } else {
        n = ring_get(r, 0, m->len - m->got);  // truncated tail: discard
      }
      if(n == 0)
        break;
      m->got += n;
      moved = 1;
    }
    if(moved)
      wake(s);                        // s may be waiting for ring space
    if(m->got < m->len)
      break;

    m->done = 1;
    incur[s] = 0;
    if(m->bound){
      struct mpi_req *q = m->bound;
      int bytes = m->len <= q->cap ? m->len : q->cap;
      fill_status(&q->st, m->src, m->tag, bytes,
                  m->len > q->cap ? MPI_ERR_TRUNCATE : MPI_SUCCESS);
      q->done = 1;
      free(m);
    }
  }
  return moved;
}

// Push queued sends to dst, oldest first (keeps per-pair order).
static int
progress_dst(int d)
{
  struct ring *r = ring_of(me, d);
  int moved = 0;
  struct mpi_req *q;

  while((q = sq_head[d]) != 0){
    while(q->off < q->total){
      uint n;
      if(q->off < HDR_SIZE)
        n = ring_put(r, (const char *)&q->h + q->off, HDR_SIZE - q->off);
      else
        n = ring_put(r, q->sbuf + (q->off - HDR_SIZE), q->total - q->off);
      if(n == 0)
        break;
      q->off += n;
      moved = 1;
    }
    if(q->off < q->total)
      break;
    sq_head[d] = q->next;
    if(sq_head[d] == 0)
      sq_tail[d] = 0;
    q->next = 0;
    fill_status(&q->st, d, q->h.tag, q->h.len, MPI_SUCCESS);
    q->done = 1;
  }
  if(moved)
    wake(d);
  return moved;
}

static int
progress(void)
{
  int moved = 0;
  for(int s = 0; s < nranks; s++){
    if(s == me)
      continue;
    if(sq_head[s])
      moved |= progress_dst(s);
    moved |= progress_src(s);
  }
  return moved;
}

static int
has_work(void)
{
  for(int s = 0; s < nranks; s++){
    if(s == me)
      continue;
    if(ring_used(ring_of(s, me)) >= (incur[s] ? 1 : HDR_SIZE))
      return 1;
    if(sq_head[s] && ring_space(ring_of(me, s)) > 0)
      return 1;
  }
  return __atomic_load_n(&shm->aborted, __ATOMIC_ACQUIRE);
}

// Called after a round of the caller's loop made no progress.
static void
idle(int *spins)
{
  check_abort();
  if(++*spins < SPIN_LIMIT){
    if(nranks > NCPU)
      sched_yield();
    return;
  }
  *spins = 0;
  __atomic_store_n(&shm->sl[me].sleeping, 1, __ATOMIC_SEQ_CST);
  if(!has_work())
    sync_wait(shm->bell[me]);
  __atomic_store_n(&shm->sl[me].sleeping, 0, __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------------------
// point-to-point on bytes: start a request, then complete it

static void
req_init(struct mpi_req *q, int kind)
{
  memset(q, 0, sizeof(*q));
  q->kind = kind;
}

static int
isend_start(struct mpi_req *q, const void *buf, int len, int dst, int tag)
{
  req_init(q, REQ_SEND);
  q->dst = dst;
  if(dst == MPI_PROC_NULL){
    fill_status(&q->st, MPI_PROC_NULL, MPI_ANY_TAG, 0, MPI_SUCCESS);
    q->done = 1;
    return MPI_SUCCESS;
  }
  if(dst == me){
    // Self-send: park a ready message on the unexpected queue, or hand it
    // straight to a matching posted receive.
    struct inmsg *m = malloc(sizeof(*m));
    if(m == 0 || (m->buf = malloc(len > 0 ? len : 1)) == 0){
      free(m);
      return MPI_ERR_OTHER;
    }
    fastcopy(m->buf, buf, len);
    m->src = me; m->tag = tag; m->len = m->got = m->cap = len;
    m->done = 1; m->bound = 0;
    struct mpi_req *r = posted_take(me, tag);
    if(r){
      int bytes = len <= r->cap ? len : r->cap;
      fastcopy(r->buf, m->buf, bytes);
      fill_status(&r->st, me, tag, bytes,
                  len > r->cap ? MPI_ERR_TRUNCATE : MPI_SUCCESS);
      r->done = 1;
      free(m->buf);
      free(m);
    } else
      uq_append(m);
    fill_status(&q->st, me, tag, len, MPI_SUCCESS);
    q->done = 1;
    return MPI_SUCCESS;
  }

  q->h.tag = tag;
  q->h.len = len;
  q->h.comm = MPI_COMM_WORLD;
  q->h.seq = seqno++;
  q->sbuf = buf;
  q->total = HDR_SIZE + len;
  if(sq_tail[dst])
    sq_tail[dst]->next = q;
  else
    sq_head[dst] = q;
  sq_tail[dst] = q;
  progress_dst(dst);
  return MPI_SUCCESS;
}

// Finish a receive that matched a message still on the unexpected queue.
static void
recv_from_um(struct mpi_req *q)
{
  struct inmsg *m = q->um;
  int bytes = m->len <= q->cap ? m->len : q->cap;
  fastcopy(q->buf, m->buf, bytes);
  fill_status(&q->st, m->src, m->tag, bytes,
              m->len > q->cap ? MPI_ERR_TRUNCATE : MPI_SUCCESS);
  free(m->buf);
  free(m);
  q->um = 0;
  q->done = 1;
}

static int
irecv_start(struct mpi_req *q, void *buf, int cap, int src, int tag)
{
  struct inmsg *m;

  req_init(q, REQ_RECV);
  q->src = src;
  q->tag = tag;
  q->buf = buf;
  q->cap = cap;
  if(src == MPI_PROC_NULL){
    fill_status(&q->st, MPI_PROC_NULL, MPI_ANY_TAG, 0, MPI_SUCCESS);
    q->done = 1;
    return MPI_SUCCESS;
  }
  // Pull in what is already there so the unexpected queue is current.
  progress();
  if((m = uq_find(src, tag)) != 0){
    uq_remove(m);
    q->um = m;
    if(m->done)
      recv_from_um(q);
    return MPI_SUCCESS;
  }
  if(posted_tail)
    posted_tail->next = q;
  else
    posted_head = q;
  posted_tail = q;
  return MPI_SUCCESS;
}

// Non-blocking completion check; drives progress once.
static int
req_test(struct mpi_req *q)
{
  if(!q->done){
    progress();
    if(q->um && q->um->done)
      recv_from_um(q);
  }
  return q->done;
}

static void
req_wait(struct mpi_req *q)
{
  int spins = 0;
  while(!q->done){
    if(progress())
      spins = 0;
    else if(q->um && q->um->done)
      recv_from_um(q);
    else
      idle(&spins);
  }
}

static int
send_bytes(const void *buf, int len, int dst, int tag)
{
  struct mpi_req q;
  int rc = isend_start(&q, buf, len, dst, tag);
  if(rc != MPI_SUCCESS)
    return rc;
  req_wait(&q);
  return MPI_SUCCESS;
}

static int
recv_bytes(void *buf, int cap, int src, int tag, MPI_Status *status)
{
  struct mpi_req q;
  int rc = irecv_start(&q, buf, cap, src, tag);
  if(rc != MPI_SUCCESS)
    return rc;
  req_wait(&q);
  if(status != MPI_STATUS_IGNORE)
    *status = q.st;
  return q.st.MPI_ERROR;
}

// ---------------------------------------------------------------------------
// environment

static int
check_comm(MPI_Comm comm)
{
  return comm == MPI_COMM_WORLD && initialized && !finalized;
}

int
PMPI_Init(int *argc, char ***argv)
{
  int n = 1;

  if(initialized)
    return mpi_err("MPI_Init", MPI_ERR_OTHER);
  if(argc && argv && *argc >= 3 && strcmp((*argv)[1], "-np") == 0){
    n = atoi((*argv)[2]);
    (*argv)[2] = (*argv)[0];
    *argv += 2;
    *argc -= 2;
  }
  if(n < 1 || n > MPI_MAX_RANKS){
    fprintf(2, "mpi: -np must be 1..%d\n", MPI_MAX_RANKS);
    return MPI_ERR_ARG;
  }

  nranks = n;
  shm_size = PAGE + n * n * sizeof(struct ring);
  shm_size = (shm_size + PAGE - 1) & ~(PAGE - 1);
  shm = mmap(0, shm_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(shm == (void *)-1 || shm == 0)
    return mpi_err("MPI_Init: mmap", MPI_ERR_OTHER);
  rings = (struct ring *)((char *)shm + PAGE);
  shm->nranks = n;
  for(int r = 0; r < n; r++){
    if((shm->bell[r] = sync_create(SYNC_SEM, 0)) < 0){
      while(--r >= 0)
        sync_destroy(shm->bell[r]);
      munmap(shm, shm_size);
      return mpi_err("MPI_Init: sync_create", MPI_ERR_OTHER);
    }
  }
  shm->pids[0] = getpid();

  me = 0;
  for(int r = 1; r < n; r++){
    int pid = fork();
    if(pid < 0){
      fprintf(2, "mpi: fork failed for rank %d\n", r);
      initialized = 1;
      PMPI_Abort(MPI_COMM_WORLD, 1);
    }
    if(pid == 0){
      me = r;
      break;
    }
    shm->pids[r] = pid;
  }
  if(n > 1)
    sched_setaffinity(getpid(), 1U << (me % NCPU));
  initialized = 1;
  // Make sure every rank exists (and every pid is recorded) before user code.
  return PMPI_Barrier(MPI_COMM_WORLD);
}

int
PMPI_Initialized(int *flag)
{
  *flag = initialized;
  return MPI_SUCCESS;
}

int
PMPI_Finalize(void)
{
  int rc;

  if(!initialized || finalized)
    return mpi_err("MPI_Finalize", MPI_ERR_OTHER);
  rc = PMPI_Barrier(MPI_COMM_WORLD);
  finalized = 1;
  if(me == 0){
    for(int r = 1; r < nranks; r++)
      waitpid(shm->pids[r], 0, 0);
    for(int r = 0; r < nranks; r++)
      sync_destroy(shm->bell[r]);
  }
  return rc;
}

int
PMPI_Abort(MPI_Comm comm, int errorcode)
{
  (void)comm;
  if(shm){
    __atomic_store_n(&shm->aborted, 1, __ATOMIC_SEQ_CST);
    for(int r = 0; r < nranks; r++){
      if(r != me && shm->pids[r] > 0)
        kill(shm->pids[r]);
    }
  }
  exit(errorcode ? errorcode : 1);
}

int
PMPI_Comm_rank(MPI_Comm comm, int *rank)
{
  if(!check_comm(comm))
    return MPI_ERR_COMM;
  *rank = me;
  return MPI_SUCCESS;
}

int
PMPI_Comm_size(MPI_Comm comm, int *size)
{
  if(!check_comm(comm))
    return MPI_ERR_COMM;
  *size = nranks;
  return MPI_SUCCESS;
}

int
PMPI_Get_processor_name(char *name, int *resultlen)
{
  const char *s = "xv6-rpi3";
  int i;
  for(i = 0; s[i]; i++)
    name[i] = s[i];
  name[i] = 0;
  *resultlen = i;
  return MPI_SUCCESS;
}

int
PMPI_Type_size(MPI_Datatype type, int *size)
{
  int sz = typesize(type);
  if(sz < 0)
    return MPI_ERR_TYPE;
  *size = sz;
  return MPI_SUCCESS;
}

unsigned long
PMPI_Wtime_us(void)
{
  return clock_us();
}

double
PMPI_Wtime(void)
{
  return (double)clock_us() / 1000000.0;
}

double
PMPI_Wtick(void)
{
  return 1e-6;
}

// ---------------------------------------------------------------------------
// point-to-point

static int
check_args(MPI_Comm comm, int count, MPI_Datatype type, int peer, int any_ok)
{
  if(!check_comm(comm))
    return MPI_ERR_COMM;
  if(count < 0)
    return MPI_ERR_COUNT;
  if(typesize(type) < 0)
    return MPI_ERR_TYPE;
  if(peer == MPI_PROC_NULL || (any_ok && peer == MPI_ANY_SOURCE))
    return MPI_SUCCESS;
  if(peer < 0 || peer >= nranks)
    return MPI_ERR_RANK;
  return MPI_SUCCESS;
}

int
PMPI_Send(const void *buf, int count, MPI_Datatype type, int dest, int tag,
         MPI_Comm comm)
{
  int rc = check_args(comm, count, type, dest, 0);
  if(rc == MPI_SUCCESS && (tag < 0 || tag > MPI_TAG_UB))
    rc = MPI_ERR_TAG;
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Send", rc);
  return send_bytes(buf, count * typesize(type), dest, tag);
}

int
PMPI_Recv(void *buf, int count, MPI_Datatype type, int source, int tag,
         MPI_Comm comm, MPI_Status *status)
{
  int rc = check_args(comm, count, type, source, 1);
  if(rc == MPI_SUCCESS && tag != MPI_ANY_TAG && (tag < 0 || tag > MPI_TAG_UB))
    rc = MPI_ERR_TAG;
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Recv", rc);
  rc = recv_bytes(buf, count * typesize(type), source, tag, status);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Recv", rc);
}

int
PMPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
             int dest, int sendtag,
             void *recvbuf, int recvcount, MPI_Datatype recvtype,
             int source, int recvtag, MPI_Comm comm, MPI_Status *status)
{
  // Sends are buffered, so send-then-receive cannot deadlock.
  int rc = PMPI_Send(sendbuf, sendcount, sendtype, dest, sendtag, comm);
  if(rc != MPI_SUCCESS)
    return rc;
  return PMPI_Recv(recvbuf, recvcount, recvtype, source, recvtag, comm, status);
}

int
PMPI_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status)
{
  struct inmsg *m;
  int spins = 0;
  int rc = check_args(comm, 0, MPI_BYTE, source, 1);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Probe", rc);
  if(source == MPI_PROC_NULL){
    fill_status(status, MPI_PROC_NULL, MPI_ANY_TAG, 0, MPI_SUCCESS);
    return MPI_SUCCESS;
  }
  // A header no posted receive takes lands on the queue, and its length is
  // known as soon as the header is in.
  while((m = uq_find(source, tag)) == 0){
    if(progress())
      spins = 0;
    else
      idle(&spins);
  }
  fill_status(status, m->src, m->tag, m->len, MPI_SUCCESS);
  return MPI_SUCCESS;
}

int
PMPI_Iprobe(int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status)
{
  struct inmsg *m;
  int rc = check_args(comm, 0, MPI_BYTE, source, 1);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Iprobe", rc);
  *flag = 0;
  if(source == MPI_PROC_NULL){
    *flag = 1;
    fill_status(status, MPI_PROC_NULL, MPI_ANY_TAG, 0, MPI_SUCCESS);
    return MPI_SUCCESS;
  }
  progress();
  if((m = uq_find(source, tag)) != 0){
    *flag = 1;
    fill_status(status, m->src, m->tag, m->len, MPI_SUCCESS);
  }
  return MPI_SUCCESS;
}

// ---------------------------------------------------------------------------
// non-blocking point-to-point

int
PMPI_Isend(const void *buf, int count, MPI_Datatype type, int dest, int tag,
          MPI_Comm comm, MPI_Request *request)
{
  struct mpi_req *q;
  int rc = check_args(comm, count, type, dest, 0);
  if(rc == MPI_SUCCESS && (tag < 0 || tag > MPI_TAG_UB))
    rc = MPI_ERR_TAG;
  if(rc == MPI_SUCCESS && request == 0)
    rc = MPI_ERR_ARG;
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Isend", rc);
  if((q = malloc(sizeof(*q))) == 0)
    return mpi_err("MPI_Isend", MPI_ERR_OTHER);
  rc = isend_start(q, buf, count * typesize(type), dest, tag);
  if(rc != MPI_SUCCESS){
    free(q);
    return mpi_err("MPI_Isend", rc);
  }
  q->heap = 1;
  *request = q;
  return MPI_SUCCESS;
}

int
PMPI_Irecv(void *buf, int count, MPI_Datatype type, int source, int tag,
          MPI_Comm comm, MPI_Request *request)
{
  struct mpi_req *q;
  int rc = check_args(comm, count, type, source, 1);
  if(rc == MPI_SUCCESS && tag != MPI_ANY_TAG && (tag < 0 || tag > MPI_TAG_UB))
    rc = MPI_ERR_TAG;
  if(rc == MPI_SUCCESS && request == 0)
    rc = MPI_ERR_ARG;
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Irecv", rc);
  if((q = malloc(sizeof(*q))) == 0)
    return mpi_err("MPI_Irecv", MPI_ERR_OTHER);
  irecv_start(q, buf, count * typesize(type), source, tag);
  q->heap = 1;
  *request = q;
  return MPI_SUCCESS;
}

// Release a completed request: copy its status out, free it, null the handle.
static int
req_finish(MPI_Request *request, MPI_Status *status)
{
  struct mpi_req *q = *request;
  int err = q->st.MPI_ERROR;
  if(q->pers && !q->pers->active){
    // Completing an inactive persistent request returns an empty status.
    if(status != MPI_STATUS_IGNORE)
      fill_status(status, MPI_ANY_SOURCE, MPI_ANY_TAG, 0, MPI_SUCCESS);
    return MPI_SUCCESS;
  }
  if(status != MPI_STATUS_IGNORE){
    *status = q->st;
    if(q->kind == REQ_SEND)
      status->MPI_ERROR = MPI_SUCCESS;
  }
  if(q->pers){
    q->pers->active = 0;
    return err;
  }
  if(q->heap)
    free(q);
  *request = MPI_REQUEST_NULL;
  return err;
}

static void
null_status(MPI_Status *status)
{
  fill_status(status, MPI_ANY_SOURCE, MPI_ANY_TAG, 0, MPI_SUCCESS);
}

int
PMPI_Wait(MPI_Request *request, MPI_Status *status)
{
  if(request == 0)
    return mpi_err("MPI_Wait", MPI_ERR_ARG);
  if(*request == MPI_REQUEST_NULL){
    null_status(status);
    return MPI_SUCCESS;
  }
  req_wait(*request);
  return req_finish(request, status);
}

int
PMPI_Test(MPI_Request *request, int *flag, MPI_Status *status)
{
  if(request == 0 || flag == 0)
    return mpi_err("MPI_Test", MPI_ERR_ARG);
  if(*request == MPI_REQUEST_NULL){
    *flag = 1;
    null_status(status);
    return MPI_SUCCESS;
  }
  *flag = req_test(*request);
  return *flag ? req_finish(request, status) : MPI_SUCCESS;
}

int
PMPI_Waitall(int count, MPI_Request *requests, MPI_Status *statuses)
{
  int rc = MPI_SUCCESS;
  for(int i = 0; i < count; i++){
    MPI_Status *st = statuses == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE
                                                     : &statuses[i];
    int r = PMPI_Wait(&requests[i], st);
    if(r != MPI_SUCCESS)
      rc = r;
  }
  return rc;
}

int
PMPI_Testall(int count, MPI_Request *requests, int *flag,
            MPI_Status *statuses)
{
  // All or nothing: release requests only once every one is complete.
  progress();
  for(int i = 0; i < count; i++){
    struct mpi_req *q = requests[i];
    if(q && !q->done && q->um && q->um->done)
      recv_from_um(q);
    if(q && !q->done){
      *flag = 0;
      return MPI_SUCCESS;
    }
  }
  *flag = 1;
  return PMPI_Waitall(count, requests, statuses);
}

int
PMPI_Waitany(int count, MPI_Request *requests, int *index, MPI_Status *status)
{
  int spins = 0, live;

  for(;;){
    live = 0;
    for(int i = 0; i < count; i++){
      struct mpi_req *q = requests[i];
      if(q == MPI_REQUEST_NULL || (q->pers && !q->pers->active))
        continue;
      live = 1;
      if(!q->done && q->um && q->um->done)
        recv_from_um(q);
      if(q->done){
        *index = i;
        return req_finish(&requests[i], status);
      }
    }
    if(!live){
      *index = MPI_UNDEFINED;
      null_status(status);
      return MPI_SUCCESS;
    }
    if(progress())
      spins = 0;
    else
      idle(&spins);
  }
}

int
PMPI_Get_count(const MPI_Status *status, MPI_Datatype type, int *count)
{
  int sz = typesize(type);
  if(sz < 0)
    return MPI_ERR_TYPE;
  *count = status->_bytes % sz ? MPI_UNDEFINED : status->_bytes / sz;
  return MPI_SUCCESS;
}

// ---------------------------------------------------------------------------
// collectives (binomial trees over point-to-point, internal negative tags)

static int
bcast_bytes(void *buf, int len, int root, int tag)
{
  int vr = (me - root + nranks) % nranks, mask = 1, rc;

  while(mask < nranks){
    if(vr & mask){
      rc = recv_bytes(buf, len, (vr - mask + root) % nranks, tag,
                      MPI_STATUS_IGNORE);
      if(rc != MPI_SUCCESS)
        return rc;
      break;
    }
    mask <<= 1;
  }
  mask >>= 1;
  while(mask > 0){
    if(vr + mask < nranks){
      rc = send_bytes(buf, len, (vr + mask + root) % nranks, tag);
      if(rc != MPI_SUCCESS)
        return rc;
    }
    mask >>= 1;
  }
  return MPI_SUCCESS;
}

#define REDUCE_LOOP(T)                                                    \
  do {                                                                    \
    T *a = (T *)acc;                                                      \
    const T *b = (const T *)in;                                           \
    for(int i = 0; i < count; i++){                                       \
      switch(op){                                                         \
      case MPI_SUM:  a[i] = a[i] + b[i]; break;                           \
      case MPI_PROD: a[i] = a[i] * b[i]; break;                           \
      case MPI_MAX:  if(b[i] > a[i]) a[i] = b[i]; break;                  \
      case MPI_MIN:  if(b[i] < a[i]) a[i] = b[i]; break;                  \
      case MPI_LAND: a[i] = a[i] && b[i]; break;                          \
      case MPI_LOR:  a[i] = a[i] || b[i]; break;                          \
      case MPI_BAND: a[i] = a[i] & b[i]; break;                           \
      case MPI_BOR:  a[i] = a[i] | b[i]; break;                           \
      case MPI_BXOR: a[i] = a[i] ^ b[i]; break;                           \
      }                                                                   \
    }                                                                     \
  } while(0)

#define REDUCE_FLOAT(T)                                                   \
  do {                                                                    \
    T *a = (T *)acc;                                                      \
    const T *b = (const T *)in;                                           \
    for(int i = 0; i < count; i++){                                       \
      switch(op){                                                         \
      case MPI_SUM:  a[i] = a[i] + b[i]; break;                           \
      case MPI_PROD: a[i] = a[i] * b[i]; break;                           \
      case MPI_MAX:  if(b[i] > a[i]) a[i] = b[i]; break;                  \
      case MPI_MIN:  if(b[i] < a[i]) a[i] = b[i]; break;                  \
      }                                                                   \
    }                                                                     \
  } while(0)

static void
combine(void *acc, const void *in, int count, MPI_Datatype type, MPI_Op op)
{
  switch(type){
  case MPI_CHAR:           REDUCE_LOOP(char); break;
  case MPI_BYTE:
  case MPI_UNSIGNED_CHAR:  REDUCE_LOOP(unsigned char); break;
  case MPI_SHORT:          REDUCE_LOOP(short); break;
  case MPI_UNSIGNED_SHORT: REDUCE_LOOP(unsigned short); break;
  case MPI_INT:            REDUCE_LOOP(int); break;
  case MPI_UNSIGNED:       REDUCE_LOOP(unsigned int); break;
  case MPI_LONG:
  case MPI_LONG_LONG:      REDUCE_LOOP(long); break;
  case MPI_UNSIGNED_LONG:  REDUCE_LOOP(unsigned long); break;
  case MPI_FLOAT:          REDUCE_FLOAT(float); break;
  case MPI_DOUBLE:         REDUCE_FLOAT(double); break;
  }
}

static int
reduce_impl(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
            MPI_Op op, int root, int tag)
{
  int len = count * typesize(type);
  int vr = (me - root + nranks) % nranks, mask = 1, rc = MPI_SUCCESS;
  char *acc, *tmp;

  if(sendbuf == MPI_IN_PLACE)
    sendbuf = recvbuf;
  acc = malloc(len > 0 ? len : 1);
  tmp = malloc(len > 0 ? len : 1);
  if(acc == 0 || tmp == 0){
    free(acc);
    free(tmp);
    return MPI_ERR_OTHER;
  }
  fastcopy(acc, sendbuf, len);

  while(mask < nranks){
    if((vr & mask) == 0){
      int child = vr | mask;
      if(child < nranks){
        rc = recv_bytes(tmp, len, (child + root) % nranks, tag,
                        MPI_STATUS_IGNORE);
        if(rc != MPI_SUCCESS)
          break;
        combine(acc, tmp, count, type, op);
      }
    } else {
      rc = send_bytes(acc, len, (vr - mask + root) % nranks, tag);
      break;
    }
    mask <<= 1;
  }
  if(rc == MPI_SUCCESS && me == root)
    fastcopy(recvbuf, acc, len);
  free(acc);
  free(tmp);
  return rc;
}

static int
check_coll(MPI_Comm comm, int count, MPI_Datatype type, int root)
{
  int rc = check_args(comm, count, type, root, 0);
  if(rc == MPI_SUCCESS && root == MPI_PROC_NULL)
    rc = MPI_ERR_RANK;
  return rc;
}

int
PMPI_Barrier(MPI_Comm comm)
{
  int rc;
  if(!check_comm(comm))
    return mpi_err("MPI_Barrier", MPI_ERR_COMM);
  if(nranks == 1)
    return MPI_SUCCESS;
  if((rc = reduce_impl(0, 0, 0, MPI_BYTE, MPI_BOR, 0, TAG_BARRIER)) == 0)
    rc = bcast_bytes(0, 0, 0, TAG_BARRIER);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Barrier", rc);
}

int
PMPI_Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, root);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(buf, count * typesize(type), root, TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Bcast", rc);
}

static int
check_op(MPI_Op op, MPI_Datatype type)
{
  if(type == MPI_FLOAT || type == MPI_DOUBLE)
    return op >= MPI_SUM && op <= MPI_MIN;   // no logical/bitwise on reals
  return op >= MPI_SUM && op <= MPI_BXOR;
}

int
PMPI_Reduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
           MPI_Op op, int root, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, root);
  if(rc == MPI_SUCCESS && !check_op(op, type))
    rc = MPI_ERR_OP;
  if(rc == MPI_SUCCESS)
    rc = reduce_impl(sendbuf, recvbuf, count, type, op, root, TAG_REDUCE);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Reduce", rc);
}

int
PMPI_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, 0);
  if(rc == MPI_SUCCESS && !check_op(op, type))
    rc = MPI_ERR_OP;
  if(rc == MPI_SUCCESS)
    rc = reduce_impl(sendbuf, recvbuf, count, type, op, 0, TAG_REDUCE);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(recvbuf, count * typesize(type), 0, TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Allreduce", rc);
}

int
PMPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
           void *recvbuf, int recvcount, MPI_Datatype recvtype,
           int root, MPI_Comm comm)
{
  int slen, rlen, rc = check_coll(comm, sendcount, sendtype, root);
  if(rc == MPI_SUCCESS && me == root)
    rc = check_coll(comm, recvcount, recvtype, root);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Gather", rc);
  slen = sendcount * typesize(sendtype);
  if(me != root)
    return send_bytes(sendbuf, slen, root, TAG_GATHER);

  rlen = recvcount * typesize(recvtype);
  for(int r = 0; r < nranks; r++){
    char *dst = (char *)recvbuf + r * rlen;
    if(r == me){
      if(sendbuf != MPI_IN_PLACE)
        fastcopy(dst, sendbuf, slen < rlen ? slen : rlen);
    } else if((rc = recv_bytes(dst, rlen, r, TAG_GATHER,
                               MPI_STATUS_IGNORE)) != MPI_SUCCESS)
      return mpi_err("MPI_Gather", rc);
  }
  return MPI_SUCCESS;
}

int
PMPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
            void *recvbuf, int recvcount, MPI_Datatype recvtype,
            int root, MPI_Comm comm)
{
  int slen, rlen, rc = check_coll(comm, recvcount, recvtype, root);
  if(rc == MPI_SUCCESS && me == root)
    rc = check_coll(comm, sendcount, sendtype, root);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Scatter", rc);
  rlen = recvcount * typesize(recvtype);
  if(me != root){
    rc = recv_bytes(recvbuf, rlen, root, TAG_SCATTER, MPI_STATUS_IGNORE);
    return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Scatter", rc);
  }

  slen = sendcount * typesize(sendtype);
  for(int r = 0; r < nranks; r++){
    const char *src = (const char *)sendbuf + r * slen;
    if(r == me){
      if(recvbuf != MPI_IN_PLACE)
        fastcopy(recvbuf, src, slen < rlen ? slen : rlen);
    } else if((rc = send_bytes(src, slen, r, TAG_SCATTER)) != MPI_SUCCESS)
      return mpi_err("MPI_Scatter", rc);
  }
  return MPI_SUCCESS;
}

int
PMPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm)
{
  int rc = check_coll(comm, recvcount, recvtype, 0);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Allgather", rc);
  if(sendbuf == MPI_IN_PLACE){
    // My block already sits at recvbuf[me]; gather it from there.
    sendbuf = (char *)recvbuf + me * recvcount * typesize(recvtype);
    sendcount = recvcount;
    sendtype = recvtype;
    if(me == 0)
      sendbuf = MPI_IN_PLACE;
  }
  rc = PMPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  0, comm);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(recvbuf, nranks * recvcount * typesize(recvtype), 0,
                     TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Allgather", rc);
}

int
PMPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
             void *recvbuf, int recvcount, MPI_Datatype recvtype,
             MPI_Comm comm)
{
  int slen, rlen, rc = check_coll(comm, sendcount, sendtype, 0);
  if(rc == MPI_SUCCESS)
    rc = check_coll(comm, recvcount, recvtype, 0);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Alltoall", rc);
  slen = sendcount * typesize(sendtype);
  rlen = recvcount * typesize(recvtype);
  // Buffered sends: post all, then collect all.
  for(int i = 1; i < nranks; i++){
    int r = (me + i) % nranks;
    if((rc = send_bytes((const char *)sendbuf + r * slen, slen, r,
                        TAG_ALLTOALL)) != MPI_SUCCESS)
      return mpi_err("MPI_Alltoall", rc);
  }
  fastcopy((char *)recvbuf + me * rlen, (const char *)sendbuf + me * slen,
           slen < rlen ? slen : rlen);
  for(int i = 1; i < nranks; i++){
    int r = (me - i + nranks) % nranks;
    if((rc = recv_bytes((char *)recvbuf + r * rlen, rlen, r, TAG_ALLTOALL,
                        MPI_STATUS_IGNORE)) != MPI_SUCCESS)
      return mpi_err("MPI_Alltoall", rc);
  }
  return MPI_SUCCESS;
}

// ---------------------------------------------------------------------------
// v-collectives (linear exchanges over point-to-point)

static int
check_counts(const int *counts, const int *displs)
{
  if(counts == 0 || displs == 0)
    return MPI_ERR_ARG;
  for(int r = 0; r < nranks; r++)
    if(counts[r] < 0)
      return MPI_ERR_COUNT;
  return MPI_SUCCESS;
}

// Post every receive first, then every send, then wait for all of them.
// recvs/sends hold up to MPI_MAX_RANKS entries; len < 0 skips a peer.
static int
exchange(char *const *rbuf, const int *rlen, const char *const *sbuf,
         const int *slen, int tag)
{
  struct mpi_req rq[MPI_MAX_RANKS], sq[MPI_MAX_RANKS];
  int rc = MPI_SUCCESS, err;

  for(int i = 1; i < nranks; i++){
    int r = (me - i + nranks) % nranks;
    if(rlen[r] >= 0 && (err = irecv_start(&rq[r], rbuf[r], rlen[r], r, tag)) != MPI_SUCCESS)
      return err;
  }
  for(int i = 1; i < nranks; i++){
    int r = (me + i) % nranks;
    if(slen[r] >= 0 && (err = isend_start(&sq[r], sbuf[r], slen[r], r, tag)) != MPI_SUCCESS)
      return err;
  }
  for(int r = 0; r < nranks; r++){
    if(r == me)
      continue;
    if(slen[r] >= 0)
      req_wait(&sq[r]);
    if(rlen[r] >= 0){
      req_wait(&rq[r]);
      if(rq[r].st.MPI_ERROR != MPI_SUCCESS)
        rc = rq[r].st.MPI_ERROR;
    }
  }
  return rc;
}

int
PMPI_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, const int *recvcounts, const int *displs,
                MPI_Datatype recvtype, MPI_Comm comm)
{
  char *rbuf[MPI_MAX_RANKS];
  const char *sbuf[MPI_MAX_RANKS];
  int rlen[MPI_MAX_RANKS], slen[MPI_MAX_RANKS];
  int rsz = typesize(recvtype), rc = check_coll(comm, 0, recvtype, 0);
  if(rc == MPI_SUCCESS)
    rc = check_counts(recvcounts, displs);
  if(rc == MPI_SUCCESS && sendbuf != MPI_IN_PLACE)
    rc = check_coll(comm, sendcount, sendtype, 0);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Allgatherv", rc);
  char *mine = (char *)recvbuf + (long)displs[me] * rsz;
  int mylen = recvcounts[me] * rsz;
  if(sendbuf != MPI_IN_PLACE){
    int n = sendcount * typesize(sendtype);
    fastcopy(mine, sendbuf, n < mylen ? n : mylen);
  }
  for(int r = 0; r < nranks; r++){
    rbuf[r] = (char *)recvbuf + (long)displs[r] * rsz;
    rlen[r] = recvcounts[r] * rsz;
    sbuf[r] = mine;
    slen[r] = mylen;
  }
  rc = exchange(rbuf, rlen, sbuf, slen, TAG_ALLGATHERV);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Allgatherv", rc);
}

int
PMPI_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
             void *recvbuf, const int *recvcounts, const int *displs,
             MPI_Datatype recvtype, int root, MPI_Comm comm)
{
  int rc = check_coll(comm, sendbuf == MPI_IN_PLACE ? 0 : sendcount, sendtype, root);
  if(rc == MPI_SUCCESS && me == root)
    rc = check_counts(recvcounts, displs);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Gatherv", rc);
  int slen = sendcount * typesize(sendtype);
  if(me != root)
    return send_bytes(sendbuf, slen, root, TAG_GATHERV);
  int rsz = typesize(recvtype);
  for(int r = 0; r < nranks; r++){
    char *dst = (char *)recvbuf + (long)displs[r] * rsz;
    int rlen = recvcounts[r] * rsz;
    if(r == me){
      if(sendbuf != MPI_IN_PLACE)
        fastcopy(dst, sendbuf, slen < rlen ? slen : rlen);
    } else if((rc = recv_bytes(dst, rlen, r, TAG_GATHERV, MPI_STATUS_IGNORE)) != MPI_SUCCESS)
      return mpi_err("MPI_Gatherv", rc);
  }
  return MPI_SUCCESS;
}

int
PMPI_Alltoallv(const void *sendbuf, const int *sendcounts, const int *sdispls,
               MPI_Datatype sendtype, void *recvbuf, const int *recvcounts,
               const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
{
  char *rbuf[MPI_MAX_RANKS];
  const char *sbuf[MPI_MAX_RANKS];
  int rlen[MPI_MAX_RANKS], slen[MPI_MAX_RANKS];
  int rc = check_coll(comm, 0, sendtype, 0);
  if(rc == MPI_SUCCESS)
    rc = check_coll(comm, 0, recvtype, 0);
  if(rc == MPI_SUCCESS)
    rc = check_counts(sendcounts, sdispls);
  if(rc == MPI_SUCCESS)
    rc = check_counts(recvcounts, rdispls);
  if(rc != MPI_SUCCESS)
    return mpi_err("MPI_Alltoallv", rc);
  int ssz = typesize(sendtype), rsz = typesize(recvtype);
  for(int r = 0; r < nranks; r++){
    rbuf[r] = (char *)recvbuf + (long)rdispls[r] * rsz;
    rlen[r] = recvcounts[r] * rsz;
    sbuf[r] = (const char *)sendbuf + (long)sdispls[r] * ssz;
    slen[r] = sendcounts[r] * ssz;
  }
  fastcopy(rbuf[me], sbuf[me], slen[me] < rlen[me] ? slen[me] : rlen[me]);
  rc = exchange(rbuf, rlen, sbuf, slen, TAG_ALLTOALLV);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Alltoallv", rc);
}

// ---------------------------------------------------------------------------
// "non-blocking" collectives: they complete inside the call and return a
// request that is already done, which is a legal (if unhelpful) schedule.

static int
done_request(MPI_Request *request)
{
  struct mpi_req *q = malloc(sizeof(*q));
  if(q == 0)
    return MPI_ERR_OTHER;
  req_init(q, REQ_SEND);
  fill_status(&q->st, MPI_ANY_SOURCE, MPI_ANY_TAG, 0, MPI_SUCCESS);
  q->done = 1;
  q->heap = 1;
  *request = q;
  return MPI_SUCCESS;
}

int
PMPI_Iallreduce(const void *sendbuf, void *recvbuf, int count,
                MPI_Datatype type, MPI_Op op, MPI_Comm comm,
                MPI_Request *request)
{
  if(request == 0)
    return mpi_err("MPI_Iallreduce", MPI_ERR_ARG);
  int rc = PMPI_Allreduce(sendbuf, recvbuf, count, type, op, comm);
  return rc == MPI_SUCCESS ? done_request(request) : rc;
}

int
PMPI_Ibarrier(MPI_Comm comm, MPI_Request *request)
{
  if(request == 0)
    return mpi_err("MPI_Ibarrier", MPI_ERR_ARG);
  int rc = PMPI_Barrier(comm);
  return rc == MPI_SUCCESS ? done_request(request) : rc;
}

int
PMPI_Ineighbor_alltoallv(const void *sendbuf, const int *sendcounts,
                         const int *sdispls, MPI_Datatype sendtype,
                         void *recvbuf, const int *recvcounts,
                         const int *rdispls, MPI_Datatype recvtype,
                         MPI_Comm comm, MPI_Request *request)
{
  // Needs a graph or Cartesian communicator, and there is only WORLD.
  return mpi_err("MPI_Ineighbor_alltoallv", MPI_ERR_COMM);
}

// Synchronous sends behave as the (effectively buffered) standard ones.
int
PMPI_Issend(const void *buf, int count, MPI_Datatype type, int dest, int tag,
            MPI_Comm comm, MPI_Request *request)
{
  return PMPI_Isend(buf, count, type, dest, tag, comm, request);
}

int
PMPI_Ssend(const void *buf, int count, MPI_Datatype type, int dest, int tag,
           MPI_Comm comm)
{
  return PMPI_Send(buf, count, type, dest, tag, comm);
}

// ---------------------------------------------------------------------------
// persistent requests

static int
persist_init(int kind, void *buf, int count, MPI_Datatype type, int peer,
             int tag, MPI_Comm comm, MPI_Request *request, const char *fn)
{
  int rc = check_args(comm, count, type, peer, kind == REQ_RECV);
  if(rc == MPI_SUCCESS && !(kind == REQ_RECV && tag == MPI_ANY_TAG) &&
     (tag < 0 || tag > MPI_TAG_UB))
    rc = MPI_ERR_TAG;
  if(rc == MPI_SUCCESS && request == 0)
    rc = MPI_ERR_ARG;
  if(rc != MPI_SUCCESS)
    return mpi_err(fn, rc);
  struct mpi_req *q = malloc(sizeof(*q));
  struct mpi_persist *p = malloc(sizeof(*p));
  if(q == 0 || p == 0){
    free(q);
    free(p);
    return mpi_err(fn, MPI_ERR_OTHER);
  }
  p->kind = kind;
  p->buf = buf;
  p->len = count * typesize(type);
  p->peer = peer;
  p->tag = tag;
  p->active = 0;
  req_init(q, kind);
  q->done = 1;
  q->heap = 1;
  q->pers = p;
  *request = q;
  return MPI_SUCCESS;
}

int
PMPI_Send_init(const void *buf, int count, MPI_Datatype type, int dest,
               int tag, MPI_Comm comm, MPI_Request *request)
{
  return persist_init(REQ_SEND, (void *)buf, count, type, dest, tag, comm,
                      request, "MPI_Send_init");
}

int
PMPI_Recv_init(void *buf, int count, MPI_Datatype type, int source, int tag,
               MPI_Comm comm, MPI_Request *request)
{
  return persist_init(REQ_RECV, buf, count, type, source, tag, comm, request,
                      "MPI_Recv_init");
}

int
PMPI_Start(MPI_Request *request)
{
  struct mpi_req *q;
  struct mpi_persist *p;
  int rc;

  if(request == 0 || (q = *request) == 0 || (p = q->pers) == 0)
    return mpi_err("MPI_Start", MPI_ERR_ARG);
  if(p->active)
    return mpi_err("MPI_Start", MPI_ERR_ARG);   // still active
  // isend_start/irecv_start reinitialise the request; keep what is ours.
  if(p->kind == REQ_SEND)
    rc = isend_start(q, p->buf, p->len, p->peer, p->tag);
  else
    rc = irecv_start(q, p->buf, p->len, p->peer, p->tag);
  q->heap = 1;
  q->pers = p;
  if(rc != MPI_SUCCESS){
    q->done = 1;
    return mpi_err("MPI_Start", rc);
  }
  p->active = 1;
  return MPI_SUCCESS;
}

int
PMPI_Startall(int count, MPI_Request *requests)
{
  for(int i = 0; i < count; i++){
    int rc = PMPI_Start(&requests[i]);
    if(rc != MPI_SUCCESS)
      return rc;
  }
  return MPI_SUCCESS;
}

int
PMPI_Request_free(MPI_Request *request)
{
  struct mpi_req *q;
  if(request == 0)
    return mpi_err("MPI_Request_free", MPI_ERR_ARG);
  if((q = *request) == MPI_REQUEST_NULL)
    return MPI_SUCCESS;
  // Full MPI lets an active request finish in the background; here the
  // caller waits for it, since nothing progresses outside MPI calls.
  req_wait(q);
  free(q->pers);
  if(q->heap)
    free(q);
  *request = MPI_REQUEST_NULL;
  return MPI_SUCCESS;
}

int
PMPI_Waitsome(int incount, MPI_Request *requests, int *outcount, int *indices,
              MPI_Status *statuses)
{
  int spins = 0;

  if(outcount == 0 || indices == 0)
    return mpi_err("MPI_Waitsome", MPI_ERR_ARG);
  for(;;){
    int live = 0, n = 0, rc = MPI_SUCCESS;
    for(int i = 0; i < incount; i++){
      struct mpi_req *q = requests[i];
      if(q == MPI_REQUEST_NULL || (q->pers && !q->pers->active))
        continue;
      live = 1;
      if(!q->done && q->um && q->um->done)
        recv_from_um(q);
      if(q->done){
        MPI_Status *st = statuses == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE
                                                         : &statuses[n];
        int r = req_finish(&requests[i], st);
        if(r != MPI_SUCCESS)
          rc = r;
        indices[n++] = i;
      }
    }
    if(!live){
      *outcount = MPI_UNDEFINED;
      return MPI_SUCCESS;
    }
    if(n > 0){
      *outcount = n;
      return rc;
    }
    if(progress())
      spins = 0;
    else
      idle(&spins);
  }
}

// ---------------------------------------------------------------------------
// MPI_x is an alias of PMPI_x, so a program can wrap MPI_x and call PMPI_x.

#define ALIAS(name) \
  extern __typeof(PMPI_##name) MPI_##name __attribute__((alias("PMPI_" #name)));
ALIAS(Init) ALIAS(Initialized) ALIAS(Finalize) ALIAS(Abort)
ALIAS(Comm_rank) ALIAS(Comm_size) ALIAS(Get_processor_name) ALIAS(Type_size)
ALIAS(Wtime) ALIAS(Wtick) ALIAS(Wtime_us)
ALIAS(Send) ALIAS(Recv) ALIAS(Sendrecv) ALIAS(Probe) ALIAS(Iprobe)
ALIAS(Get_count) ALIAS(Isend) ALIAS(Irecv) ALIAS(Issend) ALIAS(Ssend)
ALIAS(Wait) ALIAS(Test) ALIAS(Waitall) ALIAS(Testall) ALIAS(Waitany)
ALIAS(Waitsome) ALIAS(Send_init) ALIAS(Recv_init) ALIAS(Start)
ALIAS(Startall) ALIAS(Request_free)
ALIAS(Barrier) ALIAS(Bcast) ALIAS(Reduce) ALIAS(Allreduce) ALIAS(Gather)
ALIAS(Gatherv) ALIAS(Scatter) ALIAS(Allgather) ALIAS(Allgatherv)
ALIAS(Alltoall) ALIAS(Alltoallv) ALIAS(Iallreduce) ALIAS(Ibarrier)
ALIAS(Ineighbor_alltoallv)
