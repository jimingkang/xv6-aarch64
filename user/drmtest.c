#include <uapi/types.h>
#include "user/user.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <uapi/drm.h>

// Smoke test for the VC4 DRM/KMS driver.  It creates
// the two scanout GEM objects, draws complete frames off-screen, and flips
// only at vertical blank.  No Linux libdrm is required by this xv6 ABI.

struct test_buffer {
  struct drm_mode_create_dumb dumb;
  struct drm_mode_fb_cmd fb;
  uint32 *pixels;
};

static void
paint(struct test_buffer *b, int which)
{
  uint32 stride = b->dumb.pitch / sizeof(uint32);

  for(uint32 y = 0; y < b->dumb.height; y++){
    for(uint32 x = 0; x < b->dumb.width; x++){
      uint32 r = which ? (x * 255 / b->dumb.width) : 24;
      uint32 g = which ? 32 : (y * 255 / b->dumb.height);
      uint32 blue = which ? (y * 255 / b->dumb.height) :
                            (x * 255 / b->dumb.width);
      // Both the native HVS plane and firmware fallback use XRGB8888.
      b->pixels[y * stride + x] = (r << 16) | (g << 8) | blue;
    }
  }
}

static int
make_buffer(int fd, struct drm_mode_modeinfo *mode, struct test_buffer *b)
{
  struct drm_mode_map_dumb map;

  memset(b, 0, sizeof(*b));
  b->dumb.width = mode->hdisplay;
  b->dumb.height = mode->vdisplay;
  b->dumb.bpp = 32;
  if(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &b->dumb) < 0)
    return -1;
  memset(&map, 0, sizeof(map));
  map.handle = b->dumb.handle;
  if(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
    return -1;
  b->pixels = mmap(0, b->dumb.size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, map.offset);
  if(b->pixels == MAP_FAILED)
    return -1;
  memset(&b->fb, 0, sizeof(b->fb));
  b->fb.width = b->dumb.width;
  b->fb.height = b->dumb.height;
  b->fb.pitch = b->dumb.pitch;
  b->fb.bpp = 32;
  b->fb.depth = 24;
  b->fb.handle = b->dumb.handle;
  return ioctl(fd, DRM_IOCTL_MODE_ADDFB, &b->fb);
}

int
main(int argc, char **argv)
{
  struct drm_version version;
  struct drm_mode_card_res res;
  struct drm_mode_get_connector connector;
  struct test_buffer buffer[2];
  int fd;

  (void)argc;
  (void)argv;
  if((fd = open("/dev/dri/card0", O_RDWR)) < 0){
    printf("drmtest: cannot open /dev/dri/card0\n");
    exit(1);
  }
  memset(&version, 0, sizeof(version));
  memset(&res, 0, sizeof(res));
  if(ioctl(fd, DRM_IOCTL_VERSION, &version) < 0 ||
     ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0){
    printf("drmtest: cannot query DRM resources\n");
    exit(1);
  }
  memset(&connector, 0, sizeof(connector));
  connector.connector_id = res.connector_id;
  if(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
     connector.connection != DRM_MODE_CONNECTED){
    printf("drmtest: connector is not available\n");
    exit(1);
  }
  printf("drmtest: %s %s, connector %d, mode %s\n", version.name,
         version.desc, connector.connector_id, connector.mode.name);

  for(int i = 0; i < 2; i++){
    if(make_buffer(fd, &connector.mode, &buffer[i]) < 0){
      printf("drmtest: cannot create buffer %d\n", i);
      exit(1);
    }
    paint(&buffer[i], i);
  }

  for(int frame = 0; frame < 8; frame++){
    struct drm_mode_crtc_page_flip flip;
    int i = frame & 1;

    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = res.crtc_id;
    flip.fb_id = buffer[i].fb.fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    if(ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0){
      printf("drmtest: page flip %d failed\n", frame);
      exit(1);
    }
    printf("drmtest: displayed fb=%d sequence=%d\n",
           flip.fb_id, flip.sequence);
    sleep(1);
  }

  // close() restores scanout slot zero before releasing the DRM master.
  for(int i = 0; i < 2; i++)
    munmap(buffer[i].pixels, buffer[i].dumb.size);
  close(fd);
  printf("drmtest: PASS\n");
  exit(0);
}
