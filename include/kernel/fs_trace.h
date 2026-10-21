#ifndef XV6_FS_TRACE_H
#define XV6_FS_TRACE_H

// Build with `make FS_TRACE=1` to enable the intentionally verbose VFS trace.
// Keep it compiled out by default: pathname walking and directory reads are
// hot paths and an always-on serial trace changes timing substantially.
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

#endif
