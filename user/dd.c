// Safely install a downloaded fs.img into the currently selected raw-root
// partition.  Unlike traditional dd, this deliberately supports only the
// guarded /dev/sdroot destination.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define DEFAULT_BS (32 * 1024)
#define LOAD_CHUNK (64 * 1024)

static void
usage(void)
{
  fprintf(2, "usage: dd [if=/boot/fs.img] [of=/dev/sdroot] [bs=BYTES]\n");
  exit(1);
}

static int
prefix(char *s, char *p)
{
  return strncmp(s, p, strlen(p)) == 0;
}

int
main(int argc, char **argv)
{
  char *input = "/boot/fs.img";
  char *output = "/dev/sdroot";
  int bs = DEFAULT_BS;
  uint image_bytes = FSSIZE * BSIZE;

  for(int i = 1; i < argc; i++){
    if(prefix(argv[i], "if="))
      input = argv[i] + 3;
    else if(prefix(argv[i], "of="))
      output = argv[i] + 3;
    else if(prefix(argv[i], "bs="))
      bs = atoi(argv[i] + 3);
    else
      usage();
  }
  if(strcmp(output, "/dev/sdroot") != 0 || bs < 512 ||
     bs > 64 * 1024 || bs % 512)
    usage();

  int in = open(input, O_RDONLY);
  struct stat st;
  if(in < 0 || fstat(in, &st) < 0 || st.type != T_FILE ||
     st.size != image_bytes){
    fprintf(2, "dd: %s must be an exact %d-byte xv6 fs.img\n",
            input, image_bytes);
    if(in >= 0)
      close(in);
    exit(1);
  }

  // Load before opening the updater.  The first device write freezes native
  // root I/O permanently, so no later executable/library/root-file access is
  // permitted.  32 MiB is intentional and bounded by FSSIZE.
  uchar *image = malloc(image_bytes);
  if(image == 0){
    fprintf(2, "dd: cannot allocate %d-byte staging buffer\n", image_bytes);
    close(in);
    exit(1);
  }
  printf("dd: loading %s (%d MiB) before root freeze\n",
         input, image_bytes / (1024 * 1024));
  uint loaded = 0, next = 4 * 1024 * 1024;
  while(loaded < image_bytes){
    int want = image_bytes - loaded;
    if(want > LOAD_CHUNK)
      want = LOAD_CHUNK;
    int n = read(in, image + loaded, want);
    if(n <= 0){
      fprintf(2, "dd: input ended at %d/%d bytes\n", loaded, image_bytes);
      close(in);
      exit(1);
    }
    loaded += n;
    if(loaded >= next){
      printf("dd: loaded %d/%d MiB\n", loaded / (1024 * 1024),
             image_bytes / (1024 * 1024));
      next += 4 * 1024 * 1024;
    }
  }
  close(in);
  struct superblock *sb = (struct superblock *)(image + BSIZE);
  if(sb->magic != FSMAGIC || sb->size != FSSIZE){
    fprintf(2, "dd: invalid xv6 superblock magic=%x size=%d\n",
            sb->magic, sb->size);
    exit(1);
  }

  int out = open(output, O_WRONLY);
  if(out < 0){
    fprintf(2, "dd: cannot open guarded updater %s\n", output);
    exit(1);
  }
  printf("dd: WARNING: replacing mounted root; do not remove power\n");
  uint done = 0;
  while(done < image_bytes){
    int n = image_bytes - done;
    if(n > bs)
      n = bs;
    if(write(out, image + done, n) != n){
      fprintf(2, "dd: write/verify failed at %d bytes\n", done);
      close(out);
      fprintf(2, "dd: root is frozen; power-cycle and recover if needed\n");
      for(;;)
        sleep_ticks(1000);
    }
    done += n;
  }
  close(out);
  printf("dd: %d bytes installed and read-back verified\n", done);
  printf("dd: root filesystem is frozen; power-cycle Raspberry Pi now\n");
  for(;;)
    sleep_ticks(1000);
}
