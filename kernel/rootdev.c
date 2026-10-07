// Root block device: where xv6 block numbers live on the SD card.
//
// rootdev_init() scans the MBR and registers block devices (blkdev.c): the
// whole card, every primary partition, and ROOTDEV.  The buffer cache asks
// for "block B of ROOTDEV"; blk_rw() calls root_blk_rw(), which turns that
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
//
// Which partition is the root follows the embedded-Linux rule: the kernel
// command line's root= (cmdline.c) names it -- /dev/mmcblk0p2 or
// PARTUUID=<MBR disk signature>-02 -- and the kernel resolves the name
// itself (Linux: name_to_dev_t()).  Without root=, or if the named partition
// holds no xv6 filesystem, the old auto-detection runs: type 0x7f first,
// then any partition whose block 1 carries the xv6 superblock magic.
// Mounting the root is not done here: do_mounts.c mounts /dev/root (ROOTDEV)
// once the first process runs.

#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "fat32.h"
#include "blkdev.h"

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

// rootfstype= : the default xv6fs root is dev 1 (ROOTDEV); an ext2 root is
// partition ext2_root_part, mounted by ext2.c/vfs.c (see do_mounts.c).
static int root_fstype = ROOTFS_XV6;
static int ext2_root_part;
static void register_root(void);

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

