#ifndef XV6_USER_JPEG_H
#define XV6_USER_JPEG_H

// Baseline JPEG encoder.  Pixels are requested top-down as 8-bit RGB.  The
// encoder uses YCbCr 4:2:0, integer DCT and bounded streaming output, so it is
// also suitable for writing individual frames into a future MJPEG stream.
typedef void (*jpeg_pixel_fn)(void *arg, int x, int y,
                              uchar *r, uchar *g, uchar *b);

int jpeg_encode(int fd, int width, int height, int quality,
                jpeg_pixel_fn pixel, void *arg, uint *written);

// Encode into a caller-provided memory buffer of cap bytes.  Returns 0 on
// success and stores the total JPEG size in *written; returns -1 if the image
// does not fit in cap bytes or any parameter is invalid.
int jpeg_encode_mem(uchar *dst, int cap, int width, int height, int quality,
                    jpeg_pixel_fn pixel, void *arg, uint *written);

#endif
