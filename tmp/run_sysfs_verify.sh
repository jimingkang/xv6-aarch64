#!/bin/bash
cd /Users/jimingkang/Documents/xv6-aarch64 || exit 1
QIN=$(mktemp -u /tmp/xv6qin.XXXXXX)
LOG=/tmp/xv6qout.log
rm -f "$QIN" "$LOG"
mkfifo "$QIN"

qemu-system-aarch64 -machine raspi3b -kernel kernel/kernel8-xv6_wifi.img \
  -display none -serial null -serial mon:stdio \
  -drive file=fs.img,if=sd,format=raw < "$QIN" > "$LOG" 2>&1 &
QPID=$!

# Hold the fifo open so the writer side never sees EOF.
exec 9>"$QIN"

send() { printf '%s\n' "$1" >&9; }

sleep 6
send "root"          # login
sleep 2
send "xv6"           # password
sleep 3

echo "===== /sys tree ====="
send "ls /sys"
sleep 2
send "ls /sys/devices"
sleep 2
send "ls /sys/bus"
sleep 2

echo "===== console device attrs ====="
send "cat /sys/devices/bcm2837-mini-uart/name"
sleep 1
send "cat /sys/devices/bcm2837-mini-uart/ioaddr"
sleep 1
send "cat /sys/devices/bcm2837-mini-uart/subsystem"
sleep 1
send "cat /sys/devices/bcm2837-mini-uart/driver"
sleep 1

echo "===== ov5647 device + writable attribute ====="
send "ls /sys/devices/ov5647"
sleep 1
send "cat /sys/devices/ov5647/test_pattern"
sleep 1
send "echo 2 > /sys/devices/ov5647/test_pattern"
sleep 1
send "cat /sys/devices/ov5647/test_pattern"
sleep 1
send "cat /sys/devices/ov5647/driver"
sleep 1
send "cat /sys/devices/ov5647/modalias"
sleep 1

echo "===== bus drivers dir ====="
send "ls /sys/bus/platform/drivers"
sleep 2
send "cat /sys/bus/platform/drivers/ov5647/name"
sleep 2

exec 9>&-
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null
rm -f "$QIN"
echo "===== serial log ====="
cat "$LOG"
