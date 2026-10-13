#ifndef _UAPI_FCNTL_H
#define _UAPI_FCNTL_H

/*
 * File-control ABI shared by the kernel and user space.
 *
 * Keep this header limited to numeric ABI constants.  Function declarations
 * and libc compatibility aliases belong in the public <fcntl.h> wrapper;
 * kernel-private declarations belong under kernel/.
 */
#define O_RDONLY   0x000
#define O_WRONLY   0x001
#define O_RDWR     0x002
#define O_CREATE   0x200
#define O_TRUNC    0x400
#define O_NONBLOCK 0x800

#define F_GETFL 3
#define F_SETFL 4

#endif
