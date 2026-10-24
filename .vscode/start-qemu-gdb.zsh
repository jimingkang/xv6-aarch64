#!/bin/zsh

set -u
unsetopt BG_NICE

port=25501
qemu_listening()
{
  /usr/sbin/lsof -nP -iTCP:${port} -sTCP:LISTEN >/dev/null 2>&1
}

# Reuse a QEMU GDB stub that was started manually.
if qemu_listening; then
  echo "QEMU_GDB_READY port=${port} existing=1"
  while qemu_listening; do
    sleep 1
  done
  exit 0
fi

# Only the readiness watcher may run in the background.  QEMU itself must stay
# in this task's foreground process group: its stdio chardev is the xv6 UART.
# If QEMU is launched with `make qemu-gdb &`, the integrated terminal may keep
# local echoing input while the background QEMU cannot read it.  That looks
# exactly like login accepted Enter but never woke up to print "Password:".
(
  for attempt in {1..1200}; do
    if qemu_listening; then
      echo "QEMU_GDB_READY port=${port} existing=0"
      exit 0
    fi
    sleep 0.1
  done
  echo "QEMU_GDB_ERROR port=${port} did not start listening"
  exit 1
) &

# Keep make/QEMU in the foreground so it owns the VS Code terminal input.
exec make qemu-gdb