static int
hexval(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse a hex or decimal field; returns -1 on a bad character.
static long
parsenum(char *s, int len, int base)
{
  long v = 0;
  if(len <= 0)
    return -1;
  for(int i = 0; i < len; i++){
    int d = hexval(s[i]);
    if(d < 0 || d >= base)
      return -1;
    v = v * base + d;
  }
  return v;
}

// name_to_dev_t() for the names xv6 knows.  Returns the MBR partition
// number 1..4, 0 for the whole card, -1 if the name is not understood.
static int
root_param_partition(char *spec, uint32 disksig)
{
  if(strncmp(spec, "/dev/", 5) == 0)
    spec += 5;
  if(strncmp(spec, "mmcblk0", 7) == 0){
    if(spec[7] == 0)
      return 0;
    if(spec[7] == 'p' && spec[8] >= '1' && spec[8] <= '4' && spec[9] == 0)
      return spec[8] - '0';
    return -1;
  }
  // PARTUUID=SSSSSSSS-PP: MBR disk signature (hex) and partition (hex).
  if(strncmp(spec, "PARTUUID=", 9) == 0){
    char *u = spec + 9;
    int len = strlen(u);
    if(len != 11 || u[8] != '-')
      return -1;
    long sig = parsenum(u, 8, 16), part = parsenum(u + 9, 2, 16);
    if(sig < 0 || part < 1 || part > 4)
      return -1;
    if((uint32)sig != disksig){
      printf("rootdev: %s: disk signature is %x\n", spec, disksig);
      return -1;
    }
    return part;
  }
  return -1;
}

// rootfstype=xv6fs (default) or ext2.  Anything else is reported and the
// xv6fs default is kept instead of failing the boot.
static void
check_rootfstype(void)
{
  char t[16];
  if(!cmdline_get("rootfstype", t, sizeof(t)) || strncmp(t, "xv6fs", 6) == 0 ||
     strncmp(t, "xv6", 4) == 0)
    return;
  if(strncmp(t, "ext2", 5) == 0){
    root_fstype = ROOTFS_EXT2;
    return;
  }
  printf("rootdev: rootfstype=%s not supported, using xv6fs\n", t);
}

int
rootdev_fstype(void)
{
  return root_fstype;
}

// MBR partition (1..4) holding the ext2 root, or 0.
int
rootdev_ext2_part(void)
{
  return root_fstype == ROOTFS_EXT2 ? ext2_root_part : 0;
}

void
rootdev_init(void)
{
  initlock(&gate.lock, "rootdev");
  gate.readers = 0;
  gate.updating = 0;
  root.mode = ROOT_BARE;

  blkdev_init();
  if(sdsector(0, sector, 0) < 0)
    panic("rootdev: sector 0");
  blkdev_register(BLKDEV_MMC, "mmcblk0", 0, 0, BLK_SECTOR, 0);

  // A superfloppy FAT32 volume has its BPB directly in sector zero.
  if(fat32_is_boot_sector(sector)){
    if(fat32mount(0, 0) == 0 && (cmdline_init(), setup_fsimg_root()) == 0){
      printf("rootdev: FS.IMG in superfloppy, %d sectors\n", root.sectors);
      register_root();
      return;
    }
    panic("rootdev: FAT32 superfloppy has no FS.IMG");
  }

  if(sector[510] != 0x55 || sector[511] != 0xaa){
    // QEMU development mode attaches fs.img itself as the SD card.
    cmdline_init();
    printf("rootdev: no MBR; using bare xv6 image\n");
    register_root();
    return;
  }

  // The probes below reuse sector[], so keep the four entries first.
  uchar entries[4][16];
  uint32 disksig = le32(sector + 440);   // PARTUUID=<disksig>-<NN>
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
    // Every used primary partition becomes a block device, mmcblk0pN.
    if(part[4] != 0 && part[4] != 0x05 && part[4] != 0x0f &&
       start != 0 && sectors != 0){
      char name[16] = "mmcblk0p0";
      name[8] = '1' + i;
      blkdev_register(BLKDEV_MMC_PART(i + 1), name, start, sectors,
                      BLK_SECTOR, 0);
    }
  }

  // bootfs is needed for /boot, BCM/MT7601 firmware and cmdline_xv6.txt.
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

  cmdline_init();
  check_rootfstype();

  // root= from the command line.
  char spec[64];
  if(cmdline_get("root", spec, sizeof(spec)) && spec[0] &&
     root_fstype == ROOTFS_EXT2){
    // The ext2 driver mounts it; the xv6 partition below is still looked
    // for, because /dev/sdroot and the ext2 journal live there.
    int n = root_param_partition(spec, disksig);
    if(n >= 1 && n <= 4 && entries[n - 1][4] != 0){
      ext2_root_part = n;
      printf("rootdev: root=%s rootfstype=ext2 -> mmcblk0p%d\n", spec, n);
    } else {
      printf("rootdev: root=%s: no such partition, using xv6fs\n", spec);
      root_fstype = ROOTFS_XV6;
    }
  } else if(cmdline_get("root", spec, sizeof(spec)) && spec[0]){
    int n = root_param_partition(spec, disksig);
    if(n >= 1 && n <= 4 && entries[n - 1][4] != 0 &&
       setup_raw_root(le32(entries[n - 1] + 8),
                      le32(entries[n - 1] + 12)) == 0){
      printf("rootdev: root=%s -> mmcblk0p%d lba=%d sectors=%d size=%d MiB\n",
             spec, n, root.lba, root.sectors, root.sectors / 2048);
    } else if(n < 0){
      printf("rootdev: root=%s: unknown device name, auto-detecting\n",
             spec);
    } else {
      printf("rootdev: root=%s: no xv6 filesystem there, auto-detecting\n",
             spec);
    }
  }

  // Auto-detection.  Prefer type 0x7f, but accept any other primary
  // partition whose block-1 superblock has the xv6 magic: macOS diskutil
  // can create Linux partitions but not an arbitrary type.
  for(int pass = 0; pass < 2 && root.mode != ROOT_RAW; pass++){
    for(int i = 0; i < 4; i++){
      const uchar *part = entries[i];
      uchar type = part[4];
      if((pass == 0) != (type == XV6_PARTITION_TYPE))
        continue;
      if(type != 0 && type != 0x05 && type != 0x0f &&
         type != 0x0b && type != 0x0c &&
         setup_raw_root(le32(part + 8), le32(part + 12)) == 0){
        printf("rootdev: raw xv6 root p%d lba=%d sectors=%d size=%d MiB\n",
               i + 1, root.lba, root.sectors, root.sectors / 2048);
        break;
      }
    }
  }

  if(root.mode == ROOT_RAW){
    register_root();
    return;
  }
  if(fat32ready() && setup_fsimg_root() == 0){
    printf("rootdev: using FAT32 FS.IMG compatibility mapping, %d sectors\n",
           root.sectors);
    register_root();
    return;
  }
  if(root_fstype == ROOTFS_EXT2){
    printf("rootdev: no xv6 partition; ext2 root without /dev/sdroot\n");
    blkdev_print();
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

// blkdev rw for ROOTDEV.  blk_rw() has already checked that the block lies
// inside FSSIZE; the buf sleeplock keeps two writers of the same block apart
// and sdsector() serialises the card.
static int
root_blk_rw(struct blkdev *bd, uint32 first, uchar *data, uint32 count,
            int write)
{
  int r = 0;
  (void)bd;
  gate_enter();
  for(uint32 i = 0; i < count; i++){
    if(sdsector(root_sector_lba(first + i), data + i * SECTOR_SIZE,
                write) < 0){
      r = -1;
      break;
    }
  }
  gate_exit();
  return r;
}

static void
register_root(void)
{
  blkdev_register(ROOTDEV, "root", 0,
                  FSSIZE * SECTORS_PER_BLOCK, BSIZE, root_blk_rw);
  blkdev_print();
}

// What /dev/root really is, for the "VFS: Mounted root" message.
char*
rootdev_name(void)
{
  if(root.mode == ROOT_FSIMG)
    return "FS.IMG on bootfs";
  if(root.mode == ROOT_BARE)
    return "mmcblk0 (bare image)";
  for(int n = 1; n <= 4; n++){
    struct blkdev *bd = blkdev_get(BLKDEV_MMC_PART(n));
    if(bd && bd->start == root.lba)
      return bd->name;
  }
  return "raw partition";
}
