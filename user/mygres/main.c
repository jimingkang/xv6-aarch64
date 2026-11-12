#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/mman.h>
#include <fcntl.h>
#include "user/user.h"

// mygres: a deliberately small PostgreSQL-style storage-engine exercise.
//
// It has four layers that can be inspected independently:
//   main.db   fixed-size heap rows and a durable database header
//   main.wal  one durable transaction record used for crash REDO
//   postmaster anonymous MAP_SHARED buffer pool, lock and hash index
//   mygres.ipc named rendezvous page for clients and forked workers
//   mygres    CLI / interactive transaction shell
//
// Like PostgreSQL, the resident postmaster creates the main Unix shared area
// with MAP_SHARED|MAP_ANONYMOUS before it forks backend workers.  Short-lived
// clients never map that main area.  Since xv6 has no AF_UNIX socket yet, one
// small named mapping is used only as an IPC rendezvous page; semaphores put
// idle workers and clients to sleep, so request dispatch does not poll.
//
// Concurrency is intentionally simple: readers and commits serialize on one
// process-shared spin lock.  Transactions stage changes privately and hold the
// shared lock only while committing, which gives atomic durable commits and
// read-committed visibility without pretending to implement MVCC.

#define DB_DIR       "/var/lib/mygres"
#define DB_PATH      "/var/lib/mygres/main.db"
#define WAL_PATH     "/var/lib/mygres/main.wal"
#define OLD_SHM_PATH "/var/lib/mygres/main.shm"
#define IPC_PATH     "/var/run/mygres.ipc"

#define PAGE_SIZE    4096U
#define MAX_ROWS     1024U
#define INDEX_SIZE   2048U
#define MAX_TX_OPS   32U
#define MAX_WORKERS  4U

#define DB_MAGIC     0x4d594442U  /* MYDB */
#define WAL_MAGIC    0x4d59574cU  /* MYWL */
#define SHM_MAGIC    0x4d595348U  /* MYSH */
#define IPC_MAGIC    0x4d594950U  /* MYIP */
#define DB_VERSION   1U

#define SYNC_SEM     2

#define SLOT_IDLE      0U
#define SLOT_CLAIMED   1U
#define SLOT_REQUEST   2U
#define SLOT_RESPONSE  3U

#define IPC_PING       1U
#define IPC_GET        2U
#define IPC_COMMIT     3U
#define IPC_SCAN       4U
#define IPC_CHECK      5U
#define IPC_STATS      6U
#define IPC_RECOVER    7U
#define IPC_PREPARE    8U

#define ROW_FREE     0U
#define ROW_USED     1U
#define ROW_TOMB     2U

#define OP_PUT       1U
#define OP_DELETE    2U

struct db_header {
  uint32 magic;
  uint32 version;
  uint32 row_size;
  uint32 max_rows;
  uint64 txid;
  uint64 nrows;
  uint64 checkpoint_lsn;
  uint32 checksum;
  uint32 reserved;
  uchar pad[PAGE_SIZE - 48];
};

struct db_row {
  uint32 state;
  uint32 key;
  uint64 value;
  uint64 txid;
  uint32 checksum;
  uint32 reserved;
  uchar pad[32];
};

struct wal_op {
  uint32 type;
  uint32 slot;
  struct db_row row;
  uchar pad[8];
};

struct wal_page {
  uint32 magic;
  uint32 version;
  uint32 committed;
  uint32 nops;
  uint64 txid;
  uint64 target_count;
  uint32 checksum;
  uint32 reserved;
  struct wal_op ops[MAX_TX_OPS];
  uchar pad[PAGE_SIZE - 40 - MAX_TX_OPS * sizeof(struct wal_op)];
};

// The shared file is a cache, not authoritative storage.  db_txid identifies
// the database image from which rows[] and index[] were built.
struct shm_state {
  volatile uint32 lock;
  uint32 magic;
  uint32 version;
  uint32 ready;
  uint64 db_txid;
  uint64 nrows;
  uint64 commits;
  uint64 recoveries;
  uint64 index_hits;
  uint64 index_misses;
  uint32 index[INDEX_SIZE];       // row slot + 1; zero means empty
  struct db_row rows[MAX_ROWS];
};

struct database {
  struct shm_state *shm;
  uint64 shm_bytes;
};

struct tx_op {
  uint32 type;
  uint32 key;
  uint64 value;
};

struct transaction {
  int active;
  uint nops;
  uint64 start_txid;
  struct tx_op ops[MAX_TX_OPS];
};

struct ipc_request {
  uint32 command;
  uint32 nops;
  uint32 key;
  uint32 reserved;
  uint64 value;
  struct tx_op ops[MAX_TX_OPS];
};

struct ipc_response {
  int status;
  uint32 key;
  uint64 value;
  uint64 txid;
  uint64 rows;
  uint64 commits;
  uint64 recoveries;
  uint64 index_hits;
  uint64 index_misses;
};

struct ipc_slot {
  volatile uint32 state;
  uint32 client_pid;
  uint32 worker_pid;
  uint32 reserved;
  uint64 request_id;
  int request_sem;
  int response_sem;
  struct ipc_request request;
  struct ipc_response response;
};

struct ipc_state {
  uint32 magic;
  uint32 version;
  volatile uint32 ready;
  uint32 generation;
  uint32 postmaster_pid;
  uint32 nworkers;
  uint32 reserved[2];
  struct ipc_slot slots[MAX_WORKERS];
};

struct ipc_client {
  struct ipc_state *state;
  uint64 bytes;
  uint64 next_request;
};

typedef char db_header_is_page[(sizeof(struct db_header) == PAGE_SIZE) ? 1 : -1];
typedef char db_row_is_64[(sizeof(struct db_row) == 64) ? 1 : -1];
typedef char wal_op_is_80[(sizeof(struct wal_op) == 80) ? 1 : -1];
typedef char wal_page_is_page[(sizeof(struct wal_page) == PAGE_SIZE) ? 1 : -1];
typedef char ipc_fits_page[(sizeof(struct ipc_state) <= PAGE_SIZE) ? 1 : -1];

static struct db_header disk_header;
static struct wal_page wal_buffer;
static struct db_row disk_row;

static void pgwire_listener_loop(struct database *db);

static uint64
roundup_page(uint64 n)
{
  return (n + PAGE_SIZE - 1) & ~(uint64)(PAGE_SIZE - 1);
}

static uint32
hash_bytes(void *vp, uint n)
{
  uchar *p = vp;
  uint32 h = 2166136261U;
  for(uint i = 0; i < n; i++){
    h ^= p[i];
    h *= 16777619U;
  }
  return h;
}

static uint32
hash_key(uint32 key)
{
  key ^= key >> 16;
  key *= 0x7feb352dU;
  key ^= key >> 15;
  key *= 0x846ca68bU;
  key ^= key >> 16;
  return key;
}

static void
seal_header(struct db_header *h)
{
  h->checksum = 0;
  h->checksum = hash_bytes(h, sizeof(*h));
}

static int
valid_header(struct db_header *h)
{
  uint32 saved = h->checksum;
  h->checksum = 0;
  uint32 got = hash_bytes(h, sizeof(*h));
  h->checksum = saved;
  return h->magic == DB_MAGIC && h->version == DB_VERSION &&
         h->row_size == sizeof(struct db_row) && h->max_rows == MAX_ROWS &&
         saved == got;
}

static void
seal_row(struct db_row *r)
{
  r->checksum = 0;
  r->checksum = hash_bytes(r, sizeof(*r));
}

static int
valid_row(struct db_row *r)
{
  uchar *p = (uchar *)r;
  int zero = 1;
  for(uint i = 0; i < sizeof(*r); i++)
    if(p[i] != 0){
      zero = 0;
      break;
    }
  if(zero)
    return 1;

  uint32 saved = r->checksum;
  r->checksum = 0;
  uint32 got = hash_bytes(r, sizeof(*r));
  r->checksum = saved;
  return (r->state == ROW_FREE || r->state == ROW_USED ||
          r->state == ROW_TOMB) && saved == got;
}

