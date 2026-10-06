#ifndef XV6_CAMSENSOR_H
#define XV6_CAMSENSOR_H

// Camera sensor interface between the CSI-2 receiver (unicam.c, the
// "bridge") and image sensor drivers (ov5647.c, ...), modelled on the Linux
// V4L2 sub-device API but reduced to what xv6 needs:
//
//   Linux                                   here
//   struct v4l2_subdev                      struct camsensor
//   core_ops.s_power / runtime PM           ops->power_on / power_off
//   video_ops.s_stream                      ops->stream_on(mode) / stream_off
//   v4l2_ctrl_handler (cached, applied
//     by __v4l2_ctrl_handler_setup)         ops->set_ctrl, applied at stream_on
//   pad_ops.enum_frame_size / get_fmt       modes[] (struct cam_mode)
//   fwnode endpoint (data-lanes, ...)       struct camsensor_bus
//   v4l2_async_register_subdev + notifier   camsensor_register(); the bridge
//                                           probes registered sensors in turn
//
// The bridge never touches sensor registers and the sensor never touches
// Unicam: everything the receiver needs to know is in cam_mode and
// camsensor_bus.

#include "camera.h"

// One output format of a sensor.
struct cam_mode {
  char *name;
  uint32 width;
  uint32 height;
  uint32 bytesperline;    // CSI-2 packed bytes per line, multiple of 16
  uint8  csi_dt;          // CSI-2 data type, e.g. CSI2_DT_RAW10
  uint32 format;          // CAM_FMT_* reported to user space (Bayer order)
  const void *priv;       // sensor private: register table, timings, ...
};

// How the sensor is wired to the receiver (Linux: the DT endpoint).
struct camsensor_bus {
  int data_lanes;         // 1 or 2 on CSI1
  int noncontinuous_clk;  // clock lane gated between packets
  int vc;                 // CSI-2 virtual channel of the image stream
};

struct camsensor;

// All callbacks run in process context with the bridge's capture lock held;
// they may sleep or busy-wait.  Return 0 on success.
struct sensor_ops {
  // Supply power and clocks, check the chip ID, leave the CSI-2 lanes in
  // LP-11 ("stream off").  Called before every capture and by probing.
  int  (*power_on)(struct camsensor *s);
  void (*power_off)(struct camsensor *s);
  // Program `mode` plus the cached controls and start transmitting.
  int  (*stream_on)(struct camsensor *s, const struct cam_mode *mode);
  int  (*stream_off)(struct camsensor *s);
  // Cache a control value; it takes effect at the next stream_on.
  // Returns -1 for an unsupported control or value.  May be 0.
  int  (*set_ctrl)(struct camsensor *s, int id, int value);
};

// Controls (Linux V4L2_CID_* equivalents).
#define CAM_CTRL_TEST_PATTERN  1    // 0 off, sensor-specific patterns 1..n

struct camsensor {
  char *name;
  const struct sensor_ops *ops;
  const struct cam_mode *modes;     // modes[0] is the default
  int nmodes;
  struct camsensor_bus bus;
  int warmup_frames;                // frames to discard while AE settles
  void *priv;                       // driver state
};

int camsensor_register(struct camsensor *s);

// CSI-2 data types
#define CSI2_DT_RAW8   0x2a
#define CSI2_DT_RAW10  0x2b
#define CSI2_DT_RAW12  0x2c

#endif
