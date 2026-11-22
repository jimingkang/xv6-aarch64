#ifndef _UAPI_VIDEODEV2_H
#define _UAPI_VIDEODEV2_H

// A deliberately small V4L2 capture ABI.  The structure and flag names match
// Linux so camera applications have the same programming model, but xv6 uses
// compact fixed ioctl numbers rather than Linux's architecture-dependent
// _IOC encoding.  Existing cam_frame_hdr read() users remain supported.

#include "types.h"

#define v4l2_fourcc(a, b, c, d) \
  ((uint32)(a) | ((uint32)(b) << 8) | ((uint32)(c) << 16) | \
   ((uint32)(d) << 24))

#define V4L2_PIX_FMT_SBGGR10P v4l2_fourcc('p', 'B', 'A', 'A')
#define V4L2_PIX_FMT_SGBRG10P v4l2_fourcc('p', 'G', 'A', 'A')
#define V4L2_PIX_FMT_SGRBG10P v4l2_fourcc('p', 'g', 'A', 'A')
#define V4L2_PIX_FMT_SRGGB10P v4l2_fourcc('p', 'R', 'A', 'A')

#define V4L2_BUF_TYPE_VIDEO_CAPTURE  1
#define V4L2_MEMORY_MMAP             1
#define V4L2_FIELD_NONE              1

#define V4L2_CAP_VIDEO_CAPTURE  0x00000001U
#define V4L2_CAP_READWRITE      0x01000000U
#define V4L2_CAP_STREAMING      0x04000000U
#define V4L2_CAP_DEVICE_CAPS    0x80000000U

#define V4L2_BUF_FLAG_MAPPED    0x00000001U
#define V4L2_BUF_FLAG_QUEUED    0x00000002U
#define V4L2_BUF_FLAG_DONE      0x00000004U
#define V4L2_BUF_CAP_SUPPORTS_MMAP 0x00000001U

#define V4L2_CID_TEST_PATTERN   0x009f0903U

struct v4l2_capability {
  char driver[16];
  char card[32];
  char bus_info[32];
  uint32 version;
  uint32 capabilities;
  uint32 device_caps;
  uint32 reserved[3];
};

struct v4l2_fmtdesc {
  uint32 index;
  uint32 type;
  uint32 flags;
  char description[32];
  uint32 pixelformat;
  uint32 reserved[4];
};

struct v4l2_pix_format {
  uint32 width;
  uint32 height;
  uint32 pixelformat;
  uint32 field;
  uint32 bytesperline;
  uint32 sizeimage;
  uint32 colorspace;
  uint32 priv;
};

struct v4l2_format {
  uint32 type;
  struct v4l2_pix_format pix;
};

struct v4l2_requestbuffers {
  uint32 count;
  uint32 type;
  uint32 memory;
  uint32 capabilities;
  uint32 reserved[4];
};

struct v4l2_buffer {
  uint32 index;
  uint32 type;
  uint32 bytesused;
  uint32 flags;
  uint32 field;
  uint32 sequence;
  uint64 timestamp_us;
  uint32 memory;
  uint32 offset;
  uint32 length;
  uint32 reserved[2];
};

struct v4l2_control {
  uint32 id;
  int value;
};

// xv6-private command encoding; semantics follow the corresponding VIDIOC.
#define VIDIOC_QUERYCAP   0x5600U
#define VIDIOC_ENUM_FMT   0x5601U
#define VIDIOC_G_FMT      0x5602U
#define VIDIOC_S_FMT      0x5603U
#define VIDIOC_REQBUFS    0x5604U
#define VIDIOC_QUERYBUF   0x5605U
#define VIDIOC_QBUF       0x5606U
#define VIDIOC_DQBUF      0x5607U
#define VIDIOC_STREAMON   0x5608U
#define VIDIOC_STREAMOFF  0x5609U
#define VIDIOC_G_CTRL     0x560aU
#define VIDIOC_S_CTRL     0x560bU

#endif
