#!/bin/sh
# Build the TINY210_QEMU stand-in kernel with USB (QEMU_USB=1: A10 EHCI0 +
# usbcore/usbkbd/usbmouse) and start QEMU halted for gdb (-s -S, port 1234).
# Used by .vscode/tasks.json; also usable by hand.
#
#   ports/tiny210/qemu-usb-debug.sh [storage|host|none] [extra qemu args...]
#
#   storage  a 16 MiB usb-storage stick on usb-bus.0 (high speed, enumerates)
#   host     pass a Mac USB device through: USB_VID=0x0bda USB_PID=0x8179
#            (needs libusb access; high-speed devices only, see below)
#   none     no device at boot; add one later from the monitor
#
# Serial console: this terminal.  QEMU monitor: nc localhost 4444, e.g.
#   info usb
#   device_add usb-storage,bus=usb-bus.0,drive=ud,id=stick   (needs -drive)
#   device_add usb-host,bus=usb-bus.0,vendorid=0x..,productid=0x..,id=h
#   device_del h
# The xv6 EHCI driver scans the root ports only at boot, so a device added
# later shows up in "info usb" but is not enumerated by xv6 (no hot-plug).
# usb-kbd/usb-mouse and real keyboards are full/low speed and belong to the
# OHCI companion, which xv6 has no driver for.
set -e
cd "$(dirname "$0")/../.."
MODE=${1:-storage}
[ $# -gt 0 ] && shift
BUILD=build/tiny210-qemu-usb
QEMU=${QEMU_ARM:-qemu-system-arm}
GDB_PORT=${GDB_PORT:-1234}
MON_PORT=${MON_PORT:-4444}

make -f ports/tiny210/Makefile BUILD=$BUILD QEMU_TEST=1 QEMU_USB=1 \
  USB_CORE_TRACE=${USB_CORE_TRACE:-0} USB_ENUM_TRACE=${USB_ENUM_TRACE:-1} USB_XFER_TRACE=${USB_XFER_TRACE:-0} \
  USBHID_TRACE=${USBHID_TRACE:-${USBKBD_TRACE:-0}} all >$BUILD.log 2>&1 \
  || { tail -30 $BUILD.log; echo "build failed, see $BUILD.log"; exit 1; }

DISK=$BUILD/usb-stick.img
[ -f $DISK ] || dd if=/dev/zero of=$DISK bs=1m count=16 2>/dev/null
case $MODE in
storage) DEV="-device usb-storage,bus=usb-bus.0,drive=ud,id=stick" ;;
host)    : "${USB_VID:?set USB_VID (e.g. 0x0bda)}" "${USB_PID:?set USB_PID}"
         DEV="-device usb-host,bus=usb-bus.0,vendorid=$USB_VID,productid=$USB_PID,id=h" ;;
none)    DEV= ;;
*)       echo "mode must be storage, host or none"; exit 2 ;;
esac

# Free a QEMU left over from an earlier debug session.
pkill -f "qemu-system-arm.*$BUILD/xv6-tiny210.uImage" 2>/dev/null || true

echo "qemu-usb-debug: waiting for gdb on :$GDB_PORT, monitor on :$MON_PORT"
exec $QEMU -M cubieboard -m 512M -display none -serial mon:stdio \
  -monitor tcp:127.0.0.1:$MON_PORT,server,nowait \
  -gdb tcp::$GDB_PORT -S \
  -kernel $BUILD/xv6-tiny210.uImage -append "console=ttySAC0,115200" \
  -drive if=none,id=ud,format=raw,file=$DISK $DEV "$@"
