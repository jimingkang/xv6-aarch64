#ifndef XV6_S3C_FB_H
#define XV6_S3C_FB_H
// Platform data for the S5PV210 FIMD driver (Linux s3c_fb_platdata):
// the board file supplies the panel timings and the pin setup.
struct s3c_fb_panel {
  const char *name;
  uint32 xres, yres, hz;
  uint32 hfp, hbp, hsw, vfp, vbp, vsw;
  int ivclk, ihsync, ivsync, ivden;  // VIDCON1 polarity bits
};
struct s3c_fb_platdata {
  const struct s3c_fb_panel *panel;
  void (*setup_gpio)(void);          // LCD pins and DISPLAY_CONTROL
};
#endif
