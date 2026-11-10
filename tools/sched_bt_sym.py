#!/usr/bin/env python3
"""Annotate a sched-trace log with kernel symbol names.

  python3 tools/sched_bt_sym.py kernel/kernel.sym < console.log > console.sym.log

Every `lr=0x...` / `x30(lr)=0x...` value that falls inside the kernel's .text
is followed by <function+offset>, so the `bt#N` lines printed by
PROC_SCHED_TRACE read as a call chain.  `chan=0x...` values that fall on a
kernel data symbol get <symbol+offset> too (e.g. <cons+0x98> = &cons.r).  Use the kernel.sym from the same build
that produced the log.
"""
import bisect
import re
import sys


def load_symbols(path, data=False):
    syms = []
    text_lo = text_hi = None
    for line in open(path):
        parts = line.split()
        if len(parts) != 2:
            continue
        addr, name = int(parts[0], 16), parts[1]
        if name == ".text":
            text_lo = addr
        elif name == ".rodata":
            text_hi = addr
        elif not name.startswith(".") and not name.startswith("$"):
            syms.append((addr, name))
    in_text = lambda a: text_lo is None or text_lo <= a < (text_hi or 1 << 64)
    syms = sorted(s for s in syms if in_text(s[0]) != data)
    return syms, [a for a, _ in syms]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    syms, addrs = load_symbols(sys.argv[1])
    dsyms, daddrs = load_symbols(sys.argv[1], data=True)
    pat = re.compile(r"((?:x30\(lr\)|lr)=0x([0-9a-fA-F]{16}))")
    cpat = re.compile(r"(chan=0x([0-9a-fA-F]{16}))")

    def lookup(m, table, keys, limit):
        v = int(m.group(2), 16)
        i = bisect.bisect_right(keys, v) - 1
        if v == 0 or i < 0 or v - keys[i] > limit:
            return m.group(1)
        return "%s <%s+0x%x>" % (m.group(1), table[i][1], v - keys[i])

    for line in sys.stdin:
        line = pat.sub(lambda m: lookup(m, syms, addrs, 0x10000), line)
        line = cpat.sub(lambda m: lookup(m, dsyms, daddrs, 0x1000), line)
        sys.stdout.write(line)


if __name__ == "__main__":
    main()
