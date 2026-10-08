#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define DB_DIR       "/mnt/ext2/minisqlite"
#define DB_PATH      "/mnt/ext2/minisqlite/main.db"
#define WAL_PATH     "/mnt/ext2/minisqlite/main.wal"
#define PAGE_SIZE    4096U
#define DB_PAGES     256U
#define SLOT_SIZE    64U
#define SLOTS_PER_PAGE (PAGE_SIZE / SLOT_SIZE)
#define TOTAL_SLOTS  (DB_PAGES * SLOTS_PER_PAGE)
#define DB_MAGIC     0x4d534442U  /* MSDB */
#define WAL_MAGIC    0x4d53574cU  /* MSWL */
#define DB_VERSION   1U
#define SLOT_FREE    0U
#define SLOT_USED    1U
#define SLOT_TOMB    2U

struct db_header {
  uint32 magic;
  uint32 version;
  uint32 page_size;
  uint32 pages;
  uint64 txid;
  uint64 count;
  uint32 checksum;
  uchar pad[PAGE_SIZE - 36];
};

struct db_slot {
  uint32 state;
  uint32 key;
  uint64 value;
  uint64 txid;
  uint32 checksum;
  uchar pad[SLOT_SIZE - 28];
};

struct wal_record {
  uint32 magic;
  uint32 version;
  uint32 committed;
  uint32 page_index;
  uint32 slot_index;
  uint32 reserved;
  uint64 txid;
  uint64 count;
  struct db_slot slot;
  uint32 checksum;
  uchar pad[PAGE_SIZE - 108];
};

typedef char header_size_must_be_page[(sizeof(struct db_header) == PAGE_SIZE) ? 1 : -1];
typedef char slot_size_must_be_64[(sizeof(struct db_slot) == SLOT_SIZE) ? 1 : -1];
typedef char wal_size_must_be_page[(sizeof(struct wal_record) == PAGE_SIZE) ? 1 : -1];

static struct db_header hdr;
static struct db_slot slotbuf;
static struct db_slot pagebuf[SLOTS_PER_PAGE];
static struct wal_record walbuf;
static uchar expected_valid[512];
static uint64 expected_value[512];

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
         h->page_size == PAGE_SIZE && h->pages == DB_PAGES && saved == got;
}

static void
seal_slot(struct db_slot *s)
{
  s->checksum = 0;
  s->checksum = hash_bytes(s, sizeof(*s));
}

static int
valid_slot(struct db_slot *s)
{
  // A freshly ftruncate()d sparse database reads as zero-filled pages.  Treat
  // an entirely zero slot as the canonical never-used SLOT_FREE value.
  uchar *p = (uchar*)s;
  int allzero = 1;
  for(uint i = 0; i < sizeof(*s); i++)
    if(p[i] != 0){
      allzero = 0;
      break;
    }
  if(allzero)
    return 1;
  uint32 saved = s->checksum;
  s->checksum = 0;
  uint32 got = hash_bytes(s, sizeof(*s));
  s->checksum = saved;
  return (s->state == SLOT_FREE || s->state == SLOT_USED || s->state == SLOT_TOMB) &&
         saved == got;
}

static void
seal_wal(struct wal_record *w)
{
  w->checksum = 0;
  w->checksum = hash_bytes(w, sizeof(*w));
}

static int
valid_wal(struct wal_record *w)
{
  uint32 saved = w->checksum;
  w->checksum = 0;
  uint32 got = hash_bytes(w, sizeof(*w));
  w->checksum = saved;
  return w->magic == WAL_MAGIC && w->version == DB_VERSION &&
         w->committed == 1 && w->page_index < DB_PAGES &&
         w->slot_index < SLOTS_PER_PAGE && valid_slot(&w->slot) && saved == got;
}

static uint64
slot_offset(uint slotno)
{
  return (uint64)PAGE_SIZE + (uint64)slotno * SLOT_SIZE;
}

static int
read_header(int fd, struct db_header *h)
{
  return pread(fd, h, sizeof(*h), 0) == sizeof(*h) && valid_header(h) ? 0 : -1;
}

static int
read_slot(int fd, uint slotno, struct db_slot *s)
{
  if(pread(fd, s, sizeof(*s), slot_offset(slotno)) != sizeof(*s))
    return -1;
  return valid_slot(s) ? 0 : -1;
}

