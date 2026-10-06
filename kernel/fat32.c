// FAT32 driver for the Raspberry Pi boot partition (bootfs).
//
// This file only understands FAT32: the BPB, the FAT chains, directory
// entries with long names, and file data.  It is used for /boot and for
// firmware loading.  It does NOT know where the xv6 root filesystem lives;
// that is rootdev.c.  rootdev.c calls fat32mount() for the boot partition it
// found in the MBR, and, only for old cards that keep the root image as the
// file FS.IMG, fat32mapfile()/fat32mapsector() to translate file sectors to
// card sectors.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "fat32.h"

#define SECTOR_SIZE       512
#define FAT32_EOC         0x0ffffff8U
#define MAX_FILE_CLUSTERS 65536
#define FAT_LFN_MAX 20
#define FAT_LFN_CHARS 255
#define FAT_LFN_STORAGE (FAT_LFN_MAX * 13)

static struct {
  int fat;
  int mapped;              // file_size/clusters describe a mapped file
  uint32 partition_lba;
  uint32 fat_lba;
  uint32 data_lba;
  uint32 sectors_per_cluster;
  uint32 total_clusters;
  uint32 root_cluster;
  uint32 fat_sectors;
  uint32 fat_count;
  uint32 reserved_sectors;
  uint32 fsinfo_sector;
  uint32 backup_sector;
  uint32 next_alloc_cluster;
  uint32 file_size;
  uint32 cluster_count;
  uint32 clusters[MAX_FILE_CLUSTERS];
} diskmap;

static uchar scratch[SECTOR_SIZE];
static struct spinlock fat32_lock;

static uint16 le16(const uchar *p);
static uint32 le32(const uchar *p);
static uint32 cluster_lba(uint32 cluster);
static uint32 fat_next(uint32 cluster);
static int fat_set_next(uint32 cluster, uint32 next);
static int fat_alloc_cluster(uint32 *cluster_out);
static int fat_free_chain(uint32 cluster);
static int fat_mark_fsinfo_unknown(void);
static uint32 fat_slot_cluster(const uchar entry[32]);
static void entry_name(uchar *entry, char *out);

int
fat32ready(void)
{
  return diskmap.fat;
}

static int
path_to_name11(char *path, char name[11])
{
  int base = 0, ext = 8;
  if(path == 0 || *path++ != '/' || *path == 0)
    return -1;
  memset(name, ' ', 11);
  while(*path && *path != '.'){
    char c = *path++;
    if(base == 8 || c == '/') return -1;
    if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
    name[base++] = c;
  }
  if(*path == '.') path++;
  while(*path){
    char c = *path++;
    if(ext == 11 || c == '/') return -1;
    if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
    name[ext++] = c;
  }
  return 0;
}

struct fat_dir_slot {
  uint32 sector;
  uint16 offset;
  uchar entry[32];
};

struct fat_path_match {
  struct fat_dir_slot short_slot;
  int lfn_count;
  struct fat_dir_slot lfn_slots[FAT_LFN_MAX];
};

// FAT operations are serialized by fat32_lock.  Keep the large path/LFN
// scratch objects here instead of nesting them on xv6's single-page kernel
// stack.  fat32createfile() used to consume roughly another page when it
// called fat_find_path(), which could corrupt the stack before the first
// directory-allocation diagnostic was printed.
static struct {
  char path_name[FAT_LFN_CHARS + 1];
  uint16 lfn_chars[FAT_LFN_STORAGE];
  struct fat_dir_slot lfn_slots[FAT_LFN_MAX];
  struct fat_path_match match;
  struct fat_dir_slot create_slots[FAT_LFN_MAX + 2];
} fat_work;

static int
fat_path_name(char *path, char name[FAT_LFN_CHARS + 1], int *length)
{
  int n = 0;
  if(path == 0 || path[0] != '/' || path[1] == 0)
    return -1;
  for(char *p = path + 1; *p; p++){
    uchar c = *p;
    if(c == '/' || c < 0x20 || c > 0x7e ||
       c == '"' || c == '*' || c == ':' || c == '<' ||
       c == '>' || c == '?' || c == '\\' || c == '|')
      return -1;
    if(n == FAT_LFN_CHARS)
      return -1;
    name[n++] = c;
  }
  if(n == 0 || name[n - 1] == '.' || name[n - 1] == ' ')
    return -1;
  name[n] = 0;
  *length = n;
  return 0;
}

static uchar
fat_lfn_checksum(const uchar name[11])
{
  uchar sum = 0;
  for(int i = 0; i < 11; i++)
    sum = (sum >> 1) + (sum << 7) + name[i];
  return sum;
}

static uint16
fat_lfn_char(const uchar entry[32], int index)
{
  static const uchar offsets[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
  };
  int off = offsets[index % 13];
  return le16(entry + off);
}

