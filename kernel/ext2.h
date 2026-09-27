#ifndef XV6_EXT2_H
#define XV6_EXT2_H

#define EXT2_NAME_MAX 255

struct ext2_user_dirent {
  uint32 inode;
  uint32 size;
  uint16 mode;
  uint8 type;
  char name[EXT2_NAME_MAX + 1];
};

#endif
