#!/usr/bin/env python3
"""Annotate an MM_TRACE log with kernel symbol names.

  python3 tools/mm_trace_sym.py kernel/kernel.sym < console.log > console.mm.log

MM_TRACE (make MM_TRACE=1, see include/kernel/mm_trace.h) ends page, slab
and kmalloc lines with "<- 0x<return address>".  Each such address inside
the kernel's .text becomes "<- function+0xoffset", so the line says who
allocated or freed.  Use the kernel.sym from the build that produced the log.
"""
import bisect
import re
import sys


def load_text_symbols(path):
    syms, lo, hi = [], None, None
    for line in open(path):
        parts = line.split()
        if len(parts) != 2:
            continue
        addr, name = int(parts[0], 16), parts[1]
        if name == ".text":
            lo = addr
        elif name == ".rodata":
            hi = addr
        elif not name.startswith(".") and not name.startswith("$"):
            syms.append((addr, name))
    syms.sort()
    return syms, lo, hi


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    syms, lo, hi = load_text_symbols(sys.argv[1])
    addrs = [a for a, _ in syms]

    def name(m):
        a = int(m.group(1), 16)
        if lo is None or hi is None or not lo <= a < hi:
            return m.group(0)
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            return m.group(0)
        return "<- %s+0x%x" % (syms[i][1], a - syms[i][0])

    pat = re.compile(r"<- 0x([0-9a-fA-F]{16})")
    for line in sys.stdin:
        sys.stdout.write(pat.sub(name, line))


if __name__ == "__main__":
    main()
