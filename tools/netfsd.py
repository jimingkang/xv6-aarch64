#!/usr/bin/env python3
"""Read-only xv6 NetFS server.

The wire protocol is intentionally small and stateless.  Restricting v1 to
idempotent operations makes UDP retry safe and provides a clean base for later
leases, write intents, cache validation and replication.
"""

import argparse
import os
import socket
import stat
import struct
from pathlib import Path

MAGIC = b"NFS1"
VERSION = 1
OP_STAT = 1
OP_READDIR = 2
OP_READ = 3
T_DIR = 1
T_FILE = 2
DATA_MAX = 1200
HEADER = struct.Struct("!4sBBHIiIIIIHH")


class NetFSServer:
    def __init__(self, root: Path):
        self.root = root.resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError(f"export root is not a directory: {self.root}")
        self.cache = {}

    def resolve(self, wire_path: bytes) -> Path:
        path = wire_path.decode("utf-8", "strict")
        if not path.startswith("/") or "\x00" in path:
            raise PermissionError("invalid path")
        candidate = (self.root / path.lstrip("/")).resolve(strict=False)
        if os.path.commonpath((self.root, candidate)) != str(self.root):
            raise PermissionError("path escapes export root")
        return candidate

    @staticmethod
    def file_type(mode: int) -> int:
        if stat.S_ISDIR(mode):
            return T_DIR
        if stat.S_ISREG(mode):
            return T_FILE
        return 0

    def reply(self, op, xid, status=0, offset=0, payload=b"", ino=0,
              size=0, file_type=0):
        return HEADER.pack(MAGIC, VERSION, op, 0, xid, status, offset,
                           len(payload), ino & 0xFFFFFFFF,
                           size & 0xFFFFFFFF, file_type, 0) + payload

    def handle(self, packet: bytes) -> bytes | None:
        if len(packet) < HEADER.size:
            return None
        (magic, version, op, _flags, xid, _status, offset, length,
         _ino, _size, _type, path_len) = HEADER.unpack_from(packet)
        if magic != MAGIC or version != VERSION or path_len == 0:
            return None
        if HEADER.size + path_len != len(packet) or path_len >= 128:
            return self.reply(op, xid, -22)
        try:
            path = self.resolve(packet[HEADER.size:])
            if op == OP_STAT:
                info = path.stat()
                kind = self.file_type(info.st_mode)
                if not kind:
                    return self.reply(op, xid, -95)
                return self.reply(op, xid, ino=info.st_ino,
                                  size=info.st_size, file_type=kind)
            if op == OP_READDIR:
                if not path.is_dir():
                    return self.reply(op, xid, -20)
                entries = []
                for entry in sorted(path.iterdir(), key=lambda p: p.name):
                    encoded = entry.name.encode("utf-8", "strict")
                    # xv6's on-wire struct dirent currently has DIRSIZ=14.
                    if not encoded or len(encoded) > 14:
                        continue
                    info = entry.stat()
                    kind = self.file_type(info.st_mode)
                    if kind:
                        entries.append((encoded, info, kind))
                if offset >= len(entries):
                    return self.reply(op, xid)
                name, info, kind = entries[offset]
                return self.reply(op, xid, payload=name, ino=info.st_ino,
                                  size=info.st_size, file_type=kind)
            if op == OP_READ:
                if not path.is_file():
                    return self.reply(op, xid, -21)
                amount = min(length, DATA_MAX)
                with path.open("rb", buffering=0) as stream:
                    stream.seek(offset)
                    data = stream.read(amount)
                info = path.stat()
                return self.reply(op, xid, offset=offset, payload=data,
                                  ino=info.st_ino, size=info.st_size,
                                  file_type=T_FILE)
            return self.reply(op, xid, -95)
        except FileNotFoundError:
            return self.reply(op, xid, -2)
        except PermissionError:
            return self.reply(op, xid, -13)
        except (OSError, UnicodeError, ValueError):
            return self.reply(op, xid, -5)

    def serve(self, bind: str, port: int):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((bind, port))
        print(f"netfsd: exporting {self.root} on {bind}:{port} read-only",
              flush=True)
        while True:
            packet, peer = sock.recvfrom(1472)
            if len(packet) >= HEADER.size:
                xid = HEADER.unpack_from(packet)[4]
                key = (peer, xid)
                response = self.cache.get(key)
                if response is None:
                    response = self.handle(packet)
                    if response is not None:
                        if len(self.cache) >= 256:
                            self.cache.pop(next(iter(self.cache)))
                        self.cache[key] = response
                if response is not None:
                    sock.sendto(response, peer)


def main():
    parser = argparse.ArgumentParser(description="xv6 read-only NetFS server")
    parser.add_argument("--root", default=".", help="directory to export")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5640)
    args = parser.parse_args()
    NetFSServer(Path(args.root)).serve(args.bind, args.port)


if __name__ == "__main__":
    main()