static int
clear_wal(void)
{
  int fd = open(WAL_PATH, O_CREATE | O_RDWR);
  if(fd < 0)
    return -1;
  int ok = ftruncate(fd, 0) == 0 && fsync(fd) == 0;
  close(fd);
  return ok ? 0 : -1;
}

static int
recover_db(int verbose)
{
  int wfd = open(WAL_PATH, O_RDONLY);
  if(wfd < 0)
    return 0;
  int n = pread(wfd, &walbuf, sizeof(walbuf), 0);
  close(wfd);
  if(n == 0)
    return 0;
  if(n != sizeof(walbuf) || !valid_wal(&walbuf)){
    printf("minisqlite: ignoring incomplete/corrupt WAL (%d bytes)\n", n);
    return clear_wal();
  }

  int dbfd = open(DB_PATH, O_RDWR);
  if(dbfd < 0 || read_header(dbfd, &hdr) < 0){
    if(dbfd >= 0) close(dbfd);
    return -1;
  }
  // Always redo a valid WAL.  Even when the header reached storage first, the
  // target slot may still be old after a torn/power-failed pair of DB writes.
  // Rewriting both is idempotent and only then may the WAL be cleared.
  uint slotno = walbuf.page_index * SLOTS_PER_PAGE + walbuf.slot_index;
  if(pwrite(dbfd, &walbuf.slot, sizeof(walbuf.slot), slot_offset(slotno)) !=
       sizeof(walbuf.slot)){
    close(dbfd);
    return -1;
  }
  hdr.txid = walbuf.txid;
  hdr.count = walbuf.count;
  seal_header(&hdr);
  if(pwrite(dbfd, &hdr, sizeof(hdr), 0) != sizeof(hdr) || fsync(dbfd) < 0){
    close(dbfd);
    return -1;
  }
  if(verbose)
    printf("minisqlite: WAL REDO tx=%l page=%d slot=%d\n",
           walbuf.txid, walbuf.page_index, walbuf.slot_index);
  close(dbfd);
  return clear_wal();
}

static int
initialize(void)
{
  mkdir(DB_DIR);
  int fd = open(DB_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0)
    return -1;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = DB_MAGIC;
  hdr.version = DB_VERSION;
  hdr.page_size = PAGE_SIZE;
  hdr.pages = DB_PAGES;
  seal_header(&hdr);
  uint64 bytes = (uint64)PAGE_SIZE + (uint64)DB_PAGES * PAGE_SIZE;
  int ok = ftruncate(fd, bytes) == 0 &&
           pwrite(fd, &hdr, sizeof(hdr), 0) == sizeof(hdr) && fsync(fd) == 0;
  close(fd);
  if(!ok || clear_wal() < 0)
    return -1;
  printf("minisqlite: initialized %s pages=%d slots=%d bytes=%l\n",
         DB_PATH, DB_PAGES, TOTAL_SLOTS, bytes);
  return 0;
}

// Returns 1 when found, 0 when absent and -1 on corruption/I/O error.  For an
// absent key, *slotno receives the first tombstone or empty slot for insertion.
static int
find_key(int fd, uint32 key, uint *slotno, struct db_slot *found)
{
  uint start = (key * 2654435761U) % TOTAL_SLOTS;
  uint first_tomb = TOTAL_SLOTS;
  for(uint probe = 0; probe < TOTAL_SLOTS; probe++){
    uint pos = (start + probe) % TOTAL_SLOTS;
    if(read_slot(fd, pos, &slotbuf) < 0)
      return -1;
    if(slotbuf.state == SLOT_USED && slotbuf.key == key){
      *slotno = pos;
      if(found) *found = slotbuf;
      return 1;
    }
    if(slotbuf.state == SLOT_TOMB && first_tomb == TOTAL_SLOTS)
      first_tomb = pos;
    if(slotbuf.state == SLOT_FREE){
      *slotno = first_tomb != TOTAL_SLOTS ? first_tomb : pos;
      return 0;
    }
  }
  if(first_tomb != TOTAL_SLOTS){
    *slotno = first_tomb;
    return 0;
  }
  return -1;
}

