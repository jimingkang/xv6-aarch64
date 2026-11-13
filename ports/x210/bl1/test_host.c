/* Host test of the BL1 logic that does not touch hardware:
   CRC-32, uImage header check against the real xv6-x210.uImage, and the
   ATAG list BL1 builds, read back with the kernel's own parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/types.h"
#include "bl1lib.h"
#include "../atag.h"

#define CHECK(c) do { if(!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while(0)

int main(int argc, char **argv) {
  CHECK(crc32(0, (const uchar*)"123456789", 9) == 0xCBF43926U);
  if(argc != 2) { fprintf(stderr, "usage: %s xv6-x210.uImage\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb"); CHECK(f);
  static uchar img[32 << 20];
  size_t n = fread(img, 1, sizeof(img), f); fclose(f);
  struct uimage_info ui;
  const char *bad = uimage_check(img, &ui);
  if(bad) { fprintf(stderr, "FAIL uimage_check: %s\n", bad); return 1; }
  CHECK(ui.load == 0x30008000U && ui.entry == 0x30008000U);
  CHECK(UIMAGE_HDR + ui.size == n);
  CHECK(crc32(0, img + UIMAGE_HDR, ui.size) == ui.dcrc);
  img[UIMAGE_HDR + 100] ^= 1;
  CHECK(crc32(0, img + UIMAGE_HDR, ui.size) != ui.dcrc);
  img[2] ^= 1;
  CHECK(uimage_check(img, &ui) != 0);

  static uint32 tags[1024];
  uint32 len = atag_build(tags, 0x30000000U, 256U << 20, "console=ttySAC2,115200 xv6 bl1");
  CHECK(len > 0 && len < 256);
  struct bootparams bp;
  CHECK(atag_parse((uintptr)tags, &bp) == 0);
  CHECK(bp.kind == 1 && bp.nmem == 1);
  CHECK(bp.mem_base[0] == 0x30000000U && bp.mem_size[0] == 256U << 20);
  CHECK(strcmp(bp.cmdline, "console=ttySAC2,115200 xv6 bl1") == 0);
  printf("BL1 host test: CRC-32, uImage check of %s (%u bytes @ %#x), ATAG build/parse OK\n",
         argv[1], ui.size, ui.load);
  return 0;
}
