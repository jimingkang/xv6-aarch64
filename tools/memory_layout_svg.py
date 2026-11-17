#!/usr/bin/env python3
# Tiny210 memory layout: virtual <-> physical <-> /proc/iomem, /proc/meminfo.
# Regenerate: python3 tools/memory_layout_svg.py doc/readme/tiny210/images/memory-layout.svg
#             rsvg-convert -z 1.5 doc/readme/tiny210/images/memory-layout.svg -o doc/readme/tiny210/images/memory-layout.png
import sys
from xml.sax.saxutils import escape as esc

W = 1640
FONT = "'PingFang SC','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"

C = {  # fill, stroke
    "mmio":   ("#fde2e1", "#c0392b"),
    "hole":   ("#f2f2f2", "#9a9a9a"),
    "unused": ("#ececec", "#8a8a8a"),
    "fb":     ("#e3d7f5", "#6c3fb5"),
    "gui":    ("#efe1f7", "#8e44ad"),
    "kalloc": ("#d9f2df", "#1e8449"),
    "bss":    ("#dbe8fb", "#1f5fae"),
    "data":   ("#d3e2fa", "#1f5fae"),
    "code":   ("#c6d8f7", "#1f5fae"),
    "atag":   ("#fff3cd", "#b7950b"),
    "user":   ("#fde9cf", "#d35400"),
    "mmap":   ("#f6e0f0", "#a93d8c"),
}

# Rows, top (high address) to bottom. Identity-mapped rows are shared by the
# virtual and physical columns.
TOP = 150
rows = [
    # key, height, kind, PA label lines, iomem lines, meminfo lines
    ("hi",   70, "mmio", ["0xF2000000 – 0xF800FFFF", "VIC0-3、FIMD(LCD)"],
     ["f2000000-f23fffff : s5pv210-vic vic0-3",
      "f8000000-f800ffff : s3c-fb fimd"], []),
    ("io",  128, "mmio", ["0xE0100000 – 0xEC2FFFFF", "外设寄存器（1 MiB 段，", "Strongly-ordered, XN）"],
     ["e0100000-e010ffff : s5pv210-clock clock",
      "e0200000-e0200fff : s5pv210-gpio gpio",
      "e0200000-e0200fff : s5pv210-keys gpio-eint",
      "e2500000-e25003ff : s5pv210-pwm pwm",
      "e2700000-e27003ff : s3c2410-wdt wdt",
      "e2900000-e29003ff : s5pv210-uart uart",
      "eb000000-eb0fffff : s3c-sdhci hsmmc",
      "ec100000-ec100fff : s5p-ehci usb-phy",
      "ec200000-ec2fffff : s5p-ehci ehci"], []),
    ("gap",  44, "hole", ["0x40000000 – 0xDFFFFFFF", "无 DRAM（iRAM 0xD0020000 仅 BL1 用）"], [], []),
    ("unused", 46, "unused", ["0x28400000 – 0x3FFFFFFF  380 MiB", "未使用：不在页分配器内"],
     ["  28400000-3fffffff : Unused (outside page allocator)"], []),
    ("fb",   36, "fb", ["0x28000000 – 0x283FFFFF  4 MiB", "LCD 帧缓冲（Normal 不可缓存）"],
     ["  28000000-283fffff : s3c-fb framebuffer"], []),
    ("gui",  36, "gui", ["0x27000000 – 0x27FFFFFF  16 MiB", "win32k 表面缓存区"],
     ["  27000000-27ffffff : win32k surface arena"], []),
    ("kalloc", 104, "kalloc", ["0x2053A000 – 0x26FFFFFF  ≈106.8 MiB", "页分配器 kalloc（4 KiB 空闲链表）",
                               "PHYSTOP = DRAM + 112 MiB", "用户页 / 内核栈 / L2 页表 / 管道"],
     ["  2053a000-26ffffff : Page allocator (kalloc)"],
     ["MemTotal:     109336 kB", "MemFree / MemAvailable", "AnonPages    (用户页)", "KernelStack  (每进程 1 页)",
      "PageTables   (L2 表部分)", "KallocRange: 2053a000-26ffffff"]),
    ("bss",  62, "bss", ["0x20040000 – 0x20539FFF  ≈5 MiB", "内核 .bss：L1 根池 4 MiB(roots)、", "bcache、fw_buf、diskmap…"],
     ["  20040000-20539fff : Kernel bss"],
     ["KernelBss:      5096 kB", "PageTables   (L1 根 16 KiB/个)", "Buffers:          30 kB (bcache)"]),
    ("data", 34, "data", ["0x2002F000 – 0x2003FFFF  68 KiB", ".rodata/.initcall/.data"],
     ["  2002f000-2003ffff : Kernel data"], ["KernelData:       68 kB"]),
    ("code", 34, "code", ["0x20008000 – 0x2002EFFF  156 KiB", ".text（_entry 在最前）"],
     ["  20008000-2002efff : Kernel code"], ["KernelText:      156 kB"]),
    ("atag", 30, "atag", ["0x20000000 – 0x20007FFF  32 KiB", "ATAG(DRAM+0x100)/零页"],
     ["  20000000-20007fff : Reserved (ATAGs, below kernel)"], []),
    ("low", 150, "hole", ["0x00000000 – 0x1FFFFFFF", "无 DRAM（iROM 等）", "内核不映射这段物理地址"], [], []),
]
y = TOP
for i, r in enumerate(rows):
    rows[i] = dict(key=r[0], h=r[1], kind=r[2], pa=r[3], iomem=r[4], meminfo=r[5], y=y)
    y += r[1]
