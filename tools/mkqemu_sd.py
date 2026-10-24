#!/usr/bin/env python3
"""Build a sparse Raspberry Pi SD-card image for xv6/QEMU.

The result contains an MBR and three primary partitions:

  p1  FAT32 boot filesystem
  p2  raw xv6fs (type 0x7f)
  p3  ext2 root filesystem (type 0x83)

This intentionally avoids loop devices and host mounts, so it works during a
normal, unprivileged build on both macOS and Linux.
"""

import argparse
import math
import os
import struct
from pathlib import Path


SECTOR = 512
MIB = 1024 * 1024
TOTAL_MIB = 512
P1_START = 2048
P1_SECTORS = 64 * MIB // SECTOR
P2_START = P1_START + P1_SECTORS
P2_SECTORS = 64 * MIB // SECTOR
P3_START = P2_START + P2_SECTORS
TOTAL_SECTORS = TOTAL_MIB * MIB // SECTOR
P3_SECTORS = TOTAL_SECTORS - P3_START
DISK_SIGNATURE = 0x58563633  # ASCII-ish "XV63", stable PARTUUID prefix.


def put16(buf, off, value):
    struct.pack_into("<H", buf, off, value)


def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)


def partition_entry(boot, ptype, start, sectors):
    # LBA fields are authoritative.  Saturated CHS values keep old tools happy.
    return struct.pack("<B3sB3sII", boot, b"\xfe\xff\xff", ptype,
                       b"\xfe\xff\xff", start, sectors)


def short_name(name, used):
    illegal = '"*+,/:;<=>?[\\]|'

    def clean(text):
        out = ""
        for ch in text.upper():
            if ch == " " or ord(ch) < 0x20 or ch in illegal:
                continue
            out += ch if ord(ch) < 128 else "_"
        return out

    stem, dot, ext = name.rpartition(".")
    if not dot:
        stem, ext = name, ""
    base = clean(stem)
    suffix = clean(ext)[:3]
    candidate = (base[:8].ljust(8) + suffix.ljust(3)).encode("ascii")
    simple = (name == name.upper() and 1 <= len(base) <= 8 and
              len(suffix) <= 3 and candidate not in used)
    if simple:
        used.add(candidate)
        return candidate, False
    for serial in range(1, 100000):
        tail = "~" + str(serial)
        alias = (base[:8 - len(tail)] + tail).ljust(8) + suffix.ljust(3)
        candidate = alias.encode("ascii")
        if candidate not in used:
            used.add(candidate)
            return candidate, True
    raise RuntimeError(f"cannot make an 8.3 alias for {name}")


def lfn_checksum(name11):
    value = 0
    for byte in name11:
        value = ((value & 1) << 7) + (value >> 1) + byte
        value &= 0xff
    return value


def lfn_entries(name, name11):
    chars = [ord(ch) for ch in name]
    chars.append(0)
    while len(chars) % 13:
        chars.append(0xffff)
    count = len(chars) // 13
    checksum = lfn_checksum(name11)
    result = []
    positions = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
    for order in range(count, 0, -1):
        entry = bytearray(32)
        entry[0] = order | (0x40 if order == count else 0)
        entry[11] = 0x0f
        entry[13] = checksum
        chunk = chars[(order - 1) * 13:order * 13]
        for pos, char in zip(positions, chunk):
            put16(entry, pos, char)
        result.append(bytes(entry))
    return result


def short_entry(name11, first_cluster, size):
    entry = bytearray(32)
    entry[:11] = name11
    entry[11] = 0x20
    put16(entry, 20, first_cluster >> 16)
    put16(entry, 26, first_cluster & 0xffff)
    put32(entry, 28, size)
    return bytes(entry)