static int
fat_lfn_matches(const uint16 chars[FAT_LFN_STORAGE],
                const char *name, int length)
{
  int i;
  for(i = 0; i < length; i++){
    uint16 c = chars[i];
    char expected = name[i];
    if(expected >= 'a' && expected <= 'z')
      expected -= 'a' - 'A';
    if(c > 0x7f)
      return 0;
    if(c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    if(c != (uchar)expected)
      return 0;
  }
  return chars[length] == 0 || chars[length] == 0xffff;
}

static int
fat_find_path(char *path, struct fat_path_match *match)
{
  char *name = fat_work.path_name;
  uint16 *lfn_chars = fat_work.lfn_chars;
  struct fat_dir_slot *lfn_slots = fat_work.lfn_slots;
  char short_name[11];
  uint32 cluster = diskmap.root_cluster;
  int name_len, have_short, lfn_count = 0, next_order = 0;
  uchar lfn_sum = 0;

  if(!diskmap.fat || match == 0 ||
     fat_path_name(path, name, &name_len) < 0)
    return -1;
  have_short = path_to_name11(path, short_name) == 0;
  memset(lfn_chars, 0xff, sizeof(lfn_chars));
  for(uint32 visited = 0; visited < diskmap.total_clusters; visited++){
    if(cluster < 2 || cluster >= diskmap.total_clusters + 2)
      return -1;
    uint32 first = cluster_lba(cluster);
    for(uint32 s = 0; s < diskmap.sectors_per_cluster; s++){
      uint32 sector = first + s;
      if(sdsector(sector, scratch, 0) < 0)
        return -1;
      for(int off = 0; off < SECTOR_SIZE; off += 32){
        uchar *entry = scratch + off;
        if(entry[0] == 0)
          return 0;
        if(entry[0] == 0xe5){
          lfn_count = next_order = 0;
          continue;
        }
        if(entry[11] == 0x0f){
          int order = entry[0] & 0x1f;
          if(entry[0] & 0x40){
            memset(lfn_chars, 0xff, sizeof(lfn_chars));
            lfn_count = order <= FAT_LFN_MAX ? 0 : FAT_LFN_MAX;
            next_order = order;
            lfn_sum = entry[13];
          }
          if(order == 0 || order > FAT_LFN_MAX ||
             order != next_order || entry[12] != 0 ||
             le16(entry + 26) != 0 || entry[13] != lfn_sum ||
             lfn_count >= FAT_LFN_MAX){
            lfn_count = next_order = 0;
            continue;
          }
          for(int i = 0; i < 13; i++)
            lfn_chars[(order - 1) * 13 + i] = fat_lfn_char(entry, i);
          lfn_slots[lfn_count].sector = sector;
          lfn_slots[lfn_count].offset = off;
          memmove(lfn_slots[lfn_count].entry, entry, 32);
          lfn_count++;
          next_order--;
          continue;
        }
        if(entry[11] & 0x08){
          lfn_count = next_order = 0;
          continue;
        }
        int short_match = have_short &&
                          memcmp(entry, short_name, 11) == 0;
        int long_match = lfn_count > 0 && next_order == 0 &&
                         lfn_sum == fat_lfn_checksum(entry) &&
                         fat_lfn_matches(lfn_chars, name, name_len);
        if(short_match || long_match){
          match->short_slot.sector = sector;
          match->short_slot.offset = off;
          memmove(match->short_slot.entry, entry, 32);
          match->lfn_count = long_match ? lfn_count : 0;
          if(long_match)
            memmove(match->lfn_slots, lfn_slots,
                    lfn_count * sizeof(lfn_slots[0]));
          return 1;
        }
        lfn_count = next_order = 0;
      }
    }
    uint32 next = fat_next(cluster);
    if(next >= FAT32_EOC)
      return 0;
    if(next < 2)
      return -1;
    cluster = next;
  }
  return -1;
}

static int
fat_find_root(char name[11], struct fat_dir_slot *found,
              struct fat_dir_slot *free_slot, uint32 *last_cluster)
{
  uint32 cluster = diskmap.root_cluster;
  struct fat_dir_slot available;
  int have_available = 0;

  for(uint32 visited = 0; visited < diskmap.total_clusters; visited++){
    if(cluster < 2 || cluster >= diskmap.total_clusters + 2)
      return -1;
    uint32 first = cluster_lba(cluster);
    for(uint32 s = 0; s < diskmap.sectors_per_cluster; s++){
      uint32 sector = first + s;
      if(sdsector(sector, scratch, 0) < 0)
        return -1;
      for(int off = 0; off < SECTOR_SIZE; off += 32){
        uchar *entry = scratch + off;
        if(entry[0] == 0){
          if(!have_available){
            available.sector = sector;
            available.offset = off;
            memset(available.entry, 0, sizeof(available.entry));
            have_available = 1;
          }
          if(free_slot)
            *free_slot = available;
          if(last_cluster)
            *last_cluster = cluster;
          return 0;
        }
        if(entry[0] == 0xe5){
          if(!have_available){
            available.sector = sector;
            available.offset = off;
            memmove(available.entry, entry, sizeof(available.entry));
            have_available = 1;
          }
          continue;
        }
        if(entry[11] == 0x0f || (entry[11] & 0x08))
          continue;
        if(memcmp(entry, name, 11) == 0){
          if(found){
            found->sector = sector;
            found->offset = off;
            memmove(found->entry, entry, sizeof(found->entry));
          }
          if(last_cluster)
            *last_cluster = cluster;
          return 1;
        }
      }
    }
    uint32 next = fat_next(cluster);
    if(next >= FAT32_EOC){
      if(free_slot && have_available)
        *free_slot = available;
      if(last_cluster)
        *last_cluster = cluster;
      return 0;
    }
    if(next < 2)
      return -1;
    cluster = next;
  }
  return -1;
}

static int
fat_write_slot(struct fat_dir_slot *slot, const uchar entry[32])
{
  if(sdsector(slot->sector, scratch, 0) < 0)
    return -1;
  memmove(scratch + slot->offset, entry, 32);
  return sdsector(slot->sector, scratch, 1);
}

static int
fat_reserve_root_slots(int count, struct fat_dir_slot *slots)
{
  int started = 0, found = 0;
  uint32 cluster = diskmap.root_cluster;
  uint32 tail = 0;
  if(count <= 0 || count > FAT_LFN_MAX + 2)
    return -1;

  for(uint32 visited = 0; visited < diskmap.total_clusters; visited++){
    if(cluster < 2 || cluster >= diskmap.total_clusters + 2)
      return -1;
    uint32 first = cluster_lba(cluster);
    for(uint32 s = 0; s < diskmap.sectors_per_cluster; s++){
      uint32 sector = first + s;
      if(sdsector(sector, scratch, 0) < 0)
        return -1;
      for(int off = 0; off < SECTOR_SIZE; off += 32){
        if(!started && scratch[off] == 0)
          started = 1;
        if(started){
          slots[found].sector = sector;
          slots[found].offset = off;
          memmove(slots[found].entry, scratch + off, 32);
          if(++found == count)
            return 0;
        }
      }
    }
    tail = cluster;
    uint32 next = fat_next(cluster);
    if(next >= FAT32_EOC){
      uint32 new_cluster;
      printf("fat32: extending root directory after cluster %d\n", tail);
      if(fat_alloc_cluster(&new_cluster) < 0 ||
         fat_set_next(tail, new_cluster) < 0)
        return -1;
      printf("fat32: root directory extended with cluster %d\n",
             new_cluster);
      cluster = new_cluster;
      started = 1;
      continue;
    }
    if(next < 2)
      return -1;
    cluster = next;
  }
  return -1;
}

static void
fat_lfn_set_char(uchar entry[32], int index, uint16 value)
{
  static const uchar offsets[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
  };
  int off = offsets[index % 13];
  entry[off] = value;
  entry[off + 1] = value >> 8;
}

static void
fat_make_lfn_entry(uchar entry[32], const char *name, int length,
                   int order, int count, uchar checksum)
{
  memset(entry, 0xff, 32);
  entry[0] = order | (order == count ? 0x40 : 0);
  entry[11] = 0x0f;
  entry[12] = 0;
  entry[13] = checksum;
  entry[26] = 0;
  entry[27] = 0;
  for(int i = 0; i < 13; i++){
    int char_index = (order - 1) * 13 + i;
    uint16 value = 0xffff;
    if(char_index < length)
      value = (uchar)name[char_index];
    else if(char_index == length)
      value = 0;
    fat_lfn_set_char(entry, i, value);
  }
}

static int
fat_make_short_alias(const char *name, int length, char short_name[11])
{
  char base[8], ext[3];
  int base_len = 0, ext_len = 0, dot = -1;
  memset(short_name, ' ', 11);
  for(int i = 0; i < length; i++)
    if(name[i] == '.')
      dot = i;
  for(int i = 0; i < (dot < 0 ? length : dot) && base_len < 6; i++){
    char c = name[i];
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9')){
      if(c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
      base[base_len++] = c;
    }
  }
  if(base_len == 0){
    base[0] = 'X';
    base[1] = 'V';
    base[2] = '6';
    base_len = 3;
  }
  if(dot >= 0){
    for(int i = dot + 1; i < length && ext_len < 3; i++){
      char c = name[i];
      if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9')){
        if(c >= 'a' && c <= 'z')
          c -= 'a' - 'A';
        ext[ext_len++] = c;
      }
    }
  }
  memmove(short_name, base, base_len);
  short_name[base_len] = '~';
  short_name[base_len + 1] = '1';
  memmove(short_name + 8, ext, ext_len);
  return 0;
}

