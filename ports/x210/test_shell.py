#!/usr/bin/env python3
"""Boot the actual ARM32 image and exercise user processes through serial."""
import os
from pathlib import Path
import select
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/x210-qemu"
qemu = os.environ.get("QEMU_ARM") or shutil.which("qemu-system-arm")
if not qemu:
    fallback = Path("/opt/homebrew/opt/qemu/bin/qemu-system-arm")
    if fallback.exists():
        qemu = str(fallback)
if not qemu:
    raise SystemExit("qemu-system-arm is required (or set QEMU_ARM)")
subprocess.run(["make", "-f", "ports/x210/Makefile", "BUILD=build/x210-qemu",
                "QEMU_TEST=1", "all"], cwd=ROOT, check=True,
               stdout=subprocess.DEVNULL)
# QEMU has no S5PV210 machine. cubieboard is a Cortex-A8 board; the legacy
# uImage (IH_OS_LINUX) at DRAM+0x8000 and no -dtb make QEMU use the same
# handoff as the X210 U-Boot bootm: MMU off, r0=0, r1=machine id,
# r2=ATAG list at DRAM+0x100 (ATAG_CORE, ATAG_MEM, ATAG_CMDLINE).
proc = subprocess.Popen([qemu, "-M", "cubieboard", "-m", "512M",
                         "-nographic", "-monitor", "none",
                         "-kernel", f"{BUILD}/xv6-x210.uImage",
                         "-append", "console=ttySAC2,115200 xv6test"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = bytearray()

def expect(token, start=0, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if token in log[start:]:
            return bytes(log[start:])
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if ready:
            chunk = os.read(proc.stdout.fileno(), 8192)
            if not chunk:
                break
            log.extend(chunk)
        if proc.poll() is not None:
            break
    raise AssertionError(f"missing {token!r}:\n{log.decode(errors='replace')}")

def command(text, result=None, timeout=15, faults=False):
    start = len(log)
    proc.stdin.write(text.encode() + b"\n")
    proc.stdin.flush()
    out = expect(b"$ ", start, timeout)
    # Result checks use CRLF to distinguish command echo from command output.
    if result is not None and result not in out:
        raise AssertionError(f"{text}: missing {result!r} in {out!r}")
    if b"panic:" in out or (b"ARM fault" in out and not faults) or b"cannot" in out:
        raise AssertionError(out.decode(errors="replace"))
    return out

try:
    banner = expect(b"$ ")
    for token in (b"xv6 ARM32 / QEMU cubieboard", b"machid=4104 r2=40000100",
                  b"atags: at 40000100", b"atags: memory 40000000 + 512 MiB",
                  b'atags: cmdline "console=ttySAC2,115200 xv6test"',
                  b"kernel 40008000-", b"DRAM 40000000-48000000"):
        if token not in banner:
            raise AssertionError(f"boot banner missing {token!r}:\n{banner.decode(errors='replace')}")
    if b"r2 ignored" in banner:
        raise AssertionError("ATAGs not found at r2")
    command("echo ARM32_OK", b"\r\nARM32_OK\r\n")
    command("echo PIPE_OK | cat", b"\r\nPIPE_OK\r\n")
    command("echo FILE_OK > /test")
    command("cat /test", b"\r\nFILE_OK\r\n")
    command("mkdir /scratch")
    command("cd /scratch")
    command("echo RELATIVE_OK > relative")
    command("cat relative", b"\r\nRELATIVE_OK\r\n")
    command("cd /")
    command("ls /bin", b"forktest")
    command("armcheck", b"armcheck: OK", faults=True)
    command("forktest", b"fork test OK", timeout=40)
    command("stressfs", b"read", timeout=30)
    command("echo AFTER_STRESS", b"\r\nAFTER_STRESS\r\n")
    command("echo one; echo two", b"\r\ntwo\r\n")
    print("PASS: Cortex-A8 bootm/ATAG handoff (r1/r2, ATAG_MEM, ATAG_CMDLINE) at DRAM+0x8000, init/sh, fork/exec/wait, pipes, redirection, directories, timer sleep, user isolation/fault recovery, fork growth/reaping and filesystem stress")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    (BUILD / "shell-test.log").write_bytes(log)
