#ifndef XV6_FAT32_H
#define XV6_FAT32_H

struct fat32_file {
  uint32 first_cluster;
  uint32 size;
};

struct fat32_reader {
  struct fat32_file file;
  uint32 cluster;
  uint32 cluster_offset;
  uint32 position;
};

struct fat32_dirent {
  uint32 cluster;
  uint32 size;
  int directory;
  char name[13];
};

int fat32ready(void);
int fat32openroot(char *name11, struct fat32_file *file);
int fat32pread(struct fat32_file *file, uint32 offset, void *dst, int n);
int fat32statpath(char *path, struct fat32_dirent *de);
int fat32readdirroot(int index, struct fat32_dirent *de);
int fat32readerinit(struct fat32_reader *reader, struct fat32_file *file);
int fat32readnext(struct fat32_reader *reader, void *dst, int n);

#endif
