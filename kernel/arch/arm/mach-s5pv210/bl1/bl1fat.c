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

static char lc(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* name as an 8.3 short name ("XV6.IMG" -> "XV6     IMG"); 0 if it is not one. */
static int
short_name(const char *name, char out[11])
{
  int i = 0, n = 0;
  for(int k = 0; k < 11; k++) out[k] = ' ';
  for(; name[i] && name[i] != '.'; i++){
    if(n == 8) return 0;
    char c = name[i];
    out[n++] = c >= 'a' && c <= 'z' ? c - 32 : c;
  }
  if(n == 0) return 0;
  if(name[i] == '.'){
    i++;
    for(n = 8; name[i]; i++){
      if(n == 11 || name[i] == '.') return 0;
      char c = name[i];
      out[n++] = c >= 'a' && c <= 'z' ? c - 32 : c;
    }
  }
  return 1;
}

/* Find `name` in the root directory, by long (VFAT) name, case-insensitive,
   or by its 8.3 short name; return first cluster and size. */
int
fat_find(const char *name, uint32 *first, uint32 *size)
{
  char sn[11], lfn[256];
  int have_sn = short_name(name, sn), lfn_ok = 0, nlen = 0;
  while(name[nlen]) nlen++;
  for(int k = 0; k < 256; k++) lfn[k] = 0;
  for(uint32 c = fat.root; c >= 2 && c < 0x0ffffff8; c = fat_next(c))
    for(uint32 s = 0; s < (1U << fat.spc_shift); s++){
      if(rd_sec(cluster_lba(c) + s, sec, 1) < 0)
        return -1;
      for(int o = 0; o < 512; o += 32){
        const uint8 *d = sec + o;
        if(d[0] == 0)
          return -1;                         /* end of directory */
        if(d[0] == 0xe5){ lfn_ok = 0; continue; }
        if((d[11] & 0x3f) == 0x0f){          /* VFAT long-name slot */
          int ord = d[0] & 0x1f;
          if(d[0] & 0x40){ for(int k = 0; k < 256; k++) lfn[k] = 0; lfn_ok = 1; }
          if(ord == 0 || ord > 19){ lfn_ok = 0; continue; }
          static const uint8 pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
          for(int k = 0; k < 13; k++){
            uint16 ch = d[pos[k]] | d[pos[k]+1] << 8;
            int at = (ord - 1)*13 + k;
            if(at < 255) lfn[at] = ch == 0xffff ? 0 : (ch > 0x7e ? '?' : (char)ch);
          }
          continue;
        }
        if(d[11] & 0x18){ lfn_ok = 0; continue; }   /* volume label or directory */
        int match = 0;
        if(lfn_ok){
          int k = 0;
          while(k < nlen && lc(lfn[k]) == lc(name[k])) k++;
          match = k == nlen && lfn[k] == 0;
        }
        if(!match && have_sn){
          int k = 0;
          while(k < 11 && d[k] == (uint8)sn[k]) k++;
          match = k == 11;
        }
        lfn_ok = 0;
        if(match){
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
