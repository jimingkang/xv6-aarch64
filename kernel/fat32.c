// Minimal FAT32 container driver for an xv6 fs.img file.
//
// The Raspberry Pi sees the whole SD card, not macOS names such as disk4s1.
// Locate a FAT32 partition through the MBR, find FS.IMG in its root directory,
// cache the file's cluster chain, and expose its already-allocated data sectors
// as the xv6 block device.  File growth and FAT metadata updates are purposely
// not supported; fs.img is copied to the FAT volume at its final fixed size.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define SECTOR_SIZE       512
#define FAT32_EOC         0x0ffffff8U
#define MAX_FILE_CLUSTERS 4096

static struct {
  int fat;
  uint32 partition_lba;
  uint32 fat_lba;
  uint32 data_lba;
  uint32 sectors_per_cluster;
  uint32 file_size;
  uint32 cluster_count;
  uint32 clusters[MAX_FILE_CLUSTERS];
} diskmap;

static uchar scratch[SECTOR_SIZE];

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
  case 0xee: return "GPT-protective";
  default:   return "unknown";
  }
}

static uint16
le16(const uchar *p)
{
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32
le32(const uchar *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) |
         ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static int
is_fat32_boot_sector(const uchar *s)
{
  return s[510] == 0x55 && s[511] == 0xaa &&
         le16(s + 11) == SECTOR_SIZE &&
         s[13] != 0 && le16(s + 14) != 0 &&
         le32(s + 36) != 0 && le32(s + 44) >= 2;
}

static uint32
cluster_lba(uint32 cluster)
{
  return diskmap.data_lba +
         (cluster - 2) * diskmap.sectors_per_cluster;
}

static uint32
fat_next(uint32 cluster)
{
  uint32 offset = cluster * 4;
  uint32 sector = diskmap.fat_lba + offset / SECTOR_SIZE;
  uint32 pos = offset % SECTOR_SIZE;
  if(sdsector(sector, scratch, 0) < 0)
    panic("fat32: read FAT");
  return le32(scratch + pos) & 0x0fffffffU;
}

static int
short_name_is_fsimg(const uchar *entry)
{
  static const char name[11] = {
    'F', 'S', ' ', ' ', ' ', ' ', ' ', ' ', 'I', 'M', 'G'
  };
  for(int i = 0; i < 11; i++)
    if(entry[i] != (uchar)name[i])
      return 0;
  return 1;
}

static int
find_fsimg(uint32 root, uint32 *first_cluster, uint32 *size)
{
  uint32 cluster = root;
  for(uint32 visited = 0; visited < MAX_FILE_CLUSTERS; visited++){
    if(cluster < 2 || cluster >= FAT32_EOC)
      return -1;

    uint32 first_sector = cluster_lba(cluster);
    for(uint32 s = 0; s < diskmap.sectors_per_cluster; s++){
      if(sdsector(first_sector + s, scratch, 0) < 0)
        return -1;
      for(int off = 0; off < SECTOR_SIZE; off += 32){
        uchar *entry = scratch + off;
        if(entry[0] == 0x00)
          return -1;
        if(entry[0] == 0xe5 || entry[11] == 0x0f || (entry[11] & 0x08))
          continue;
        if(short_name_is_fsimg(entry)){
          *first_cluster = ((uint32)le16(entry + 20) << 16) |
                           le16(entry + 26);
          *size = le32(entry + 28);
          return 0;
        }
      }
    }
    cluster = fat_next(cluster);
  }
  return -1;
}

static int
setup_fat32(uint32 partition_lba)
{
  if(sdsector(partition_lba, scratch, 0) < 0 ||
     !is_fat32_boot_sector(scratch))
    return -1;

  uint32 reserved = le16(scratch + 14);
  uint32 fats = scratch[16];
  uint32 fat_sectors = le32(scratch + 36);
  uint32 root = le32(scratch + 44);

  diskmap.partition_lba = partition_lba;
  diskmap.sectors_per_cluster = scratch[13];
  diskmap.fat_lba = partition_lba + reserved;
  diskmap.data_lba = diskmap.fat_lba + fats * fat_sectors;

  printf("fat32: BPB lba=%d bytes/sector=%d sectors/cluster=%d\n",
         partition_lba, le16(scratch + 11), diskmap.sectors_per_cluster);
  printf("fat32: reserved=%d FATs=%d sectors/FAT=%d root-cluster=%d\n",
         reserved, fats, fat_sectors, root);
  printf("fat32: FAT lba=%d data lba=%d\n",
         diskmap.fat_lba, diskmap.data_lba);

  uint32 first_cluster;
  uint32 file_size;
  if(find_fsimg(root, &first_cluster, &file_size) < 0)
    return -1;
  if(file_size < FSSIZE * BSIZE){
    printf("fat32: FS.IMG too small: %d bytes\n", file_size);
    return -1;
  }

  uint32 needed = (file_size +
                    diskmap.sectors_per_cluster * SECTOR_SIZE - 1) /
                   (diskmap.sectors_per_cluster * SECTOR_SIZE);
  if(needed > MAX_FILE_CLUSTERS)
    panic("fat32: FS.IMG cluster chain too long");

  uint32 cluster = first_cluster;
  for(uint32 i = 0; i < needed; i++){
    if(cluster < 2 || cluster >= FAT32_EOC)
      panic("fat32: short FS.IMG chain");
    diskmap.clusters[i] = cluster;
    diskmap.cluster_count++;
    if(i + 1 < needed)
      cluster = fat_next(cluster);
  }

  diskmap.file_size = file_size;
  diskmap.fat = 1;
  return 0;
}

void
fat32init(void)
{
  if(sdsector(0, scratch, 0) < 0)
    panic("fat32: sector 0");

  // A superfloppy FAT32 volume has its BPB directly in sector zero.
  if(is_fat32_boot_sector(scratch) && setup_fat32(0) == 0){
    printf("fat32: FS.IMG found in superfloppy, size=%d\n",
           diskmap.file_size);
    return;
  }

  // Otherwise inspect the four DOS/MBR partition entries.
  if(scratch[510] == 0x55 && scratch[511] == 0xaa){
    // setup_fat32() reuses scratch, so preserve all entries before probing.
    uchar entries[4][16];
    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 16; j++)
        entries[i][j] = scratch[446 + i * 16 + j];

    printf("fat32: MBR partition table\n");
    for(int i = 0; i < 4; i++){
      const uchar *part = entries[i];
      uchar type = part[4];
      uint32 start = le32(part + 8);
      uint32 sectors = le32(part + 12);
      uint32 end = sectors == 0 ? start : start + sectors - 1;
      printf("fat32: p%d boot=%s type=0x%x (%s)\n",
             i + 1, part[0] == 0x80 ? "yes" : "no",
             type, partition_type_name(type));
      printf("fat32:    start=%d end=%d sectors=%d size=%d MiB\n",
             start, end, sectors, sectors / 2048);
    }

    for(int i = 0; i < 4; i++){
      const uchar *part = entries[i];
      uchar type = part[4];
      uint32 start = le32(part + 8);
      if((type == 0x0b || type == 0x0c) && start != 0 &&
         setup_fat32(start) == 0){
        printf("fat32: selected p%d lba=%d FS.IMG=%d bytes clusters=%d\n",
               i + 1, start, diskmap.file_size, diskmap.cluster_count);
        return;
      }
    }
  }

  // QEMU development mode attaches fs.img itself as the SD card.
  diskmap.fat = 0;
  printf("fat32: no FS.IMG container; using raw xv6 image\n");
}

static uint32
file_sector_lba(uint32 file_sector)
{
  if(!diskmap.fat)
    return file_sector;

  uint32 index = file_sector / diskmap.sectors_per_cluster;
  uint32 within = file_sector % diskmap.sectors_per_cluster;
  if(index >= diskmap.cluster_count)
    panic("fat32: file sector outside FS.IMG");
  return cluster_lba(diskmap.clusters[index]) + within;
}

void
fat32rw(struct buf *b, int write)
{
  if(!holdingsleep(&b->lock))
    panic("fat32rw: buf not locked");
  if(b->blockno >= FSSIZE)
    panic("fat32rw: blockno too big");

  uint32 first = b->blockno * (BSIZE / SECTOR_SIZE);
  for(int i = 0; i < BSIZE / SECTOR_SIZE; i++){
    uint32 lba = file_sector_lba(first + i);
    if(sdsector(lba, b->data + i * SECTOR_SIZE, write) < 0)
      panic(write ? "fat32: write FS.IMG" : "fat32: read FS.IMG");
  }
}
