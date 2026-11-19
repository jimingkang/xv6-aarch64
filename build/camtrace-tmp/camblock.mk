
# Camera data path, numbered steps 0-12 (include/kernel/cam_trace.h,
# doc/readme/rpi3/camera.md):
#   CAM_TRACE=1  one line per step, every FS/FE that moves a DMA slot
#   CAM_TRACE=2  + every OV5647 SCCB register write and every warm-up FS
CAM_TRACE ?= 0
CFLAGS += -DCAM_TRACE=$(CAM_TRACE)
CAM_TRACE_CONFIG = $K/.cam_trace.config
.PHONY: FORCE_CAM_TRACE_CONFIG
FORCE_CAM_TRACE_CONFIG:

$(CAM_TRACE_CONFIG): FORCE_CAM_TRACE_CONFIG
	@cfg='$(CAM_TRACE)'; \
	old=$$(test -f $@ && cat $@ || true); \
	if test "$$old" != "$$cfg"; then printf '%s\n' "$$cfg" > $@; fi

$K/drivers/media/platform/unicam.o \
$K/drivers/media/i2c/ov5647.o: $(CAM_TRACE_CONFIG) include/kernel/cam_trace.h