static void
entry_name(uchar *entry, char *out)
{
  int n = 0;
  for(int i = 0; i < 8 && entry[i] != ' '; i++) out[n++] = entry[i];
  if(entry[8] != ' '){
    out[n++] = '.';
    for(int i = 8; i < 11 && entry[i] != ' '; i++) out[n++] = entry[i];
  }
  out[n] = 0;
}

static int
fat32readdirroot_locked(int index, struct fat32_dirent *de)
{
  uint32 cluster;
  int seen = 0;
  uint16 lfn_chars[FAT_LFN_STORAGE];
  int lfn_count = 0, next_order = 0;
  uchar lfn_sum = 0;
  if(!diskmap.fat || index < 0 || de == 0 ||
     sdsector(diskmap.partition_lba, scratch, 0) < 0)
    return -1;
  memset(lfn_chars, 0xff, sizeof(lfn_chars));
  cluster = le32(scratch + 44);
  for(uint32 visited = 0; visited < MAX_FILE_CLUSTERS; visited++){
    if(cluster < 2 || cluster >= FAT32_EOC) return 0;
    uint32 first = cluster_lba(cluster);
    for(uint32 s = 0; s < diskmap.sectors_per_cluster; s++){
      if(sdsector(first + s, scratch, 0) < 0) return -1;
      for(int off = 0; off < SECTOR_SIZE; off += 32){
        uchar *e = scratch + off;
        if(e[0] == 0) return 0;
        if(e[0] == 0xe5){
          lfn_count = next_order = 0;
          continue;
        }
        if(e[11] == 0x0f){
          int order = e[0] & 0x1f;
          if(e[0] & 0x40){
            memset(lfn_chars, 0xff, sizeof(lfn_chars));
            lfn_count = 0;
            next_order = order;
            lfn_sum = e[13];
          }
          if(order == 0 || order > FAT_LFN_MAX || order != next_order ||
             e[12] != 0 || le16(e + 26) != 0 || e[13] != lfn_sum){
            lfn_count = next_order = 0;
            continue;
          }
          for(int i = 0; i < 13; i++)
            lfn_chars[(order - 1) * 13 + i] = fat_lfn_char(e, i);
          lfn_count++;
          next_order--;
          continue;
        }
        if(e[11] & 0x08){
          lfn_count = next_order = 0;
          continue;
        }
        if(seen++ != index){
          lfn_count = next_order = 0;
          continue;
        }
        memset(de, 0, sizeof(*de));
        de->cluster = ((uint32)le16(e + 20) << 16) | le16(e + 26);
        de->size = le32(e + 28);
        de->directory = (e[11] & 0x10) != 0;
        if(lfn_count > 0 && next_order == 0 &&
           lfn_sum == fat_lfn_checksum(e)){
          int n = 0;
          while(n < (int)sizeof(de->name) - 1 &&
                lfn_chars[n] != 0 && lfn_chars[n] != 0xffff &&
                lfn_chars[n] <= 0x7f){
            de->name[n] = lfn_chars[n];
            n++;
          }
          de->name[n] = 0;
          if(n == 0)
            entry_name(e, de->name);
        } else {
          entry_name(e, de->name);
        }
        return 1;
      }
    }
    cluster = fat_next(cluster);
  }
  return 0;
}