static void
seal_wal(struct wal_page *w)
{
  w->checksum = 0;
  w->checksum = hash_bytes(w, sizeof(*w));
}

static int
valid_wal(struct wal_page *w)
{
  uint32 saved = w->checksum;
  w->checksum = 0;
  uint32 got = hash_bytes(w, sizeof(*w));
  w->checksum = saved;
  if(w->magic != WAL_MAGIC || w->version != DB_VERSION ||
     w->committed != 1 || w->nops > MAX_TX_OPS || saved != got)
    return 0;
  for(uint i = 0; i < w->nops; i++)
    if(w->ops[i].slot >= MAX_ROWS ||
       (w->ops[i].type != OP_PUT && w->ops[i].type != OP_DELETE) ||
       !valid_row(&w->ops[i].row))
      return 0;
  return 1;
}

static uint64
row_offset(uint slot)
{
  return PAGE_SIZE + (uint64)slot * sizeof(struct db_row);
}

static void
shared_lock(volatile uint32 *lock)
{
  for(;;){
    uint old, failed, one = 1;
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(lock) : "memory");
    if(old == 0){
      asm volatile("stxr %w0, %w1, [%2]"
                   : "=&r"(failed) : "r"(one), "r"(lock) : "memory");
      if(failed == 0)
        return;
    } else {
      asm volatile("clrex" ::: "memory");
    }
    sched_yield();
  }
}

static void
shared_unlock(volatile uint32 *lock)
{
  uint zero = 0;
  asm volatile("stlr %w0, [%1]" :: "r"(zero), "r"(lock) : "memory");
}

static int
atomic_claim(volatile uint32 *word, uint32 expected, uint32 desired)
{
  uint32 old, failed;
  for(;;){
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(word) : "memory");
    if(old != expected){
      asm volatile("clrex" ::: "memory");
      return 0;
    }
    asm volatile("stxr %w0, %w1, [%2]"
                 : "=&r"(failed) : "r"(desired), "r"(word) : "memory");
    if(failed == 0)
      return 1;
  }
}

static void
atomic_publish(volatile uint32 *word, uint32 value)
{
  asm volatile("stlr %w0, [%1]" :: "r"(value), "r"(word) : "memory");
}

static int
ensure_directories(void)
{
  // mkdir returns -1 for an existing directory on xv6; both cases are fine.
  mkdir("/var");
  mkdir("/var/lib");
  mkdir("/var/run");
  mkdir(DB_DIR);
  return 0;
}

static int
read_db_header(int fd, struct db_header *h)
{
  return pread(fd, h, sizeof(*h), 0) == sizeof(*h) && valid_header(h) ? 0 : -1;
}

static int
read_db_row(int fd, uint slot, struct db_row *r)
{
  if(slot >= MAX_ROWS ||
     pread(fd, r, sizeof(*r), row_offset(slot)) != sizeof(*r))
    return -1;
  return valid_row(r) ? 0 : -1;
}

static int
clear_wal(void)
{
  int fd = open(WAL_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0)
    return -1;
  int ok = fsync(fd) == 0;
  close(fd);
  return ok ? 0 : -1;
}

static int
write_wal(struct wal_page *w)
{
  int fd = open(WAL_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0)
    return -1;
  int ok = pwrite(fd, w, sizeof(*w), 0) == sizeof(*w) && fsync(fd) == 0;
  close(fd);
  return ok ? 0 : -1;
}

static int
read_wal(struct wal_page *w)
{
  int fd = open(WAL_PATH, O_RDONLY);
  if(fd < 0)
    return 0;
  struct stat st;
  if(fstat(fd, &st) < 0){
    close(fd);
    return -1;
  }
  if(st.size == 0){
    close(fd);
    return 0;
  }
  int n = pread(fd, w, sizeof(*w), 0);
  close(fd);
  if(n != sizeof(*w) || !valid_wal(w))
    return 2;                   // incomplete or torn transaction
  return 1;                     // committed transaction requires REDO
}

