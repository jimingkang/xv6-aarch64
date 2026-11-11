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
// arriving header is matched against the posted receive (at most one, since
// everything is blocking); if it does not match, the payload is copied into
// a malloc'd buffer on the unexpected queue.  Because every wait loop keeps
// draining all incoming rings, a blocked sender never stops its own
// receiving side, and two ranks that send to each other first cannot hang.
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

struct recvreq;

// A message being drained from a ring or parked on the unexpected queue.
struct inmsg {
  struct inmsg *next;
  int src, tag, len, got;
  char *buf;                  // user buffer (bound) or malloc'd copy
  int cap;                    // bytes of buf that may be written
  int done;
  struct recvreq *bound;      // the posted receive it is delivered into
};

struct recvreq {
  int src, tag;
  char *buf;
  int cap;
  int done;
  MPI_Status st;
};

static struct shared *shm;
static uint shm_size;
static struct ring *rings;
static int me = 0, nranks = 1, initialized, finalized;
static int seqno;
static struct inmsg *incur[MPI_MAX_RANKS];   // per-source message in flight
static struct inmsg *uq_head, *uq_tail;      // unexpected queue, arrival order
static struct recvreq *posted;

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
  if(id < 1 || id > 0x0a || (sz != 1 && sz != 2 && sz != 4 && sz != 8) ||
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
req_match(struct recvreq *q, int src, int tag)
{
  return (q->src == MPI_ANY_SOURCE || q->src == src) && tag_match(q->tag, tag);
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
        MPI_Abort(MPI_COMM_WORLD, 1);
      }
      m->next = 0;
      m->src = s;
      m->tag = h.tag;
      m->len = h.len;
      m->got = 0;
      m->done = 0;
      m->bound = 0;
      if(posted && !posted->done && req_match(posted, s, h.tag)){
        m->bound = posted;
        m->buf = posted->buf;
        m->cap = posted->cap;
        posted = 0;                   // one message per receive
      } else {
        m->buf = malloc(h.len > 0 ? h.len : 1);
        if(m->buf == 0){
          fprintf(2, "mpi: rank %d: out of memory\n", me);
          MPI_Abort(MPI_COMM_WORLD, 1);
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
      struct recvreq *q = m->bound;
      int bytes = m->len <= q->cap ? m->len : q->cap;
      fill_status(&q->st, m->src, m->tag, bytes,
                  m->len > q->cap ? MPI_ERR_TRUNCATE : MPI_SUCCESS);
      q->done = 1;
      free(m);
    }
  }
  return moved;
}

static int
progress(void)
{
  int moved = 0;
  for(int s = 0; s < nranks; s++)
    if(s != me)
      moved |= progress_src(s);
  return moved;
}

static int
has_work(int send_dst)
{
  for(int s = 0; s < nranks; s++){
    if(s == me)
      continue;
    struct ring *r = ring_of(s, me);
    if(ring_used(r) >= (incur[s] ? 1 : HDR_SIZE))
      return 1;
  }
  if(send_dst >= 0 && ring_space(ring_of(me, send_dst)) > 0)
    return 1;
  return __atomic_load_n(&shm->aborted, __ATOMIC_ACQUIRE);
}

