#ifndef XV6_FIRMWARE_H
#define XV6_FIRMWARE_H
// request_firmware() (Linux <linux/firmware.h> subset). There is no storage
// driver on the ARM32 boards yet, so firmware blobs are linked into the
// kernel (Makefile: FIRMWARE) and looked up by file name.
struct firmware {
  const char *name;
  const uchar *data;
  uint size;
};
int request_firmware(const struct firmware **fw, const char *name);
#endif
