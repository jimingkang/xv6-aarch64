#ifndef XV6_USER_CAMPROC_H
#define XV6_USER_CAMPROC_H

// Shared RAW10 -> JPEG conversion used by camshot and camserver.
//
// camproc_jpeg() takes one captured frame as returned by read(/dev/video0):
// a struct cam_frame_hdr followed by packed CSI-2 RAW10.  It unpacks the
// mosaic, demosaics, applies gray-world white balance + auto level, and emits
// a baseline JPEG into the caller's buffer.  The mosaic scratch buffer is
// reused between calls, so the caller may call it repeatedly per frame.
//
// Returns 0 on success and stores the JPEG byte count in *written (may be 0).
int camproc_jpeg(uchar *dst, int cap, const uchar *frame, int n,
                 int half, int wb, int quality, uint *written);

// Zero-copy variant for /dev/video0 mmap users.  hdr is returned by the
// header-only dequeue read and raw points at its completed DMA slot.
int camproc_jpeg_raw(uchar *dst, int cap, const struct cam_frame_hdr *hdr,
                     const uchar *raw, int half, int wb, int quality,
                     uint *written);

#endif
