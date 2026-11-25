#ifndef _UAPI_DRM_H
#define _UAPI_DRM_H

#include "types.h"

// Compact DRM/KMS ABI for xv6.  Object names and operation semantics follow
// Linux DRM, while command numbers and fixed-size result structures are xv6
// specific and are not Linux binary-ABI compatible.
#define DRM_IOCTL_VERSION             0x6400U
#define DRM_IOCTL_MODE_GETRESOURCES   0x6401U
#define DRM_IOCTL_MODE_GETCONNECTOR   0x6402U
#define DRM_IOCTL_MODE_GETCRTC        0x6403U
#define DRM_IOCTL_MODE_CREATE_DUMB    0x6404U
#define DRM_IOCTL_MODE_MAP_DUMB       0x6405U
#define DRM_IOCTL_MODE_ADDFB          0x6406U
#define DRM_IOCTL_MODE_RMFB           0x6407U
#define DRM_IOCTL_MODE_PAGE_FLIP      0x6408U
#define DRM_IOCTL_WAIT_VBLANK         0x6409U

#define DRM_MODE_CONNECTED       1U
#define DRM_MODE_DISCONNECTED    2U
#define DRM_MODE_CONNECTOR_HDMIA 11U
#define DRM_MODE_PAGE_FLIP_EVENT 1U
#define DRM_MODE_FB_DIRTY_ANNOTATE_COPY 1U

struct drm_version {
  uint32 version_major;
  uint32 version_minor;
  uint32 version_patchlevel;
  char name[32];
  char desc[64];
};

struct drm_mode_modeinfo {
  uint32 clock;
  uint16 hdisplay;
  uint16 hsync_start;
  uint16 hsync_end;
  uint16 htotal;
  uint16 vdisplay;
  uint16 vsync_start;
  uint16 vsync_end;
  uint16 vtotal;
  uint32 vrefresh;
  uint32 flags;
  char name[32];
};

struct drm_mode_card_res {
  uint32 count_fbs;
  uint32 count_crtcs;
  uint32 count_connectors;
  uint32 count_encoders;
  uint32 fb_id;
  uint32 crtc_id;
  uint32 connector_id;
  uint32 encoder_id;
  uint32 min_width;
  uint32 max_width;
  uint32 min_height;
  uint32 max_height;
};

struct drm_mode_get_connector {
  uint32 connector_id;
  uint32 encoder_id;
  uint32 connector_type;
  uint32 connection;
  uint32 count_modes;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_crtc {
  uint32 crtc_id;
  uint32 fb_id;
  uint32 x;
  uint32 y;
  uint32 mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_create_dumb {
  uint32 height;
  uint32 width;
  uint32 bpp;
  uint32 flags;
  uint32 handle;
  uint32 pitch;
  uint64 size;
};

struct drm_mode_map_dumb {
  uint32 handle;
  uint32 pad;
  uint64 offset;
};

struct drm_mode_fb_cmd {
  uint32 fb_id;
  uint32 width;
  uint32 height;
  uint32 pitch;
  uint32 bpp;
  uint32 depth;
  uint32 handle;
};

struct drm_mode_crtc_page_flip {
  uint32 crtc_id;
  uint32 fb_id;
  uint32 flags;
  uint32 sequence;
  uint64 user_data;
};

struct drm_wait_vblank {
  uint64 sequence;
  uint64 timestamp_us;
};

#endif
