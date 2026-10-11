// Host-side directory scan for mkfs.  Kept in its own file because mkfs.c
// redefines the symbol `stat` to xv6_stat and pulls in xv6's fs.h, whose
// `struct dirent` collides with the host <dirent.h> one.  This file uses only
// host headers, so the two dirent definitions never share a translation unit.

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

// Copy the names of the regular files directly inside dir into names[] (each a
// NUL-terminated string of at most outsz-1 bytes), returning the count, or -1
// if dir cannot be opened.  Used to ingest a built modules tree into the
// filesystem image's /lib/modules, the way `make modules_install` + depmod do
// on a real Linux rootfs.
int
mkfs_list_dir(char *dir, char (*names)[64], int max, int outsz)
{
  DIR *d = opendir(dir);
  struct dirent *e;
  struct stat st;
  char path[1024];
  int n = 0;

  if(d == 0)
    return -1;
  while((e = readdir(d)) != 0){
    if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if(stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    if(n >= max)
      continue;
    strncpy(names[n], e->d_name, outsz - 1);
    names[n][outsz - 1] = 0;
    n++;
  }
  closedir(d);
  return n;
}