BOTTOM = y
R = {r["key"]: r for r in rows}

# Virtual column: identity rows above 0x20000000, user window below it.
low = R["low"]
vsub = [  # key, height, kind, label lines
    ("vgap2", 26, "hole", ["0x18000000 – 0x1FFFFFFF  未映射"]),
    ("vmmap", 44, "mmap", ["0x10000000 – 0x17FFFFFF  mmap 窗口", "1 MiB 用户段（/dev/fb0 mmap）"]),
    ("vgap1", 26, "hole", ["0x02000000 – 0x0FFFFFFF  未映射"]),
    ("vuser", 54, "user", ["0x00000000 – 0x01FFFFFF  用户空间", "代码/数据/堆/栈，4 KiB 页(L2)", "sz < 32 MiB"]),
]
assert sum(v[1] for v in vsub) == low["h"]
vy = low["y"]
V = {}
for k, h, kind, lab in vsub:
    V[k] = dict(y=vy, h=h, kind=kind, lab=lab)
    vy += h

VX, VW = 40, 300          # virtual column
PX, PW = 700, 300         # physical column
IX = 1040                  # /proc/iomem text
MX = 1380                 # /proc/meminfo text
out = []
o = out.append
o(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{BOTTOM + 190}" '
  f'viewBox="0 0 {W} {BOTTOM + 190}" font-family="{FONT}" font-size="12">')
o('<defs>'
  '<marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
  '<path d="M0,0 L10,5 L0,10 z" fill="#555"/></marker>'
  '<marker id="arO" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
  '<path d="M0,0 L10,5 L0,10 z" fill="#d35400"/></marker>'
  '<marker id="arP" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
  '<path d="M0,0 L10,5 L0,10 z" fill="#a93d8c"/></marker>'
  '</defs>')
o(f'<rect width="{W}" height="{BOTTOM + 190}" fill="#ffffff"/>')
o(f'<text x="{W/2}" y="34" text-anchor="middle" font-size="22" font-weight="600" fill="#222">'
  'Tiny210 (S5PV210) xv6 内存布局：虚拟地址 ↔ 物理地址 ↔ /proc/iomem、/proc/meminfo</text>')
o(f'<text x="{W/2}" y="58" text-anchor="middle" font-size="13" fill="#555">'
  '实板地址（DRAM 0x20000000，512 MiB）；QEMU 替身 DRAM 在 0x40000000，结构相同。纵轴高地址在上，不按比例。</text>')

def head(x, w, t, sub):
    o(f'<text x="{x + w/2}" y="{TOP - 34}" text-anchor="middle" font-size="16" font-weight="600" fill="#222">{esc(t)}</text>')
    o(f'<text x="{x + w/2}" y="{TOP - 16}" text-anchor="middle" font-size="11.5" fill="#666">{esc(sub)}</text>')

head(VX, VW, "虚拟地址空间（每个进程一张 L1 表）", "TTBR0 only，TTBCR=0；内核部分仅特权级可访问")
head(PX, PW, "物理地址空间", "kvminit / tiny210_map_devices 建立映射")
o(f'<text x="{IX}" y="{TOP - 34}" font-size="16" font-weight="600" fill="#222">/proc/iomem</text>')
o(f'<text x="{IX}" y="{TOP - 16}" font-size="11.5" fill="#666">（实际文件按地址升序；此处与左侧行对齐）</text>')
o(f'<text x="{MX}" y="{TOP - 34}" font-size="16" font-weight="600" fill="#222">/proc/meminfo</text>')
o(f'<text x="{MX}" y="{TOP - 16}" font-size="11.5" fill="#666">（数值为实板预期，动态项随负载变化）</text>')

