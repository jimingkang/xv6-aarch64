#!/usr/bin/env python3
"""Wrap ARM32 xv6 in a legacy U-Boot kernel header (no compression)."""
import argparse
import struct
import zlib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
payload = args.input.read_bytes()
# IH_OS_LINUX invokes the ARM Linux handoff, including cache/MMU cleanup.
# xv6 ignores r0/r1/r2; this is a boot protocol choice, not a Linux kernel.
values = [0x27051956, 0, 0, len(payload), 0x40200000, 0x40200000,
          zlib.crc32(payload), 5, 2, 2, 0, b"xv6 ARM32 T113 serial shell"]
fmt = ">7I4B32s"
header = struct.pack(fmt, *values)
values[1] = zlib.crc32(header)
header = struct.pack(fmt, *values)
args.output.write_bytes(header + payload)
assert len(header) == 64
check = bytearray(header)
check[4:8] = b"\0" * 4
assert zlib.crc32(check) == values[1]
assert zlib.crc32(payload) == values[6]
print(f"{args.output}: ARM legacy kernel, load/entry 0x40200000, {len(payload)} bytes")
