#!/usr/bin/env python3
"""Boot image helpers for the 九鼎 X210 (Samsung S5PV210) xv6 port.

check-zimage IMAGE LOADADDR      verify the ARM zImage header (bootz / Samsung bootm)
uimage IMAGE OUTPUT LOADADDR     wrap IMAGE in a legacy U-Boot kernel header
bl1 CODE OUTPUT                  make the 16 KiB BL1 the S5PV210 iROM loads
                                 (same format as the X210 BSP's mkx210 tool)
check-bl1 BL1                    verify a BL1 header and checksum
sdimg OUTPUT BL1CODE KERNEL      raw SD image, no U-Boot: xv6 BL1 @ sector 1,
                                 xv6 uImage @ sector 49
selftest                         check the BL1 and SD layout code

S5PV210 iROM (BL0) SD boot: read 16 KiB from sector 1 into iRAM at
0xD0020000, verify the 16-byte header, run from 0xD0020010.
  0x0 BL1 size (16384)   0x4 reserved (0)
  0x8 checksum = sum of bytes 16..16383    0xC reserved (0)
ports/x210/bl1 is that BL1: it sets up clocks, UART2 and DDR, then copies
the xv6 uImage from sector 49 (where the BSP keeps BL2/U-Boot) to
0x30007FC0 with the iROM's CopySDMMCtoMem and jumps to 0x30008000.
"""
import struct
import sys
import zlib
from pathlib import Path

ZMAGIC = 0x016F2818
BL1_SIZE = 16 * 1024
BL1_HDR = 16
SECTOR = 512
BL1_SECTOR = 1          # iROM fixed location
KERNEL_SECTOR = 49      # BL2 position on X210; bl1.c KERNEL_SECTOR
KERNEL_MAX = 32 * 1024 * 1024   # bl1lib.c uimage_check limit


def check_zimage(path, load):
    data = Path(path).read_bytes()
    magic, start, end, endian = struct.unpack_from("<4I", data, 0x24)
    assert magic == ZMAGIC, f"{path}: no zImage magic at 0x24"
    assert endian == 0x04030201, f"{path}: bad endianness marker"
    assert end - start == len(data), f"{path}: header size {end - start} != file {len(data)}"
    print(f"{path}: zImage-compatible image, run address {load:#010x}, {len(data)} bytes")


def uimage_bytes(payload, load):
    # IH_OS_LINUX(5) IH_ARCH_ARM(2) IH_TYPE_KERNEL(2) IH_COMP_NONE(0):
    # bootm then uses the ARM Linux handoff (caches/MMU off, r2 = ATAGs).
    values = [0x27051956, 0, 0, len(payload), load, load,
              zlib.crc32(payload), 5, 2, 2, 0, b"xv6 ARM32 X210 S5PV210"]
    fmt = ">7I4B32s"
    header = struct.pack(fmt, *values)
    values[1] = zlib.crc32(header)
    header = struct.pack(fmt, *values)
    assert len(header) == 64
    return header + payload


def uimage(src, dst, load):
    out = uimage_bytes(Path(src).read_bytes(), load)
    Path(dst).write_bytes(out)
    print(f"{dst}: legacy uImage, load/entry {load:#010x}, {len(out) - 64} bytes")


def bl1_bytes(code):
    body = bytearray(code[:BL1_SIZE - BL1_HDR])
    body += bytes(BL1_SIZE - BL1_HDR - len(body))
    checksum = sum(body) & 0xFFFFFFFF
    return struct.pack("<4I", BL1_SIZE, 0, checksum, 0) + bytes(body)


def check_bl1_bytes(data, name="BL1"):
    assert len(data) >= BL1_SIZE, f"{name}: shorter than 16 KiB"
    size, _, checksum, _ = struct.unpack_from("<4I", data, 0)
    want = sum(data[BL1_HDR:BL1_SIZE]) & 0xFFFFFFFF
    assert checksum == want, f"{name}: checksum {checksum:#x} != {want:#x}"
    # The BSP mkx210 leaves "S5PC" in the size word; the iROM loads 16 KiB anyway.
    assert size in (BL1_SIZE, 0x43503553), f"{name}: size word {size:#x}"
    return checksum


def bl1(src, dst):
    code = Path(src).read_bytes()
    out = bl1_bytes(code)
    Path(dst).write_bytes(out)
    print(f"{dst}: BL1 from first {BL1_SIZE - BL1_HDR} bytes of {src}, "
          f"checksum {check_bl1_bytes(out, dst):#010x}")


