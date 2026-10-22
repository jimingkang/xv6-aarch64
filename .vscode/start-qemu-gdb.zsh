#!/bin/zsh

set -u
unsetopt BG_NICE

port=25501
qemu_pid=""

qemu_listening()
{
  /usr/sbin/lsof -nP -iTCP:${port} -sTCP:LISTEN >/dev/null 2>&1
}

cleanup()
{
  if [[ -n "${qemu_pid}" ]] && kill -0 "${qemu_pid}" >/dev/null 2>&1; then
    kill "${qemu_pid}" >/dev/null 2>&1
    wait "${qemu_pid}" >/dev/null 2>&1
  fi
}

trap cleanup INT TERM EXIT

# Reuse a QEMU GDB stub that was started manually.
if qemu_listening; then
  echo "QEMU_GDB_READY port=${port} existing=1"
  while qemu_listening; do
    sleep 1
  done
  exit 0
fi

make qemu-gdb &
qemu_pid=$!

# Do not release the debugger until QEMU has actually bound its GDB socket.
for attempt in {1..1200}; do
  if qemu_listening; then
    echo "QEMU_GDB_READY port=${port} existing=0"
    wait "${qemu_pid}"
    status=$?
    qemu_pid=""
    trap - INT TERM EXIT
    exit ${status}
  fi

  if ! kill -0 "${qemu_pid}" >/dev/null 2>&1; then
    wait "${qemu_pid}"
    status=$?
    qemu_pid=""
    trap - INT TERM EXIT
    exit ${status}
  fi

  sleep 0.1
done

echo "QEMU_GDB_ERROR port=${port} did not start listening"
exit 1
