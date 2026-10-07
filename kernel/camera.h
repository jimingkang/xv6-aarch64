#ifndef XV6_CAMERA_H
#define XV6_CAMERA_H

// /dev/video0: shared by the kernel driver (unicam.c) and user programs.
//
// read(fd, buf, n) captures one frame and returns a struct cam_frame_hdr
// followed by the image data.  n must be at least the header plus one
// frame of the current mode; CAM_READ_MAX is always enough.
//
// The first read powers the sensor and starts it streaming (one warm-up);
// subsequent reads grab a fresh frame on demand without another power cycle.
// The stream is stopped when the last reference to the fd is closed.
//
// write(fd, "pattern=1", 9) selects a sensor test pattern for later
// captures (0 off; on the OV5647 1 colour bars, 2 colour squares, 3 random);
// useful to check the CSI-2 path independently of focus and lighting.

#define CAM_MAGIC         0x314d4143U     // "CAM1"

// MIPI CSI-2 RAW10, packed (4 pixels in 5 bytes), by Bayer order: the
// colours of the top-left 2x2 block, row by row (V4L2 SBGGR10P etc.).
#define CAM_FMT_SBGGR10P  1
#define CAM_FMT_SGBRG10P  2
#define CAM_FMT_SGRBG10P  3
#define CAM_FMT_SRGGB10P  4

// Largest frame the driver will capture (its DMA area holds this much).
#define CAM_MAX_FRAME_BYTES  (512 * 1024)
#define CAM_BUFFER_COUNT     2
#define CAM_SLOT_BYTES       CAM_MAX_FRAME_BYTES
#define CAM_MMAP_BYTES       (CAM_BUFFER_COUNT * CAM_SLOT_BYTES)
// Camera mappings live outside the sbrk() heap.  They are read-only,
// non-cacheable aliases of the two Unicam DMA slots.
#define CAM_MMAP_BASE        0x0000003ff0000000ULL

struct cam_frame_hdr {
  uint32 magic;
  uint32 width;
  uint32 height;
  uint32 bytesperline;
  uint32 format;              // CAM_FMT_*
  uint32 data_bytes;          // bytes that follow this header
  uint32 sequence;            // frames received since the stream started
  uint32 reserved;              // mmap mode: completed DMA slot (0 or 1)
  uint64 timestamp_us;        // clock_us() at frame end
};

#define CAM_READ_MAX  (sizeof(struct cam_frame_hdr) + CAM_MAX_FRAME_BYTES)

#endif
