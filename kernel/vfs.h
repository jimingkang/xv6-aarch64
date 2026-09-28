#ifndef XV6_VFS_H
#define XV6_VFS_H

struct vnode;

// Return 1 when path belongs to a VFS mount and was opened, 0 when the path
// belongs to the native xv6 filesystem, and -1 for an error below a mount.
int  vfsopen(char *path, int omode, struct vnode **out);
void vfsclose(struct vnode *vn);
int  vfsread(struct vnode *vn, int user_dst, uint64 dst, uint off, uint n);
int  vfsstat(struct vnode *vn, struct stat *st);

#endif
