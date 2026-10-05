/* Services outside serial-shell bringup have no implementations. Native
   paths fall through VFS; other optional backends fail explicitly. */
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
void epollinit(void) {}
void epollclose(struct epoll *p) { (void)p; }
void ptyclose(struct pty *p,int m) { (void)p; (void)m; }
int ptyread(struct pty *p,int m,uint64 a,int n) { (void)p;(void)m;(void)a;(void)n;return -1; }
int ptywrite(struct pty *p,int m,uint64 a,int n) { (void)p;(void)m;(void)a;(void)n;return -1; }
int net_tcp_close(int s) { (void)s;return -1; }
int net_tcp_read(int s,uint64 a,int n,int f) { (void)s;(void)a;(void)n;(void)f;return -1; }
int net_tcp_write(int s,uint64 a,int n,int f) { (void)s;(void)a;(void)n;(void)f;return -1; }
int vfsopen(char *p,int m,struct vnode **v) { (void)p;(void)m;(void)v;return 0; }
void vfsclose(struct vnode *v) { (void)v; }
int vfsstat(struct vnode *v,struct stat *s) { (void)v;(void)s;return -1; }
int vfsread(struct vnode *v,int u,uint64 a,uint o,uint n) { (void)v;(void)u;(void)a;(void)o;(void)n;return -1; }
int vfswrite(struct vnode *v,int u,uint64 a,uint o,uint n) { (void)v;(void)u;(void)a;(void)o;(void)n;return -1; }
