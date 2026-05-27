#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import time


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLPREFIX = os.environ.get("TOOLPREFIX", "aarch64-elf-")
GDBPORT = os.environ.get("GDBPORT", "26000")
GDB = os.environ.get("XV6_GDB", TOOLPREFIX + "gdb")
TRACE_SCRIPT = os.environ.get("XV6_TRACE_SCRIPT", "tools/trace_xv6.py")
GDB_LOG = os.environ.get("XV6_GDB_LOG", "trace/gdb.log")
SHOW_GDB = os.environ.get("XV6_SHOW_GDB", "0") == "1"
COMMANDS = [
    command.strip()
    for command in os.environ.get("XV6_TRACE_COMMANDS", "ls;usertests").split(";")
    if command.strip()
]
DONE_PATTERNS = [
    pattern.strip()
    for pattern in os.environ.get(
        "XV6_TRACE_DONE_PATTERNS",
        "ALL TESTS PASSED;SOME TESTS FAILED",
    ).split(";")
    if pattern.strip()
]
TIMEOUT_SECONDS = int(os.environ.get("XV6_TRACE_TIMEOUT", "600"))
EXIT_ON_DONE = os.environ.get("XV6_TRACE_EXIT_ON_DONE", "1") != "0"


def start_qemu():
    master_fd, slave_fd = pty.openpty()
    process = subprocess.Popen(
        ["make", "TOOLPREFIX=%s" % TOOLPREFIX, "GDBPORT=%s" % GDBPORT, "qemu-gdb"],
        cwd=ROOT,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)
    return process, master_fd


def start_gdb():
    env = os.environ.copy()
    env.setdefault("XV6_GDB_REMOTE", "localhost:%s" % GDBPORT)
    env.setdefault("XV6_TRACE_QUIET", "1")
    os.makedirs(os.path.dirname(GDB_LOG) or ".", exist_ok=True)
    gdb_output = None if SHOW_GDB else open(GDB_LOG, "w")
    return subprocess.Popen(
        [GDB, "-q", "kernel/kernel", "-x", TRACE_SCRIPT],
        cwd=ROOT,
        env=env,
        stdout=gdb_output,
        stderr=subprocess.STDOUT,
    ), gdb_output


def stop_process(process, name):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    print("[runner] stopped %s" % name)


def main():
    if not COMMANDS:
        print("[runner] no commands configured")
        return 2

    print("[runner] qemu-gdb port: %s" % GDBPORT)
    print("[runner] commands: %s" % ", ".join(COMMANDS))

    qemu, qemu_fd = start_qemu()
    gdb = None
    gdb_output = None
    command_index = 0
    qemu_buffer = ""
    gdb_started = False
    start_time = time.time()

    try:
        while True:
            if time.time() - start_time > TIMEOUT_SECONDS:
                print("\n[runner] timeout after %d seconds" % TIMEOUT_SECONDS)
                return 1

            if qemu.poll() is not None:
                print("\n[runner] qemu exited with %s" % qemu.returncode)
                return qemu.returncode or 0

            readable, _, _ = select.select([qemu_fd], [], [], 0.1)
            if qemu_fd not in readable:
                continue

            data = os.read(qemu_fd, 4096)
            if not data:
                continue

            text = data.decode(errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            qemu_buffer += text
            qemu_buffer = qemu_buffer[-8192:]

            if not gdb_started and "qemu-system-aarch64" in qemu_buffer:
                gdb, gdb_output = start_gdb()
                gdb_started = True
                if SHOW_GDB:
                    print("[runner] started gdb trace")
                else:
                    print("[runner] started gdb trace, log: %s" % GDB_LOG)

            if "$ " in qemu_buffer and command_index < len(COMMANDS):
                command = COMMANDS[command_index]
                command_index += 1
                os.write(qemu_fd, (command + "\n").encode())
                print("[runner] sent command: %s" % command)
                qemu_buffer = ""

            if command_index >= len(COMMANDS):
                for pattern in DONE_PATTERNS:
                    if pattern in qemu_buffer:
                        print("\n[runner] done pattern matched: %s" % pattern)
                        if EXIT_ON_DONE:
                            os.write(qemu_fd, b"\x01x")
                        return 0
    finally:
        if gdb is not None:
            stop_process(gdb, "gdb")
        if gdb_output is not None:
            gdb_output.close()
        stop_process(qemu, "qemu")
        os.close(qemu_fd)


if __name__ == "__main__":
    raise SystemExit(main())
