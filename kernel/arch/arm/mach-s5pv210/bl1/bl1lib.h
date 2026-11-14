/* Hardware-independent parts of the X210 BL1 (also built on the host
   by `mkx210 hosttest`): CRC-32, uImage header check, ATAG list. */
#define UIMAGE_MAGIC 0x27051956U
#define UIMAGE_HDR 64
struct uimage_info { uint32 size, load, entry, dcrc; char name[33]; };
uint32 crc32(uint32 crc, const uchar *p, uint32 n);
/* 0 if hdr is a valid legacy ARM kernel uImage; else a short reason. */
const char *uimage_check(const uchar *hdr, struct uimage_info *ui);
/* Build CORE, MEM, CMDLINE, NONE at dst; returns bytes written. */
uint32 atag_build(uint32 *dst, uint32 mem_base, uint32 mem_size, const char *cmdline);