int
fat32readdirroot(int index, struct fat32_dirent *de)
{
  acquire(&fat32_lock);
  int result = fat32readdirroot_locked(index, de);
  release(&fat32_lock);
  return result;
}

static int
fat32statpath_locked(char *path, struct fat32_dirent *de)
{
  struct fat_path_match match;
  if(path[0] == '/' && path[1] == 0){
    memset(de, 0, sizeof(*de)); de->directory = 1; return 0;
  }
  if(fat_find_path(path, &match) != 1)
    return -1;
  memset(de, 0, sizeof(*de));
  de->cluster = fat_slot_cluster(match.short_slot.entry);
  de->size = le32(match.short_slot.entry + 28);
  de->directory = (match.short_slot.entry[11] & 0x10) != 0;
  entry_name(match.short_slot.entry, de->name);
  return 0;
}

int
fat32statpath(char *path, struct fat32_dirent *de)
{
  acquire(&fat32_lock);
  int result = fat32statpath_locked(path, de);
  release(&fat32_lock);
  return result;
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

int
fat32_is_boot_sector(const uchar *s)
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
  uint32 entries_per_sector = SECTOR_SIZE / 4;
  uint32 sector = diskmap.fat_lba + cluster / entries_per_sector;
  uint32 pos = (cluster % entries_per_sector) * 4;
  if(sdsector(sector, scratch, 0) < 0)
    panic("fat32: read FAT");
  return le32(scratch + pos) & 0x0fffffffU;
}

static int
fat_set_next(uint32 cluster, uint32 next)
{
  uint32 entries_per_sector = SECTOR_SIZE / 4;
  uint32 sector_offset = cluster / entries_per_sector;
  uint32 pos = (cluster % entries_per_sector) * 4;
  if(cluster < 2 || cluster >= diskmap.total_clusters + 2 ||
     sector_offset >= diskmap.fat_sectors)
    return -1;
  for(uint32 copy = 0; copy < diskmap.fat_count; copy++){
    uint32 sector = diskmap.fat_lba + copy * diskmap.fat_sectors +
                    sector_offset;
    if(sdsector(sector, scratch, 0) < 0)
      return -1;
    uint32 current = le32(scratch + pos);
    uint32 value = (current & 0xf0000000U) | (next & 0x0fffffffU);
    scratch[pos] = value;
    scratch[pos + 1] = value >> 8;
    scratch[pos + 2] = value >> 16;
    scratch[pos + 3] = value >> 24;
    if(sdsector(sector, scratch, 1) < 0)
      return -1;
  }
  return 0;
}

static int
fat_mark_fsinfo_unknown(void)
{
  uint32 sectors[2], count = 0;
  if(diskmap.fsinfo_sector == 0 || diskmap.fsinfo_sector == 0xffff)
    return 0;
  sectors[count++] = diskmap.fsinfo_sector;
  if(diskmap.backup_sector != 0 &&
     diskmap.backup_sector != 0xffff &&
     diskmap.backup_sector + diskmap.fsinfo_sector <
       diskmap.reserved_sectors)
    sectors[count++] = diskmap.backup_sector + diskmap.fsinfo_sector;
  for(uint32 i = 0; i < count; i++){
    if(sectors[i] == 0xffff ||
       sdsector(diskmap.partition_lba + sectors[i], scratch, 0) < 0)
      return -1;
    if(le32(scratch) != 0x41615252U ||
       le32(scratch + 484) != 0x61417272U)
      continue;
    memset(scratch + 488, 0xff, 4);
    scratch[492] = diskmap.next_alloc_cluster;
    scratch[493] = diskmap.next_alloc_cluster >> 8;
    scratch[494] = diskmap.next_alloc_cluster >> 16;
    scratch[495] = diskmap.next_alloc_cluster >> 24;
    if(sdsector(diskmap.partition_lba + sectors[i], scratch, 1) < 0)
      return -1;
  }
  return 0;
}

static int
fat_alloc_cluster(uint32 *cluster_out)
{
  uint32 start = diskmap.next_alloc_cluster;
  uint32 loaded_sector = 0xffffffffU;
  if(start < 2 || start >= diskmap.total_clusters + 2)
    start = 2;

  // Cache the FAT sector while scanning candidates.  A 512-byte FAT sector
  // contains 128 FAT32 entries; fat_next() here used to reread that same
  // sector once per candidate and made root-directory growth appear hung.
  for(uint32 checked = 0; checked < diskmap.total_clusters; checked++){
    uint32 start_index = start - 2;
    uint32 index = checked < diskmap.total_clusters - start_index
                     ? start_index + checked
                     : checked - (diskmap.total_clusters - start_index);
    uint32 cluster = index + 2;
    uint32 sector_offset = cluster / (SECTOR_SIZE / 4);
    uint32 pos = (cluster % (SECTOR_SIZE / 4)) * 4;
    if(sector_offset != loaded_sector){
      if(sector_offset >= diskmap.fat_sectors ||
         sdsector(diskmap.fat_lba + sector_offset, scratch, 0) < 0)
        return -1;
      loaded_sector = sector_offset;
    }
    if((le32(scratch + pos) & 0x0fffffffU) != 0)
      continue;
    if(fat_set_next(cluster, 0x0fffffffU) < 0)
      return -1;
    memset(scratch, 0, sizeof(scratch));
    uint32 first = cluster_lba(cluster);
    for(uint32 i = 0; i < diskmap.sectors_per_cluster; i++)
      if(sdsector(first + i, scratch, 1) < 0)
        return -1;
    diskmap.next_alloc_cluster = cluster + 1;
    if(diskmap.next_alloc_cluster >= diskmap.total_clusters + 2)
      diskmap.next_alloc_cluster = 2;
    if(fat_mark_fsinfo_unknown() < 0)
      return -1;
    *cluster_out = cluster;
    return 0;
  }
  return -1;
}

