#!/usr/bin/env python3
"""Minimal read-only TFTP server (RFC 1350, octet mode) for updating the
Tiny210 over Wi-Fi: `make tiny210-tftp` serves build/tiny210 on port 6969
(no root needed); on the board, `tftp` then fetches xv6_tiny210.img into
/boot and `reboot` boots it.

usage: tftpserve.py DIRECTORY [PORT]
"""
import os
import socket
import sys
import threading

BLOCK = 512
TIMEOUT = 2.0
RETRIES = 5


def error(sock, addr, code, text):
    sock.sendto(b"\x00\x05" + code.to_bytes(2, "big") + text.encode() + b"\x00", addr)


def serve_file(root, filename, addr):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))                 # transfer ID: a fresh port per transfer
    sock.settimeout(TIMEOUT)
    name = os.path.basename(filename)  # never leave the served directory
    path = os.path.join(root, name)
    if not name or not os.path.isfile(path):
        error(sock, addr, 1, "file not found")
        print(f"tftp: {addr[0]} asked for {filename!r}: not found", flush=True)
        return
    with open(path, "rb") as f:
        data = f.read()
    print(f"tftp: sending {name} ({len(data)} bytes) to {addr[0]}", flush=True)
    block = 1
    offset = 0
    while True:
        chunk = data[offset:offset + BLOCK]
        packet = b"\x00\x03" + (block & 0xFFFF).to_bytes(2, "big") + chunk
        for _ in range(RETRIES):
            sock.sendto(packet, addr)
            try:
                while True:
                    reply, src = sock.recvfrom(516)
                    if src != addr or len(reply) < 4:
                        continue
                    if reply[1] == 4 and int.from_bytes(reply[2:4], "big") == (block & 0xFFFF):
                        break
                    if reply[1] == 5:
                        print("tftp: client aborted", flush=True)
                        return
                break
            except socket.timeout:
                continue
        else:
            print(f"tftp: {addr[0]} stopped acknowledging at block {block}", flush=True)
            return
        if len(chunk) < BLOCK:
            print(f"tftp: {name} sent to {addr[0]}", flush=True)
            return
        block += 1
        offset += BLOCK


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6969
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", port))
    print(f"tftp: serving {os.path.abspath(root)} on UDP port {port} (Ctrl-C to stop)", flush=True)
    while True:
        packet, addr = sock.recvfrom(1024)
        if len(packet) < 4 or packet[1] != 1:      # only RRQ
            error(sock, addr, 4, "read requests only")
            continue
        filename = packet[2:].split(b"\x00")[0].decode(errors="replace")
        threading.Thread(target=serve_file, args=(root, filename, addr), daemon=True).start()


if __name__ == "__main__":
    main()
