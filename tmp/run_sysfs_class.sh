#!/bin/bash
cd /Users/jimingkang/Documents/xv6-aarch64 || exit 1
QIN=$(mktemp -u /tmp/xv6qin.XXXXXX)
LOG=/tmp/xv6qclass.log
rm -f "$QIN" "$LOG"
mkfifo "$QIN"

qemu-system-aarch64 -machine raspi3b -kernel kernel/kernel8-xv6_wifi.img \
  -display none -serial null -serial mon:stdio \
  -drive file=fs.img,if=sd,format=raw < "$QIN" > "$LOG" 2>&1 &
QPID=$!
exec 9>"$QIN"
send() { printf '%s\n' "$1" >&9; }

sleep 6; send "root"; sleep 2; send "xv6"; sleep 3

echo "===== /sys/class directory (functional groups) ====="
send "ls /sys/class"
sleep 1

echo "===== /sys/class/tty -- entries are symlinks with arrows ====="
send "ls /sys/class/tty"
sleep 1

echo "===== /sys/class/video4linux -- camera symlink ====="
send "ls /sys/class/video4linux"
sleep 1

echo "===== traversal: read an attribute through the symlink path ====="
send "cat /sys/class/tty/bcm2837-mini-uart/name"
sleep 1
send "cat /sys/class/video4linux/ov5647/uevent"
sleep 1

echo "===== a non-link must fail readlink (ls shows no arrow) ====="
send "ls /sys/devices/ov5647"
sleep 2

exec 9>&-
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; rm -f "$QIN"
echo "===== serial log ====="
grep -v "exec: dynamic" "$LOG" | sed -n '/xv6-rpi3 login/,$p'
