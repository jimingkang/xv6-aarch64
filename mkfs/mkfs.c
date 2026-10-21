#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include <kernel/types.h>
#include <kernel/fs.h>
#include <kernel/stat.h>
#include <kernel/param.h>

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);

// mkfs/lsdir.c: list the regular files in a directory (host headers isolated in
// that file so they don't clash with xv6's struct dirent / redefined stat).
int mkfs_list_dir(char *dir, char (*names)[64], int max, int outsz);

#define MAXMODULES 32

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, binino, libino, modino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // A `-M <dir>` option names the installed-modules staging tree (what
  // `make modules_install` populates).  Every regular file in it lands in the
  // image's /lib/modules verbatim -- mkfs no longer guesses a module by its
  // ".ko" suffix, just as a Linux rootfs simply has files under /lib/modules.
  // The option and its argument are blanked so the /bin loop skips them.
  char *moddir = 0;
  char *libfile = 0;
  char *interpfile = 0;
  for(i = 2; i < argc; i++){
    if(strcmp(argv[i], "-M") == 0 && i + 1 < argc){
      moddir = argv[i + 1];
      argv[i] = argv[i + 1] = 0;
      i++;
    } else if(strcmp(argv[i], "-L") == 0 && i + 1 < argc){
      libfile = argv[i + 1];
      argv[i] = argv[i + 1] = 0;
      i++;
    } else if(strcmp(argv[i], "-I") == 0 && i + 1 < argc){
      interpfile = argv[i + 1];
      argv[i] = argv[i + 1] = 0;
      i++;
    }
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  // Build /bin into the image itself.  init must not have to manufacture the
  // executable namespace on every boot.
  binino = ialloc(T_DIR);
  bzero(&de, sizeof(de));
  de.inum = xshort(binino);
  strcpy(de.name, "bin");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(binino);
  strcpy(de.name, ".");
  iappend(binino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(binino, &de, sizeof(de));

  // Build /lib/modules too, so a loadable .ko lives where insmod expects it --
  // the same directory Linux keeps /lib/modules/$(uname -r) in.
  libino = ialloc(T_DIR);
  modino = ialloc(T_DIR);
  bzero(&de, sizeof(de));
  de.inum = xshort(libino);
  strcpy(de.name, "lib");
  iappend(rootino, &de, sizeof(de));
  bzero(&de, sizeof(de));
  de.inum = xshort(modino);
  strcpy(de.name, "modules");
  iappend(libino, &de, sizeof(de));
  bzero(&de, sizeof(de));
  de.inum = xshort(libino);
  strcpy(de.name, ".");
  iappend(libino, &de, sizeof(de));
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(libino, &de, sizeof(de));
  bzero(&de, sizeof(de));
  de.inum = xshort(modino);
  strcpy(de.name, ".");
  iappend(modino, &de, sizeof(de));
  bzero(&de, sizeof(de));
  de.inum = xshort(libino);
  strcpy(de.name, "..");
  iappend(modino, &de, sizeof(de));

  // Ingest the installed modules tree into /lib/modules.  Each file is copied
  // under its real name, exactly as a modules_install'd .ko sits in
  // /lib/modules/$(uname -r)/ on Linux.
  if(moddir){
    char mnames[MAXMODULES][64];
    char mpath[1024];
    int mn = mkfs_list_dir(moddir, mnames, MAXMODULES, 64);
    if(mn < 0){
      fprintf(stderr, "mkfs: cannot open modules directory %s\n", moddir);
      exit(1);
    }
    for(int m = 0; m < mn; m++){
      snprintf(mpath, sizeof(mpath), "%s/%s", moddir, mnames[m]);
      if((fd = open(mpath, 0)) < 0)
        die(mpath);
      inum = ialloc(T_FILE);
      bzero(&de, sizeof(de));
      de.inum = xshort(inum);
      strncpy(de.name, mnames[m], DIRSIZ);
      iappend(modino, &de, sizeof(de));
      while((cc = read(fd, buf, sizeof(buf))) > 0)
        iappend(inum, buf, cc);
      rinode(inum, &din);
      din.nlink = xshort(1);
      winode(inum, &din);
      close(fd);
    }
  }

  // Install the runtime shared C library at /lib/libc.so.  Keeping this as an
  // explicit option prevents the ordinary remaining-arguments loop from
  // accidentally treating a shared library as a /bin program.
  if(libfile){
    char *libname = strrchr(libfile, '/');
    libname = libname ? libname + 1 : libfile;
    if((fd = open(libfile, 0)) < 0)
      die(libfile);
    inum = ialloc(T_FILE);
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, libname, DIRSIZ);
    iappend(libino, &de, sizeof(de));
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);
    rinode(inum, &din);
    din.nlink = xshort(1);
    winode(inum, &din);
    close(fd);
  }

  // Install the user-space ELF interpreter named by PT_INTERP.  It lives in
  // /lib beside libc.so but is a standalone static executable: the loader
  // cannot itself depend on the library it is responsible for relocating.
  if(interpfile){
    char *name = strrchr(interpfile, '/');
    name = name ? name + 1 : interpfile;
    if((fd = open(interpfile, 0)) < 0)
      die(interpfile);
    inum = ialloc(T_FILE);
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, name, DIRSIZ);
    iappend(libino, &de, sizeof(de));
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);
    rinode(inum, &din);
    din.nlink = xshort(1);
    winode(inum, &din);
    close(fd);
  }

  // Everything left on the command line is a user program for /bin.
  for(i = 2; i < argc; i++){
    if(argv[i] == 0)                  // consumed by the -M option
      continue;
    // Store by basename only; the caller passes user/_foo build names.
    char *shortname = strrchr(argv[i], '/');
    shortname = shortname ? shortname + 1 : argv[i];

    // "_foo" build names lose the leading underscore so the host never runs
    // them by accident.
    uint dir = binino;
    if(shortname[0] == '_')
      shortname += 1;

    if((fd = open(argv[i], 0)) < 0)
      die(argv[i]);

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(dir, &de, sizeof(de));

    // The bootstrap code enters user space with exec("init"), so /init is
    // the sole executable retained at the root.  All commands live in /bin.
    int rootlink = dir == binino && strcmp(shortname, "init") == 0;
    if(rootlink){
      bzero(&de, sizeof(de));
      de.inum = xshort(inum);
      strncpy(de.name, shortname, DIRSIZ);
      iappend(rootino, &de, sizeof(de));
    }

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    // /bin/init and /init refer to the same inode.
    rinode(inum, &din);
    din.nlink = xshort(rootlink ? 2 : 1);
    winode(inum, &din);

    close(fd);
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  // Directory sizes are rounded to a complete filesystem block in xv6.
  rinode(binino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(binino, &din);

  rinode(libino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(libino, &din);

  rinode(modino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(modino, &din);

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
