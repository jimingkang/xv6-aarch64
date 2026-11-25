#ifndef XV6_BCM2708_FB_H
#define XV6_BCM2708_FB_H

// Shared firmware scanout allocation used by fbdev compatibility and the
// first-stage DRM/KMS driver.
struct bcm2708_fb_info {
  uint32 width;
  uint32 height;
  uint32 virtual_height;
  uint32 depth;
  uint32 pitch;
  uint64 base;
  uint32 size;
  uint32 frame_bytes;
  uint32 buffer_count;
  uint32 front_buffer;
  uint64 sequence;
};

int bcm2708_fb_get_info(struct bcm2708_fb_info *info);
int bcm2708_fb_page_flip(uint32 index, uint64 *sequence);
int bcm2708_fb_wait_vsync(uint64 *sequence);

#endif