static int
fat_free_chain(uint32 cluster)
{
  uint32 loaded_sector = 0xffffffffU;
  int changed = 0;

  // A camera image occupies many adjacent clusters.  The old implementation
  // called fat_next() plus fat_set_next() for every cluster, causing one FAT
  // read and two read/modify/write operations per cluster on a two-copy FAT.
  // Truncating a 320x240 BMP could therefore issue more than 500 SD-sector
  // transactions before userspace printed its next message.  Keep one FAT
  // sector in scratch, clear all chain entries found in it, then mirror that
  // complete sector to every FAT copy.
  for(uint32 visited = 0; cluster >= 2 &&
       cluster < diskmap.total_clusters + 2 &&
       visited < diskmap.total_clusters; visited++){
    uint32 sector_offset = cluster / (SECTOR_SIZE / 4);
    uint32 pos = (cluster % (SECTOR_SIZE / 4)) * 4;
    if(sector_offset >= diskmap.fat_sectors)
      return -1;
    if(sector_offset != loaded_sector){
      if(loaded_sector != 0xffffffffU){
        for(uint32 copy = 0; copy < diskmap.fat_count; copy++)
          if(sdsector(diskmap.fat_lba + copy * diskmap.fat_sectors +
                      loaded_sector, scratch, 1) < 0)
            return -1;
      }
      if(sdsector(diskmap.fat_lba + sector_offset, scratch, 0) < 0)
        return -1;
      loaded_sector = sector_offset;
    }
    uint32 current = le32(scratch + pos);
    uint32 next = current & 0x0fffffffU;
    uint32 value = current & 0xf0000000U;
    scratch[pos] = value;
    scratch[pos + 1] = value >> 8;
    scratch[pos + 2] = value >> 16;
    scratch[pos + 3] = value >> 24;
    changed = 1;
    if(next == 0 || next >= FAT32_EOC){
      cluster = 0;
      break;
    }
    cluster = next;
  }
  if(loaded_sector != 0xffffffffU){
    for(uint32 copy = 0; copy < diskmap.fat_count; copy++)
      if(sdsector(diskmap.fat_lba + copy * diskmap.fat_sectors +
                  loaded_sector, scratch, 1) < 0)
        return -1;
  }
  if(changed && fat_mark_fsinfo_unknown() < 0)
    return -1;
  return cluster == 0 ? 0 : -1;
}

static int
short_name_equal(const uchar *entry, const char *name11)
{
  for(int i = 0; i < 11; i++)
    if(entry[i] != (uchar)name11[i])
      return 0;
  return 1;
}

