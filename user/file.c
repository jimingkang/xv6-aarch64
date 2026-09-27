#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

static char *
typename(short type)
{
  if(type == T_DIR) return "directory";
  if(type == T_FILE) return "regular file";
  if(type == T_DEVICE) return "device";
  return "unknown";
}

int
main(int argc, char **argv)
{
  if(argc < 2){
    fprintf(2, "usage: file path...\n");
    exit(1);
  }
  for(int i = 1; i < argc; i++){
    struct stat st;
    if(stat(argv[i], &st) < 0){
      fprintf(2, "file: cannot stat %s\n", argv[i]);
      continue;
    }
    printf("%s: %s, inode %d, %d bytes\n",
           argv[i], typename(st.type), st.ino, st.size);
  }
  exit(0);
}
