// Root block device: where xv6 block numbers live on the SD card.
//
// The buffer cache asks for "block B of ROOTDEV"; rootdev_rw() turns that
// into two 512-byte SD sectors.  This file interprets no filesystem.  It reads
// the MBR, recognises the xv6 root partition by the superblock in block 1,
// and mounts the FAT32 boot partition for /boot and firmware.  The xv6 root
// image is reached in one of three ways:
//
//   raw    an MBR primary partition (type 0x7f preferred) holds the image:
//          sector = partition start + 2*B + i
//   fsimg  old cards keep the image as the file FS.IMG inside FAT32.  FAT32
//          maps the file sector to a card sector (fat32mapsector()); the
//          xv6 blocks are still xv6 blocks, FAT32 only says where they sit.
//   bare   QEMU attaches fs.img itself as the whole card: sector = 2*B + i

#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "fat32.h"

#define SECTOR_SIZE        512
#define SECTORS_PER_BLOCK  (BSIZE / SECTOR_SIZE)
#define XV6_PARTITION_TYPE 0x7f

enum { ROOT_BARE, ROOT_RAW, ROOT_FSIMG };

static struct {
  int mode;
  uint32 lba;       // raw: first sector of the partition
  uint32 sectors;   // raw: partition size; fsimg: FS.IMG size in sectors
} root;

// /dev/sdroot overwrites the raw partition in place.  The gate lets block
// I/O already in flight drain and then blocks all further root I/O.
static struct {
  struct spinlock lock;
  int readers;
  int updating;
} gate;

static uchar sector[SECTOR_SIZE];

static uint32
le32(const uchar *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) |
         ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static char *
partition_type_name(uchar type)
{
  switch(type){
  case 0x00: return "empty";
  case 0x01: return "FAT12";
  case 0x04: return "FAT16<32M";
  case 0x06: return "FAT16";
  case 0x0b: return "FAT32";
  case 0x0c: return "FAT32-LBA";
  case 0x0e: return "FAT16-LBA";
  case 0x83: return "Linux";
  case XV6_PARTITION_TYPE: return "xv6 raw filesystem";
  case 0xee: return "GPT-protective";
  default:   return "unknown";
  }
}

// Does this partition start with an xv6 image?  Block 1 is the superblock;
// with 1024-byte blocks it begins two sectors into the partition.
static int
setup_raw_root(uint32 start, uint32 sectors)
{
  if(start == 0 || sectors < FSSIZE * SECTORS_PER_BLOCK ||
     sdsector(start + SECTORS_PER_BLOCK, sector, 0) < 0 ||
     le32(sector) != FSMAGIC || le32(sector + 4) != FSSIZE)
    return -1;
  root.mode = ROOT_RAW;
  root.lba = start;
  root.sectors = sectors;
  return 0;
}

// Compatibility: the root image is the file FS.IMG in the mounted FAT32.
static int
setup_fsimg_root(void)
{
  uint32 bytes;
  if(fat32mapfile("FS      IMG", &bytes) < 0)
    return -1;
  if(bytes < FSSIZE * BSIZE){
    printf("rootdev: FS.IMG too small: %d bytes\n", bytes);
    return -1;
  }
  root.mode = ROOT_FSIMG;
  root.sectors = bytes / SECTOR_SIZE;
  return 0;
}

