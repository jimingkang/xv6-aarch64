// fbtest: /dev/fb0 through ioctl() and mmap(), as a Linux fbdev program
// would use it. Draws colour bars and a gradient, checks the pixels read
// back, that a forked child shares the mapping, and that munmap() works.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fb.h"
#include "kernel/mman.h"
#include "user/user.h"

static void
fail(char *why)
{
  printf("fbtest: FAIL %s\n", why);
  exit(1);
}

int
main(int argc, char *argv[])
{
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  int fd = open("/dev/fb0", O_RDWR);
  if(fd < 0)
    fail("open /dev/fb0");
  if(ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0)
    fail("ioctl");
  if(ioctl(fd, 0x1234, 0) != -1)
    fail("unknown ioctl accepted");
  printf("fbtest: %s %dx%d %d bpp, line %d, %d bytes, rgb offsets %d/%d/%d\n",
         fix.id, var.xres, var.yres, var.bits_per_pixel, fix.line_length,
         fix.smem_len, var.red.offset, var.green.offset, var.blue.offset);
  if(var.bits_per_pixel != 32 || fix.line_length < var.xres*4)
    fail("geometry");

  uint *fb = mmap(0, fix.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if(fb == MAP_FAILED)
    fail("mmap");
  uint stride = fix.line_length/4;
  static const uint bar[8] = {0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
  for(uint y = 0; y < var.yres; y++)
    for(uint x = 0; x < var.xres; x++)
      fb[y*stride + x] = y < var.yres/2 ? bar[x*8/var.xres]
                                         : (x*255/var.xres) << 8 | (y*255/var.yres);
  for(uint y = 0; y < var.yres; y += 37)
    for(uint x = 0; x < var.xres; x += 29){
      uint want = y < var.yres/2 ? bar[x*8/var.xres] : (x*255/var.xres) << 8 | (y*255/var.yres);
      if(fb[y*stride + x] != want)
        fail("pixel readback");
    }

  int pid = fork();
  if(pid < 0)
    fail("fork");
  if(pid == 0){
    fb[0] = 0x00abcdef;   // the child writes through its inherited mapping
    exit(0);
  }
  wait(0);
  if(fb[0] != 0x00abcdef)
    fail("mapping not shared with the child");

  if(mmap(0, fix.smem_len, PROT_READ, MAP_SHARED, 0, 0) != MAP_FAILED)
    fail("mmap of a non-fb file accepted");
  if(munmap(fb, fix.smem_len) < 0)
    fail("munmap");
  pid = fork();             // touching the unmapped buffer must fault
  if(pid == 0){
    volatile uint *gone = fb;
    printf("fbtest: child reads unmapped %x (expect a fault)\n", gone[0]);
    exit(0);
  }
  int status = 0;
  wait(&status);
  if(status == 0)
    fail("buffer still mapped after munmap");
  if(argc > 1)
    sleep_ticks(atoi(argv[1]) * 10);   // keep the picture: fbtest <seconds> (10 ticks/s)
  close(fd);
  printf("fbtest: OK (mapped at %x, %d bytes)\n", (uint)fb, fix.smem_len);
  exit(0);
}
