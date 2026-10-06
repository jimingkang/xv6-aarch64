#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define WALDIR  "/mnt/ext2/waltest"
#define WALFILE "/mnt/ext2/waltest/redo.wal"
#define DBFILE  "/mnt/ext2/waltest/data.db"
#define MAGIC   0x57414c31U

struct record {
  uint32 magic;
  uint32 kind;
  uint64 txid;
  uint64 value;
  uint32 checksum;
  uint32 pad;
};

static uint32
checksum(struct record *r)
{
  uint32 h = 2166136261U;
  uchar *p = (uchar*)r;
  for(uint i = 0; i < sizeof(*r) - 8; i++){
    h ^= p[i];
    h *= 16777619U;
  }
  return h;
}

static int
valid(struct record *r, uint32 kind)
{
  return r->magic == MAGIC && r->kind == kind && r->checksum == checksum(r);
}

static int
open_rw(char *path)
{
  return open(path, O_CREATE | O_RDWR);
}

static int
store(char *path, struct record *r)
{
  int fd = open_rw(path);
  if(fd < 0 || ftruncate(fd, sizeof(*r)) < 0 ||
     pwrite(fd, r, sizeof(*r), 0) != sizeof(*r) || fdatasync(fd) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
load(char *path, struct record *r)
{
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  int n = pread(fd, r, sizeof(*r), 0);
  close(fd);
  return n == sizeof(*r) ? 0 : -1;
}

static int
clear_wal(void)
{
  int fd = open_rw(WALFILE);
  if(fd < 0 || ftruncate(fd, 0) < 0 || fsync(fd) < 0){
    if(fd >= 0) close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
initialize(void)
{
  mkdir(WALDIR); // EEXIST is harmless in this small libc.
  struct record data;
  memset(&data, 0, sizeof(data));
  data.magic = MAGIC; data.kind = 2; data.txid = 0; data.value = 0;
  data.checksum = checksum(&data);
  if(store(DBFILE, &data) < 0 || clear_wal() < 0)
    return -1;
  printf("waltest: initialized %s value=0\n", WALDIR);
  return 0;
}

// stop_after_wal deliberately leaves a durable committed redo record without
// updating data.db. Power may be removed at the printed marker.
static int
commit_value(uint64 value, int stop_after_wal)
{
  struct record data, wal;
  uint64 txid = 1;
  if(load(DBFILE, &data) == 0 && valid(&data, 2))
    txid = data.txid + 1;
  memset(&wal, 0, sizeof(wal));
  wal.magic = MAGIC; wal.kind = 1; wal.txid = txid; wal.value = value;
  wal.checksum = checksum(&wal);
  if(store(WALFILE, &wal) < 0){
    printf("waltest: WAL write/fsync failed\n");
    return -1;
  }
  printf("waltest: WAL durable tx=%l value=%l\n", txid, value);
  if(stop_after_wal){
    printf("waltest: STOP-AFTER-WAL; cut power now, then run waltest recover\n");
    return 0;
  }
  wal.kind = 2;
  wal.checksum = checksum(&wal);
  if(store(DBFILE, &wal) < 0 || clear_wal() < 0)
    return -1;
  printf("waltest: commit complete tx=%l value=%l\n", txid, value);
  return 0;
}

static int
recover(void)
{
  struct record wal, data;
  int have_wal = load(WALFILE, &wal) == 0 && valid(&wal, 1);
  int have_data = load(DBFILE, &data) == 0 && valid(&data, 2);
  if(have_wal && (!have_data || wal.txid >= data.txid)){
    wal.kind = 2;
    wal.checksum = checksum(&wal);
    if(store(DBFILE, &wal) < 0 || clear_wal() < 0)
      return -1;
    data = wal;
    have_data = 1;
    printf("waltest: REDO tx=%l value=%l\n", data.txid, data.value);
  } else if(have_wal && clear_wal() < 0){
    return -1;
  }
  if(!have_data){
    printf("waltest: no valid database record\n");
    return -1;
  }
  printf("waltest: recovered tx=%l value=%l\n", data.txid, data.value);
  return 0;
}

static int
large_test(void)
{
  static char path[] = "/mnt/ext2/waltest/large.sparse";
  uint64 size = 5ULL * 1024 * 1024 * 1024;
  uint64 marker = 0x1122334455667788ULL, got = 0;
  int fd = open_rw(path);
  if(fd < 0 || ftruncate(fd, size) < 0 ||
     pwrite(fd, &marker, sizeof(marker), size - sizeof(marker)) != sizeof(marker) ||
     fsync(fd) < 0 ||
     pread(fd, &got, sizeof(got), size - sizeof(got)) != sizeof(got) ||
     got != marker){
    if(fd >= 0) close(fd);
    printf("waltest: 5 GiB triple-indirect sparse test failed\n");
    return -1;
  }
  close(fd);
  printf("waltest: 5 GiB triple-indirect sparse test passed\n");
  return 0;
}

static int
fsops_test(void)
{
  static char dir[] = "/mnt/ext2/waltest/ops";
  static char a[] = "/mnt/ext2/waltest/ops/a";
  static char b[] = "/mnt/ext2/waltest/ops/b";
  uint64 value = 0xaabbccddU, got = 0;
  unlink(a); unlink(b); unlink(dir);
  if(mkdir(dir) < 0) return -1;
  int fd = open(a, O_CREATE | O_RDWR);
  if(fd < 0 || pwrite(fd, &value, sizeof(value), 8192) != sizeof(value) ||
     ftruncate(fd, 8192 + sizeof(value)) < 0 || fsync(fd) < 0 ||
     pread(fd, &got, sizeof(got), 8192) != sizeof(got) || got != value){
    if(fd >= 0) close(fd);
    return -1;
  }
  close(fd);
  if(rename(a, b) < 0 || unlink(b) < 0 || unlink(dir) < 0)
    return -1;
  printf("waltest: mkdir/rename/unlink/ftruncate/pread/pwrite passed\n");
  return 0;
}

static int
put_file(char *path, uint64 v)
{
  int fd = open(path, O_CREATE | O_RDWR);
  if(fd < 0) return -1;
  int ok = ftruncate(fd, 0) == 0 && pwrite(fd, &v, sizeof(v), 0) == sizeof(v);
  close(fd);
  return ok ? 0 : -1;
}

static int
get_file(char *path, uint64 *v)
{
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  int n = pread(fd, v, sizeof(*v), 0);
  close(fd);
  return n == sizeof(*v) ? 0 : -1;
}

#define CHECK(c, msg) do { if(!(c)){ printf("waltest: FAIL %s\n", msg); return -1; } } while(0)

// Semantics fixed together with the external journal.
static int
semantics_test(void)
{
  static char d[]  = "/mnt/ext2/waltest/sem";
  static char a[]  = "/mnt/ext2/waltest/sem/a";
  static char b[]  = "/mnt/ext2/waltest/sem/b";
  static char sd[] = "/mnt/ext2/waltest/sem/sub";
  static char sd2[]= "/mnt/ext2/waltest/sem/sub/inner";
  static char sd3[]= "/mnt/ext2/waltest/sem/sub/inner/self";
  uint64 v = 0;
  unlink(a); unlink(b); unlink(sd2); unlink(sd); unlink(d);
  CHECK(mkdir(d) == 0, "mkdir sem");

  // 1. rename over an existing file replaces it (atomic replace pattern).
  CHECK(put_file(a, 111) == 0 && put_file(b, 222) == 0, "create a/b");
  CHECK(rename(a, b) == 0, "rename a over b");
  CHECK(get_file(b, &v) == 0 && v == 111, "b holds a's data");
  CHECK(get_file(a, &v) < 0, "a is gone");

  // 2. a directory cannot be moved below itself.
  CHECK(mkdir(sd) == 0 && mkdir(sd2) == 0, "mkdir sub/inner");
  CHECK(rename(sd, sd3) < 0, "rename dir into own subtree rejected");
  CHECK(unlink(sd2) == 0 && unlink(sd) == 0, "rmdir sub/inner");

  // 3. an open descriptor keeps its file after unlink, and is not redirected
  //    to a new file created under the same name.
  int fd = open(b, O_RDWR);
  CHECK(fd >= 0, "open b");
  CHECK(unlink(b) == 0, "unlink open b");
  CHECK(put_file(b, 333) == 0, "recreate b");
  CHECK(pread(fd, &v, sizeof(v), 0) == sizeof(v) && v == 111,
        "old fd still reads unlinked data");
  uint64 w = 444;
  CHECK(pwrite(fd, &w, sizeof(w), 0) == sizeof(w), "write to unlinked file");
  close(fd);                       // orphan is reclaimed here
  CHECK(get_file(b, &v) == 0 && v == 333, "new b untouched by old fd");
  CHECK(unlink(b) == 0 && unlink(d) == 0, "cleanup");
  printf("waltest: rename-replace, subtree check, unlink-while-open passed\n");
  return 0;
}

// Journal power-cut test: endless metadata-heavy operations.  Cut power at any
// moment; after reboot the kernel prints "xjournal: replayed ..." when a
// committed transaction had not been checkpointed, and e2fsck must be clean.
static int
journal_stress(void)
{
  static char d[] = "/mnt/ext2/waltest/j";
  char p1[64], p2[64];
  mkdir(d);
  for(uint64 i = 0;; i++){
    int k = i % 16;
    strcpy(p1, "/mnt/ext2/waltest/j/f00");
    strcpy(p2, "/mnt/ext2/waltest/j/g00");
    p1[21] = p2[21] = '0' + k / 10;
    p1[22] = p2[22] = '0' + k % 10;
    unlink(p2);
    if(put_file(p1, i) < 0 || rename(p1, p2) < 0){
      printf("waltest: jstress failed at %l\n", i);
      return -1;
    }
    if(i % 50 == 0)
      printf("waltest: jstress %l operations; cut power any time\n", i);
  }
}

int
main(int argc, char **argv)
{
  if(argc < 2){
    printf("usage: waltest init|commit N|prepare N|recover|show|large|fsops|semantics|jstress\n");
    exit(1);
  }
  int r = -1;
  if(strcmp(argv[1], "init") == 0) r = initialize();
  else if(strcmp(argv[1], "commit") == 0 && argc == 3)
    r = commit_value((uint64)atoi(argv[2]), 0);
  else if(strcmp(argv[1], "prepare") == 0 && argc == 3)
    r = commit_value((uint64)atoi(argv[2]), 1);
  else if(strcmp(argv[1], "recover") == 0 || strcmp(argv[1], "show") == 0)
    r = recover();
  else if(strcmp(argv[1], "large") == 0) r = large_test();
  else if(strcmp(argv[1], "fsops") == 0) r = fsops_test();
  else if(strcmp(argv[1], "semantics") == 0) r = semantics_test();
  else if(strcmp(argv[1], "jstress") == 0) r = journal_stress();
  else printf("waltest: bad command\n");
  exit(r < 0);
}
