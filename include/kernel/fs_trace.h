#ifndef XV6_FS_TRACE_H
#define XV6_FS_TRACE_H

// FS_TRACE is the lower-level xv6fs teaching trace (superblock, inode cache,
// block mapping).  VFS_TRACE is the filesystem-independent dispatch trace
// (mounts, pathname walking, struct file and inode/file operation calls).
// Leave both disabled for normal boot: these are hot paths and serial output
// changes timing substantially.
#ifndef FS_TRACE
#define FS_TRACE 0
#endif

#if FS_TRACE
#define FSTRACE(...) printf("fs-trace: " __VA_ARGS__)
#else
// Keep arguments type-checked and considered "used" by -Wall while allowing
// the optimizer to remove the whole branch and all format strings.
#define FSTRACE(...) do { if (0) printf("fs-trace: " __VA_ARGS__); } while (0)
#endif

#ifndef VFS_TRACE
#define VFS_TRACE 0
#endif

#if VFS_TRACE
#define VFSTRACE(...) printf("vfs-trace: " __VA_ARGS__)
#else
#define VFSTRACE(...) do { if (0) printf("vfs-trace: " __VA_ARGS__); } while (0)
#endif

#endif
