#ifndef XV6_VC4_NATIVE_H
#define XV6_VC4_NATIVE_H

#include <kernel/types.h>
#include <kernel/bcm2708_fb.h>

// Native BCM2837 display backend.  The firmware is used only to establish the
// HDMI link/mode; scanout memory, HVS display lists, Pixel Valve 2 and vblank
// delivery are owned by this driver after vc4_native_kms_init() succeeds.
int vc4_native_kms_init(const struct bcm2708_fb_info *firmware_mode,
                        struct bcm2708_fb_info *scanout);
int vc4_native_page_flip(uint32 slot, uint64 *sequence);
int vc4_native_wait_vblank(uint64 *sequence);
int vc4_native_is_active(void);
void vc4_native_irq(void);

#endif