def box(x, w, y, h, kind, lines, mono_first=True, dash=False):
    f, s = C[kind]
    d = ' stroke-dasharray="5 3"' if dash else ''
    o(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{f}" stroke="{s}" stroke-width="1.2"{d}/>')
    lh = 14
    ty = y + h/2 - (len(lines) - 1) * lh / 2 + 4
    for i, t in enumerate(lines):
        fam = f' font-family="{MONO}" font-size="11.5"' if (i == 0 and mono_first and t.startswith("0x")) else ''
        wt = ' font-weight="600"' if i == 0 else ''
        o(f'<text x="{x + w/2}" y="{ty + i*lh}" text-anchor="middle" fill="#222"{fam}{wt}>{esc(t)}</text>')

# Physical column + iomem + meminfo.
for r in rows:
    box(PX, PW, r["y"], r["h"], r["kind"], r["pa"], dash=r["kind"] == "hole")
    for i, t in enumerate(r["iomem"]):
        o(f'<text x="{IX}" y="{r["y"] + 15 + i*13.4}" font-family="{MONO}" font-size="11.2" fill="#222" '
          f'xml:space="preserve">{esc(t)}</text>')
    if r["iomem"]:
        _, s = C[r["kind"]]
        o(f'<line x1="{PX+PW}" y1="{r["y"] + 11}" x2="{IX - 6}" y2="{r["y"] + 11}" stroke="{s}" stroke-width="1" stroke-dasharray="2 3"/>')
    for i, t in enumerate(r["meminfo"]):
        o(f'<text x="{MX}" y="{r["y"] + 15 + i*14}" font-family="{MONO}" font-size="11.2" fill="#1d3b6e" '
          f'xml:space="preserve">{esc(t)}</text>')

# System RAM bracket for iomem.
ram_top, ram_bot = R["unused"]["y"], R["atag"]["y"] + R["atag"]["h"]
bx = IX - 14
o(f'<path d="M{bx+6},{ram_top+2} L{bx},{ram_top+2} L{bx},{ram_bot-2} L{bx+6},{ram_bot-2}" fill="none" stroke="#1e8449" stroke-width="1.5"/>')
o(f'<text x="{IX}" y="{R["unused"]["y"] - 6}" font-family="{MONO}" font-size="11.2" font-weight="600" fill="#1e8449">20000000-3fffffff : System RAM</text>')

# Virtual column: identity rows mirror the physical ones.
for r in rows[:-1]:
    lab = list(r["pa"][:1])
    if r["kind"] == "hole":
        lab += ["未映射"]
    elif r["kind"] == "mmio":
        lab += ["内核：外设恒等映射"]
    elif r["key"] == "unused":
        lab += ["未映射"]
    else:
        lab += ["内核恒等映射 VA = PA"]
    box(VX, VW, r["y"], r["h"], r["kind"], lab, dash=r["kind"] in ("hole",) or r["key"] == "unused")
for k, v in V.items():
    box(VX, VW, v["y"], v["h"], v["kind"], v["lab"], dash=v["kind"] == "hole")

# MAXVA line.
my = low["y"]
o(f'<line x1="{VX - 30}" y1="{my}" x2="{PX + PW + 4}" y2="{my}" stroke="#c0392b" stroke-width="2"/>')
o(f'<text x="{PX + 8}" y="{my + 16}" font-size="11.5" font-weight="600" fill="#c0392b">↑ MAXVA = 0x20000000 = DRAM_BASE</text>')
o(f'<text x="{PX + 8}" y="{my + 30}" font-size="11" fill="#c0392b">红线以下是用户窗口（虚拟）</text>')

# Identity arrows (VA = PA) for mapped rows.
for key in ("hi", "io", "fb", "gui", "kalloc", "bss", "data", "code", "atag"):
    r = R[key]
    yy = r["y"] + r["h"]/2
    o(f'<line x1="{VX+VW+4}" y1="{yy}" x2="{PX-4}" y2="{yy}" stroke="#555" stroke-width="1.3" marker-end="url(#ar)"/>')
o(f'<text x="{(VX+VW+PX)/2}" y="{R["gap"]["y"] + 13}" text-anchor="middle" font-size="11.5" fill="#555">内核：1 MiB 段，VA = PA</text>')
o(f'<text x="{(VX+VW+PX)/2}" y="{R["gap"]["y"] + 27}" text-anchor="middle" font-size="11" fill="#777">DRAM：Normal 不可缓存（D-cache 关）</text>')
o(f'<text x="{(VX+VW+PX)/2}" y="{R["gap"]["y"] + 41}" text-anchor="middle" font-size="11" fill="#777">MMIO：Strongly-ordered, XN</text>')

# User pages -> kalloc frames.
u = V["vuser"]
ka = R["kalloc"]
x1, y1 = VX + VW + 4, u["y"] + u["h"]/2
x2, y2 = PX - 4, ka["y"] + ka["h"] - 18
o(f'<path d="M{x1},{y1} L{x2-45},{y1} C{x2-20},{y1} {x2-20},{y2} {x2},{y2}" fill="none" stroke="#d35400" stroke-width="2" marker-end="url(#arO)"/>')
o(f'<text x="{x1 + 8}" y="{y1 + 16}" font-size="11.5" fill="#d35400" font-weight="600">L2 页表：4 KiB 用户页 → 任意 kalloc 页</text>')
o(f'<text x="{x1 + 8}" y="{y1 + 30}" font-size="11" fill="#d35400">（AnonPages；页表本身也是 kalloc 页）</text>')

# mmap window -> framebuffer.
m = V["vmmap"]
fb = R["fb"]
x1, y1 = VX + VW + 4, m["y"] + m["h"]/2
x2, y2 = PX - 4, fb["y"] + fb["h"] - 6
o(f'<path d="M{x1},{y1} L{x2-90},{y1} C{x2-60},{y1} {x2-60},{y2} {x2},{y2}" fill="none" stroke="#a93d8c" stroke-width="2" marker-end="url(#arP)"/>')
o(f'<text x="{x1 + 8}" y="{y1 - 6}" font-size="11.5" fill="#a93d8c" font-weight="600">remap_pfn_range：1 MiB 用户段 → 帧缓冲</text>')

# Kernel stacks / L2 tables note on kalloc.


# Legend / notes.
ny = BOTTOM + 36
notes = [
    "对应关系：/proc/iomem 的每一行就是右侧物理区域；MMIO 行来自 devices.c 平台设备的 MEM 资源，DRAM 子区来自 setup.c 的 arch_iomem_regions() 和内核链接符号。",
    "/proc/meminfo：MemTotal/MemFree = kalloc 区；KernelText/Data/Bss = 内核镜像三段；PageTables = L1 根（bss 里的静态池，16 KiB/个）+ L2 表（kalloc 页）；",
    "Buffers = bss 里的 bcache（30×1 KiB）；KernelStack 与 AnonPages 都从 kalloc 区分配。512 MiB DRAM 只有 112 MiB 交给 kalloc（PHYSTOP），其余为保留区或未使用。",
    "每个进程的 L1 表 = 内核 L1 表的拷贝（uvmcreate 复制 16 KiB）+ 自己的用户映射，因此所有进程都能看到内核映射，但 AP 位只允许特权级访问。",
]
o(f'<text x="40" y="{ny - 10}" font-size="14" font-weight="600" fill="#222">说明</text>')
for i, t in enumerate(notes):
    o(f'<text x="40" y="{ny + 12 + i*20}" font-size="12.5" fill="#333">{esc(t)}</text>')
lx = 40
ly = ny + 12 + len(notes)*20 + 14
for kind, name in [("code", "内核镜像"), ("kalloc", "页分配器"), ("gui", "win32k"), ("fb", "帧缓冲"),
                   ("atag", "ATAG 保留"), ("unused", "未使用 DRAM"), ("mmio", "MMIO"), ("user", "用户页"),
                   ("mmap", "mmap 窗口"), ("hole", "无映射 / 非 DRAM")]:
    f, s = C[kind]
    o(f'<rect x="{lx}" y="{ly - 11}" width="16" height="12" fill="{f}" stroke="{s}"/>')
    o(f'<text x="{lx + 22}" y="{ly}" font-size="12" fill="#333">{name}</text>')
    lx += 22 + len(name) * 13 + 30
o('</svg>')
open(sys.argv[1], "w").write("\n".join(out))