static int
commit_slot(int dbfd, uint slotno, struct db_slot *s, uint64 newcount,
            int stop_after_wal)
{
  memset(&walbuf, 0, sizeof(walbuf));
  walbuf.magic = WAL_MAGIC;
  walbuf.version = DB_VERSION;
  walbuf.committed = 1;
  walbuf.page_index = slotno / SLOTS_PER_PAGE;
  walbuf.slot_index = slotno % SLOTS_PER_PAGE;
  walbuf.txid = s->txid;
  walbuf.count = newcount;
  walbuf.slot = *s;
  seal_wal(&walbuf);

  int wfd = open(WAL_PATH, O_CREATE | O_RDWR | O_TRUNC);
  if(wfd < 0 || pwrite(wfd, &walbuf, sizeof(walbuf), 0) != sizeof(walbuf) ||
     fsync(wfd) < 0){
    if(wfd >= 0) close(wfd);
    return -1;
  }
  close(wfd);
  if(stop_after_wal){
    printf("minisqlite: WAL durable tx=%l; cut power now\n", s->txid);
    return 0;
  }

  if(pwrite(dbfd, s, sizeof(*s), slot_offset(slotno)) != sizeof(*s))
    return -1;
  hdr.txid = s->txid;
  hdr.count = newcount;
  seal_header(&hdr);
  if(pwrite(dbfd, &hdr, sizeof(hdr), 0) != sizeof(hdr) || fsync(dbfd) < 0)
    return -1;
  return clear_wal();
}