// Called after a round of the caller's loop made no progress.
static void
idle(int *spins, int send_dst)
{
  check_abort();
  if(++*spins < SPIN_LIMIT){
    if(nranks > NCPU)
      sched_yield();
    return;
  }
  *spins = 0;
  __atomic_store_n(&shm->sl[me].sleeping, 1, __ATOMIC_SEQ_CST);
  if(!has_work(send_dst))
    sync_wait(shm->bell[me]);
  __atomic_store_n(&shm->sl[me].sleeping, 0, __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------------------
// point-to-point on bytes

static int
send_bytes(const void *buf, int len, int dst, int tag)
{
  struct ring *r;
  struct msghdr h;
  const char *p;
  int off = 0, total, spins = 0;

  if(dst == MPI_PROC_NULL)
    return MPI_SUCCESS;
  if(dst == me){
    // Self-send: park a ready message on the unexpected queue.
    struct inmsg *m = malloc(sizeof(*m));
    if(m == 0 || (m->buf = malloc(len > 0 ? len : 1)) == 0)
      return MPI_ERR_OTHER;
    fastcopy(m->buf, buf, len);
    m->src = me; m->tag = tag; m->len = m->got = m->cap = len;
    m->done = 1; m->bound = 0;
    uq_append(m);
    return MPI_SUCCESS;
  }

  r = ring_of(me, dst);
  h.tag = tag;
  h.len = len;
  h.comm = MPI_COMM_WORLD;
  h.seq = seqno++;
  total = HDR_SIZE + len;
  while(off < total){
    uint n;
    if(off < HDR_SIZE)
      n = ring_put(r, (const char *)&h + off, HDR_SIZE - off);
    else {
      p = (const char *)buf + (off - HDR_SIZE);
      n = ring_put(r, p, total - off);
    }
    if(n > 0){
      off += n;
      wake(dst);
      spins = 0;
      continue;
    }
    if(progress())                   // keep our own receive side moving
      spins = 0;
    else
      idle(&spins, dst);
  }
  return MPI_SUCCESS;
}

static int
recv_bytes(void *buf, int cap, int src, int tag, MPI_Status *status)
{
  struct inmsg *m;
  struct recvreq q;
  int spins = 0;

  if(src == MPI_PROC_NULL){
    fill_status(status, MPI_PROC_NULL, MPI_ANY_TAG, 0, MPI_SUCCESS);
    return MPI_SUCCESS;
  }

  // Pull in what is already there so the unexpected queue is current.
  progress();
  if((m = uq_find(src, tag)) != 0){
    uq_remove(m);
    while(!m->done){
      if(progress())
        spins = 0;
      else
        idle(&spins, -1);
    }
    int bytes = m->len <= cap ? m->len : cap;
    fastcopy(buf, m->buf, bytes);
    int err = m->len > cap ? MPI_ERR_TRUNCATE : MPI_SUCCESS;
    fill_status(status, m->src, m->tag, bytes, err);
    free(m->buf);
    free(m);
    return err;
  }

  q.src = src;
  q.tag = tag;
  q.buf = buf;
  q.cap = cap;
  q.done = 0;
  posted = &q;
  while(!q.done){
    if(progress())
      spins = 0;
    else
      idle(&spins, -1);
  }
  posted = 0;
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
MPI_Init(int *argc, char ***argv)
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
      MPI_Abort(MPI_COMM_WORLD, 1);
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
  return MPI_Barrier(MPI_COMM_WORLD);
}

int
MPI_Initialized(int *flag)
{
  *flag = initialized;
  return MPI_SUCCESS;
}

int
MPI_Finalize(void)
{
  int rc;

  if(!initialized || finalized)
    return mpi_err("MPI_Finalize", MPI_ERR_OTHER);
  rc = MPI_Barrier(MPI_COMM_WORLD);
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
MPI_Abort(MPI_Comm comm, int errorcode)
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
MPI_Comm_rank(MPI_Comm comm, int *rank)
{
  if(!check_comm(comm))
    return MPI_ERR_COMM;
  *rank = me;
  return MPI_SUCCESS;
}

int
MPI_Comm_size(MPI_Comm comm, int *size)
{
  if(!check_comm(comm))
    return MPI_ERR_COMM;
  *size = nranks;
  return MPI_SUCCESS;
}

int
MPI_Get_processor_name(char *name, int *resultlen)
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
MPI_Type_size(MPI_Datatype type, int *size)
{
  int sz = typesize(type);
  if(sz < 0)
    return MPI_ERR_TYPE;
  *size = sz;
  return MPI_SUCCESS;
}

unsigned long
MPI_Wtime_us(void)
{
  return clock_us();
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
MPI_Send(const void *buf, int count, MPI_Datatype type, int dest, int tag,
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
MPI_Recv(void *buf, int count, MPI_Datatype type, int source, int tag,
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
MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
             int dest, int sendtag,
             void *recvbuf, int recvcount, MPI_Datatype recvtype,
             int source, int recvtag, MPI_Comm comm, MPI_Status *status)
{
  // Sends are buffered, so send-then-receive cannot deadlock.
  int rc = MPI_Send(sendbuf, sendcount, sendtype, dest, sendtag, comm);
  if(rc != MPI_SUCCESS)
    return rc;
  return MPI_Recv(recvbuf, recvcount, recvtype, source, recvtag, comm, status);
}

int
MPI_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status)
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
  // With no receive posted every arriving header lands on the queue, and its
  // length is known as soon as the header is in.
  while((m = uq_find(source, tag)) == 0){
    if(progress())
      spins = 0;
    else
      idle(&spins, -1);
  }
  fill_status(status, m->src, m->tag, m->len, MPI_SUCCESS);
  return MPI_SUCCESS;
}

int
MPI_Get_count(const MPI_Status *status, MPI_Datatype type, int *count)
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
MPI_Barrier(MPI_Comm comm)
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
MPI_Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, root);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(buf, count * typesize(type), root, TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Bcast", rc);
}

static int
check_op(MPI_Op op)
{
  return op >= MPI_SUM && op <= MPI_BXOR;
}

int
MPI_Reduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
           MPI_Op op, int root, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, root);
  if(rc == MPI_SUCCESS && !check_op(op))
    rc = MPI_ERR_OP;
  if(rc == MPI_SUCCESS)
    rc = reduce_impl(sendbuf, recvbuf, count, type, op, root, TAG_REDUCE);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Reduce", rc);
}

int
MPI_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm)
{
  int rc = check_coll(comm, count, type, 0);
  if(rc == MPI_SUCCESS && !check_op(op))
    rc = MPI_ERR_OP;
  if(rc == MPI_SUCCESS)
    rc = reduce_impl(sendbuf, recvbuf, count, type, op, 0, TAG_REDUCE);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(recvbuf, count * typesize(type), 0, TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Allreduce", rc);
}

int
MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
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
MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
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
MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
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
  rc = MPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  0, comm);
  if(rc == MPI_SUCCESS)
    rc = bcast_bytes(recvbuf, nranks * recvcount * typesize(recvtype), 0,
                     TAG_BCAST);
  return rc == MPI_SUCCESS ? rc : mpi_err("MPI_Allgather", rc);
}

int
MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
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
