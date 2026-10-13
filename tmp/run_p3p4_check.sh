#!/bin/bash
cd /Users/jimingkang/Documents/xv6-aarch64 || exit 1
QIN=$(mktemp -u /tmp/xv6qin.XXXXXX)
LOG=/tmp/xv6qpp.log
rm -f "$QIN" "$LOG"
mkfifo "$QIN"
qemu-system-aarch64 -machine raspi3b -kernel kernel/kernel8-xv6_wifi.img \
  -display none -serial null -serial mon:stdio \
  -drive file=fs.img,if=sd,format=raw < "$QIN" > "$LOG" 2>&1 &
QPID=$!
exec 9>"$QIN"
send() { printf '%s\n' "$1" >&9; }
sleep 6; send "root"; sleep 2; send "xv6"; sleep 3

echo "[P3 console ioaddr RO attribute]"
send "cat /sys/devices/bcm2837-mini-uart/ioaddr"; sleep 1
echo "[P3 ov5647 test_pattern RW attribute: read, write 2, read back]"
send "cat /sys/devices/ov5647/test_pattern"; sleep 1
send "echo 2 > /sys/devices/ov5647/test_pattern"; sleep 1
send "cat /sys/devices/ov5647/test_pattern"; sleep 1
echo "[P4 /sys/class + symlink arrows + traversal]"
send "ls /sys/class"; sleep 1
send "ls /sys/class/tty"; sleep 1
send "ls /sys/class/video4linux"; sleep 1
send "cat /sys/class/video4linux/ov5647/test_pattern"; sleep 2

exec 9>&-
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; rm -f "$QIN"
echo "===== serial log ====="
grep -v "exec: dynamic" "$LOG" | sed -n '/xv6-rpi3 login/,$p'
