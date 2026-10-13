#!/bin/bash
cd /Users/jimingkang/Documents/xv6-aarch64 || exit 1
QIN=$(mktemp -u /tmp/xv6qin.XXXXXX)
LOG=/tmp/xv6qout2.log
rm -f "$QIN" "$LOG"
mkfifo "$QIN"

qemu-system-aarch64 -machine raspi3b -kernel kernel/kernel8-xv6_wifi.img \
  -display none -serial null -serial mon:stdio \
  -drive file=fs.img,if=sd,format=raw < "$QIN" > "$LOG" 2>&1 &
QPID=$!
exec 9>"$QIN"
send() { printf '%s\n' "$1" >&9; }

sleep 6; send "root"; sleep 2; send "xv6"; sleep 3

echo "===== per-device uevent (env dump) ====="
send "ls /sys/devices/ov5647"
sleep 1
send "cat /sys/devices/ov5647/uevent"
sleep 1

echo "===== fire a hotplug uevent (echo add > .../uevent) ====="
send "echo add > /sys/devices/bcm2837-mini-uart/uevent"
sleep 2

echo "===== per-bus attributes + drivers_probe ====="
send "ls /sys/bus/platform"
sleep 1
send "cat /sys/bus/platform/name"
sleep 1
send "cat /sys/bus/platform/drivers_probe"
sleep 1
send "echo bcm2837-mini-uart > /sys/bus/platform/drivers_probe"
sleep 2

echo "===== read-only attr must reject a write ====="
send "echo nope > /sys/devices/ov5647/name"
sleep 1
send "cat /sys/devices/ov5647/name"
sleep 2

exec 9>&-
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; rm -f "$QIN"
echo "===== serial log ====="
cat "$LOG"
