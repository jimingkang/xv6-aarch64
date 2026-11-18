#!/usr/bin/env python3
# Raspberry Pi 3 (rpi3-lock) memory layout and MMU page-table diagrams.
# Values come from a QEMU raspi3b boot with `make MMU_TRACE=3`
# (tools/manim/rpi3-mmu-trace3.log) and `cat /proc/meminfo /proc/iomem`.
#
#   python3 tools/rpi3_memory_svg.py doc/readme/rpi3/images
#   for f in memory-layout mmu-page-tables; do
#     rsvg-convert -z 1.5 doc/readme/rpi3/images/$f.svg -o doc/readme/rpi3/images/$f.png; done
import os
import sys
from xml.sax.saxutils import escape as esc

FONT = "'PingFang SC','Hiragino Sans GB','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"

C = {  # fill, stroke
    "zero":   ("#fff3cd", "#b7950b"),
    "code":   ("#c6d8f7", "#1f5fae"),
    "bss":    ("#dbe8fb", "#1f5fae"),
    "kpt":    ("#ffe0b8", "#c86400"),
    "buddy":  ("#d9f2df", "#1e8449"),
    "carve":  ("#e3d7f5", "#6c3fb5"),
    "dtb":    ("#fff3cd", "#b7950b"),
    "memmap": ("#d6eef0", "#16808a"),
    "vc":     ("#ececec", "#8a8a8a"),
    "mmio":   ("#fde2e1", "#c0392b"),
    "hole":   ("#f6f6f6", "#b0b0b0"),
    "kstack": ("#f9e3d3", "#c0582b"),
    "user":   ("#fde9cf", "#d35400"),
    "ld":     ("#fbe3c4", "#b9770e"),
    "stack":  ("#fcd9c0", "#c0582b"),
    "guard":  ("#eeeeee", "#888888"),
    "mmap":   ("#f6e0f0", "#a93d8c"),
    "heap":   ("#fdf2e2", "#d35400"),
}


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.out = w, h, []
        o = self.out.append
        o(f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
          f'viewBox="0 0 {w} {h}" font-family="{FONT}" font-size="12">')
        o('<defs>')
        for name, col in (("ar", "#555"), ("arB", "#1f5fae"), ("arO", "#d35400"),
                          ("arR", "#c0392b"), ("arG", "#1e8449"), ("arP", "#6c3fb5")):
            o(f'<marker id="{name}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
              f'markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{col}"/></marker>')
        o('</defs>')
        o(f'<rect width="{w}" height="{h}" fill="#ffffff"/>')

    def o(self, s):
        self.out.append(s)

    def text(self, x, y, s, size=12, anchor="start", weight="normal", fill="#222", mono=False):
        fam = f' font-family="{MONO}"' if mono else ""
        self.o(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" '
               f'font-weight="{weight}" fill="{fill}"{fam}>{esc(s)}</text>')

    def box(self, x, y, w, h, kind, dash=False, rx=3):
        f, s = C[kind]
        d = ' stroke-dasharray="5,3"' if dash else ""
        self.o(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{f}" stroke="{s}" stroke-width="1.2"{d}/>')

    def line(self, x1, y1, x2, y2, col="#555", marker="ar", width=1.3, dash=False):
        d = ' stroke-dasharray="5,3"' if dash else ""
        m = f' marker-end="url(#{marker})"' if marker else ""
        self.o(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{col}" stroke-width="{width}"{d}{m}/>')

    def path(self, d, col="#555", marker="ar", width=1.3, dash=False, fill="none", opacity=1):
        da = ' stroke-dasharray="5,3"' if dash else ""
        m = f' marker-end="url(#{marker})"' if marker else ""
        self.o(f'<path d="{d}" fill="{fill}" stroke="{col}" stroke-width="{width}" '
               f'opacity="{opacity}"{da}{m}/>')

    def save(self, path):
        self.o('</svg>')
        with open(path, "w") as f:
            f.write("\n".join(self.out) + "\n")


# ---------------------------------------------------------------------------
# 1. memory-layout.svg: user half (TTBR0, the whole 64-bit VA space) on the
#    left, physical memory in the middle, kernel half (TTBR1) on the right;
#    each half goes VA -> L1 -> L2 -> L3 -> PA.  Below: the table pages
#    themselves, zoomed in, with who points to whom.

def memory_layout(path):
    TOP = 200
    CW = dict(va=330, l1=128, l2=150, l3=250)
    GAP = 8
    UX0 = 40                                   # user block, left to right
    U = {}
    x = UX0
    for k in ("va", "l1", "l2", "l3"):
        U[k] = x
        x += CW[k] + GAP
    PX, PW = x + 60, 330                       # physical column
    KX0 = PX + PW + 60                         # kernel block, mirrored
    K = {}
    x = KX0
    for k in ("l3", "l2", "l1", "va"):
        K[k] = x
        x += CW[k] + GAP
    W = x + 40

    def hx16(v):
        h = f"{v:016X}"
        return "0x" + "_".join(h[i:i + 4] for i in range(0, 16, 4))

    # ---- user half: the whole 64-bit VA space, high address first ---------
    urows = [
        ([hx16(0xFFFFFF8000000000) + " –", hx16(0xFFFFFFFFFFFFFFFF), "VA[63:39] 全 1 → TTBR1：内核半边，见右侧"], 54, "kstack", True),
        ([hx16(0x0000008000000000) + " –", hx16(0xFFFFFF7FFFFFFFFF), "非规范地址：VA[63:39] 不全 0 也不全 1",
          "两个 TTBR 都不查，访问即 Translation fault"], 66, "hole", True),
        ([hx16(0x0000004000000000) + " –", hx16(0x0000007FFFFFFFFF), "TTBR0 可达（T0SZ=25），xv6 不用（MAXUVA=256G）"], 54, "hole", True),
        ([hx16(0x0000003FF0000000) + " –", hx16(0x0000003FFFFFFFFF), "MMAP_TOP 以上 256 MiB：留空（旧摄像头窗口）"], 54, "hole", True),
        ([hx16(0x0000001000000000) + " –", hx16(0x0000003FEFFFFFFF), "mmap 窗口（64 GiB 起，自顶向下）：",
          "ld.so 映射 libc.so、/dev/video0、匿名 mmap"], 66, "mmap", False),
        ([hx16(0x0000000000009000) + " –", hx16(0x0000000FFFFFFFFF), "brk 堆：从 sz 向上，最多到最低的 VMA"], 54, "heap", True),
        ([hx16(0x0000000000005000) + " –", hx16(0x0000000000008FFF), "用户栈 4 页，sp = 0x8F50"], 50, "stack", False),
        ([hx16(0x0000000000004000), "保护页（PTE_U 清零）"], 36, "guard", False),
        ([hx16(0x0000000000002000) + " –", hx16(0x0000000000003FFF), "ld-xv6.so，入口 0x2784 = elr"], 50, "ld", False),
        ([hx16(0x0000000000000000) + " –", hx16(0x0000000000001FFF), "/bin/ls .text @0 + .data @0x1900"], 50, "user", False),
    ]
    uL1 = [(0, 0, ["TTBR1", "（右侧）"], "kstack", True),
           (1, 1, ["不查表"], "hole", True),
           (2, 2, ["[256..511]", "无效"], "hole", True),
           (3, 4, ["[64..255]", "→ L2", "（mmap 时建）"], "mmap", False),
           (5, 9, ["[0]", "→ L2", "0x3B621000", "", "（堆可延伸到", "[1..63]）"], "user", False)]
    uL2 = [(0, 2, ["—"], "hole", True),
           (3, 4, ["按需 → L3"], "mmap", False),
           (5, 5, ["[0] 同一张 L3", "其余按需"], "heap", True),
           (6, 9, ["[0] → L3", "0x3B624000", "", "（VA 0–2 MiB）"], "user", False)]
    uL3 = [(3, 4, ["libc 文本页 PTE_SHARED", "（多进程共用物理页）"], "mmap", False),
           (5, 5, ["[9..] 尚未映射", "（缺页时才分配）"], "heap", True),
           (6, 6, ["[5..8] → 0x3B639000–", "0x3B63C000"], "stack", False),
           (7, 7, ["[4] 0x…70B（无 PTE_U）"], "guard", False),
           (8, 8, ["[2] → 0x3CA000", "[3] → 0x3B637000"], "ld", False),
           (9, 9, ["[0] → 0x3B622000", "[1] → 0x3B623000"], "user", False)]

    # ---- kernel half (TTBR1) ----------------------------------------------
    krows = [
        ([hx16(0xFFFFFFC000000000) + " –", "MAXVA 以上：未用"], 40, "hole", True),
        (["KSTACK(slot) = MAXVA − (slot+1)·8K", "4K 内核栈 + 4K 保护页",
          "pid 19：" + hx16(0xFFFFFFBFFFFE0000)], 54, "kstack", False),
        ([hx16(0xFFFFFF8040001000) + " –", "未映射空洞"], 40, "hole", True),
        ([hx16(0xFFFFFF8040000000), "ARM local 4 KiB"], 40, "mmio", False),
        ([hx16(0xFFFFFF803F000000), "BCM2837 外设 16 MiB"], 40, "mmio", False),
        ([hx16(0xFFFFFF803C000000), "VideoCore：不映射"], 36, "hole", True),
        ([hx16(0xFFFFFF8000400000) + " –", "RAM 线性映射（除 carve-out）", "mem_map / 伙伴 / DTB"], 80, "buddy", False),
        ([hx16(0xFFFFFF8007000000), "carve-out：ramdisk/DMA/模块区"], 40, "carve", False),
        ([hx16(0xFFFFFF8000200000) + " –", "伙伴低端页 + 内核页表"], 40, "kpt", False),
        ([hx16(0xFFFFFF80000BD000), ".rodata/.data/.bss（RW, XN）"], 40, "bss", False),
        ([hx16(0xFFFFFF8000080000), ".text（RO, 可执行）"], 40, "code", False),
        ([hx16(0xFFFFFF8000000000), "第 0 页 spin-table"], 40, "zero", False),
    ]
    kL1 = [(0, 0, ["[256..511]", "超出 MAXVA"], "hole", True),
           (1, 1, ["[255]", "→ L2（运行时）"], "kstack", False),
           (2, 2, ["[2..254] 无效"], "hole", True),
           (3, 3, ["[1] → L2", "0x3F5000"], "kpt", False),
           (4, 11, ["[0]", "→ L2", "0x3FE000", "", "（KERNBASE", "+ 0–1 GiB）"], "kpt", False)]
    kL2 = [(0, 0, ["—"], "hole", True),
           (1, 1, ["[511] → L3", "（allocproc 时建）"], "kstack", False),
           (2, 2, ["—"], "hole", True),
           (3, 3, ["[0] → L3 0x3F4000"], "kpt", False),
           (4, 4, ["[504..511] → 8 张 L3"], "kpt", False),
           (5, 5, ["[480..503] 无效"], "hole", True),
           (6, 6, ["[2..55] [59..479]", "2 MiB 块描述符", "（低两位 01，无 L3）"], "buddy", False),
           (7, 7, ["[56..58] → 3 张 L3"], "kpt", False),
           (8, 8, ["[1] → L3 0x3EF000"], "kpt", False),
           (9, 11, ["[0] → L3 0x3F0000"], "kpt", False)]
    kL3 = [(1, 1, ["栈页（kalloc）", "槽间空项作保护页"], "kstack", False),
           (3, 3, ["L3 0x3F4000 [0] Device"], "mmio", False),
           (4, 4, ["L3 0x3FD000…0x3F6000", "4096 页 Device, XN"], "mmio", False),
           (6, 6, ["475 个 2 MiB 块 = 950 MiB", "Normal-WB, RW, XN"], "buddy", False),
           (7, 7, ["L3 0x3F3000/3F2000/3F1000", "WB · Normal-NC · RWX"], "carve", False),
           (8, 8, ["L3 0x3EF000 [0..511] RW XN"], "kpt", False),
           (9, 9, ["L3 0x3F0000 [189..511] RW XN"], "bss", False),
           (10, 10, ["L3 0x3F0000 [128..188] RO"], "code", False),
           (11, 11, ["L3 0x3F0000 [0] RW XN"], "zero", False)]

    prow_defs = [
        ("local", 34, "mmio", ["0x40000000 – 0x40000FFF  4 KiB", "ARM local"], "BCM2836/7 ARM-local MMIO"),
        ("periph", 44, "mmio", ["0x3F000000 – 0x3FFFFFFF  16 MiB", "BCM2837 外设（UART 0x3F215000…）"], "BCM2837 peripheral MMIO window"),
        ("vc", 38, "vc", ["0x3C000000 – 0x3EFFFFFF  48 MiB", "VideoCore（DTB 不报为 RAM）"], "Reserved (VideoCore)"),
        ("memmap", 38, "memmap", ["0x3B6A0000 – 0x3BFFFFFF  9600 KiB", "mem_map：245760 × struct page"], "mem_map (struct page[])"),
        ("buddy3", 190, "buddy", ["0x08016000 – 0x3B69FFFF  ≈822 MiB", "伙伴系统（最大一段）",
                                  "用户页、用户页表页、内核栈页", "大多来自这里（0x3B6xxxxx）"], "MemTotal 965372 kB"),
        ("dtb", 38, "dtb", ["0x08000000 – 0x08015D95  87 KiB", "设备树 DTB"], "Flattened Device Tree"),
        ("buddy2", 34, "buddy", ["0x07600000 – 0x07FFFFFF  10 MiB", "伙伴系统"], None),
        ("carve", 50, "carve", ["0x07000000 – 0x075FFFFF  6 MiB", "ramdisk · 摄像头 DMA · 模块区"], "Reserved (carve-out window)"),
        ("buddy1", 60, "buddy", ["0x00400000 – 0x06FFFFFF  108 MiB", "伙伴系统"], None),
        ("kpt", 44, "kpt", ["0x003EF000 – 0x003FFFFF  68 KiB", "内核页表 17 页（见下方放大 ②）"], "Kernel page tables"),
        ("buddy0", 40, "buddy", ["0x001BA000 – 0x003EEFFF  2260 KiB", "伙伴低端（ld.so .text 0x3CA000）"], None),
        ("bss", 44, "bss", ["0x000BD000 – 0x001B9C2F  1012 KiB", ".rodata/.data/.bss（启动表 0xC9000–）"], "Kernel data/bss"),
        ("text", 38, "code", ["0x00080000 – 0x000BCFFF  244 KiB", ".text（_entry 在最前）"], "Kernel code"),
        ("zero", 38, "zero", ["0x00000000 – 0x0007FFFF  512 KiB", "spin-table / armstub"], "Reserved (spin-table)"),
    ]

    def layout(rows):
        pos, y = [], TOP
        for r in rows:
            pos.append((y, r[1]))
            y += r[1]
        return pos, y

    upos, ubot = layout(urows)
    kpos, kbot = layout(krows)
    VBOTTOM = max(ubot, kbot, TOP + sum(d[1] for d in prow_defs) * 1.12)
    # stretch the shorter half to the same height
    def stretch(pos, bot):
        f = (VBOTTOM - TOP) / (bot - TOP)
        return [(TOP + (y - TOP) * f, h * f) for y, h in pos]
    upos, kpos = stretch(upos, ubot), stretch(kpos, kbot)
    total = sum(d[1] for d in prow_defs)
    P, py = {}, TOP
    for key, h, kind, lines, note in prow_defs:
        hh = h * (VBOTTOM - TOP) / total
        P[key] = dict(y=py, h=hh, kind=kind, lines=lines, note=note)
        py += hh

    ZTOP = VBOTTOM + 110
    H = ZTOP + 470
    s = Svg(W, H)
    s.text(W / 2, 36, "xv6 rpi3-lock（Raspberry Pi 3 / BCM2837, AArch64）内存布局：用户半边（TTBR0）← 物理地址 → 内核半边（TTBR1）",
           size=21, anchor="middle", weight="600")
    s.text(W / 2, 60, "QEMU raspi3b，DTB 报告 RAM 0x0–0x3BFFFFFF（960 MiB）；数值取自 make MMU_TRACE=3 启动日志和 /proc/iomem、/proc/meminfo。"
           "各列内高地址在上，不按比例。", size=13, anchor="middle", fill="#555")

    def colhead(x, w, t, sub, y, col="#222"):
        s.text(x + w / 2, y, t, size=13.5, anchor="middle", weight="600", fill=col)
        if sub:
            s.text(x + w / 2, y + 16, sub, size=11, anchor="middle", fill="#666")

    def frame(x0, x1, fill, stroke, title, col):
        s.o(f'<rect x="{x0 - 12}" y="{TOP - 108}" width="{x1 - x0 + 24}" height="{VBOTTOM - TOP + 120}" '
            f'rx="8" fill="{fill}" stroke="{stroke}" stroke-width="1"/>')
        s.text(x0, TOP - 84, title, size=15, weight="600", fill=col)

    def draw_rows(X, rows, pos):
        for (lines, h, kind, dash), (y0, hh) in zip(rows, pos):
            s.box(X, y0, CW["va"], hh, kind, dash=dash)
            n = len(lines)
            for i, t in enumerate(lines):
                s.text(X + 8, y0 + hh / 2 - (n - 1) * 6.75 + i * 13.5 + 4, t,
                       size=11.5 if t.startswith("0x") else 11, mono=t.startswith("0x"),
                       weight="600" if t.startswith("0x") else "normal")

    def draw_cells(x, w, cells, pos):
        for a, b, lines, kind, dash in cells:
            y0 = pos[a][0]
            y1 = pos[b][0] + pos[b][1]
            s.box(x + 3, y0 + 2, w - 6, y1 - y0 - 4, kind, dash=dash)
            n = len(lines)
            for i, t in enumerate(lines):
                yy = y0 + (y1 - y0) / 2 - (n - 1) * 6.5 + i * 13 + 4
                s.text(x + w / 2, yy, t, size=11, anchor="middle", mono=t.startswith("[") or t.startswith("0x"))

    def chain(cols, pos, rows_with_lines):
        xs = sorted(cols)
        for r in rows_with_lines:
            yy = pos[r][0] + pos[r][1] / 2
            for a, b in zip(xs[:-1], xs[1:]):
                s.line(a[1], yy, b[0], yy, col="#999", marker=None, width=0.8)

    # user block
    frame(U["va"], U["l3"] + CW["l3"], "#f3f7ff", "#b9cbe8",
          "用户半边：TTBR0_EL1 = 当前进程（例 pid 19 exec /bin/ls 后：L1 根 0x3B60A000）", "#1f5fae")
    colhead(U["va"], CW["va"], "64 位虚拟地址（全部）", "低半边 0 – 0x7F_FFFFFFFF 归 TTBR0", TOP - 40, "#1f5fae")
    colhead(U["l1"], CW["l1"], "L1 0x3B60A000", "VA[38:30]", TOP - 40, "#1f5fae")
    colhead(U["l2"], CW["l2"], "L2 0x3B621000", "VA[29:21]", TOP - 40, "#1f5fae")
    colhead(U["l3"], CW["l3"], "L3 0x3B624000", "VA[20:12]", TOP - 40, "#1f5fae")
    chain([(U["va"], U["va"] + CW["va"]), (U["l1"] + 3, U["l1"] + CW["l1"] - 3),
           (U["l2"] + 3, U["l2"] + CW["l2"] - 3), (U["l3"] + 3, U["l3"] + CW["l3"] - 3)],
          upos, [4, 6, 7, 8, 9])
    draw_rows(U["va"], urows, upos)
    draw_cells(U["l1"], CW["l1"], uL1, upos)
    draw_cells(U["l2"], CW["l2"], uL2, upos)
    draw_cells(U["l3"], CW["l3"], uL3, upos)

    # kernel block (mirrored: L3 nearest the physical column)
    frame(K["l3"], K["va"] + CW["va"], "#fff8ef", "#e7c9a3",
          "内核半边：TTBR1_EL1 = 0x3FF000（全系统共用一张；17 个表页在 PA 0x3EF000–0x3FFFFF）", "#c86400")
    colhead(K["l3"], CW["l3"], "L3 表 / 2 MiB 块", "VA[20:12]", TOP - 40, "#c86400")
    colhead(K["l2"], CW["l2"], "L2", "VA[29:21]", TOP - 40, "#c86400")
    colhead(K["l1"], CW["l1"], "L1 0x3FF000", "VA[38:30]", TOP - 40, "#c86400")
    colhead(K["va"], CW["va"], "内核虚拟地址", "KERNBASE 0xFFFFFF80_00000000 + PA", TOP - 40, "#c86400")
    chain([(K["l3"] + 3, K["l3"] + CW["l3"] - 3), (K["l2"] + 3, K["l2"] + CW["l2"] - 3),
           (K["l1"] + 3, K["l1"] + CW["l1"] - 3), (K["va"], K["va"] + CW["va"])],
          kpos, [1, 3, 4, 6, 7, 8, 9, 10, 11])
    draw_rows(K["va"], krows, kpos)
    draw_cells(K["l1"], CW["l1"], kL1, kpos)
    draw_cells(K["l2"], CW["l2"], kL2, kpos)
    draw_cells(K["l3"], CW["l3"], kL3, kpos)

    # physical column
    colhead(PX, PW, "物理地址", "DTB /memory + 外设；IPS = 32 位", TOP - 40)
    for key, r in P.items():
        s.box(PX, r["y"], PW, r["h"], r["kind"], dash=(key == "vc"))
        lines = list(r["lines"])
        if r["note"] and r["h"] >= 14 * (len(lines) + 1) + 6:
            lines.append(("iomem: " if not r["note"].startswith("MemTotal") else "") + r["note"])
        for i, t in enumerate(lines):
            note = i == len(lines) - 1 and r["note"] and len(lines) > len(r["lines"])
            s.text(PX + 8, r["y"] + 14 + i * 13.5, t, size=10.5 if note else (11.5 if i == 0 else 11),
                   weight="600" if i == 0 else "normal", mono=(i == 0), fill="#666" if note else "#222")

    def pm(key, frac=0.5):
        return P[key]["y"] + P[key]["h"] * frac

    def ulink(row, key, frac=0.5, yoff=0):
        y0 = upos[row][0] + upos[row][1] / 2 + yoff
        x0, x1 = U["l3"] + CW["l3"] - 3, PX - 4
        y1 = pm(key, frac)
        s.path(f"M {x0} {y0} C {x0 + 40} {y0}, {x1 - 40} {y1}, {x1} {y1}", col="#d35400", marker="arO", width=1.3)

    def klink(row, key, col, mk, frac=0.5, yoff=0, width=1.3, dash=False):
        y0 = kpos[row][0] + kpos[row][1] / 2 + yoff
        x0, x1 = K["l3"] + 3, PX + PW + 4
        y1 = pm(key, frac)
        s.path(f"M {x0} {y0} C {x0 - 40} {y0}, {x1 + 40} {y1}, {x1} {y1}", col=col, marker=mk, width=width, dash=dash)

    ulink(3, "buddy3", 0.35)
    ulink(6, "buddy3", 0.62)
    ulink(8, "buddy3", 0.68, -5)
    ulink(8, "buddy0", 0.55, 5)
    ulink(9, "buddy3", 0.74)
    R, G, B, Pp, O = ("#c0392b", "arR"), ("#1e8449", "arG"), ("#1f5fae", "arB"), ("#6c3fb5", "arP"), ("#c0582b", "arO")
    klink(1, "buddy3", *O, frac=0.85, dash=True)
    klink(3, "local", *R)
    klink(4, "periph", *R)
    for k, f in (("memmap", .5), ("buddy3", .2), ("dtb", .5), ("buddy2", .5), ("buddy1", .5)):
        klink(6, k, *G, frac=f, width=1.0)
    klink(7, "carve", *Pp)
    klink(8, "buddy0", *B, yoff=-4)
    klink(8, "kpt", *B, yoff=4)
    klink(9, "bss", *B)
    klink(10, "text", *B)
    klink(11, "zero", *B)

    # ---- zoom panels under each half: the table pages and who points to whom
    ZW, ZH = 360, 21

    def zstack(ZX, ytop, items, title, col):
        s.text(ZX, ytop - 12, title, size=13.5, weight="600", fill=col)
        pos, y = {}, ytop
        for addr, label, kind, table in items:
            if addr is None:
                s.text(ZX + ZW / 2, y + 13, label, size=11, anchor="middle", fill="#888")
                y += ZH
                continue
            f, st = C[kind]
            sw = 2.2 if table else 1
            s.o(f'<rect x="{ZX}" y="{y}" width="{ZW}" height="{ZH - 3}" rx="2" fill="{f}" stroke="{st}" stroke-width="{sw}"/>')
            s.text(ZX + 6, y + 13.5, f"{addr:08x}", size=11, mono=True, weight="600" if table else "normal")
            s.text(ZX + 76, y + 13.5, label, size=11, weight="600" if table else "normal")
            pos[addr] = y + (ZH - 3) / 2
            y += ZH
        return pos, y

    def ptr(ZX, pos, a, b, idx, bulge, col, mk, width=1.2):
        y0, y1 = pos[a], pos[b]
        x0 = ZX + ZW
        xb = x0 + bulge
        s.path(f"M {x0} {y0} C {xb} {y0}, {xb} {y1}, {x0 + 2} {y1}", col=col, marker=mk, width=width)
        s.text(ZX + ZW - 5, y1 + 4, "← " + idx, size=10.5, anchor="end", fill=col, mono=True, weight="600")

    uitems = [
        (0x3B63C000, "栈 ×4 → VA 5000–8FFF", "stack", False),
        (0x3B638000, "保护页 → VA 0x4000", "guard", False),
        (0x3B637000, "ld.so .data → VA 0x3000", "ld", False),
        (0x3B624000, "L3 表（VA 0–2 MiB）", "kpt", True),
        (0x3B623000, "ls .data → VA 0x1000", "user", False),
        (0x3B622000, "ls .text → VA 0x0", "user", False),
        (0x3B621000, "L2 表（VA 0–1 GiB）", "kpt", True),
        (0x3B60A000, "L1 根表（TTBR0）", "kpt", True),
        (None, "≈ 低端伙伴段 0x001BA000–0x003EEFFF ≈", None, False),
        (0x003CA000, "ld.so .text → VA 0x2000", "ld", False),
    ]
    UZX = U["l2"]
    upz, ubz = zstack(UZX, ZTOP, uitems, "放大 ①：用户页表与数据页（伙伴系统，pid 19）", "#1f5fae")
    s.path(f"M {PX} {pm('buddy3', 0.9)} C {PX - 200} {pm('buddy3', 0.9) + 40}, {UZX + ZW + 100} {ZTOP - 120}, {UZX + ZW / 2} {ZTOP - 30}",
           col="#7a9cc6", marker="arB", width=1.0, dash=True)
    Bc, Oc = ("#1f5fae", "arB"), ("#d35400", "arO")
    ptr(UZX, upz, 0x3B60A000, 0x3B621000, "L1[0]", 46, *Bc, width=1.8)
    ptr(UZX, upz, 0x3B621000, 0x3B624000, "L2[0]", 70, *Bc, width=1.8)
    ptr(UZX, upz, 0x3B624000, 0x3B622000, "[0]", 30, *Oc)
    ptr(UZX, upz, 0x3B624000, 0x3B623000, "[1]", 20, *Oc)
    ptr(UZX, upz, 0x3B624000, 0x003CA000, "[2]", 120, *Oc)
    ptr(UZX, upz, 0x3B624000, 0x3B637000, "[3]", 22, *Oc)
    ptr(UZX, upz, 0x3B624000, 0x3B638000, "[4]", 40, *Oc)
    ptr(UZX, upz, 0x3B624000, 0x3B63C000, "[5..8]", 60, *Oc)
    s.text(UZX, ubz + 10, "粗线框 = 页表页，细线框 = 数据页：两者在物理上混在一起，", size=10.5, fill="#555")
    s.text(UZX, ubz + 24, "都是 kalloc 按需取的任意空闲页。TTBR0 指向最下面的 L1。", size=10.5, fill="#555")

    kitems = [(0x3FF000, "L1 根表（TTBR1）", "kpt", True),
              (0x3FE000, "L2 表（0–1 GiB）", "kpt", True)]
    for k in range(8):
        kitems.append((0x3FD000 - k * 0x1000, f"L3 外设 {0x3F000000 + k * 0x200000:08X}", "mmio", True))
    kitems += [(0x3F5000, "L2 表（1–2 GiB）", "kpt", True),
               (0x3F4000, "L3 ARM local", "mmio", True),
               (0x3F3000, "L3 ramdisk", "carve", True),
               (0x3F2000, "L3 摄像头 DMA", "carve", True),
               (0x3F1000, "L3 模块区", "carve", True),
               (0x3F0000, "L3 PA 0–2 MiB", "bss", True),
               (0x3EF000, "L3 PA 2–4 MiB", "bss", True)]
    KZX = K["l2"]
    kpz, kbz = zstack(KZX, ZTOP, kitems, "放大 ②：内核页表 17 页（memblock，PA 0x3EF000–0x3FFFFF）", "#c86400")
    s.path(f"M {PX + PW} {pm('kpt')} C {PX + PW + 160} {pm('kpt') + 40}, {KZX - 80} {ZTOP - 120}, {KZX + ZW / 2} {ZTOP - 30}",
           col="#e0a060", marker="arO", width=1.0, dash=True)
    Kc, Rc = ("#c86400", "arO"), ("#c0392b", "arR")
    ptr(KZX, kpz, 0x3FF000, 0x3FE000, "L1[0]", 30, *Kc, width=1.8)
    ptr(KZX, kpz, 0x3FF000, 0x3F5000, "L1[1]", 150, *Kc, width=1.8)
    ptr(KZX, kpz, 0x3FE000, 0x3FD000, "[504..511]", 50, *Rc)
    s.o(f'<path d="M {KZX + ZW + 4} {kpz[0x3FD000] - 8} h 6 V {kpz[0x3F6000] + 8} h -6" fill="none" stroke="#c0392b" stroke-width="1.2"/>')
    ptr(KZX, kpz, 0x3FE000, 0x3F3000, "[56]", 100, *Kc)
    ptr(KZX, kpz, 0x3FE000, 0x3F2000, "[57]", 115, *Kc)
    ptr(KZX, kpz, 0x3FE000, 0x3F1000, "[58]", 130, *Kc)
    ptr(KZX, kpz, 0x3FE000, 0x3F0000, "[0]", 175, *Kc)
    ptr(KZX, kpz, 0x3FE000, 0x3EF000, "[1]", 200, *Kc)
    ptr(KZX, kpz, 0x3F5000, 0x3F4000, "L2[0]", 26, *Kc)
    s.text(KZX - 260, ZTOP + 20, "L2 0x3FE000 的 [2..55]、[59..479]", size=10.5, fill="#1e8449")
    s.text(KZX - 260, ZTOP + 34, "是 2 MiB 块，直接指向 RAM，没有 L3", size=10.5, fill="#1e8449")
    s.text(KZX - 260, ZTOP + 54, "KSTACK 区 L1[255] 的 L2/L3 运行时", size=10.5, fill="#555")
    s.text(KZX - 260, ZTOP + 68, "从伙伴系统取，不在这 17 页里", size=10.5, fill="#555")

    # notes under the user zoom
    ly = max(ubz + 60, ZTOP + 260)
    s.text(UX0, ly, "要点", size=14, weight="600")
    notes = [
        "1. 64 位 VA 只有两头有效：VA[63:39] 全 0 走 TTBR0（左，当前进程），全 1 走 TTBR1（右，内核）；",
        "    中间 0x0000_0080_… – 0xFFFF_FF7F_… 是非规范空洞。切换进程只换 TTBR0（switchuvm）。",
        "2. 两半都是 L1 → L2 → L3 三级（4 KiB 粒度、39 位 VA、无 L0）；内核线性映射在 L2 就用 2 MiB 块结束。",
        "3. 内核 17 个表页来自 memblock（4 MiB 以下自顶向下，永不释放）；用户 3 个表页和 9 个数据页来自伙伴系统。",
        "4. 用户 PTE = 0x…74B：Normal-WB、AP=01（EL0 RW）、AF、ISH，没有 UXN/PXN；fork 时可写页改成 RO + COW。",
    ]
    for i, t in enumerate(notes):
        s.text(UX0, ly + 24 + i * 20, t, size=12.5)
    s.save(path)

# ---------------------------------------------------------------------------
# 2. mmu-page-tables.svg: VA split, bootstrap tables, final TTBR1 tree,
#    a process TTBR0 tree and the descriptor format.

def page_tables(path):
    W, H = 1640, 1560
    s = Svg(W, H)
    s.text(W / 2, 36, "rpi3-lock 的 MMU：39 位 VA、4 KiB 粒度、三级页表（L1 → L2 → L3），启动两阶段与进程页表",
           size=21, anchor="middle", weight="600")
    s.text(W / 2, 60, "TCR_EL1 = 0xB5193519：T0SZ = T1SZ = 25，TG0 = TG1 = 4K，IRGN/ORGN = WB-WA，SH = Inner；MAIR_EL1 = 0xFF4400",
           size=13, anchor="middle", fill="#555")

    # --- VA bit split -------------------------------------------------------
    y = 96
    s.text(40, y, "① 虚拟地址拆分（TTBR1 用于 VA[63:39] 全 1，TTBR0 用于全 0）", size=15, weight="600")
    fields = [("63..39", "全 1 → TTBR1\n全 0 → TTBR0", 170, "hole"), ("38..30", "L1 索引 (9)", 150, "code"),
              ("29..21", "L2 索引 (9)", 150, "buddy"), ("20..12", "L3 索引 (9)", 150, "kpt"),
              ("11..0", "页内偏移 (12)", 150, "user")]
    x = 60
    for bits, lab, w, kind in fields:
        s.box(x, y + 14, w, 52, kind)
        s.text(x + w / 2, y + 34, bits, size=13, anchor="middle", weight="600", mono=True)
        for i, l in enumerate(lab.split("\n")):
            s.text(x + w / 2, y + 50 + i * 13, l, size=11.5, anchor="middle")
        x += w
    s.text(x + 20, y + 32, "L1 项管 1 GiB，L2 项管 2 MiB（可直接是块），L3 项管 4 KiB 页。", size=12.5)
    s.text(x + 20, y + 50, "没有 L0：T0SZ=25 ⇒ 2^39 = 512 GiB，每半边只需一张 512 项的 L1 表。", size=12.5)

    # --- bootstrap tables ---------------------------------------------------
    y = 200
    s.text(40, y, "② 阶段 1：entry.S（MMU 关）→ entrypgtinit() 在 .bss 里填 4 张静态表", size=15, weight="600")
    by = y + 20
    def tbl(x, yy, w, title, entries, kind):
        s.box(x, yy, w, 22 + 16 * len(entries), kind)
        s.text(x + 8, yy + 15, title, size=12, weight="600")
        for i, e in enumerate(entries):
            s.text(x + 8, yy + 33 + i * 16, e, size=11, mono=True)
        return 22 + 16 * len(entries)
    s.text(60, by + 14, "TTBR0 =", size=12, weight="600")
    tbl(130, by, 210, "l1entrypgt @ 0xCC000", ["[0] = 0xCB003 → L2"], "bss")
    tbl(400, by, 330, "l2entrypgt @ 0xCB000（恒等映射）", ["[0..1] 块 PA 0–0x3FFFFF  Normal-WB"], "bss")
    s.line(340, by + 33, 398, by + 33, col="#1f5fae", marker="arB")
    s.text(60, by + 84, "TTBR1 =", size=12, weight="600")
    tbl(130, by + 70, 210, "l1kpgt @ 0xCA000", ["[0] = 0xC9003 → L2"], "bss")
    tbl(400, by + 70, 330, "l2kpgt @ 0xC9000（高地址）", ["[0..1]   块 → PA 0–0x3FFFFF",
                                                     "[64..65] 块 → DTB 0x8000000",
                                                     "[505]    块 → 0x3F200000 Device (UART)"], "bss")
    s.line(340, by + 103, 398, by + 103, col="#1f5fae", marker="arB")
    s.text(760, by + 16, "然后：写 TTBR0/TTBR1、TCR、MAIR → tlbi vmalle1 → SCTLR |= M|C|I", size=12.5)
    s.text(760, by + 36, "→ br _start（VA 0xFFFFFF80000xxxxx），PC 从恒等映射跳到高地址。", size=12.5)
    s.text(760, by + 56, "恒等映射只为 “开 MMU 的那条指令之后、跳转之前” 还能取指。", size=12.5)
    s.text(760, by + 84, "阶段 2：main() → memblock_init(DTB) → kvminit() 用 memblock 页建正式表", size=12.5, weight="600")
    s.text(760, by + 104, "→ kvminithart()：TTBR1 = 0x3FF000，TTBR0 = 0，flush_tlb()；4 张启动表从此作废。", size=12.5)
    s.text(760, by + 124, "→ page_alloc_init()：mem_map + memblock_free_all() 交给伙伴系统。", size=12.5)

    # --- final kernel table -------------------------------------------------
    y = 400
    s.text(40, y, "③ 正式内核页表（TTBR1_EL1 = 0x3FF000），共 17 个表页 = 68 KiB", size=15, weight="600")
    ty = y + 20
    tbl(40, ty + 60, 200, "L1 根 @ 0x3FF000", ["[0]   → L2 0x3FE000", "[1]   → L2 0x3F5000",
                                               "[255] → L2（KSTACK，", "       allocproc 时建）"], "kpt")
    # L2 0x3FE000
    l2 = ["[0]       → L3 0x3F0000", "[1]       → L3 0x3EF000", "[2..55]   2 MiB 块  0x400000–",
          "[56..58]  → L3 0x3F3000/2000/1000", "[59..479] 2 MiB 块  0x7600000–", "[504..511]→ 8 张 L3 0x3FD000–0x3F6000"]
    tbl(300, ty, 300, "L2 @ 0x3FE000（VA 0–1 GiB）", l2, "kpt")
    s.path(f"M 240 {ty + 97} C 270 {ty + 97}, 270 {ty + 30}, 298 {ty + 30}", col="#c86400", marker="arO")
    tbl(300, ty + 140, 300, "L2 @ 0x3F5000（VA 1–2 GiB）", ["[0] → L3 0x3F4000"], "kpt")
    s.path(f"M 240 {ty + 113} C 270 {ty + 113}, 270 {ty + 156}, 298 {ty + 156}", col="#c86400", marker="arO")
    # leaves
    leaves = [
        (ty - 4, "L3 0x3F0000：[0] 第 0 页 RW XN；[128..188] .text RO 可执行；[189..511] 数据 RW XN", "code"),
        (ty + 24, "L3 0x3EF000：[0..511] 0x200000–0x3FFFFF 4 KiB 页（本可用一个 2 MiB 块）", "bss"),
        (ty + 52, "2 MiB 块：线性映射 RAM，Normal-WB RW XN（共 475 块 = 950 MiB）", "buddy"),
        (ty + 80, "L3 ×3：ramdisk(WB) / 摄像头 DMA(Normal-NC) / 模块区(RWX)", "carve"),
        (ty + 108, "L3 ×8：0x3F000000–0x3FFFFFFF 4096 个 Device-nGnRnE 页", "mmio"),
        (ty + 156, "L3 0x3F4000：[0] 0x40000000 ARM local，Device", "mmio"),
    ]
    for yy, t, kind in leaves:
        s.box(680, yy, 900, 24, kind)
        s.text(690, yy + 16, t, size=12)
    for i, src in enumerate([ty + 30, ty + 46, ty + 62, ty + 78, ty + 94, ty + 110]):
        tgt = [ty + 8, ty + 36, ty + 64, ty + 92, ty + 64, ty + 120][i]
        if i == 4:
            continue
        s.line(600, src, 678, tgt, col="#888", marker="ar", width=1)
    s.line(600, ty + 172, 678, ty + 168, col="#888", marker="ar", width=1)

    # --- process TTBR0 -----------------------------------------------------
    y = 640
    s.text(40, y, "④ exec /bin/ls（pid 19）建的 TTBR0：3 个表页 + 9 个用户页（MMU_TRACE=3 实测）", size=15, weight="600")
    py = y + 20
    tbl(40, py + 20, 230, "L1 根 kalloc 0x3B60A000", ["[0] = 0x3B621003 → L2", "其余 511 项无效"], "user")
    tbl(330, py + 20, 230, "L2 kalloc 0x3B621000", ["[0] = 0x3B624003 → L3", "（VA 0–0x1FFFFF）"], "user")
    s.line(270, py + 49, 328, py + 49, col="#d35400", marker="arO")
    l3 = ["[0] 0x3B62274B → pa 0x3B622000  ls .text",
          "[1] 0x3B62374B → pa 0x3B623000  ls .data/.bss",
          "[2] 0x3CA74B   → pa 0x3CA000    ld.so .text",
          "[3] 0x3B63774B → pa 0x3B637000  ld.so .data",
          "[4] 0x3B63870B → 保护页（PTE_U=0）",
          "[5..8] → 0x3B639000–0x3B63C000  用户栈",
          ]
    tbl(620, py, 420, "L3 kalloc 0x3B624000", l3, "user")
    s.line(560, py + 49, 618, py + 40, col="#d35400", marker="arO")
    s.text(1080, py + 16, "顺序（exec.c）：", size=12.5, weight="600")
    steps = ["uvmcreate() → 空 L1 根",
             "loadimage(): uvmalloc() 逐页 kalloc+清零，",
             "  第一次 mappages() 时 walk() 建 L2、L3",
             "  loadseg(): uva2ka() 找页，readi 按页拷文件",
             "PT_INTERP → 在 PGROUNDUP(sz) 再装 ld.so",
             "uvmalloc 5 页，uvmclear 第一页成保护页",
             "copyout argv/auxv，elr = ld.so 入口",
             "提交：vm->pagetable = 新表，switchuvm()",
             "  写 TTBR0 + tlbi，再 uvmfree(旧表)"]
    for i, t in enumerate(steps):
        s.text(1080, py + 36 + i * 17, t, size=12)
    s.text(40, py + 140, "fork（sh pid 16 → pid 19）：allocproc 建内核栈（映射进 TTBR1 的 KSTACK）和空 L1 根，uvmcopy 逐页：", size=12.5)
    s.text(40, py + 158, "子表 walk() 建 L2/L3，PTE 指向父进程同一物理页，get_page() 使 refcount=2，父子 PTE 都改成 RO + PTE_COW；保护页无 PTE_U 原样共享。",
           size=12.5)
    s.text(40, py + 176, "随后 exec 丢弃这张 COW 表（uvmfree），只 put_page 不复制 —— 所以 fork+exec 只为子进程真正分配 3+9 页。", size=12.5)

    # --- descriptor format --------------------------------------------------
    y = 880
    s.text(40, y, "⑤ 描述符格式（以用户页 0x3B62274B 为例）", size=15, weight="600")
    bits = [("63..58", "软件", "hole", 90), ("57", "COW", "kstack", 50), ("56", "SPECIAL", "hole", 66),
            ("55", "SHARED", "hole", 62), ("54", "UXN", "mmio", 50), ("53", "PXN", "mmio", 50),
            ("47..12", "输出地址 PA[47:12]\n= 0x3B622", "buddy", 200), ("11", "nG", "hole", 40),
            ("10", "AF=1", "code", 50), ("9..8", "SH=11\nInner", "code", 60), ("7..6", "AP=01\nEL0 RW", "user", 70),
            ("5", "NS", "hole", 40), ("4..2", "AttrIndx\n=2 WB", "memmap", 80), ("1..0", "11\n页/表", "kpt", 60)]
    x = 40
    for b, lab, kind, w in bits:
        s.box(x, y + 14, w, 58, kind)
        s.text(x + w / 2, y + 30, b, size=11.5, anchor="middle", weight="600", mono=True)
        for i, l in enumerate(lab.split("\n")):
            s.text(x + w / 2, y + 46 + i * 13, l, size=11, anchor="middle")
        x += w
    expl = [
        "L1/L2 描述符低两位：11 = 指向下一级表（表描述符 0x…003），01 = 块（L2 的 2 MiB 块）；L3 低两位必须是 11 才是页。",
        "AP[2:1]：00 EL1 RW，01 EL0+EL1 RW（PTE_U），10 EL1 RO（PTE_RO），11 EL0+EL1 RO。fork 后 COW 页 = AP 11 + bit57。",
        "AttrIndx 选 MAIR 字节：0 = 0x00 Device-nGnRnE（外设），1 = 0x44 Normal 不可缓存（摄像头 DMA），2 = 0xFF Normal 写回（RAM）。",
        "内核数据都设 UXN|PXN（XN）；.text 只读不设 XN；模块区既可写又可执行。用户页两个 XN 都没设：EL0 和 EL1 都能执行用户页。",
        "bit55-57 是 ARMv8 留给软件的位：PTE_SHARED（共享 libc 文本）、PTE_SPECIAL（VM_PFNMAP 设备页）、PTE_COW（写时复制）。",
    ]
    for i, t in enumerate(expl):
        s.text(40, y + 100 + i * 20, t, size=12.5)
    s.text(40, y + 220, "⑥ 一次地址翻译：CPU 取 TTBRn → L1[VA[38:30]] → L2[VA[29:21]]（块则结束）→ L3[VA[20:12]] → PA = 描述符[47:12] | VA[11:0]。"
           "TLB 未命中时硬件按 TCR 的 IRGN/ORGN 用可缓存访问遍历这些表。", size=12.5)
    s.text(40, y + 240, "    例：exec 返回 EL0 后先执行 ld.so 的入口 VA 0x2784：L1[0] → L2[0] → L3[2] = 0x3CA74B → PA 0x3CA784。", size=12.5)

    # --- comparison with tiny210 -------------------------------------------
    y = 1160
    s.text(40, y, "⑦ 与 tiny210（ARMv7）对比：内核根表放在哪、映射几套、进程表里有什么", size=15, weight="600")
    s.o(f'<line x1="820" y1="{y + 20}" x2="820" y2="{y + 420}" stroke="#bbb" stroke-dasharray="4,4"/>')
    # left: tiny210
    lx, ly = 40, y + 30
    s.text(lx, ly + 4, "tiny210：短描述符，只用 TTBR0（TTBCR=0），VA = PA", size=13.5, weight="600", fill="#1f5fae")
    s.text(lx, ly + 26, "开 MMU：kvminithart() 写 TTBR0 后直接置 SCTLR.M；内核链接在物理地址 0x20008000，只有一套恒等映射。", size=11.5)
    tbl(lx, ly + 44, 330, "内核 L1 = roots[0]（.bss 静态池）", ["16 KiB 对齐，4096 项 × 4 B", "1 MiB 段：DRAM 恒等 + 外设 MMIO",
                                                          "池共 256 个根，占 bss 4 MiB"], "bss")
    tbl(lx + 400, ly + 44, 340, "进程 A 的 L1 = roots[5]", ["memmove(kernel L1, 16 KiB) 整张复制",
                                                         "+ 用户 L1[0..] → L2 粗页表", "  L2 = 256 项 × 4 B = 1 KiB，占 4 KiB 页"], "user")
    tbl(lx + 400, ly + 140, 340, "进程 B 的 L1 = roots[6]", ["又一份内核映射副本 + 自己的 L2"], "user")
    s.line(lx + 330, ly + 80, lx + 398, ly + 80, col="#1f5fae", marker="arB", dash=True)
    s.text(lx + 335, ly + 72, "复制", size=11, fill="#1f5fae")
    s.text(lx, ly + 170, "TTBR0 =", size=12.5, weight="600")
    s.line(lx + 60, ly + 165, lx + 398, ly + 160, col="#d35400", marker="arO")
    s.text(lx + 120, ly + 158, "switchuvm()：换整张表", size=11, fill="#d35400")
    tnotes = ["• 内核映射存在于每个进程的根里，切换进程 = 换掉整个地址空间。",
              "• 两级：L1（1 MiB）→ L2（4 KiB 页）；L2 只用 1 KiB，浪费 3 KiB。",
              "• fork：uvmcopy() 每页 kalloc + memmove，真复制。",
              "• 根表数受静态池限制（256 个地址空间）。"]
    for i, t in enumerate(tnotes):
        s.text(lx, ly + 214 + i * 20, t, size=12)
    # right: rpi3
    rx, ry = 850, y + 30
    s.text(rx, ry + 4, "rpi3-lock：4 KiB 粒度三级表，TTBR1 = 内核，TTBR0 = 用户", size=13.5, weight="600", fill="#c86400")
    s.text(rx, ry + 26, "内核链接在 0xFFFFFF8000080000，装在 PA 0x80000：恒等映射只活到 br _start，此后只用高地址映射。", size=11.5)
    tbl(rx, ry + 44, 330, "阶段 1：.bss 里 4 张启动表", ["TTBR0 l1entrypgt：VA 0–4M = PA（临时）", "TTBR1 l1kpgt：KERNBASE+PA（高地址）",
                                                    "kvminithart() 后作废"], "bss")
    tbl(rx, ry + 140, 330, "阶段 2：内核 L1 0x3FF000（memblock）", ["4 KiB = 512 项 × 8 B，任何页分配器都行",
                                                              "全 RAM 线性映射 KERNBASE + PA", "所有 CPU、所有进程共用这一张"], "kpt")
    s.line(rx + 165, ry + 120, rx + 165, ry + 138, col="#c86400", marker="arO")
    tbl(rx + 400, ry + 44, 340, "进程 A 的 L1（kalloc 4 KiB）", ["空表：没有任何内核项", "L1[0] → L2 → L3 → 用户页（共 3 个表页）"], "user")
    tbl(rx + 400, ry + 124, 340, "进程 B 的 L1（kalloc 4 KiB）", ["fork：PTE 指向同一物理页，RO + COW"], "user")
    s.text(rx + 400, ry + 196, "TTBR0 = 当前进程（switchuvm 只换它）", size=12, weight="600", fill="#d35400")
    s.text(rx + 400, ry + 216, "TTBR1 = 0x3FF000（始终不变）", size=12, weight="600", fill="#c86400")
    rnotes = ["• 进程表只装用户映射；内核部分由 TTBR1 那一张承担，按 VA 最高位自动选表。",
              "• 三级：L1（1 GiB）→ L2（2 MiB，可作块）→ L3（4 KiB 页），每级正好一页。",
              "• fork：uvmcopy() 只共享 + 标 COW，写时 cow_break() 才复制。",
              "• 根表数只受内存限制；进程内核栈映射在 TTBR1 的 KSTACK 区。"]
    for i, t in enumerate(rnotes):
        s.text(rx, ry + 266 + i * 20, t, size=12)
    s.save(path)


# ---------------------------------------------------------------------------
# 3. exec-ls.svg: how exec("/bin/ls") (pid 19) builds its TTBR0 table page by
#    page; every address is from the MMU_TRACE=3 log.

def exec_flow(path):
    W, H = 1790, 900
    s = Svg(W, H)
    s.text(W / 2, 36, "exec(\"/bin/ls\") 如何把 ELF 读进来并填进进程自己的 TTBR0 页表（pid 19，MMU_TRACE=3 实测地址）",
           size=20, anchor="middle", weight="600")
    s.text(W / 2, 60, "kernel/fs/exec.c + kernel/mm/vm.c；左边是步骤，右边是结果，圆圈数字 = 哪一步建的", size=13, anchor="middle", fill="#555")

    def badge(x, y, n, col="#1f5fae"):
        s.o(f'<circle cx="{x}" cy="{y}" r="10" fill="{col}"/>')
        s.text(x, y + 4.5, str(n), size=12, anchor="middle", weight="700", fill="#fff")

    steps = [
        ("exec() → srcopen() / 读 ELF 头", ["namei(\"/bin/ls\") + ilock（inode 34，10304 字节）",
          "检查 magic、64 位、EM_AARCH64；ET_DYN（PIE），5 个程序头", "readinterp() → PT_INTERP = /lib/ld-xv6.so"]),
        ("uvmcreate()：新的空 L1 根", ["kalloc() 一页清零 → L1 0x3B60A000（512 项全无效）",
          "旧表（fork 来的 COW 表）仍在 TTBR0：失败可整张丢弃"]),
        ("loadimage(base 0) → uvmalloc(0, 0x1AC8)", ["每页：kalloc() + memset 0 → mappages(PTE_NORMAL|PTE_U)",
          "walk(alloc=1)：L1[0] 空 → kalloc L2 0x3B621000，写 0x3B621003",
          "              L2[0] 空 → kalloc L3 0x3B624000，写 0x3B624003",
          "*pte = PA | perm | AF | V：L3[0] = 0x3B62274B，L3[1] = 0x3B62374B"]),
        ("loadseg()：把文件内容拷进这些页", ["uva2ka(新表, va)：walk(alloc=0) 取 PA，再 P2V 成内核地址",
          "经 TTBR1 线性映射 readi（新表此时还没装进 TTBR0）",
          "off 0 → VA 0 共 2297 B；off 0x900 → VA 0x1900 共 424 B",
          "memsz > filesz 的 .bss 早已被 uvmalloc 清零"]),
        ("PT_INTERP：再装 ld-xv6.so", ["loadimage(base = PGROUNDUP(sz) = 0x2000)，同样 uvmalloc + loadseg",
          "L3[2] → 0x3CA000（.text 3266 B），L3[3] → 0x3B637000（.data 224 B）",
          "入口 = 0x2000 + 0x784 = 0x2784"]),
        ("栈：uvmalloc 5 页 + uvmclear + copyout", ["VA 0x4000–0x8FFF → L3[4..8]",
          "uvmclear(0x4000)：PTE_U 清零，0x…74B → 0x…70B 成保护页",
          "copyout argv 字符串、argv[]、envp、auxv → sp = 0x8F50"]),
        ("提交 + switchuvm()", ["vm->pagetable = 新表；sz = brk = 0x9000；elr = 0x2784，sp = 0x8F50",
          "uvmsync_icache()：数据写进来的页要执行，先清 D-cache / 作废 I-cache",
          "w_ttbr0_el1(0x3B60A000) + flush_tlb()；uvmfree(旧表) 只 put_page"]),
    ]
    y = 90
    sy = {}
    for i, (t, lines) in enumerate(steps, 1):
        h = 30 + 17 * len(lines)
        s.box(30, y, 640, h, "hole")
        badge(52, y + 17, i)
        s.text(70, y + 22, t, size=13.5, weight="600")
        for k, l in enumerate(lines):
            s.text(70, y + 42 + k * 17, l, size=11.5, mono=False)
        sy[i] = y + h / 2
        if i < len(steps):
            s.line(350, y + h + 1, 350, y + h + 11, col="#999", marker="ar")
        y += h + 12

    # ---- right side: TTBR0, L1, L2, L3, pages
    s.box(720, 96, 250, 52, "kstack")
    s.text(730, 116, "TTBR0_EL1", size=13, weight="600", mono=True)
    s.text(730, 136, "旧 COW 表 → 0x3B60A000", size=12)
    badge(958, 96, 7, "#c0582b")
    s.box(720, 190, 180, 70, "kpt")
    s.text(730, 210, "L1 0x3B60A000", size=12.5, weight="600", mono=True)
    s.text(730, 230, "[0] = 0x3B621003", size=11.5, mono=True)
    s.text(730, 248, "[1..511] 无效", size=11, fill="#666")
    badge(890, 190, 2, "#c86400")
    s.box(720, 300, 180, 70, "kpt")
    s.text(730, 320, "L2 0x3B621000", size=12.5, weight="600", mono=True)
    s.text(730, 340, "[0] = 0x3B624003", size=11.5, mono=True)
    s.text(730, 358, "[1..511] 无效", size=11, fill="#666")
    badge(890, 300, 3, "#c86400")
    s.line(845, 148, 845, 188, col="#c0582b", marker="arO", width=1.6)
    s.line(810, 260, 810, 298, col="#c86400", marker="arO", width=1.6)
    # L3
    rows = [
        ("[0]", "0x3B62274B", "0x3B622000", "ls .text（文件 off 0，2297 B）", "user", 3, 4),
        ("[1]", "0x3B62374B", "0x3B623000", "ls .data @+0x900（424 B）+ .bss", "user", 3, 4),
        ("[2]", "0x003CA74B", "0x003CA000", "ld.so .text（3266 B）", "ld", 5, None),
        ("[3]", "0x3B63774B", "0x3B637000", "ld.so .data @+0xCC8（224 B）", "ld", 5, None),
        ("[4]", "0x3B63870B", "0x3B638000", "保护页（无 PTE_U）", "guard", 6, None),
        ("[5]", "0x3B63974B", "0x3B639000", "栈", "stack", 6, None),
        ("[6]", "0x3B63A74B", "0x3B63A000", "栈", "stack", 6, None),
        ("[7]", "0x3B63B74B", "0x3B63B000", "栈", "stack", 6, None),
        ("[8]", "0x3B63C74B", "0x3B63C000", "栈顶：argv/envp/auxv，sp=0x8F50", "stack", 6, None),
    ]
    L3X, L3Y, RH = 960, 170, 34
    s.text(L3X, L3Y - 10, "L3 0x3B624000（第 k 项 ↔ VA k×0x1000）", size=12.5, weight="600", fill="#c86400")
    s.box(L3X - 4, L3Y - 2, 214, RH * len(rows) + 4, "kpt")
    badge(L3X + 206, L3Y - 2, 3, "#c86400")
    s.path(f"M 900 335 C 930 335, 930 {L3Y + 17}, {L3X - 6} {L3Y + 17}", col="#c86400", marker="arO", width=1.6)
    PGX = 1260
    s.text(PGX, L3Y - 10, "物理页（kalloc，伙伴系统）", size=12.5, weight="600", fill="#1e8449")
    for k, (idx, pte, pa, what, kind, st, st2) in enumerate(rows):
        y0 = L3Y + k * RH
        s.text(L3X + 4, y0 + 21, f"{idx} {pte}", size=11.5, mono=True)
        s.box(PGX, y0 + 3, 420, RH - 6, kind)
        s.text(PGX + 8, y0 + 21, pa, size=11.5, mono=True, weight="600")
        s.text(PGX + 100, y0 + 21, f"VA 0x{k:X}000  {what}", size=11.5)
        s.line(L3X + 206, y0 + RH / 2, PGX - 26, y0 + RH / 2, col="#888", marker="ar", width=1)
        badge(PGX - 14, y0 + RH / 2, st, "#1e8449")
    # files, under the page column; copies curve up the right-hand side
    FY = L3Y + RH * len(rows) + 50
    s.text(PGX, FY - 12, "文件（ext2 根文件系统，经 readi 读）", size=12.5, weight="600")
    def fbox(y, name, segs, kind):
        s.box(PGX, y, 420, 92, "hole")
        s.text(PGX + 10, y + 18, name, size=12.5, weight="600", mono=True)
        for i, t in enumerate(segs):
            s.box(PGX + 10, y + 28 + i * 30, 400, 24, kind)
            s.text(PGX + 18, y + 45 + i * 30, t, size=11, mono=True)
    fbox(FY, "/bin/ls（inode 34，10304 B）", ["off 0x000  LOAD R-X filesz 0x8F9", "off 0x900  LOAD RW- filesz 0x1A8 memsz 0x1C8"], "user")
    fbox(FY + 104, "/lib/ld-xv6.so", ["off 0x000  LOAD R-X filesz 0xCC2", "off 0xCC8  LOAD RW- filesz 0xE0"], "ld")
    def cp(y0, k, col, bend):
        y1 = L3Y + k * RH + RH / 2
        x0 = PGX + 410
        x1 = PGX + 420
        s.path(f"M {x0} {y0} C {x1 + bend} {y0}, {x1 + bend} {y1}, {x1 + 2} {y1}", col=col, marker="arO", width=1.3, dash=True)
    cp(FY + 40, 0, "#d35400", 70)
    cp(FY + 70, 1, "#d35400", 55)
    cp(FY + 144, 2, "#b9770e", 100)
    cp(FY + 174, 3, "#b9770e", 85)
    s.text(PGX, FY + 222, "虚线 = 第 4、5 步 loadseg 的 readi：uva2ka(新表) → PA → P2V，", size=11.5, fill="#d35400")
    s.text(PGX, FY + 238, "经内核 TTBR1 线性映射写入物理页", size=11.5, fill="#d35400")

    ny = y + 20
    s.text(30, ny, "要点", size=14, weight="600")
    for i, t in enumerate([
        "• exec 不按需分页：镜像的每一页在 exec 时就 kalloc + 清零 + 映射 + 读入；只有 brk 堆和 mmap 是缺页才分配。",
        "• 中间层表只在第一次需要时建：L2、L3 各一张就覆盖了 VA 0–2 MiB，所以 9 个用户页只用 3 个表页。",
        "• 新表全程由内核通过 TTBR1 的线性映射填写，直到 switchuvm() 才第一次被 TTBR0 使用。",
        "• uvmalloc 不看段的 R/W/X：所有页都是 0x…74B（EL0 RW、无 UXN/PXN）—— 代码可写、数据可执行。",
    ]):
        s.text(30, ny + 24 + i * 20, t, size=12.5)
    s.save(path)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "doc/readme/rpi3/images"
    os.makedirs(out, exist_ok=True)
    memory_layout(os.path.join(out, "memory-layout.svg"))
    page_tables(os.path.join(out, "mmu-page-tables.svg"))
    exec_flow(os.path.join(out, "exec-ls.svg"))
    print("wrote", out)