static int
put_value(uint32 key, uint64 value, int stop_after_wal)
{
  if(recover_db(0) < 0)
    return -1;
  int fd = open(DB_PATH, O_RDWR);
  if(fd < 0 || read_header(fd, &hdr) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  uint slotno;
  int found = find_key(fd, key, &slotno, 0);
  if(found < 0){
    close(fd);
    return -1;
  }
  memset(&slotbuf, 0, sizeof(slotbuf));
  slotbuf.state = SLOT_USED;
  slotbuf.key = key;
  slotbuf.value = value;
  slotbuf.txid = hdr.txid + 1;
  seal_slot(&slotbuf);
  uint64 count = hdr.count + (found ? 0 : 1);
  int r = commit_slot(fd, slotno, &slotbuf, count, stop_after_wal);
  close(fd);
  return r;
}

static int
get_value(uint32 key, uint64 *value)
{
  if(recover_db(0) < 0)
    return -1;
  int fd = open(DB_PATH, O_RDONLY);
  if(fd < 0 || read_header(fd, &hdr) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  uint slotno;
  int found = find_key(fd, key, &slotno, &slotbuf);
  close(fd);
  if(found == 1){
    *value = slotbuf.value;
    return 0;
  }
  return found == 0 ? 1 : -1;
}

static int
delete_key(uint32 key)
{
  if(recover_db(0) < 0)
    return -1;
  int fd = open(DB_PATH, O_RDWR);
  if(fd < 0 || read_header(fd, &hdr) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  uint slotno;
  int found = find_key(fd, key, &slotno, &slotbuf);
  if(found <= 0){
    close(fd);
    return found == 0 ? 1 : -1;
  }
  memset(&slotbuf, 0, sizeof(slotbuf));
  slotbuf.state = SLOT_TOMB;
  slotbuf.key = key;
  slotbuf.txid = hdr.txid + 1;
  seal_slot(&slotbuf);
  int r = commit_slot(fd, slotno, &slotbuf, hdr.count - 1, 0);
  close(fd);
  return r;
}

static int
scan_db(int print_rows)
{
  if(recover_db(0) < 0)
    return -1;
  int fd = open(DB_PATH, O_RDONLY);
  if(fd < 0 || read_header(fd, &hdr) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  uint64 used = 0, tomb = 0;
  for(uint page = 0; page < DB_PAGES; page++){
    uint64 off = (uint64)PAGE_SIZE + (uint64)page * PAGE_SIZE;
    if(pread(fd, pagebuf, PAGE_SIZE, off) != PAGE_SIZE){
      printf("minisqlite: cannot read page=%d\n", page);
      close(fd);
      return -1;
    }
    for(uint j = 0; j < SLOTS_PER_PAGE; j++){
      struct db_slot *s = &pagebuf[j];
      if(!valid_slot(s)){
        printf("minisqlite: corrupt slot=%d page=%d\n", j, page);
        close(fd);
        return -1;
      }
      if(s->state == SLOT_USED){
        used++;
        if(print_rows)
          printf("key=%d value=%l tx=%l page=%d slot=%d\n", s->key,
                 s->value, s->txid, page, j);
      } else if(s->state == SLOT_TOMB) {
        tomb++;
      }
    }
  }
  close(fd);
  printf("minisqlite: tx=%l header-count=%l scanned=%l tombstones=%l\n",
         hdr.txid, hdr.count, used, tomb);
  return used == hdr.count ? 0 : -1;
}

static uint32
next_random(uint32 *state)
{
  *state = *state * 1664525U + 1013904223U;
  return *state;
}

static int
stress_test(uint operations)
{
  if(operations == 0)
    operations = 1000;
  if(initialize() < 0)
    return -1;
  memset(expected_valid, 0, sizeof(expected_valid));
  memset(expected_value, 0, sizeof(expected_value));
  uint32 rnd = 0x76015647U;
  uint64 start = clock_us();
  for(uint i = 0; i < operations; i++){
    uint32 r = next_random(&rnd);
    uint32 key = r % 512;
    uint op = (r >> 16) % 10;
    if(op < 7){
      uint64 value = ((uint64)i << 32) | next_random(&rnd);
      if(put_value(key, value, 0) < 0)
        goto fail;
      expected_valid[key] = 1;
      expected_value[key] = value;
    } else if(op < 9){
      uint64 value = 0;
      int got = get_value(key, &value);
      if((expected_valid[key] && (got != 0 || value != expected_value[key])) ||
         (!expected_valid[key] && got != 1))
        goto fail;
    } else {
      int got = delete_key(key);
      if((expected_valid[key] && got != 0) || (!expected_valid[key] && got != 1))
        goto fail;
      expected_valid[key] = 0;
    }
    if((i + 1) % 100 == 0)
      printf("minisqlite: stress %d/%d transactions\n", i + 1, operations);
  }
  for(uint key = 0; key < 512; key++){
    uint64 value = 0;
    int got = get_value(key, &value);
    if((expected_valid[key] && (got != 0 || value != expected_value[key])) ||
       (!expected_valid[key] && got != 1))
      goto fail;
  }
  if(scan_db(0) < 0)
    goto fail;
  uint64 elapsed = clock_us() - start;
  printf("minisqlite: PASS %d transactions in %l us\n", operations, elapsed);
  return 0;
fail:
  printf("minisqlite: FAIL during transaction/check\n");
  return -1;
}

static void
usage(void)
{
  printf("usage: minisqlite init|put KEY VALUE|get KEY|delete KEY|scan|check\n");
  printf("                  insert KEY VALUE|select KEY|select all\n");
  printf("                  recover|prepare KEY VALUE|stress [OPERATIONS]\n");
}

int
main(int argc, char **argv)
{
  if(argc < 2){
    usage();
    exit(1);
  }
  int r = -1;
  if(strcmp(argv[1], "init") == 0)
    r = initialize();
  else if((strcmp(argv[1], "put") == 0 || strcmp(argv[1], "insert") == 0) && argc == 4){
    r = put_value((uint32)atoi(argv[2]), (uint64)atoi(argv[3]), 0);
    if(r == 0) printf("minisqlite: put key=%d value=%d\n", atoi(argv[2]), atoi(argv[3]));
  } else if((strcmp(argv[1], "get") == 0 || strcmp(argv[1], "select") == 0) &&
            argc == 3 && strcmp(argv[2], "all") != 0){
    uint64 value = 0;
    r = get_value((uint32)atoi(argv[2]), &value);
    if(r == 0) printf("minisqlite: key=%d value=%l\n", atoi(argv[2]), value);
    else if(r == 1) printf("minisqlite: key=%d not found\n", atoi(argv[2]));
  } else if(strcmp(argv[1], "delete") == 0 && argc == 3){
    r = delete_key((uint32)atoi(argv[2]));
    if(r == 0) printf("minisqlite: deleted key=%d\n", atoi(argv[2]));
    else if(r == 1) printf("minisqlite: key=%d not found\n", atoi(argv[2]));
  } else if(strcmp(argv[1], "scan") == 0 ||
            (strcmp(argv[1], "select") == 0 && argc == 3 && strcmp(argv[2], "all") == 0))
    r = scan_db(1);
  else if(strcmp(argv[1], "check") == 0)
    r = scan_db(0);
  else if(strcmp(argv[1], "recover") == 0){
    r = recover_db(1);
    if(r == 0) r = scan_db(0);
  } else if(strcmp(argv[1], "prepare") == 0 && argc == 4)
    r = put_value((uint32)atoi(argv[2]), (uint64)atoi(argv[3]), 1);
  else if(strcmp(argv[1], "stress") == 0)
    r = stress_test(argc == 3 ? (uint)atoi(argv[2]) : 1000);
  else
    usage();
  exit(r < 0);
}