static int
write_db_from_wal(struct wal_page *w)
{
  int fd = open(DB_PATH, O_RDWR);
  if(fd < 0 || read_db_header(fd, &disk_header) < 0){
    if(fd >= 0)
      close(fd);
    return -1;
  }

  // REDO is deliberately idempotent.  Reapplying the same final row images
  // after a crash produces the same database state.
  for(uint i = 0; i < w->nops; i++)
    if(pwrite(fd, &w->ops[i].row, sizeof(struct db_row),
              row_offset(w->ops[i].slot)) != sizeof(struct db_row)){
      close(fd);
      return -1;
    }

  disk_header.txid = w->txid;
  disk_header.nrows = w->target_count;
  disk_header.checkpoint_lsn = w->txid;
  seal_header(&disk_header);
  if(pwrite(fd, &disk_header, sizeof(disk_header), 0) !=
       sizeof(disk_header) || fsync(fd) < 0){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
index_insert(struct shm_state *s, uint slot)
{
  uint32 key = s->rows[slot].key;
  uint start = hash_key(key) % INDEX_SIZE;
  for(uint probe = 0; probe < INDEX_SIZE; probe++){
    uint pos = (start + probe) % INDEX_SIZE;
    if(s->index[pos] == 0){
      s->index[pos] = slot + 1;
      return 0;
    }
  }
  return -1;
}

static int
rebuild_index(struct shm_state *s)
{
  memset(s->index, 0, sizeof(s->index));
  for(uint i = 0; i < MAX_ROWS; i++)
    if(s->rows[i].state == ROW_USED && index_insert(s, i) < 0)
      return -1;
  return 0;
}

static int
index_lookup(struct shm_state *s, uint32 key, int account)
{
  uint start = hash_key(key) % INDEX_SIZE;
  for(uint probe = 0; probe < INDEX_SIZE; probe++){
    uint pos = (start + probe) % INDEX_SIZE;
    uint entry = s->index[pos];
    if(entry == 0){
      if(account)
        s->index_misses++;
      return -1;
    }
    uint slot = entry - 1;
    if(slot < MAX_ROWS && s->rows[slot].state == ROW_USED &&
       s->rows[slot].key == key){
      if(account)
        s->index_hits++;
      return slot;
    }
  }
  if(account)
    s->index_misses++;
  return -1;
}

static int
reload_shared_locked(struct database *db)
{
  int fd = open(DB_PATH, O_RDONLY);
  if(fd < 0 || read_db_header(fd, &disk_header) < 0){
    if(fd >= 0)
      close(fd);
    return -1;
  }

  for(uint i = 0; i < MAX_ROWS; i++){
    if(read_db_row(fd, i, &db->shm->rows[i]) < 0){
      close(fd);
      return -1;
    }
  }
  close(fd);
  if(rebuild_index(db->shm) < 0)
    return -1;
  db->shm->db_txid = disk_header.txid;
  db->shm->nrows = disk_header.nrows;
  db->shm->ready = 1;
  __sync_synchronize();
  return 0;
}

static int
recover_locked(struct database *db, int verbose)
{
  int status = read_wal(&wal_buffer);
  if(status < 0)
    return -1;
  if(status == 0)
    return 0;
  if(status == 2){
    if(verbose)
      printf("mygres: discard incomplete/corrupt WAL\n");
    return clear_wal();
  }

  if(verbose)
    printf("mygres: WAL REDO tx=%l operations=%d\n",
           wal_buffer.txid, wal_buffer.nops);
  if(write_db_from_wal(&wal_buffer) < 0)
    return -1;
  db->shm->ready = 0;
  if(reload_shared_locked(db) < 0)
    return -1;
  db->shm->recoveries++;
  return clear_wal();
}

// Build the authoritative shared buffer pool before fork().  Every backend
// inherits these mappings and therefore sees the same physical pages.  No
// independently exec'd client can attach to this anonymous main area.
static int
create_postmaster_database(struct database *db, int verbose)
{
  memset(db, 0, sizeof(*db));
  db->shm_bytes = roundup_page(sizeof(struct shm_state));

  int fd = open(DB_PATH, O_RDONLY);
  if(fd < 0 || read_db_header(fd, &disk_header) < 0){
    if(fd >= 0)
      close(fd);
    printf("mygres: database is not initialized; run 'mygres init'\n");
    return -1;
  }
  close(fd);

  db->shm = mmap(0, db->shm_bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(db->shm == MAP_FAILED){
    db->shm = 0;
    return -1;
  }

  memset((void *)db->shm, 0, sizeof(*db->shm));
  db->shm->magic = SHM_MAGIC;
  db->shm->version = DB_VERSION;
  shared_lock(&db->shm->lock);
  if(recover_locked(db, verbose) < 0){
    shared_unlock(&db->shm->lock);
    munmap(db->shm, db->shm_bytes);
    db->shm = 0;
    return -1;
  }

  fd = open(DB_PATH, O_RDONLY);
  int stale = fd < 0 || read_db_header(fd, &disk_header) < 0 ||
              !db->shm->ready || db->shm->db_txid != disk_header.txid;
  if(fd >= 0)
    close(fd);
  if(stale && reload_shared_locked(db) < 0){
    shared_unlock(&db->shm->lock);
    munmap(db->shm, db->shm_bytes);
    db->shm = 0;
    return -1;
  }
  shared_unlock(&db->shm->lock);
  return 0;
}

static void
destroy_postmaster_database(struct database *db)
{
  if(db->shm){
    munmap(db->shm, db->shm_bytes);
    db->shm = 0;
  }
}

static int
initialize_database(void)
{
  ensure_directories();
  int fd = open(DB_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0)
    return -1;

  memset(&disk_header, 0, sizeof(disk_header));
  disk_header.magic = DB_MAGIC;
  disk_header.version = DB_VERSION;
  disk_header.row_size = sizeof(struct db_row);
  disk_header.max_rows = MAX_ROWS;
  seal_header(&disk_header);
  uint64 bytes = PAGE_SIZE + (uint64)MAX_ROWS * sizeof(struct db_row);
  int ok = ftruncate(fd, bytes) == 0 &&
           pwrite(fd, &disk_header, sizeof(disk_header), 0) ==
             sizeof(disk_header) && fsync(fd) == 0;
  close(fd);
  if(!ok || clear_wal() < 0)
    return -1;

  // Remove the old file-backed main shared area from pre-postmaster builds.
  // The new server creates its main area anonymously on every start.
  unlink(OLD_SHM_PATH);
  printf("mygres: initialized %s rows=%d\n", DB_PATH, MAX_ROWS);
  return 0;
}

static int
allocate_row_slot(struct shm_state *s)
{
  int tomb = -1;
  for(uint i = 0; i < MAX_ROWS; i++){
    if(s->rows[i].state == ROW_FREE)
      return tomb >= 0 ? tomb : (int)i;
    if(s->rows[i].state == ROW_TOMB && tomb < 0)
      tomb = i;
  }
  return tomb;
}

static void
apply_wal_to_shared(struct database *db, struct wal_page *w)
{
  for(uint i = 0; i < w->nops; i++)
    db->shm->rows[w->ops[i].slot] = w->ops[i].row;
  rebuild_index(db->shm);
  db->shm->db_txid = w->txid;
  db->shm->nrows = w->target_count;
  db->shm->ready = 1;
  db->shm->commits++;
  __sync_synchronize();
}

static int
tx_stage(struct transaction *tx, uint32 type, uint32 key, uint64 value)
{
  if(!tx->active)
    return -1;
  // Last operation for one key wins within the transaction.
  for(uint i = 0; i < tx->nops; i++)
    if(tx->ops[i].key == key){
      tx->ops[i].type = type;
      tx->ops[i].value = value;
      return 0;
    }
  if(tx->nops >= MAX_TX_OPS)
    return -1;
  tx->ops[tx->nops].type = type;
  tx->ops[tx->nops].key = key;
  tx->ops[tx->nops].value = value;
  tx->nops++;
  return 0;
}

// Returns 0 for a complete commit, 2 when only a durable WAL was requested.
static int
tx_commit(struct database *db, struct transaction *tx, int wal_only)
{
  if(!tx->active)
    return -1;
  shared_lock(&db->shm->lock);
  if(recover_locked(db, 1) < 0){
    shared_unlock(&db->shm->lock);
    return -1;
  }

  memset(&wal_buffer, 0, sizeof(wal_buffer));
  wal_buffer.magic = WAL_MAGIC;
  wal_buffer.version = DB_VERSION;
  wal_buffer.committed = 1;
  wal_buffer.txid = db->shm->db_txid + 1;
  wal_buffer.target_count = db->shm->nrows;

  for(uint i = 0; i < tx->nops; i++){
    struct tx_op *op = &tx->ops[i];
    int slot = index_lookup(db->shm, op->key, 0);
    if(op->type == OP_DELETE && slot < 0)
      continue;
    if(op->type == OP_PUT && slot < 0){
      slot = allocate_row_slot(db->shm);
      if(slot < 0){
        shared_unlock(&db->shm->lock);
        printf("mygres: table is full\n");
        return -1;
      }
      wal_buffer.target_count++;
    } else if(op->type == OP_DELETE){
      wal_buffer.target_count--;
    }

    struct wal_op *wo = &wal_buffer.ops[wal_buffer.nops++];
    wo->type = op->type;
    wo->slot = slot;
    memset(&wo->row, 0, sizeof(wo->row));
    wo->row.state = op->type == OP_PUT ? ROW_USED : ROW_TOMB;
    wo->row.key = op->key;
    wo->row.value = op->value;
    wo->row.txid = wal_buffer.txid;
    seal_row(&wo->row);
  }

  if(wal_buffer.nops == 0){
    tx->active = 0;
    shared_unlock(&db->shm->lock);
    return 0;
  }

  seal_wal(&wal_buffer);
  if(write_wal(&wal_buffer) < 0){
    shared_unlock(&db->shm->lock);
    return -1;
  }

  if(wal_only){
    printf("mygres: WAL durable tx=%l operations=%d; DB pages not written\n",
           wal_buffer.txid, wal_buffer.nops);
    tx->active = 0;
    shared_unlock(&db->shm->lock);
    return 2;
  }

  if(write_db_from_wal(&wal_buffer) < 0){
    shared_unlock(&db->shm->lock);
    return -1;
  }
  apply_wal_to_shared(db, &wal_buffer);
  if(clear_wal() < 0){
    shared_unlock(&db->shm->lock);
    return -1;
  }
  tx->active = 0;
  shared_unlock(&db->shm->lock);
  return 0;
}

// 0 found, 1 absent, -1 error.
static int
db_get(struct database *db, uint32 key, uint64 *value, uint64 *txid)
{
  shared_lock(&db->shm->lock);
  int slot = index_lookup(db->shm, key, 1);
  if(slot >= 0){
    *value = db->shm->rows[slot].value;
    if(txid)
      *txid = db->shm->rows[slot].txid;
  }
  shared_unlock(&db->shm->lock);
  return slot >= 0 ? 0 : 1;
}

static int
scan_database(struct database *db, int verbose)
{
  uint64 count = 0;
  shared_lock(&db->shm->lock);
  for(uint i = 0; i < MAX_ROWS; i++)
    if(db->shm->rows[i].state == ROW_USED){
      count++;
      if(verbose)
        printf("key=%d value=%l xmin=%l slot=%d\n",
               db->shm->rows[i].key, db->shm->rows[i].value,
               db->shm->rows[i].txid, i);
    }
  uint64 expected = db->shm->nrows;
  uint64 txid = db->shm->db_txid;
  shared_unlock(&db->shm->lock);
  printf("mygres: txid=%l rows=%l scanned=%l\n", txid, expected, count);
  return count == expected ? 0 : -1;
}

static int
check_database(struct database *db)
{
  int fd = open(DB_PATH, O_RDONLY);
  if(fd < 0 || read_db_header(fd, &disk_header) < 0){
    if(fd >= 0)
      close(fd);
    return -1;
  }
  uint64 count = 0;
  for(uint i = 0; i < MAX_ROWS; i++){
    if(read_db_row(fd, i, &disk_row) < 0){
      printf("mygres: corrupt row slot=%d\n", i);
      close(fd);
      return -1;
    }
    if(disk_row.state == ROW_USED)
      count++;
  }
  close(fd);
  if(count != disk_header.nrows){
    printf("mygres: header rows=%l but disk scan found=%l\n",
           disk_header.nrows, count);
    return -1;
  }

  shared_lock(&db->shm->lock);
  int ok = rebuild_index(db->shm) == 0 && db->shm->nrows == count &&
           db->shm->db_txid == disk_header.txid;
  shared_unlock(&db->shm->lock);
  printf("mygres: CHECK %s txid=%l rows=%l hash-index=%d buckets\n",
         ok ? "PASS" : "FAIL", disk_header.txid, count, INDEX_SIZE);
  return ok ? 0 : -1;
}

static void
fill_stats(struct database *db, struct ipc_response *response)
{
  shared_lock(&db->shm->lock);
  response->txid = db->shm->db_txid;
  response->rows = db->shm->nrows;
  response->commits = db->shm->commits;
  response->recoveries = db->shm->recoveries;
  response->index_hits = db->shm->index_hits;
  response->index_misses = db->shm->index_misses;
  shared_unlock(&db->shm->lock);
}

static void
process_ipc_request(struct database *db, struct ipc_request *request,
                    struct ipc_response *response)
{
  struct transaction tx;
  uint64 value = 0, txid = 0;

  memset(response, 0, sizeof(*response));
  response->status = -1;
  switch(request->command){
  case IPC_PING:
    response->status = 0;
    break;
  case IPC_GET:
    response->key = request->key;
    response->status = db_get(db, request->key, &value, &txid);
    response->value = value;
    response->txid = txid;
    break;
  case IPC_COMMIT:
  case IPC_PREPARE:
    if(request->nops > MAX_TX_OPS)
      break;
    memset(&tx, 0, sizeof(tx));
    tx.active = 1;
    tx.nops = request->nops;
    for(uint i = 0; i < tx.nops; i++){
      if(request->ops[i].type != OP_PUT &&
         request->ops[i].type != OP_DELETE)
        return;
      tx.ops[i] = request->ops[i];
    }
    response->status = tx_commit(db, &tx,
        request->command == IPC_PREPARE);
    if(response->status == 2)
      response->status = 0;
    break;
  case IPC_SCAN:
    response->status = scan_database(db, 1);
    break;
  case IPC_CHECK:
    response->status = check_database(db);
    break;
  case IPC_STATS:
    response->status = 0;
    break;
  case IPC_RECOVER:
    shared_lock(&db->shm->lock);
    response->status = recover_locked(db, 1);
    shared_unlock(&db->shm->lock);
    if(response->status == 0)
      response->status = check_database(db);
    break;
  default:
    break;
  }
  fill_stats(db, response);
}

static struct ipc_state *
create_ipc_state(uint workers)
{
  int fd;
  struct ipc_state *ipc;

  ensure_directories();
  fd = open(IPC_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0 || ftruncate(fd, PAGE_SIZE) < 0){
    if(fd >= 0)
      close(fd);
    return 0;
  }
  ipc = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(ipc == MAP_FAILED)
    return 0;
  memset(ipc, 0, PAGE_SIZE);
  ipc->magic = IPC_MAGIC;
  ipc->version = DB_VERSION;
  ipc->generation = (uint32)clock_us();
  ipc->postmaster_pid = getpid();
  ipc->nworkers = workers;
  for(uint i = 0; i < workers; i++){
    ipc->slots[i].request_sem = sync_create(SYNC_SEM, 0);
    ipc->slots[i].response_sem = sync_create(SYNC_SEM, 0);
    if(ipc->slots[i].request_sem < 0 || ipc->slots[i].response_sem < 0){
      for(uint j = 0; j <= i; j++){
        if(ipc->slots[j].request_sem >= 0)
          sync_destroy(ipc->slots[j].request_sem);
        if(ipc->slots[j].response_sem >= 0)
          sync_destroy(ipc->slots[j].response_sem);
      }
      munmap(ipc, PAGE_SIZE);
      unlink(IPC_PATH);
      return 0;
    }
  }
  return ipc;
}

static void
destroy_ipc_state(struct ipc_state *ipc)
{
  if(ipc == 0)
    return;
  atomic_publish(&ipc->ready, 0);
  for(uint i = 0; i < ipc->nworkers && i < MAX_WORKERS; i++){
    sync_destroy(ipc->slots[i].request_sem);
    sync_destroy(ipc->slots[i].response_sem);
  }
  munmap(ipc, PAGE_SIZE);
  unlink(IPC_PATH);
}

static void
backend_loop(struct database *db, struct ipc_state *ipc, uint worker)
{
  struct ipc_slot *slot = &ipc->slots[worker];
  struct ipc_request request;
  struct ipc_response response;

  slot->worker_pid = getpid();
  __sync_synchronize();
  for(;;){
    if(sync_wait(slot->request_sem) < 0)
      break;
    if(slot->state != SLOT_REQUEST)
      continue;
    request = slot->request;
    process_ipc_request(db, &request, &response);
    slot->response = response;
    atomic_publish(&slot->state, SLOT_RESPONSE);
    if(sync_signal(slot->response_sem, 1) < 0)
      break;
  }
  exit(0);
}

static int
run_postmaster(uint workers)
{
  struct database db;
  struct ipc_state *ipc;
  int started = 0, pgwire_pid = 0;

  if(workers == 0 || workers > MAX_WORKERS){
    printf("mygres: workers must be between 1 and %d\n", MAX_WORKERS);
    return -1;
  }
  if(create_postmaster_database(&db, 1) < 0)
    return -1;
  if((ipc = create_ipc_state(workers)) == 0){
    destroy_postmaster_database(&db);
    return -1;
  }

  for(uint i = 0; i < workers; i++){
    int pid = fork();
    if(pid < 0)
      break;
    if(pid == 0)
      backend_loop(&db, ipc, i);
    ipc->slots[i].worker_pid = pid;
    started++;
  }
  if(started != (int)workers){
    for(int i = 0; i < started; i++)
      if(ipc->slots[i].worker_pid)
        kill(ipc->slots[i].worker_pid);
    for(int i = 0; i < started; i++)
      wait(0);
    destroy_ipc_state(ipc);
    destroy_postmaster_database(&db);
    return -1;
  }

  pgwire_pid = fork();
  if(pgwire_pid == 0)
    pgwire_listener_loop(&db);
  if(pgwire_pid < 0){
    for(uint i = 0; i < workers; i++)
      kill(ipc->slots[i].worker_pid);
    while(wait(0) >= 0)
      ;
    destroy_ipc_state(ipc);
    destroy_postmaster_database(&db);
    return -1;
  }

  atomic_publish(&ipc->ready, 1);
  printf("mygres: postmaster pid=%d anonymous-shared=%l bytes workers=%d\n",
         getpid(), db.shm_bytes, workers);
  printf("mygres: clients rendezvous through %s\n", IPC_PATH);
  printf("mygres: PostgreSQL wire protocol listening on TCP 5432\n");

  // A worker exit is treated as a postmaster-wide failure.  This keeps the
  // small implementation deterministic and avoids serving with fewer slots.
  wait(0);
  atomic_publish(&ipc->ready, 0);
  for(uint i = 0; i < workers; i++)
    if(ipc->slots[i].worker_pid)
      kill(ipc->slots[i].worker_pid);
  if(pgwire_pid > 0)
    kill(pgwire_pid);
  while(wait(0) >= 0)
    ;
  destroy_ipc_state(ipc);
  destroy_postmaster_database(&db);
  return -1;
}

static int
ipc_client_attach(struct ipc_client *client)
{
  int fd;
  memset(client, 0, sizeof(*client));
  client->bytes = PAGE_SIZE;
  fd = open(IPC_PATH, O_RDWR);
  if(fd < 0){
    printf("mygres: server is not running; start /bin/mygresd\n");
    return -1;
  }
  client->state = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
  close(fd);
  if(client->state == MAP_FAILED){
    client->state = 0;
    return -1;
  }
  if(client->state->magic != IPC_MAGIC ||
     client->state->version != DB_VERSION || !client->state->ready ||
     client->state->nworkers == 0 ||
     client->state->nworkers > MAX_WORKERS){
    munmap(client->state, PAGE_SIZE);
    client->state = 0;
    printf("mygres: stale IPC page; restart /bin/mygresd\n");
    return -1;
  }
  client->next_request = ((uint64)(uint32)getpid()) << 32;
  return 0;
}

static void
ipc_client_detach(struct ipc_client *client)
{
  if(client->state){
    munmap(client->state, client->bytes);
    client->state = 0;
  }
}

static int
ipc_submit(struct ipc_client *client, struct ipc_request *request,
           struct ipc_response *response)
{
  struct ipc_slot *slot = 0;

  for(;;){
    if(!client->state->ready)
      return -1;
    for(uint i = 0; i < client->state->nworkers; i++)
      if(atomic_claim(&client->state->slots[i].state,
                      SLOT_IDLE, SLOT_CLAIMED)){
        slot = &client->state->slots[i];
        break;
      }
    if(slot)
      break;
    sched_yield();
  }

  slot->client_pid = getpid();
  slot->request_id = ++client->next_request;
  slot->request = *request;
  memset(&slot->response, 0, sizeof(slot->response));
  atomic_publish(&slot->state, SLOT_REQUEST);
  if(sync_signal(slot->request_sem, 1) < 0 ||
     sync_wait(slot->response_sem) < 0 || slot->state != SLOT_RESPONSE){
    atomic_publish(&slot->state, SLOT_IDLE);
    return -1;
  }
  *response = slot->response;
  atomic_publish(&slot->state, SLOT_IDLE);
  return 0;
}

static int
parse_u64(char *s, uint64 *out)
{
  if(!s || !*s)
    return -1;
  uint64 n = 0;
  for(; *s; s++){
    if(*s < '0' || *s > '9')
      return -1;
    n = n * 10 + (*s - '0');
  }
  *out = n;
  return 0;
}

// --------------------------------------------------------------------------
// PostgreSQL v3 frontend/backend protocol (trust authentication, simple query)
// --------------------------------------------------------------------------

#define PG_MAX_MESSAGE 2048
#define PG_SSL_REQUEST 80877103U
#define PG_GSS_REQUEST 80877104U
#define PG_PROTOCOL_3  196608U

static uint16
pg_be16(uint16 value)
{
  return (value << 8) | (value >> 8);
}

static uint32
pg_be32(uint32 value)
{
  return ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) |
         ((value >> 8) & 0xff00U) | ((value >> 24) & 0xffU);
}

static int
pg_read_all(int fd, void *buffer, int length)
{
  int done = 0;
  while(done < length){
    int n = read(fd, (char *)buffer + done, length - done);
    if(n <= 0)
      return -1;
    done += n;
  }
  return 0;
}

static int
pg_write_all(int fd, void *buffer, int length)
{
  int done = 0;
  while(done < length){
    int n = write(fd, (char *)buffer + done, length - done);
    if(n <= 0)
      return -1;
    done += n;
  }
  return 0;
}

static int
pg_send(int fd, char type, void *payload, uint32 payload_length)
{
  uchar header[5];
  uint32 length = pg_be32(payload_length + 4);
  header[0] = type;
  memmove(header + 1, &length, sizeof(length));
  if(pg_write_all(fd, header, sizeof(header)) < 0)
    return -1;
  return payload_length ? pg_write_all(fd, payload, payload_length) : 0;
}

static int
pg_send_i32(int fd, char type, uint32 value)
{
  value = pg_be32(value);
  return pg_send(fd, type, &value, sizeof(value));
}

static int
pg_send_parameter(int fd, char *name, char *value)
{
  char payload[160];
  uint n = strlen(name) + 1, v = strlen(value) + 1;
  if(n + v > sizeof(payload))
    return -1;
  memmove(payload, name, n);
  memmove(payload + n, value, v);
  return pg_send(fd, 'S', payload, n + v);
}

static int
pg_send_ready(int fd, int transaction)
{
  char status = transaction ? 'T' : 'I';
  return pg_send(fd, 'Z', &status, 1);
}

static int
pg_send_command(int fd, char *tag)
{
  return pg_send(fd, 'C', tag, strlen(tag) + 1);
}

static int
pg_send_error(int fd, char *code, char *message)
{
  char payload[320];
  uint pos = 0;
  payload[pos++] = 'S';
  memmove(payload + pos, "ERROR", 6); pos += 6;
  payload[pos++] = 'V';
  memmove(payload + pos, "ERROR", 6); pos += 6;
  payload[pos++] = 'C';
  memmove(payload + pos, code, strlen(code) + 1); pos += strlen(code) + 1;
  payload[pos++] = 'M';
  uint n = strlen(message) + 1;
  if(pos + n + 1 > sizeof(payload))
    n = sizeof(payload) - pos - 1;
  memmove(payload + pos, message, n); pos += n;
  payload[pos++] = 0;
  return pg_send(fd, 'E', payload, pos);
}

static void
pg_put16(uchar *buffer, uint *position, uint16 value)
{
  value = pg_be16(value);
  memmove(buffer + *position, &value, 2);
  *position += 2;
}

static void
pg_put32(uchar *buffer, uint *position, uint32 value)
{
  value = pg_be32(value);
  memmove(buffer + *position, &value, 4);
  *position += 4;
}

static int
pg_add_field(uchar *buffer, uint *position, char *name, uint32 oid,
             uint16 width)
{
  uint n = strlen(name) + 1;
  if(*position + n + 18 > 512)
    return -1;
  memmove(buffer + *position, name, n); *position += n;
  pg_put32(buffer, position, 0);          // table OID
  pg_put16(buffer, position, 0);          // attribute number
  pg_put32(buffer, position, oid);        // type OID
  pg_put16(buffer, position, width);      // type width (-1 becomes 0xffff)
  pg_put32(buffer, position, 0xffffffffU);// type modifier
  pg_put16(buffer, position, 0);          // text format
  return 0;
}

static int
pg_send_row_description(int fd, int fields, char **names, uint32 *oids,
                        uint16 *widths)
{
  uchar payload[512];
  uint position = 0;
  pg_put16(payload, &position, fields);
  for(int i = 0; i < fields; i++)
    if(pg_add_field(payload, &position, names[i], oids[i], widths[i]) < 0)
      return -1;
  return pg_send(fd, 'T', payload, position);
}

static int
pg_send_data_row(int fd, int fields, char **values)
{
  uchar payload[512];
  uint position = 0;
  pg_put16(payload, &position, fields);
  for(int i = 0; i < fields; i++){
    if(values[i] == 0){
      pg_put32(payload, &position, 0xffffffffU);
      continue;
    }
    uint n = strlen(values[i]);
    if(position + 4 + n > sizeof(payload))
      return -1;
    pg_put32(payload, &position, n);
    memmove(payload + position, values[i], n);
    position += n;
  }
  return pg_send(fd, 'D', payload, position);
}

static void
pg_u64toa(uint64 value, char *buffer)
{
  char reverse[32];
  int n = 0;
  do {
    reverse[n++] = '0' + value % 10;
    value /= 10;
  } while(value);
  for(int i = 0; i < n; i++)
    buffer[i] = reverse[n - i - 1];
  buffer[n] = 0;
}

static int
pg_lower(int c)
{
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static char *
pg_skip_space(char *text)
{
  while(*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    text++;
  return text;
}

static int
pg_starts_with(char *text, char *prefix)
{
  text = pg_skip_space(text);
  while(*prefix)
    if(pg_lower(*text++) != pg_lower(*prefix++))
      return 0;
  return 1;
}

static int
pg_contains(char *text, char *needle)
{
  for(; *text; text++){
    char *a = text, *b = needle;
    while(*b && pg_lower(*a) == pg_lower(*b)){
      a++;
      b++;
    }
    if(*b == 0)
      return 1;
  }
  return 0;
}

static int
pg_numbers(char *text, uint64 *numbers, int maximum)
{
  int count = 0;
  while(*text && count < maximum){
    while(*text && (*text < '0' || *text > '9'))
      text++;
    if(!*text)
      break;
    uint64 value = 0;
    while(*text >= '0' && *text <= '9')
      value = value * 10 + (*text++ - '0');
    numbers[count++] = value;
  }
  return count;
}

static int
pg_send_text_result(int fd, char *name, char *value, char *tag)
{
  char *names[1] = { name };
  char *values[1] = { value };
  uint32 oids[1] = { 25 };                // TEXTOID
  uint16 widths[1] = { 0xffff };
  return pg_send_row_description(fd, 1, names, oids, widths) < 0 ||
         pg_send_data_row(fd, 1, values) < 0 ||
         pg_send_command(fd, tag) < 0 ? -1 : 0;
}

static int
pg_send_kv_result(int fd, struct db_row *rows, uint count)
{
  char *names[2] = { "key", "value" };
  uint32 oids[2] = { 23, 20 };             // INT4OID, INT8OID
  uint16 widths[2] = { 4, 8 };
  if(pg_send_row_description(fd, 2, names, oids, widths) < 0)
    return -1;
  for(uint i = 0; i < count; i++){
    char key[24], value[32];
    char *values[2] = { key, value };
    pg_u64toa(rows[i].key, key);
    pg_u64toa(rows[i].value, value);
    if(pg_send_data_row(fd, 2, values) < 0)
      return -1;
  }
  char tag[32] = "SELECT ";
  pg_u64toa(count, tag + 7);
  return pg_send_command(fd, tag);
}

static int
pg_execute_select(int fd, struct database *db, char *sql)
{
  uint64 number[4], value, txid;
  int n = pg_numbers(sql, number, 4);
  if(pg_contains(sql, "version()"))
    return pg_send_text_result(fd, "version",
      "mygres 0.1 (PostgreSQL 16.0 wire protocol)", "SELECT 1");
  if(pg_contains(sql, "set_config"))
    return pg_send_text_result(fd, "set_config", "", "SELECT 1");
  if(!pg_contains(sql, "from kv")){
    char value_buffer[32];
    pg_u64toa(n ? number[0] : 1, value_buffer);
    return pg_send_text_result(fd, "?column?", value_buffer, "SELECT 1");
  }
  if(pg_contains(sql, "where")){
    struct db_row row;
    memset(&row, 0, sizeof(row));
    if(n == 0 || number[n - 1] > 0xffffffffU)
      return pg_send_error(fd, "22P02", "invalid key");
    int found = db_get(db, number[n - 1], &value, &txid);
    if(found == 0){
      row.state = ROW_USED;
      row.key = number[n - 1];
      row.value = value;
      return pg_send_kv_result(fd, &row, 1);
    }
    return pg_send_kv_result(fd, &row, 0);
  }

  struct db_row *snapshot = malloc(MAX_ROWS * sizeof(struct db_row));
  if(snapshot == 0)
    return pg_send_error(fd, "53200", "out of memory");
  uint count = 0;
  shared_lock(&db->shm->lock);
  for(uint i = 0; i < MAX_ROWS; i++)
    if(db->shm->rows[i].state == ROW_USED)
      snapshot[count++] = db->shm->rows[i];
  shared_unlock(&db->shm->lock);
  int result = pg_send_kv_result(fd, snapshot, count);
  free(snapshot);
  return result;
}

static int
pg_auto_change(struct database *db, uint type, uint32 key, uint64 value)
{
  struct transaction tx;
  memset(&tx, 0, sizeof(tx));
  tx.active = 1;
  return tx_stage(&tx, type, key, value) < 0 ? -1 :
         tx_commit(db, &tx, 0);
}

static int
pg_execute_query(int fd, struct database *db, struct transaction *tx,
                 char *sql)
{
  uint64 number[4];
  int n;
  sql = pg_skip_space(sql);
  if(*sql == 0)
    return pg_send(fd, 'I', 0, 0);
  if(pg_starts_with(sql, "begin") || pg_starts_with(sql, "start transaction")){
    if(tx->active)
      return pg_send_error(fd, "25001", "transaction already active");
    memset(tx, 0, sizeof(*tx));
    tx->active = 1;
    return pg_send_command(fd, "BEGIN");
  }
  if(pg_starts_with(sql, "commit")){
    int status = tx->active ? tx_commit(db, tx, 0) : 0;
    return status < 0 ? pg_send_error(fd, "XX000", "commit failed") :
                        pg_send_command(fd, "COMMIT");
  }
  if(pg_starts_with(sql, "rollback")){
    memset(tx, 0, sizeof(*tx));
    return pg_send_command(fd, "ROLLBACK");
  }
  if(pg_starts_with(sql, "select"))
    return pg_execute_select(fd, db, sql);
  if(pg_starts_with(sql, "show"))
    return pg_send_text_result(fd, "setting", "on", "SHOW");

  n = pg_numbers(sql, number, 4);
  if(pg_starts_with(sql, "insert") && n >= 2 && number[0] <= 0xffffffffU){
    int status = tx->active ? tx_stage(tx, OP_PUT, number[0], number[1]) :
                              pg_auto_change(db, OP_PUT, number[0], number[1]);
    return status < 0 ? pg_send_error(fd, "XX000", "insert failed") :
                        pg_send_command(fd, "INSERT 0 1");
  }
  if(pg_starts_with(sql, "update") && n >= 2 && number[1] <= 0xffffffffU){
    int status = tx->active ? tx_stage(tx, OP_PUT, number[1], number[0]) :
                              pg_auto_change(db, OP_PUT, number[1], number[0]);
    return status < 0 ? pg_send_error(fd, "XX000", "update failed") :
                        pg_send_command(fd, "UPDATE 1");
  }
  if(pg_starts_with(sql, "delete") && n >= 1 && number[n - 1] <= 0xffffffffU){
    int status = tx->active ? tx_stage(tx, OP_DELETE, number[n - 1], 0) :
                              pg_auto_change(db, OP_DELETE, number[n - 1], 0);
    return status < 0 ? pg_send_error(fd, "XX000", "delete failed") :
                        pg_send_command(fd, "DELETE 1");
  }
  if(pg_starts_with(sql, "create table"))
    return pg_send_command(fd, "CREATE TABLE");
  return pg_send_error(fd, "0A000", "SQL feature not supported by mygres");
}

static int
pg_startup(int fd)
{
  uchar message[PG_MAX_MESSAGE];
  for(;;){
    uint32 length, code;
    if(pg_read_all(fd, &length, 4) < 0)
      return -1;
    length = pg_be32(length);
    if(length < 8 || length > sizeof(message))
      return -1;
    if(pg_read_all(fd, message, length - 4) < 0)
      return -1;
    memmove(&code, message, 4);
    code = pg_be32(code);
    if(code == PG_SSL_REQUEST || code == PG_GSS_REQUEST){
      char no = 'N';
      if(pg_write_all(fd, &no, 1) < 0)
        return -1;
      continue;
    }
    if(code != PG_PROTOCOL_3){
      pg_send_error(fd, "08P01", "only PostgreSQL protocol 3.0 is supported");
      return -1;
    }
    break;
  }

  if(pg_send_i32(fd, 'R', 0) < 0 ||
     pg_send_parameter(fd, "server_version", "16.0-mygres") < 0 ||
     pg_send_parameter(fd, "server_encoding", "UTF8") < 0 ||
     pg_send_parameter(fd, "client_encoding", "UTF8") < 0 ||
     pg_send_parameter(fd, "DateStyle", "ISO, MDY") < 0 ||
     pg_send_parameter(fd, "integer_datetimes", "on") < 0 ||
     pg_send_parameter(fd, "standard_conforming_strings", "on") < 0)
    return -1;
  uint32 backend_key[2] = { pg_be32(getpid()), pg_be32((uint32)clock_us()) };
  return pg_send(fd, 'K', backend_key, sizeof(backend_key)) < 0 ? -1 :
         pg_send_ready(fd, 0);
}

static void
pgwire_connection(int fd, struct database *db)
{
  uchar message[PG_MAX_MESSAGE];
  struct transaction tx;
  memset(&tx, 0, sizeof(tx));
  if(pg_startup(fd) < 0)
    return;
  for(;;){
    char type;
    uint32 length;
    if(pg_read_all(fd, &type, 1) < 0 || pg_read_all(fd, &length, 4) < 0)
      return;
    length = pg_be32(length);
    if(length < 4 || length - 4 >= sizeof(message))
      return;
    if(pg_read_all(fd, message, length - 4) < 0)
      return;
    message[length - 4] = 0;
    if(type == 'X')
      return;
    if(type != 'Q'){
      if(pg_send_error(fd, "0A000",
          "use PostgreSQL simple-query mode; extended protocol is not implemented") < 0 ||
         pg_send_ready(fd, tx.active) < 0)
        return;
      continue;
    }
    if(pg_execute_query(fd, db, &tx, (char *)message) < 0 ||
       pg_send_ready(fd, tx.active) < 0)
      return;
  }
}

static void
pgwire_listener_loop(struct database *db)
{
  int listener = socket_listen(5432, 8);
  if(listener < 0){
    printf("mygres: cannot listen on PostgreSQL port 5432\n");
    exit(1);
  }
  printf("mygres: pgwire listener pid=%d port=5432 auth=trust\n", getpid());
  for(;;){
    int connection = socket_accept(listener);
    if(connection < 0)
      break;
    int pid = fork();
    if(pid == 0){
      close(listener);
      pgwire_connection(connection, db);
      close(connection);
      exit(0);
    }
    close(connection);
    if(pid < 0)
      printf("mygres: cannot fork remote backend\n");
    while(waitpid(-1, 0, 1) > 0)
      ;
  }
  close(listener);
  exit(1);
}

static int
split_words(char *line, char **words, int max)
{
  int n = 0;
  char *p = line;
  while(*p && n < max){
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      *p++ = 0;
    if(!*p)
      break;
    words[n++] = p;
    while(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      p++;
  }
  if(*p)
    *p = 0;
  return n;
}

static void
shell_help(void)
{
  printf("BEGIN                         start transaction\n");
  printf("PUT key value                 stage/commit an indexed row\n");
  printf("GET key                       index lookup\n");
  printf("DELETE key                    stage/commit a tombstone\n");
  printf("COMMIT | ROLLBACK             finish transaction\n");
  printf("SCAN | CHECK | STATS          inspect database\n");
  printf("QUIT                          leave shell\n");
}

static int
submit_transaction(struct ipc_client *client, struct transaction *tx,
                   int wal_only, struct ipc_response *response)
{
  struct ipc_request request;
  memset(&request, 0, sizeof(request));
  request.command = wal_only ? IPC_PREPARE : IPC_COMMIT;
  request.nops = tx->nops;
  for(uint i = 0; i < tx->nops; i++)
    request.ops[i] = tx->ops[i];
  if(ipc_submit(client, &request, response) < 0)
    return -1;
  tx->active = 0;
  return response->status;
}

static int
submit_one(struct ipc_client *client, uint32 type, uint32 key, uint64 value,
           int wal_only, struct ipc_response *response)
{
  struct transaction tx;
  memset(&tx, 0, sizeof(tx));
  tx.active = 1;
  if(tx_stage(&tx, type, key, value) < 0)
    return -1;
  return submit_transaction(client, &tx, wal_only, response);
}

static int
submit_simple(struct ipc_client *client, uint32 command,
              struct ipc_response *response)
{
  struct ipc_request request;
  memset(&request, 0, sizeof(request));
  request.command = command;
  return ipc_submit(client, &request, response) < 0 ? -1 : response->status;
}

static int
submit_get(struct ipc_client *client, uint32 key,
           struct ipc_response *response)
{
  struct ipc_request request;
  memset(&request, 0, sizeof(request));
  request.command = IPC_GET;
  request.key = key;
  return ipc_submit(client, &request, response) < 0 ? -1 : response->status;
}

static int
run_shell(struct ipc_client *client)
{
  char line[160];
  char *word[4];
  struct transaction tx;
  memset(&tx, 0, sizeof(tx));
  printf("mygres shell: PostgreSQL-style read-committed transactions\n");
  shell_help();

  for(;;){
    printf(tx.active ? "mygres=*# " : "mygres=# ");
    if(gets(line, sizeof(line)) == 0)
      break;
    int n = split_words(line, word, 4);
    if(n == 0)
      continue;
    if(strcmp(word[0], "BEGIN") == 0 || strcmp(word[0], "begin") == 0){
      if(tx.active)
        printf("ERROR: transaction already active\n");
      else {
        memset(&tx, 0, sizeof(tx));
        tx.active = 1;
        printf("BEGIN\n");
      }
    } else if(strcmp(word[0], "COMMIT") == 0 || strcmp(word[0], "commit") == 0){
      if(!tx.active)
        printf("ERROR: no active transaction\n");
      else {
        struct ipc_response response;
        if(submit_transaction(client, &tx, 0, &response) == 0)
        printf("COMMIT\n");
        else
          printf("ERROR: commit failed\n");
      }
    } else if(strcmp(word[0], "ROLLBACK") == 0 || strcmp(word[0], "rollback") == 0){
      if(!tx.active)
        printf("ERROR: no active transaction\n");
      else {
        memset(&tx, 0, sizeof(tx));
        printf("ROLLBACK\n");
      }
    } else if((strcmp(word[0], "PUT") == 0 || strcmp(word[0], "put") == 0) && n == 3){
      uint64 key, value;
      if(parse_u64(word[1], &key) < 0 || key > 0xffffffffU ||
         parse_u64(word[2], &value) < 0)
        printf("ERROR: PUT expects an unsigned 32-bit key and integer value\n");
      else if(tx.active){
        if(tx_stage(&tx, OP_PUT, key, value) < 0)
          printf("ERROR: transaction has too many operations\n");
        else
          printf("PUT 1\n");
      } else {
        struct ipc_response response;
        if(submit_one(client, OP_PUT, key, value, 0, &response) == 0)
          printf("PUT 1\n");
        else
          printf("ERROR: PUT failed\n");
      }
    } else if((strcmp(word[0], "DELETE") == 0 || strcmp(word[0], "delete") == 0) && n == 2){
      uint64 key;
      if(parse_u64(word[1], &key) < 0 || key > 0xffffffffU)
        printf("ERROR: DELETE expects an unsigned 32-bit key\n");
      else if(tx.active){
        if(tx_stage(&tx, OP_DELETE, key, 0) < 0)
          printf("ERROR: transaction has too many operations\n");
        else
          printf("DELETE staged\n");
      } else {
        struct ipc_response response;
        if(submit_one(client, OP_DELETE, key, 0, 0, &response) == 0)
          printf("DELETE\n");
        else
          printf("ERROR: DELETE failed\n");
      }
    } else if((strcmp(word[0], "GET") == 0 || strcmp(word[0], "get") == 0) && n == 2){
      uint64 key, value, xmin;
      if(parse_u64(word[1], &key) < 0 || key > 0xffffffffU)
        printf("ERROR: GET expects an unsigned 32-bit key\n");
      else {
        int r = 1, staged = 0;
        if(tx.active)
          for(int i = (int)tx.nops - 1; i >= 0; i--)
            if(tx.ops[i].key == key){
              staged = 1;
              r = tx.ops[i].type == OP_DELETE ? 1 : 0;
              value = tx.ops[i].value;
              xmin = 0;       // uncommitted row has no server txid yet
              break;
            }
        if(!staged){
          struct ipc_response response;
          r = submit_get(client, key, &response);
          value = response.value;
          xmin = response.txid;
        }
        if(r == 0)
          printf(" key | value | xmin\n %d | %l | %l\n(1 row)\n",
                 (uint32)key, value, xmin);
        else
          printf("(0 rows)\n");
      }
    } else if(strcmp(word[0], "SCAN") == 0 || strcmp(word[0], "scan") == 0){
      struct ipc_response response;
      submit_simple(client, IPC_SCAN, &response);
    } else if(strcmp(word[0], "CHECK") == 0 || strcmp(word[0], "check") == 0){
      struct ipc_response response;
      submit_simple(client, IPC_CHECK, &response);
    } else if(strcmp(word[0], "STATS") == 0 || strcmp(word[0], "stats") == 0){
      struct ipc_response response;
      if(submit_simple(client, IPC_STATS, &response) < 0){
        printf("ERROR: STATS failed\n");
        continue;
      }
      printf("txid=%l rows=%l commits=%l recoveries=%l index-hit=%l miss=%l\n",
             response.txid, response.rows, response.commits,
             response.recoveries, response.index_hits,
             response.index_misses);
    } else if(strcmp(word[0], "HELP") == 0 || strcmp(word[0], "help") == 0){
      shell_help();
    } else if(strcmp(word[0], "QUIT") == 0 || strcmp(word[0], "quit") == 0 ||
              strcmp(word[0], "\\q") == 0){
      if(tx.active)
        printf("ROLLBACK (shell closed with active transaction)\n");
      return 0;
    } else {
      printf("ERROR: unknown command; type HELP\n");
    }
  }
  return 0;
}

static int
stress_database(struct ipc_client *client, uint workers, uint operations)
{
  if(workers == 0 || workers > 8 || operations == 0)
    return -1;
  uint64 start = clock_us();
  for(uint w = 0; w < workers; w++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      client->next_request = ((uint64)(uint32)getpid()) << 32;
      for(uint i = 0; i < operations; i++){
        uint32 key = (w * operations + i) % (MAX_ROWS / 2);
        uint64 value = (uint64)w * 1000000UL + i;
        struct ipc_response response;
        if(submit_one(client, OP_PUT, key, value, 0, &response) < 0)
          exit(1);
        if((i & 31U) == 0){
          if(submit_get(client, key, &response) != 0 ||
             response.value != value)
            exit(1);
        }
      }
      exit(0);
    }
  }
  int failed = 0;
  for(uint w = 0; w < workers; w++){
    int status = 0;
    wait(&status);
    if(status != 0)
      failed = 1;
  }
  uint64 elapsed = clock_us() - start;
  printf("mygres: stress workers=%d commits=%d elapsed=%l us %s\n",
         workers, workers * operations, elapsed, failed ? "FAIL" : "PASS");
  if(failed)
    return -1;
  struct ipc_response response;
  return submit_simple(client, IPC_CHECK, &response);
}

static void
usage(void)
{
  printf("usage: mygres init|server [WORKERS]|status|shell\n");
  printf("              put KEY VALUE|get KEY|delete KEY|scan\n");
  printf("              check|stats|recover|prepare KEY VALUE\n");
  printf("              stress [WORKERS [TRANSACTIONS_PER_WORKER]]\n");
}

int
main(int argc, char **argv)
{
  if(argc < 2){
    usage();
    exit(1);
  }
  if(strcmp(argv[1], "init") == 0)
    exit(initialize_database() < 0);
  if(strcmp(argv[1], "server") == 0){
    uint workers = argc >= 3 ? atoi(argv[2]) : MAX_WORKERS;
    exit(run_postmaster(workers) < 0);
  }

  struct ipc_client client;
  if(ipc_client_attach(&client) < 0)
    exit(1);

  int r = -1;
  struct ipc_response response;
  if(strcmp(argv[1], "status") == 0){
    r = submit_simple(&client, IPC_PING, &response);
    if(r == 0)
      printf("mygres: postmaster pid=%d workers=%d ready\n",
             client.state->postmaster_pid, client.state->nworkers);
  } else if(strcmp(argv[1], "shell") == 0)
    r = run_shell(&client);
  else if(strcmp(argv[1], "put") == 0 && argc == 4){
    uint64 key, value;
    if(parse_u64(argv[2], &key) == 0 && key <= 0xffffffffU &&
       parse_u64(argv[3], &value) == 0)
      r = submit_one(&client, OP_PUT, key, value, 0, &response);
  } else if(strcmp(argv[1], "get") == 0 && argc == 3){
    uint64 key;
    if(parse_u64(argv[2], &key) == 0 && key <= 0xffffffffU){
      r = submit_get(&client, key, &response);
      if(r == 0)
        printf("mygres: key=%d value=%l xmin=%l\n", (uint32)key,
               response.value, response.txid);
      else if(r == 1)
        printf("mygres: key=%d not found\n", (uint32)key);
    }
  } else if(strcmp(argv[1], "delete") == 0 && argc == 3){
    uint64 key;
    if(parse_u64(argv[2], &key) == 0 && key <= 0xffffffffU)
      r = submit_one(&client, OP_DELETE, key, 0, 0, &response);
  } else if(strcmp(argv[1], "scan") == 0)
    r = submit_simple(&client, IPC_SCAN, &response);
  else if(strcmp(argv[1], "check") == 0)
    r = submit_simple(&client, IPC_CHECK, &response);
  else if(strcmp(argv[1], "stats") == 0){
    r = submit_simple(&client, IPC_STATS, &response);
    if(r == 0)
      printf("mygres: txid=%l rows=%l commits=%l recoveries=%l index-hit=%l miss=%l\n",
             response.txid, response.rows, response.commits,
             response.recoveries, response.index_hits,
             response.index_misses);
  } else if(strcmp(argv[1], "recover") == 0){
    r = submit_simple(&client, IPC_RECOVER, &response);
  } else if(strcmp(argv[1], "prepare") == 0 && argc == 4){
    uint64 key, value;
    if(parse_u64(argv[2], &key) == 0 && key <= 0xffffffffU &&
       parse_u64(argv[3], &value) == 0)
      r = submit_one(&client, OP_PUT, key, value, 1, &response);
  } else if(strcmp(argv[1], "stress") == 0){
    uint workers = argc >= 3 ? atoi(argv[2]) : 4;
    uint operations = argc >= 4 ? atoi(argv[3]) : 50;
    r = stress_database(&client, workers, operations);
  } else {
    usage();
  }

  ipc_client_detach(&client);
  exit(r < 0);
}