static int
find_root_file(uint32 root, const char *name11, uint32 *first_cluster,
               uint32 *size)
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
        if(entry[0] == 0xe5 || entry[11] == 0x0f ||
           (entry[11] & 0x18))
          continue;
        if(short_name_equal(entry, name11)){
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

// Mount the FAT32 volume whose boot sector is at partition_lba.
int
fat32mount(uint32 partition_lba, uint32 partition_sectors)
{
  if(sdsector(partition_lba, scratch, 0) < 0 ||
     !fat32_is_boot_sector(scratch))
    return -1;

  uint32 reserved = le16(scratch + 14);
  uint32 fats = scratch[16];
  uint32 fat_sectors = le32(scratch + 36);
  uint32 root = le32(scratch + 44);
  uint32 total_sectors = le16(scratch + 19);
  if(total_sectors == 0)
    total_sectors = le32(scratch + 32);
  uint32 fsinfo = le16(scratch + 48);
  uint32 backup = le16(scratch + 50);
  if(fats == 0 || fat_sectors == 0 || scratch[13] == 0 ||
     (scratch[13] & (scratch[13] - 1)) != 0 ||
     fat_sectors > (0xffffffffU - reserved) / fats ||
     total_sectors <= reserved + fats * fat_sectors ||
     (partition_sectors != 0 && total_sectors > partition_sectors) ||
     total_sectors > 0xffffffffU - partition_lba ||
     fat_sectors > 0xffffffffU / (SECTOR_SIZE / 4) ||
     (fsinfo != 0xffff && fsinfo != 0 && fsinfo >= reserved) ||
     (backup != 0xffff && backup != 0 && backup >= reserved))
    return -1;
  uint32 fat_area = fats * fat_sectors;

  diskmap.partition_lba = partition_lba;
  diskmap.sectors_per_cluster = scratch[13];
  diskmap.total_clusters =
    (total_sectors - reserved - fat_area) / scratch[13];
  diskmap.root_cluster = root;
  diskmap.fat_count = fats;
  diskmap.fat_sectors = fat_sectors;
  diskmap.reserved_sectors = reserved;
  diskmap.fsinfo_sector = fsinfo;
  diskmap.backup_sector = backup;
  diskmap.next_alloc_cluster = 2;
  if(diskmap.total_clusters == 0 ||
     diskmap.total_clusters > fat_sectors * (SECTOR_SIZE / 4) - 2 ||
     root < 2 || root >= diskmap.total_clusters + 2 ||
     (backup != 0 && backup != 0xffff && fsinfo != 0xffff &&
      backup + fsinfo >= reserved))
    return -1;
  diskmap.fat_lba = partition_lba + reserved;
  diskmap.data_lba = diskmap.fat_lba + fat_area;

  // FSInfo offset 492 is an advisory next-free-cluster hint.  Use it only
  // when both signatures and the cluster range are valid; otherwise the
  // sector-cached full scan in fat_alloc_cluster() remains the fallback.
  if(fsinfo != 0 && fsinfo != 0xffff &&
     sdsector(partition_lba + fsinfo, scratch, 0) == 0 &&
     le32(scratch) == 0x41615252U &&
     le32(scratch + 484) == 0x61417272U){
    uint32 hint = le32(scratch + 492);
    if(hint >= 2 && hint < diskmap.total_clusters + 2)
      diskmap.next_alloc_cluster = hint;
  }

  // scratch now holds FSInfo, not the BPB; bytes/sector was checked above.
  printf("fat32: BPB lba=%d bytes/sector=%d sectors/cluster=%d\n",
         partition_lba, SECTOR_SIZE, diskmap.sectors_per_cluster);
  printf("fat32: reserved=%d FATs=%d sectors/FAT=%d root-cluster=%d\n",
         reserved, fats, fat_sectors, root);
  printf("fat32: FAT lba=%d data lba=%d\n",
         diskmap.fat_lba, diskmap.data_lba);
  printf("fat32: allocator next-free hint=%d\n",
         diskmap.next_alloc_cluster);

  diskmap.fat = 1;
  diskmap.mapped = 0;
  diskmap.cluster_count = 0;
  diskmap.file_size = 0;
  return 0;
}

int
fat32openroot(char *name11, struct fat32_file *file)
{
  int result;
  if(!diskmap.fat || name11 == 0 || file == 0 || strlen(name11) != 11)
    return -1;
  acquire(&fat32_lock);
  if(sdsector(diskmap.partition_lba, scratch, 0) < 0)
    goto bad;
  uint32 root = le32(scratch + 44);
  result = find_root_file(root, name11, &file->first_cluster, &file->size);
  if(result == 0){
    file->last_cluster = 0;
    file->last_cluster_index = 0;
  }
  release(&fat32_lock);
  return result;
bad:
  release(&fat32_lock);
  return -1;
}

static void
fat_slot_set_cluster(uchar entry[32], uint32 cluster)
{
  entry[20] = cluster >> 16;
  entry[21] = cluster >> 24;
  entry[26] = cluster;
  entry[27] = cluster >> 8;
}

static uint32
fat_slot_cluster(const uchar entry[32])
{
  return ((uint32)entry[20] << 16) | ((uint32)entry[21] << 24) |
         entry[26] | ((uint32)entry[27] << 8);
}

static int
fat_load_write_state(char *path, struct fat32_file *file,
                     struct fat_dir_slot *slot)
{
  struct fat_path_match match;
  if(fat_find_path(path, &match) != 1 ||
     (match.short_slot.entry[11] & 0x10))
    return -1;
  *slot = match.short_slot;
  file->first_cluster = fat_slot_cluster(slot->entry);
  file->size = le32(slot->entry + 28);
  file->last_cluster = 0;
  file->last_cluster_index = 0;
  if(file->first_cluster){
    uint32 cluster = file->first_cluster;
    for(uint32 index = 0; index < diskmap.total_clusters; index++){
      uint32 next;
      if(cluster < 2 || cluster >= diskmap.total_clusters + 2)
        return -1;
      file->last_cluster = cluster;
      file->last_cluster_index = index;
      next = fat_next(cluster);
      if(next >= FAT32_EOC)
        return 0;
      if(next < 2)
        return -1;
      cluster = next;
    }
    return -1;
  }
  return file->size == 0 ? 0 : -1;
}

int
fat32openwrite(char *path, struct fat32_file *file)
{
  struct fat_dir_slot slot;
  if(!diskmap.fat || path == 0 || file == 0)
    return -1;
  acquire(&fat32_lock);
  int result = fat_load_write_state(path, file, &slot);
  release(&fat32_lock);
  return result;
}

int
fat32openpath(char *path, struct fat32_file *file)
{
  struct fat_dir_slot slot;
  if(!diskmap.fat || path == 0 || file == 0)
    return -1;
  acquire(&fat32_lock);
  int result = fat_load_write_state(path, file, &slot);
  release(&fat32_lock);
  return result;
}

int
fat32createfile(char *path)
{
  char name[FAT_LFN_CHARS + 1], short_name[11], parsed_short[11];
  struct fat_path_match *match = &fat_work.match;
  struct fat_dir_slot *slots = fat_work.create_slots;
  uchar entry[32];
  int length, lfn_count, slot_count, exists;
  if(!diskmap.fat || fat_path_name(path, name, &length) < 0)
    return -1;
  acquire(&fat32_lock);
  printf("fat32: create lookup begin path=%s\n", path);
  exists = fat_find_path(path, match);
  printf("fat32: create lookup complete exists=%d\n", exists);
  if(exists != 0)
    goto bad;
  if(path_to_name11(path, parsed_short) == 0){
    memmove(short_name, parsed_short, sizeof(short_name));
    lfn_count = 0;
  } else {
    fat_make_short_alias(name, length, short_name);
    lfn_count = (length + 12) / 13;
    int available = 0;
    for(int suffix = '1'; suffix <= '9'; suffix++){
      short_name[7] = suffix;
      int found = fat_find_root(short_name, 0, 0, 0);
      if(found == 0){
        available = 1;
        break;
      }
      if(found < 0)
        goto bad;
    }
    if(!available)
      goto bad;
  }
  slot_count = lfn_count + 2;
  printf("fat32: create reserve begin slots=%d\n", slot_count);
  if(fat_reserve_root_slots(slot_count, slots) < 0)
    goto bad;
  printf("fat32: create reserve complete\n");
  for(int i = 0; i < lfn_count; i++){
    fat_make_lfn_entry(entry, name, length, lfn_count - i,
                       lfn_count, fat_lfn_checksum((uchar*)short_name));
    if(fat_write_slot(&slots[i], entry) < 0)
      goto bad;
  }
  memset(entry, 0, sizeof(entry));
  memmove(entry, short_name, sizeof(short_name));
  entry[11] = 0x20;
  if(fat_write_slot(&slots[lfn_count], entry) < 0)
    goto bad;
  memset(entry, 0, sizeof(entry));
  if(fat_write_slot(&slots[lfn_count + 1], entry) < 0)
    goto bad;
  release(&fat32_lock);
  return 0;
bad:
  release(&fat32_lock);
  return -1;
}

int
fat32truncatefile(char *path, struct fat32_file *file)
{
  struct fat_path_match match;
  struct fat_dir_slot slot;
  uint32 old_cluster;
  if(!diskmap.fat || path == 0 || file == 0)
    return -1;
  acquire(&fat32_lock);
  if(fat_find_path(path, &match) != 1 ||
     (match.short_slot.entry[11] & 0x10))
    goto bad;
  slot = match.short_slot;
  old_cluster = fat_slot_cluster(slot.entry);
  fat_slot_set_cluster(slot.entry, 0);
  memset(slot.entry + 28, 0, 4);
  if(fat_write_slot(&slot, slot.entry) < 0)
    goto bad;
  memset(file, 0, sizeof(*file));
  if(old_cluster && fat_free_chain(old_cluster) < 0)
    goto bad;
  release(&fat32_lock);
  return 0;
bad:
  release(&fat32_lock);
  return -1;
}

int
fat32writefile(char *path, struct fat32_file *file, uint32 offset,
               int user_src, uint64 src, int n)
{
  struct fat_path_match match;
  struct fat_dir_slot slot;
  uint32 cluster_bytes;
  int done = 0, error = 0;

  if(!diskmap.fat || file == 0 || n < 0 ||
     (uint32)n > 0xffffffffU - offset ||
     path == 0)
    return -1;
  if(n == 0)
    return 0;
  acquire(&fat32_lock);
  if(offset != file->size ||
     fat_find_path(path, &match) != 1 ||
     (match.short_slot.entry[11] & 0x10) ||
     fat_slot_cluster(match.short_slot.entry) != file->first_cluster ||
     le32(match.short_slot.entry + 28) != file->size){
    error = 1;
    goto out;
  }
  slot = match.short_slot;

  cluster_bytes = diskmap.sectors_per_cluster * SECTOR_SIZE;
  while(done < n){
    uint32 position = offset + done;
    uint32 index = position / cluster_bytes;
    uint32 cluster;
    uint32 in_cluster = position % cluster_bytes;
    uint32 sector;
    uint32 sector_offset = in_cluster % SECTOR_SIZE;
    int take = SECTOR_SIZE - sector_offset;
    while(file->last_cluster == 0 || index > file->last_cluster_index){
      uint32 new_cluster;
      if(fat_alloc_cluster(&new_cluster) < 0 ||
         (file->last_cluster &&
          fat_set_next(file->last_cluster, new_cluster) < 0)){
        error = 1;
        goto write_done;
      }
      if(file->last_cluster == 0)
        file->first_cluster = new_cluster;
      file->last_cluster = new_cluster;
      if(file->last_cluster_index != 0 ||
         file->first_cluster != new_cluster)
        file->last_cluster_index++;
    }
    if(index != file->last_cluster_index){
      error = 1;
      goto write_done;
    }
    cluster = file->last_cluster;
    sector = cluster_lba(cluster) + (in_cluster / SECTOR_SIZE);
    if(take > n - done)
      take = n - done;
    if(sector_offset || take != SECTOR_SIZE){
      if(sdsector(sector, scratch, 0) < 0){
        error = 1;
        break;
      }
    }
    if(either_copyin(scratch + sector_offset, user_src, src + done, take) < 0){
      error = 1;
      break;
    }
    if(sdsector(sector, scratch, 1) < 0){
      error = 1;
      break;
    }
    done += take;
  }

write_done:
  if(done > 0){
    file->size = offset + done;
    fat_slot_set_cluster(slot.entry, file->first_cluster);
    slot.entry[28] = file->size;
    slot.entry[29] = file->size >> 8;
    slot.entry[30] = file->size >> 16;
    slot.entry[31] = file->size >> 24;
    if(fat_write_slot(&slot, slot.entry) < 0)
      error = 1;
  }
out:
  release(&fat32_lock);
  return done > 0 ? done : (error ? -1 : 0);
}

int
fat32renamefile(char *oldpath, char *newpath)
{
  char oldname[11], newname[11];
  struct fat_dir_slot source, target;
  uint32 old_cluster = 0, new_cluster;
  int found;

  if(!diskmap.fat || path_to_name11(oldpath, oldname) < 0 ||
     path_to_name11(newpath, newname) < 0 ||
     memcmp(oldname, newname, sizeof(oldname)) == 0)
    return -1;
  acquire(&fat32_lock);
  if(fat_find_root(oldname, &source, 0, 0) != 1 ||
     (source.entry[11] & 0x10))
    goto bad;
  new_cluster = fat_slot_cluster(source.entry);
  found = fat_find_root(newname, &target, 0, 0);
  if(found < 0)
    goto bad;
  if(found == 0){
    memmove(source.entry, newname, sizeof(newname));
    if(fat_write_slot(&source, source.entry) < 0)
      goto bad;
  } else {
    if(target.entry[11] & 0x10)
      goto bad;
    old_cluster = fat_slot_cluster(target.entry);
    fat_slot_set_cluster(target.entry, new_cluster);
    memmove(target.entry + 28, source.entry + 28, 4);
    source.entry[0] = 0xe5;
    if(target.sector == source.sector){
      if(sdsector(target.sector, scratch, 0) < 0)
        goto bad;
      memmove(scratch + target.offset, target.entry, 32);
      memmove(scratch + source.offset, source.entry, 32);
      if(sdsector(target.sector, scratch, 1) < 0)
        goto bad;
    } else {
      if(fat_write_slot(&target, target.entry) < 0 ||
         fat_write_slot(&source, source.entry) < 0)
        goto bad;
    }
    if(old_cluster && old_cluster != new_cluster &&
       fat_free_chain(old_cluster) < 0)
      goto bad;
  }
  release(&fat32_lock);
  return 0;
bad:
  release(&fat32_lock);
  return -1;
}

static int
fat32pread_locked(struct fat32_file *file, uint32 offset, void *dst, int n)
{
  uint32 cluster, cluster_bytes, skip;
  uchar *out = dst;
  int done = 0;
  if(file == 0 || dst == 0 || n < 0 || offset > file->size)
    return -1;
  if((uint32)n > file->size - offset)
    n = file->size - offset;
  cluster_bytes = diskmap.sectors_per_cluster * SECTOR_SIZE;
  cluster = file->first_cluster;
  skip = offset / cluster_bytes;
  for(uint32 i = 0; i < skip; i++){
    cluster = fat_next(cluster);
    if(cluster < 2 || cluster >= FAT32_EOC)
      return -1;
  }
  offset %= cluster_bytes;
  while(done < n){
    if(cluster < 2 || cluster >= FAT32_EOC)
      return -1;
    uint32 sector_index = offset / SECTOR_SIZE;
    uint32 sector_offset = offset % SECTOR_SIZE;
    while(sector_index < diskmap.sectors_per_cluster && done < n){
      if(sdsector(cluster_lba(cluster) + sector_index, scratch, 0) < 0)
        return -1;
      int take = SECTOR_SIZE - sector_offset;
      if(take > n - done)
        take = n - done;
      memmove(out + done, scratch + sector_offset, take);
      done += take;
      sector_index++;
      sector_offset = 0;
    }
    offset = 0;
    if(done < n)
      cluster = fat_next(cluster);
  }
  return done;
}

int
fat32pread(struct fat32_file *file, uint32 offset, void *dst, int n)
{
  acquire(&fat32_lock);
  int result = fat32pread_locked(file, offset, dst, n);
  release(&fat32_lock);
  return result;
}

int
fat32readerinit(struct fat32_reader *reader, struct fat32_file *file)
{
  if(reader == 0 || file == 0 || file->first_cluster < 2)
    return -1;
  memset(reader, 0, sizeof(*reader));
  reader->file = *file;
  reader->cluster = file->first_cluster;
  return 0;
}

// Sequential FAT32 reader.  Unlike fat32pread(), it retains the current
// cluster and therefore does not walk the chain from its beginning for every
// 512-byte firmware chunk.
static int
fat32readnext_locked(struct fat32_reader *reader, void *dst, int n)
{
  uint32 cluster_bytes;
  uchar *out = dst;
  int done = 0;

  if(reader == 0 || dst == 0 || n < 0 || reader->position > reader->file.size)
    return -1;
  if((uint32)n > reader->file.size - reader->position)
    n = reader->file.size - reader->position;
  cluster_bytes = diskmap.sectors_per_cluster * SECTOR_SIZE;

  while(done < n){
    uint32 sector_index, sector_offset;
    int take;

    if(reader->cluster < 2 || reader->cluster >= FAT32_EOC)
      return -1;
    sector_index = reader->cluster_offset / SECTOR_SIZE;
    sector_offset = reader->cluster_offset % SECTOR_SIZE;
    if(sdsector(cluster_lba(reader->cluster) + sector_index, scratch, 0) < 0)
      return -1;
    take = SECTOR_SIZE - sector_offset;
    if(take > n - done)
      take = n - done;
    memmove(out + done, scratch + sector_offset, take);
    done += take;
    reader->position += take;
    reader->cluster_offset += take;
    if(reader->cluster_offset == cluster_bytes &&
       reader->position < reader->file.size){
      reader->cluster = fat_next(reader->cluster);
      reader->cluster_offset = 0;
    }
  }
  return done;
}

int
fat32readnext(struct fat32_reader *reader, void *dst, int n)
{
  acquire(&fat32_lock);
  int result = fat32readnext_locked(reader, dst, n);
  release(&fat32_lock);
  return result;
}

void
fat32init(void)
{
  initlock(&fat32_lock, "fat32");
}

// Record the cluster chain of one root-directory file so that it can be
// addressed as a flat run of 512-byte sectors.  Only one file is mapped at a
// time; rootdev.c uses this for the FS.IMG compatibility layout.  Called
// once during boot, before other CPUs or processes touch FAT32.
int
fat32mapfile(char *name11, uint32 *bytes)
{
  uint32 first_cluster, file_size;
  if(!diskmap.fat || name11 == 0 || bytes == 0)
    return -1;
  diskmap.mapped = 0;
  diskmap.cluster_count = 0;
  diskmap.file_size = 0;
  if(find_root_file(diskmap.root_cluster, name11,
                    &first_cluster, &file_size) < 0)
    return -1;

  uint32 cluster_bytes = diskmap.sectors_per_cluster * SECTOR_SIZE;
  uint32 needed = (file_size + cluster_bytes - 1) / cluster_bytes;
  if(needed > MAX_FILE_CLUSTERS){
    printf("fat32: mapped file has too many clusters\n");
    return -1;
  }
  uint32 cluster = first_cluster;
  for(uint32 i = 0; i < needed; i++){
    if(cluster < 2 || cluster >= FAT32_EOC){
      printf("fat32: mapped file has a short cluster chain\n");
      diskmap.cluster_count = 0;
      return -1;
    }
    diskmap.clusters[i] = cluster;
    diskmap.cluster_count++;
    if(i + 1 < needed)
      cluster = fat_next(cluster);
  }
  diskmap.file_size = file_size;
  diskmap.mapped = 1;
  *bytes = file_size;
  return 0;
}

// Card sector holding sector file_sector of the file mapped above.  The map
// is read-only after boot, so no lock is needed.
int
fat32mapsector(uint32 file_sector, uint32 *lba)
{
  if(!diskmap.mapped || lba == 0)
    return -1;
  uint32 index = file_sector / diskmap.sectors_per_cluster;
  uint32 within = file_sector % diskmap.sectors_per_cluster;
  if(index >= diskmap.cluster_count)
    return -1;
  *lba = cluster_lba(diskmap.clusters[index]) + within;
  return 0;
}
