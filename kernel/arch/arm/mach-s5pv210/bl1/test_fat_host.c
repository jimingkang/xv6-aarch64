/* Host test of BL1's FAT32 reader: test_fat_host CARD.img XV6.IMG-original
   [FS.IMG-original]. CARD.img is a raw card/partition image (MBR + FAT32,
   e.g. made with hdiutil -layout MBRSPUD -format UDTO on macOS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/types.h"
#include "bl1fat.h"

static unsigned char *card;
static size_t card_len;
static int rd(uint32 lba, void *dst, uint32 n)
{
  if((size_t)(lba + n) * 512 > card_len) return -1;
  memcpy(dst, card + (size_t)lba * 512, (size_t)n * 512);
  return 0;
}
static unsigned char *slurp(const char *p, size_t *len)
{
  FILE *f = fopen(p, "rb"); if(!f){ perror(p); exit(2); }
  fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(*len + 1); fread(b, 1, *len, f); fclose(f); return b;
}
static int check(const char *name11, const char *orig)
{
  size_t olen; unsigned char *o = slurp(orig, &olen);
  uint32 first, size;
  if(fat_find(name11, &first, &size) < 0){ printf("FAIL: %.11s not found\n", name11); return 1; }
  if(size != olen){ printf("FAIL: %.11s size %u != %zu\n", name11, size, olen); return 1; }
  unsigned char *buf = malloc(size + (1 << 20));
  if(fat_read(first, size, buf) < 0 || memcmp(buf, o, size)){ printf("FAIL: %.11s content\n", name11); return 1; }
  printf("ok: %.11s %u bytes, first cluster %u\n", name11, size, first);
  return 0;
}
int main(int argc, char **argv)
{
  if(argc < 3){ fprintf(stderr, "usage: %s card.img XV6.IMG [FS.IMG]\n", argv[0]); return 2; }
  card = slurp(argv[1], &card_len);
  uint32 part, spc;
  if(fat_mount(rd, &part, &spc) < 0){ printf("FAIL: no FAT32\n"); return 1; }
  printf("FAT32 at sector %u, %u sectors/cluster\n", part, spc);
  int bad = check("XV6     IMG", argv[2]);
  if(argc > 3) bad |= check("FS      IMG", argv[3]);
  uint32 a, b;
  if(fat_find("NOSUCH  BIN", &a, &b) == 0){ printf("FAIL: phantom file\n"); bad = 1; }
  printf(bad ? "BL1 FAT32 host test FAILED\n" : "BL1 FAT32 host test: OK\n");
  return bad;
}
