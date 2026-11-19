#ifndef XV6_CAM_TRACE_H
#define XV6_CAM_TRACE_H

// CAM_TRACE: the Raspberry Pi 3 camera data path (OV5647 sensor -> MIPI CSI-2
// -> Unicam CSI1 -> CAMDMA -> /dev/video0 -> camserver/camshot), told as
// numbered steps that match doc/readme/rpi3/camera.md and its diagram:
//
//   step 0  module load order: ov5647 (order 5) registers, unicam (order 4)
//           creates /dev/video0
//   step 1  boot probe: camera_bind -> power_on (CAM_GPIO0, SCCB chip id)
//   step 2  open + mmap: two DMA slots mapped read-only Normal-NC (VM_PFNMAP)
//   step 3  first read: power domain UNICAM1, CAM1 lp clock, sensor power_on
//   step 4  unicam_start: receiver registers, DMA parked on the dummy page
//   step 5  stream_on: OV5647 register tables over SCCB, MIPI leaves LP-11
//   step 6  hardware: CSI-2 FS / RAW10 lines / FE -> FIFO -> AXI -> DDR
//   step 7  warm-up: IRQ 39 / camera_wq count FS until AE settles
//   step 8  cam_grab: a FREE slot is written to IBSA0/IBEA0 (NEXT, ARMED)
//   step 9  FS: hardware latched the slot (ACTIVE), the other slot queued
//   step 10 FE: ACTIVE -> DONE, doneq push, wakeup(&cam.done_count)
//   step 11 dequeue: header-only read (zero copy, USER) or copyout; release
//   step 12 last close: stream_off, receiver stop, power off
//
//   make CAM_TRACE=0   off (default; the existing unicam/ov5647 messages only)
//   make CAM_TRACE=1   + one line per step, one line per FS/FE that moves a
//                        slot, register read-back after unicam_start
//   make CAM_TRACE=2   + every SCCB register write and every warm-up FS
//                        (about 100 lines per stream_on; slows bring-up)
//
// Lines start with "cam<step>:" so `grep '^cam[0-9]'` gives the whole story.
#ifndef CAM_TRACE
#define CAM_TRACE 0
#endif
#define CAMTRACE(level) (CAM_TRACE >= (level))

#endif
