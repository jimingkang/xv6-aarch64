// FAT32 reader for BL1 (hardware independent; also built on the host by
// test_host.c). Root directory, 8.3 names, 512-byte sectors.
#include "kernel/types.h"
#include "bl1fat.h"

static uint8 sec[512] __attribute__((aligned(4)));
static sec_read_fn rd_sec;
static uint16 le16(const uint8 *p) { return p[0] | p[1] << 8; }
static uint32 le32(const uint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32)p[3] << 24; }
static struct { uint32 fat_lba, data_lba, spc_shift, root; } fat;

int
fat_mount(sec_read_fn rd, uint32 *part_out, uint32 *spc_out)
{
  uint32 part = 0;
  rd_sec = rd;
  if(rd_sec(0, sec, 1) < 0 || sec[510] != 0x55 || sec[511] != 0xaa)
    return -1;
  if(!(sec[0x52] == 'F' && sec[0x53] == 'A' && sec[0x54] == 'T')){   /* MBR, not a bare FAT32 volume */
    int found = 0;
    for(int i = 0; i < 4 && !found; i++){
      const uint8 *e = sec + 446 + 16*i;
      if(e[4] == 0x0b || e[4] == 0x0c){ part = le32(e + 8); found = 1; }
    }
    if(!found || rd_sec(part, sec, 1) < 0)
      return -1;
  }
  uint32 spc = sec[13], reserved = le16(sec + 14), nfats = sec[16];
  uint32 fatsz = le32(sec + 36);
  if(le16(sec + 11) != 512 || spc == 0 || (spc & (spc - 1)) || fatsz == 0)
    return -1;
  fat.spc_shift = 0;
  while((1U << fat.spc_shift) < spc) fat.spc_shift++;
  fat.fat_lba = part + reserved;
  fat.data_lba = fat.fat_lba + nfats * fatsz;
  fat.root = le32(sec + 44);
  *part_out = part;
  *spc_out = spc;
  return 0;
}

static uint32 cluster_lba(uint32 c) { return fat.data_lba + ((c - 2) << fat.spc_shift); }

static uint32
fat_next(uint32 c)
{
  if(rd_sec(fat.fat_lba + (c >> 7), sec, 1) < 0)
    return 0x0fffffff;
  return le32(sec + 4*(c & 127)) & 0x0fffffff;
}

int
fat_find(const char *name11, uint32 *first, uint32 *size)
{
  for(uint32 c = fat.root; c >= 2 && c < 0x0ffffff8; c = fat_next(c))
    for(uint32 s = 0; s < (1U << fat.spc_shift); s++){
      if(rd_sec(cluster_lba(c) + s, sec, 1) < 0)
        return -1;
      for(int o = 0; o < 512; o += 32){
        const uint8 *d = sec + o;
        if(d[0] == 0)
          return -1;                         /* end of directory */
        if(d[0] == 0xe5 || (d[11] & 0x0f) == 0x0f || (d[11] & 0x18))
          continue;                          /* deleted, LFN, volume label, directory */
        int i = 0;
        while(i < 11 && d[i] == (uint8)name11[i]) i++;
        if(i == 11){
          *first = (uint32)le16(d + 20) << 16 | le16(d + 26);
          *size = le32(d + 28);
          return 0;
        }
      }
    }
  return -1;
}

/* Read the cluster chain to dst (whole clusters), merging consecutive runs. */
int
fat_read(uint32 c, uint32 size, uint8 *dst)
{
  uint32 csize = 512U << fat.spc_shift, done = 0;
  while(done < size && c >= 2 && c < 0x0ffffff8){
    uint32 start = c, run = 1, next;
    while(run < 64 && done + run*csize < size && (next = fat_next(c)) == c + 1){ c = next; run++; }
    if(rd_sec(cluster_lba(start), dst + done, run << fat.spc_shift) < 0)
      return -1;
    done += run * csize;
    c = fat_next(c);
  }
  return done >= size ? 0 : -1;
}
