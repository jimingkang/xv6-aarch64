#ifndef _UAPI_CAMERA_H
#define _UAPI_CAMERA_H

#include "types.h"

#define CAM_MAGIC         0x314d4143U

#define CAM_FMT_SBGGR10P  1
#define CAM_FMT_SGBRG10P  2
#define CAM_FMT_SGRBG10P  3
#define CAM_FMT_SRGGB10P  4

#define CAM_MAX_FRAME_BYTES  (512 * 1024)
#define CAM_BUFFER_COUNT     2
#define CAM_SLOT_BYTES       CAM_MAX_FRAME_BYTES
#define CAM_MMAP_BYTES       (CAM_BUFFER_COUNT * CAM_SLOT_BYTES)
#define CAM_MMAP_BASE        0x0000003ff0000000ULL

struct cam_frame_hdr {
  uint32 magic;
  uint32 width;
  uint32 height;
  uint32 bytesperline;
  uint32 format;
  uint32 data_bytes;
  uint32 sequence;
  uint32 reserved;
  uint64 timestamp_us;
};

#define CAM_READ_MAX  (sizeof(struct cam_frame_hdr) + CAM_MAX_FRAME_BYTES)

#endif
