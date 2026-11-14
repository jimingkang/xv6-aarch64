#ifndef XV6_BL1FAT_H
#define XV6_BL1FAT_H
typedef int (*sec_read_fn)(uint32 lba, void *dst, uint32 n);
int fat_mount(sec_read_fn rd, uint32 *part, uint32 *spc);
int fat_find(const char *name, uint32 *first, uint32 *size);  /* long or 8.3 name */
int fat_read(uint32 first, uint32 size, uint8 *dst);   /* writes whole clusters */
#endif
