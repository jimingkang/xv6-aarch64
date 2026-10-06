#ifndef XV6_USER_JPEG_H
#define XV6_USER_JPEG_H

// Baseline JPEG encoder.  Pixels are requested top-down as 8-bit RGB.  The
// encoder uses YCbCr 4:2:0, integer DCT and bounded streaming output, so it is
// also suitable for writing individual frames into a future MJPEG stream.
typedef void (*jpeg_pixel_fn)(void *arg, int x, int y,
                              uchar *r, uchar *g, uchar *b);

int jpeg_encode(int fd, int width, int height, int quality,
                jpeg_pixel_fn pixel, void *arg, uint *written);

#endif