void
rootdev_init(void)
{
  initlock(&gate.lock, "rootdev");
  gate.readers = 0;
  gate.updating = 0;
  root.mode = ROOT_BARE;

  if(sdsector(0, sector, 0) < 0)
    panic("rootdev: sector 0");

  // A superfloppy FAT32 volume has its BPB directly in sector zero.
  if(fat32_is_boot_sector(sector)){
    if(fat32mount(0, 0) == 0 && setup_fsimg_root() == 0){
      printf("rootdev: FS.IMG in superfloppy, %d sectors\n", root.sectors);
      return;
    }
    panic("rootdev: FAT32 superfloppy has no FS.IMG");
  }

  if(sector[510] != 0x55 || sector[511] != 0xaa){
    // QEMU development mode attaches fs.img itself as the SD card.
    printf("rootdev: no MBR; using bare xv6 image\n");
    return;
  }

  // The probes below reuse sector[], so keep the four entries first.
  uchar entries[4][16];
  for(int i = 0; i < 4; i++)
    for(int j = 0; j < 16; j++)
      entries[i][j] = sector[446 + i * 16 + j];

  printf("rootdev: MBR partition table\n");
  for(int i = 0; i < 4; i++){
    const uchar *part = entries[i];
    uint32 start = le32(part + 8);
    uint32 sectors = le32(part + 12);
    uint32 end = sectors == 0 ? start : start + sectors - 1;
    printf("rootdev: p%d boot=%s type=0x%x (%s)\n",
           i + 1, part[0] == 0x80 ? "yes" : "no",
           part[4], partition_type_name(part[4]));
    printf("rootdev:    start=%d end=%d sectors=%d size=%d MiB\n",
           start, end, sectors, sectors / 2048);
  }

  // Root storage is independent from bootfs.  Prefer type 0x7f, but accept
  // any other primary partition whose block-1 superblock has the xv6 magic:
  // macOS diskutil can create Linux partitions but not an arbitrary type.
  for(int i = 0; i < 4; i++){
    const uchar *part = entries[i];
    uchar type = part[4];
    if(type != 0 && type != 0x05 && type != 0x0f &&
       type != 0x0b && type != 0x0c &&
       setup_raw_root(le32(part + 8), le32(part + 12)) == 0){
      printf("rootdev: raw xv6 root p%d lba=%d sectors=%d size=%d MiB\n",
             i + 1, root.lba, root.sectors, root.sectors / 2048);
      break;
    }
  }

  // bootfs is needed for /boot and BCM/MT7601 firmware in every mode.
  for(int i = 0; i < 4; i++){
    const uchar *part = entries[i];
    uchar type = part[4];
    uint32 start = le32(part + 8);
    uint32 sectors = le32(part + 12);
    if((type == 0x0b || type == 0x0c) && start != 0 &&
       sectors != 0 && start <= 0xffffffffU - sectors &&
       fat32mount(start, sectors) == 0){
      printf("rootdev: bootfs p%d lba=%d mounted as FAT32\n", i + 1, start);
      break;
    }
  }

  if(root.mode == ROOT_RAW)
    return;
  if(fat32ready() && setup_fsimg_root() == 0){
    printf("rootdev: using FAT32 FS.IMG compatibility mapping, %d sectors\n",
           root.sectors);
    return;
  }
  panic("rootdev: no raw xv6 partition or FS.IMG");
}

int
rootdev_raw_info(uint32 *lba, uint32 *sectors)
{
  if(root.mode != ROOT_RAW || lba == 0 || sectors == 0)
    return -1;
  *lba = root.lba;
  *sectors = root.sectors;
  return 0;
}

// Quiesce the mounted native root before it is replaced in place.  Log
// transactions are stopped first; then block I/O already in rootdev_rw
// drains.  The gate is never reopened because cached inodes and buffers
// describe the old image; the updater must reboot after the copy.
int
rootdev_update_begin(void)
{
  if(root.mode != ROOT_RAW)
    return -1;
  if(rootfs_freeze_for_update() < 0)
    return -1;
  acquire(&gate.lock);
  gate.updating = 1;
  while(gate.readers != 0)
    sleep(&gate, &gate.lock);
  release(&gate.lock);
  return 0;
}

static void
gate_enter(void)
{
  acquire(&gate.lock);
  while(gate.updating)
    sleep(&gate, &gate.lock);
  gate.readers++;
  release(&gate.lock);
}

static void
gate_exit(void)
{
  acquire(&gate.lock);
  if(--gate.readers == 0)
    wakeup(&gate);
  release(&gate.lock);
}

// Sector number s of the xv6 image -> sector number on the card.
static uint32
root_sector_lba(uint32 s)
{
  uint32 lba;
  switch(root.mode){
  case ROOT_RAW:
    if(s >= root.sectors)
      panic("rootdev: sector outside raw partition");
    return root.lba + s;
  case ROOT_FSIMG:
    if(fat32mapsector(s, &lba) < 0)
      panic("rootdev: sector outside FS.IMG");
    return lba;
  default:
    return s;
  }
}

// Read or write one xv6 block for the buffer cache.  The buf sleeplock
// keeps two writers of the same block apart; sdsector() serialises the card.
void
rootdev_rw(struct buf *b, int write)
{
  if(!holdingsleep(&b->lock))
    panic("rootdev_rw: buf not locked");
  if(b->blockno >= FSSIZE)
    panic("rootdev_rw: blockno too big");

  uint32 first = b->blockno * SECTORS_PER_BLOCK;
  gate_enter();
  for(int i = 0; i < SECTORS_PER_BLOCK; i++){
    if(sdsector(root_sector_lba(first + i),
                b->data + i * SECTOR_SIZE, write) < 0)
      panic(write ? "rootdev: write block" : "rootdev: read block");
  }
  gate_exit();
}
