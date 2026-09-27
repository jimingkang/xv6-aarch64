// Minimal read-only ext2 filesystem reader.
// This is deliberately separate from xv6's root filesystem and VFS.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "ext2.h"

#define SECTOR 512
#define EXT2_MAGIC 0xef53
#define EXT2_ROOT_INO 2
#define EXT2_S_IFMT  0xf000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

struct einode {
  uint16 mode;
  uint64 size;
  uint32 block[15];
};

static struct {
  struct spinlock lock;
  int ready;
  uint32 part_lba;
  uint32 block_size;
  uint32 blocks_per_group;
  uint32 inodes_per_group;
  uint32 inode_size;
  uint32 desc_size;
  uint32 groups;
  uint32 gd_block;
} e2;

static uchar sectorbuf[SECTOR];
static uchar blockbuf[4096];
static uchar inodebuf[512];
static uint32 indirect[1024];

static uint16 le16(const uchar *p) { return p[0] | ((uint16)p[1] << 8); }
static uint32 le32(const uchar *p) {
  return p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

static int
diskbytes(uint64 byte, void *dst, uint32 n)
{
  uchar *p = dst;
  while(n){
    uint32 lba = e2.part_lba + byte / SECTOR;
    uint32 off = byte % SECTOR;
    uint32 take = SECTOR - off;
    if(take > n) take = n;
    if(sdsector(lba, sectorbuf, 0) < 0) return -1;
    memmove(p, sectorbuf + off, take);
    p += take; byte += take; n -= take;
  }
  return 0;
}

static int
readblock(uint32 bno, void *dst)
{
  if(bno == 0) return -1;
  return diskbytes((uint64)bno * e2.block_size, dst, e2.block_size);
}

static int
readinode(uint32 ino, struct einode *ip)
{
  uint32 group, index, gd_off, table, off;
  if(!e2.ready || ino == 0) return -1;
  group = (ino - 1) / e2.inodes_per_group;
  index = (ino - 1) % e2.inodes_per_group;
  if(group >= e2.groups || e2.inode_size > sizeof(inodebuf)) return -1;
  gd_off = e2.gd_block * e2.block_size + group * e2.desc_size;
  if(diskbytes(gd_off, inodebuf, e2.desc_size) < 0) return -1;
  table = le32(inodebuf + 8);
  off = table * e2.block_size + index * e2.inode_size;
  if(diskbytes(off, inodebuf, e2.inode_size) < 0) return -1;
  ip->mode = le16(inodebuf);
  ip->size = le32(inodebuf + 4);
  if((ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
    ip->size |= (uint64)le32(inodebuf + 108) << 32;
  for(int i = 0; i < 15; i++) ip->block[i] = le32(inodebuf + 40 + i*4);
  return 0;
}

static uint32
fileblock(struct einode *ip, uint32 logical)
{
  if(logical < 12) return ip->block[logical];
  logical -= 12;
  if(logical >= e2.block_size / 4 || ip->block[12] == 0) return 0;
  if(readblock(ip->block[12], indirect) < 0) return 0;
  return indirect[logical];
}

static int
nameeq(const uchar *p, int n, const char *s, int sn)
{
  if(n != sn) return 0;
  for(int i = 0; i < n; i++) if(p[i] != (uchar)s[i]) return 0;
  return 1;
}

static int
e2dirlookup(struct einode *dp, const char *name, int namelen, uint32 *ino)
{
  uint32 pos = 0, loaded = -1;
  if((dp->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
  while(pos + 8 <= dp->size){
    uint32 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      uint32 b = fileblock(dp, logical);
      if(b == 0 || readblock(b, blockbuf) < 0) return -1;
      loaded = logical;
    }
    if(inblock + 8 > e2.block_size) return -1;
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size)
      return -1;
    if(deino && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      *ino = deino;
      return 0;
    }
    pos += reclen;
  }
  return -1;
}

static int
lookup(const char *path, uint32 *ino, struct einode *ip)
{
  const char *p = path, *start;
  uint32 curino = EXT2_ROOT_INO, nextino;
  struct einode cur;
  if(readinode(curino, &cur) < 0) return -1;
  while(*p == '/') p++;
  while(*p){
    start = p;
    while(*p && *p != '/') p++;
    if(e2dirlookup(&cur, start, p - start, &nextino) < 0) return -1;
    curino = nextino;
    if(readinode(curino, &cur) < 0) return -1;
    while(*p == '/') p++;
  }
  *ino = curino;
  *ip = cur;
  return 0;
}

static int
setup_ext2(uint32 start, uint32 sectors, int partno)
{
  uchar sb[1024];
  uint32 blocks, incompat, rocompat;
  e2.part_lba = start;
  if(diskbytes(1024, sb, sizeof(sb)) < 0 || le16(sb + 56) != EXT2_MAGIC){
    printf("ext2: p%d has no ext superblock\n", partno); return -1;
  }
  incompat = le32(sb + 96);
  rocompat = le32(sb + 100);
  // FILETYPE (0x2) is understood. Reject extents, 64-bit block numbers and
  // other layouts before interpreting an ext4 volume as ext2.
  if(incompat & ~2U){
    printf("ext2: p%d unsupported incompat features=0x%x\n", partno, incompat);
    return -1;
  }
  if(rocompat & ~3U){
    printf("ext2: p%d unsupported ro-compat features=0x%x\n", partno, rocompat);
    return -1;
  }
  e2.block_size = 1024U << le32(sb + 24);
  if(e2.block_size < 1024 || e2.block_size > sizeof(blockbuf)){
    printf("ext2: p%d unsupported block size %d\n", partno, e2.block_size); return -1;
  }
  blocks = le32(sb + 4);
  e2.blocks_per_group = le32(sb + 32);
  e2.inodes_per_group = le32(sb + 40);
  e2.inode_size = le32(sb + 76) ? le16(sb + 88) : 128;
  if(e2.inode_size < 128 || e2.inodes_per_group == 0 || e2.blocks_per_group == 0)
    return -1;
  e2.desc_size = 32;
  e2.groups = (blocks + e2.blocks_per_group - 1) / e2.blocks_per_group;
  e2.gd_block = e2.block_size == 1024 ? 2 : 1;
  e2.ready = 1;
  printf("ext2: mounted p%d read-only start=%d sectors=%d block=%d groups=%d\n",
         partno, start, sectors, e2.block_size, e2.groups);
  return 0;
}

void
ext2init(void)
{
  uchar mbr[512];
  int found = 0;
  initlock(&e2.lock, "ext2");
  e2.ready = 0;
  if(sdsector(0, mbr, 0) < 0) return;
  if(mbr[510] != 0x55 || mbr[511] != 0xaa){
    printf("ext2: no MBR; disabled\n");
    return;
  }
  // Try every Linux partition. This permits p2 to remain ext4 while a simple
  // ext2 filesystem is placed in p3.
  for(int i = 0; i < 4; i++){
    uchar *p = mbr + 446 + i*16;
    if(p[4] == 0x83 && le32(p + 8) != 0){
      found = 1;
      if(setup_ext2(le32(p + 8), le32(p + 12), i + 1) == 0) return;
    }
  }
  if(!found) printf("ext2: no Linux partition; disabled\n");
  else printf("ext2: no compatible ext2 partition; disabled\n");
}

int
ext2readfile(char *path, uint64 off, void *dst, int n)
{
  struct einode ip;
  uint32 ino;
  int done = 0;
  if(n < 0) return -1;
  acquire(&e2.lock);
  if(lookup(path, &ino, &ip) < 0 || (ip.mode & EXT2_S_IFMT) != EXT2_S_IFREG){
    release(&e2.lock); return -1;
  }
  if(off >= ip.size){ release(&e2.lock); return 0; }
  if((uint64)n > ip.size - off) n = ip.size - off;
  while(done < n){
    uint32 logical = off / e2.block_size;
    uint32 boff = off % e2.block_size;
    int take = e2.block_size - boff;
    uint32 b = fileblock(&ip, logical);
    if(take > n - done) take = n - done;
    if(b == 0) memset(blockbuf, 0, e2.block_size);
    else if(readblock(b, blockbuf) < 0){ release(&e2.lock); return -1; }
    memmove((uchar*)dst + done, blockbuf + boff, take);
    off += take; done += take;
  }
  release(&e2.lock);
  return done;
}

int
ext2readdir(char *path, int index, struct ext2_user_dirent *out)
{
  struct einode dp, child;
  uint32 ino, pos = 0, loaded = -1;
  int seen = 0;
  if(index < 0) return -1;
  acquire(&e2.lock);
  if(lookup(path, &ino, &dp) < 0 || (dp.mode & EXT2_S_IFMT) != EXT2_S_IFDIR){
    release(&e2.lock); return -1;
  }
  while(pos + 8 <= dp.size){
    uint32 logical = pos / e2.block_size, inblock = pos % e2.block_size;
    if(logical != loaded){
      uint32 b = fileblock(&dp, logical);
      if(b == 0 || readblock(b, blockbuf) < 0) break;
      loaded = logical;
    }
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    uint8 type = de[7];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size) break;
    if(deino && nlen <= reclen - 8){
      if(seen++ == index){
        memset(out, 0, sizeof(*out));
        out->inode = deino; out->type = type;
        if(readinode(deino, &child) == 0){ out->size = child.size; out->mode = child.mode; }
        memmove(out->name, de + 8, nlen);
        out->name[nlen] = 0;
        release(&e2.lock); return 1;
      }
    }
    pos += reclen;
  }
  release(&e2.lock);
  return 0;
}
