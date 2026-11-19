#ifndef XV6_USBKBD_TRACE_H
#define XV6_USBKBD_TRACE_H

// USBKBD_TRACE: the USB keyboard data path on the Raspberry Pi 3 (DWC2 host
// controller -> hub -> HID boot keyboard -> usbkbd -> console/input), told as
// numbered steps that match doc/readme/rpi3/usb-keyboard.md and its diagram:
//
//   step 0  module load order (usbhid-kbd before dwc2)
//   step 1  DWC2 host-controller init (power, core reset, host mode, port)
//   step 2  root device = hub: address 1, configuration, port power
//   step 3  hub port reset, split route, keyboard address/config descriptor
//   step 4  usb_device_register -> usb bus match -> usbkbd_probe
//   step 5  probe: SET_PROTOCOL/SET_IDLE, input device, submit interrupt URB
//   step 6  channel 6 polls the interrupt-IN endpoint (NAK -> retry later)
//   step 7  IRQ 9 -> dwc2_irq -> schedule_work (top half)
//   step 8  dwc2 work: DMA result, DATA toggle, usb_hcd_giveback_urb
//   step 9  usbkbd_rx_work: report diff -> consoleintr()/input events, resubmit
//
//   make USBKBD_TRACE=0   off (default; the existing dwc2/usbkbd messages only)
//   make USBKBD_TRACE=1   + one line per step during load/configuration, each
//                           report, each key press/release and console char
//   make USBKBD_TRACE=2   + every control transfer (decoded SETUP), every URB
//                           submit/giveback, every channel-6 arm and data IRQ
//   make USBKBD_TRACE=3   + idle polling: every NAK/NYET IRQ and bInterval
//                           retry (~400 lines/s while idle; on real hardware the
//                           serial output also perturbs split timing)
//
// Lines start with "kbd<step>:" ("kbd-:" for a control transfer in any step)
// so `grep '^kbd'` gives the whole story; tools/rpi3_usbkbd_svg.py quotes them.
#ifndef USBKBD_TRACE
#define USBKBD_TRACE 0
#endif
#define KBDTRACE(level) (USBKBD_TRACE >= (level))

#endif
