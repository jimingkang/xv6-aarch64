#!/usr/bin/env python3
"""Boot image helpers for the BeagleBone Black (AM335x) xv6 port.

check-zimage IMAGE LOADADDR   verify the ARM zImage header U-Boot bootz reads
uimage IMAGE OUTPUT LOADADDR  wrap IMAGE in a legacy U-Boot kernel header
"""
import struct
import sys
import zlib
from pathlib import Path

ZMAGIC = 0x016F2818


def check_zimage(path, load):
    data = Path(path).read_bytes()
    magic, start, end, endian = struct.unpack_from("<4I", data, 0x24)
    assert magic == ZMAGIC, f"{path}: no zImage magic at 0x24"
    assert endian == 0x04030201, f"{path}: bad endianness marker"
    assert end - start == len(data), f"{path}: header size {end - start} != file {len(data)}"
    print(f"{path}: bootz image, run address {load:#010x}, {len(data)} bytes")


def uimage(src, dst, load):
    payload = Path(src).read_bytes()
    # IH_OS_LINUX(5) IH_ARCH_ARM(2) IH_TYPE_KERNEL(2) IH_COMP_NONE(0):
    # bootm then uses the ARM Linux handoff (caches/MMU off, r2 = DTB).
    values = [0x27051956, 0, 0, len(payload), load, load,
              zlib.crc32(payload), 5, 2, 2, 0, b"xv6 ARM32 AM335x BeagleBone"]
    fmt = ">7I4B32s"
    header = struct.pack(fmt, *values)
    values[1] = zlib.crc32(header)
    header = struct.pack(fmt, *values)
    assert len(header) == 64
    Path(dst).write_bytes(header + payload)
    print(f"{dst}: legacy uImage, load/entry {load:#010x}, {len(payload)} bytes")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "check-zimage" and len(sys.argv) == 4:
        check_zimage(sys.argv[2], int(sys.argv[3], 0))
    elif cmd == "uimage" and len(sys.argv) == 5:
        uimage(sys.argv[2], sys.argv[3], int(sys.argv[4], 0))
    else:
        sys.exit(__doc__)
