// insmod PATH.ko -- load a relocatable kernel module.
//
// The user side of the =m mechanism.  insmod reads a whole .ko off the
// filesystem and hands the raw ELF image to init_module(); the kernel does the
// relocation and runs the driver's init.  This is the same division of labour
// as Linux's insmod, which is little more than open + read + finit_module.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"

#define MAX_IMAGE (64 * 1024)

int
main(int argc, char *argv[])
{
  if(argc != 2){
    fprintf(2, "usage: insmod PATH.ko\n");
    exit(1);
  }

  int fd = open(argv[1], 0);
  if(fd < 0){
    fprintf(2, "insmod: cannot open %s\n", argv[1]);
    exit(1);
  }

  struct stat st;
  if(fstat(fd, &st) < 0){
    fprintf(2, "insmod: cannot stat %s\n", argv[1]);
    exit(1);
  }
  if(st.size == 0 || st.size > MAX_IMAGE){
    fprintf(2, "insmod: %s is %d bytes, out of range\n", argv[1], (int)st.size);
    exit(1);
  }

  // Read the entire image into one contiguous buffer: the kernel loader
  // indexes the section, symbol and string tables by offset into this blob.
  char *image = malloc(st.size);
  if(image == 0){
    fprintf(2, "insmod: out of memory\n");
    exit(1);
  }
  int off = 0;
  while(off < st.size){
    int n = read(fd, image + off, st.size - off);
    if(n <= 0){
      fprintf(2, "insmod: short read of %s\n", argv[1]);
      exit(1);
    }
    off += n;
  }
  close(fd);

  if(init_module(image, st.size, "") < 0){
    fprintf(2, "insmod: %s rejected by the kernel\n", argv[1]);
    exit(1);
  }

  printf("insmod: loaded %s\n", argv[1]);
  exit(0);
}