def make_fat32(image, partition_offset, partition_sectors, files):
    reserved = 32
    fat_count = 2
    sectors_per_cluster = 1
    fat_sectors = 1
    while True:
        clusters = ((partition_sectors - reserved - fat_count * fat_sectors) //
                    sectors_per_cluster)
        required = math.ceil((clusters + 2) * 4 / SECTOR)
        # Rounding can make the exact equation alternate between N and N-1.
        # A FAT that is one sector larger is valid, so stop once it covers all
        # clusters rather than requiring strict equality.
        if fat_sectors >= required:
            break
        fat_sectors = required
    data_start = reserved + fat_count * fat_sectors

    prepared = []
    used = set()
    directory_slots = 1  # final zero entry
    for source, destination in files:
        data = Path(source).read_bytes()
        alias, needs_lfn = short_name(destination, used)
        lfns = lfn_entries(destination, alias) if needs_lfn else []
        directory_slots += len(lfns) + 1
        prepared.append((destination, alias, lfns, data))

    root_clusters = max(1, math.ceil(directory_slots * 32 /
                                     (SECTOR * sectors_per_cluster)))
    root_chain = list(range(2, 2 + root_clusters))
    next_cluster = 2 + root_clusters
    allocated = []
    for destination, alias, lfns, data in prepared:
        count = max(1, math.ceil(len(data) / (SECTOR * sectors_per_cluster)))
        chain = list(range(next_cluster, next_cluster + count))
        next_cluster += count
        if chain[-1] >= clusters + 2:
            raise RuntimeError("boot files do not fit in the FAT32 partition")
        allocated.append((destination, alias, lfns, data, chain))

    boot = bytearray(SECTOR)
    boot[:3] = b"\xeb\x58\x90"
    boot[3:11] = b"XV6FAT32"
    put16(boot, 11, SECTOR)
    boot[13] = sectors_per_cluster
    put16(boot, 14, reserved)
    boot[16] = fat_count
    put16(boot, 17, 0)
    put16(boot, 19, 0)
    boot[21] = 0xf8
    put16(boot, 22, 0)
    put16(boot, 24, 63)
    put16(boot, 26, 255)
    put32(boot, 28, P1_START)
    put32(boot, 32, partition_sectors)
    put32(boot, 36, fat_sectors)
    put16(boot, 40, 0)
    put16(boot, 42, 0)
    put32(boot, 44, 2)
    put16(boot, 48, 1)
    put16(boot, 50, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    put32(boot, 67, DISK_SIGNATURE)
    boot[71:82] = b"XV6BOOT    "
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xaa"

    fsinfo = bytearray(SECTOR)
    put32(fsinfo, 0, 0x41615252)
    put32(fsinfo, 484, 0x61417272)
    used_clusters = root_clusters + sum(len(item[4]) for item in allocated)
    put32(fsinfo, 488, clusters - used_clusters)
    put32(fsinfo, 492, next_cluster)
    fsinfo[510:512] = b"\x55\xaa"

    fat = bytearray(fat_sectors * SECTOR)
    put32(fat, 0, 0x0ffffff8)
    put32(fat, 4, 0x0fffffff)

    def set_chain(chain):
        for index, cluster in enumerate(chain):
            value = chain[index + 1] if index + 1 < len(chain) else 0x0fffffff
            put32(fat, cluster * 4, value)

    set_chain(root_chain)
    for item in allocated:
        set_chain(item[4])

    image.seek(partition_offset)
    image.write(boot)
    image.seek(partition_offset + SECTOR)
    image.write(fsinfo)
    image.seek(partition_offset + 6 * SECTOR)
    image.write(boot)
    image.seek(partition_offset + 7 * SECTOR)
    image.write(fsinfo)
    for copy in range(fat_count):
        image.seek(partition_offset + (reserved + copy * fat_sectors) * SECTOR)
        image.write(fat)

    directory = bytearray(root_clusters * SECTOR * sectors_per_cluster)
    cursor = 0
    for destination, alias, lfns, data, chain in allocated:
        for entry in lfns:
            directory[cursor:cursor + 32] = entry
            cursor += 32
        directory[cursor:cursor + 32] = short_entry(alias, chain[0], len(data))
        cursor += 32
        file_offset = partition_offset + (data_start +
                      (chain[0] - 2) * sectors_per_cluster) * SECTOR
        image.seek(file_offset)
        image.write(data)
    root_offset = partition_offset + data_start * SECTOR
    image.seek(root_offset)
    image.write(directory)


def copy_into_partition(image, source, start, sectors, label):
    source_size = os.path.getsize(source)
    capacity = sectors * SECTOR
    if source_size > capacity:
        raise RuntimeError(f"{label}: {source} ({source_size} bytes) exceeds "
                           f"partition capacity ({capacity} bytes)")
    image.seek(start * SECTOR)
    with open(source, "rb") as src:
        while True:
            chunk = src.read(MIB)
            if not chunk:
                break
            image.write(chunk)


def parse_boot_file(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected SOURCE=DESTINATION")
    source, destination = value.split("=", 1)
    if not source or not destination or "/" in destination:
        raise argparse.ArgumentTypeError("boot destination must be a root filename")
    return source, destination


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--xv6", required=True)
    parser.add_argument("--ext2", required=True)
    parser.add_argument("--boot-file", action="append", default=[],
                        type=parse_boot_file, metavar="SOURCE=DESTINATION")
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w+b") as image:
        image.truncate(TOTAL_SECTORS * SECTOR)
        mbr = bytearray(SECTOR)
        put32(mbr, 440, DISK_SIGNATURE)
        mbr[446:462] = partition_entry(0x80, 0x0c, P1_START, P1_SECTORS)
        mbr[462:478] = partition_entry(0x00, 0x7f, P2_START, P2_SECTORS)
        mbr[478:494] = partition_entry(0x00, 0x83, P3_START, P3_SECTORS)
        mbr[510:512] = b"\x55\xaa"
        image.write(mbr)
        make_fat32(image, P1_START * SECTOR, P1_SECTORS, args.boot_file)
        copy_into_partition(image, args.xv6, P2_START, P2_SECTORS, "xv6fs")
        copy_into_partition(image, args.ext2, P3_START, P3_SECTORS, "ext2")

    print(f"built {output}: {TOTAL_MIB} MiB sparse SD image")
    print(f"  p1 FAT32  start={P1_START} sectors={P1_SECTORS} (64 MiB)")
    print(f"  p2 xv6fs  start={P2_START} sectors={P2_SECTORS} (64 MiB)")
    print(f"  p3 ext2   start={P3_START} sectors={P3_SECTORS} "
          f"({P3_SECTORS // 2048} MiB, root=/dev/mmcblk0p3)")


if __name__ == "__main__":
    main()