def check_bl1(path):
    c = check_bl1_bytes(Path(path).read_bytes(), path)
    print(f"{path}: BL1 header OK, checksum {c:#010x}")


def sdimg_bytes(code, kernel):
    assert len(code) <= BL1_SIZE - BL1_HDR, f"BL1 code {len(code)} bytes > {BL1_SIZE - BL1_HDR}"
    assert len(kernel) <= KERNEL_MAX, f"kernel {len(kernel)} bytes > {KERNEL_MAX}"
    assert struct.unpack_from(">I", kernel, 0)[0] == 0x27051956, "kernel must be a uImage"
    img = bytearray(KERNEL_SECTOR * SECTOR + len(kernel))
    img[BL1_SECTOR * SECTOR:BL1_SECTOR * SECTOR + BL1_SIZE] = bl1_bytes(code)
    img[KERNEL_SECTOR * SECTOR:] = kernel
    img += bytes(-len(img) % SECTOR)
    return bytes(img)


def sdimg(dst, code_path, kernel_path):
    code = Path(code_path).read_bytes()
    kernel = Path(kernel_path).read_bytes()
    img = sdimg_bytes(code, kernel)
    Path(dst).write_bytes(img)
    print(f"{dst}: {len(img)} bytes; BL1 @ sector {BL1_SECTOR} ({len(code)} bytes code), "
          f"xv6 uImage @ {KERNEL_SECTOR} ({len(kernel)} bytes, "
          f"{(len(kernel) + SECTOR - 1) // SECTOR} sectors)")
    print("  sector 0 (MBR) is left zero; write with skip=1 seek=1 to keep a card's partition table")
    print(f"  keep partitions above sector {(len(img) + SECTOR - 1) // SECTOR}")


def selftest():
    code = bytes((i * 7 + 3) & 0xFF for i in range(20000))
    b = bl1_bytes(code)
    assert len(b) == BL1_SIZE
    assert struct.unpack_from("<I", b, 0)[0] == BL1_SIZE
    assert b[16:] == code[:BL1_SIZE - 16]
    assert struct.unpack_from("<I", b, 8)[0] == sum(code[:BL1_SIZE - 16])
    # A BSP mkx210 image: "S5PC110 HEADER  " then the checksum at 8.
    bsp = bytearray(b"S5PC110 HEADER  " + code[:BL1_SIZE - 16])
    struct.pack_into("<I", bsp, 8, sum(bsp[16:]))
    check_bl1_bytes(bytes(bsp), "mkx210-style")
    kernel = bytes(range(256)) * 10
    u = uimage_bytes(kernel, 0x30008000)
    img = sdimg_bytes(code[:8000], u)
    assert img[:SECTOR] == bytes(SECTOR)
    assert img[SECTOR:SECTOR + BL1_SIZE] == bl1_bytes(code[:8000])
    assert img[33 * SECTOR:49 * SECTOR] == bytes(16 * SECTOR)
    assert img[49 * SECTOR:49 * SECTOR + len(u)] == u
    assert len(img) % SECTOR == 0
    magic, hcrc, _, size, load, ep, dcrc = struct.unpack_from(">7I", u, 0)
    assert magic == 0x27051956 and size == len(kernel) and load == ep == 0x30008000
    assert dcrc == zlib.crc32(kernel)
    assert hcrc == zlib.crc32(u[:4] + b"\0\0\0\0" + u[8:64])
    print("mkx210 selftest: BL1 header/checksum, mkx210 compatibility, SD layout BL1@1 / xv6@49, uImage OK")


if __name__ == "__main__":
    a = sys.argv[1:]
    cmd = a[0] if a else ""
    if cmd == "check-zimage" and len(a) == 3:
        check_zimage(a[1], int(a[2], 0))
    elif cmd == "uimage" and len(a) == 4:
        uimage(a[1], a[2], int(a[3], 0))
    elif cmd == "bl1" and len(a) == 3:
        bl1(a[1], a[2])
    elif cmd == "check-bl1" and len(a) == 2:
        check_bl1(a[1])
    elif cmd == "sdimg" and len(a) == 4:
        sdimg(a[1], a[2], a[3])
    elif cmd == "selftest" and len(a) == 1:
        selftest()
    else:
        sys.exit(__doc__)
