#!/usr/bin/env python3
# Tiny210 xv6: how kvminit()/kvminithart() build the ARMv7 short-descriptor
# page tables (kernel L1 sections) and how a process adds L2 tables.
# Layout follows minios' __create_page_tables diagram: code panel, macro
# boxes, result diagram (VA -> L1 -> L2 -> PA), refresh box, notes box.
# Regenerate:
#   python3 tools/mmu_page_tables_svg.py doc/readme/tiny210/images/mmu-page-tables.svg
#   rsvg-convert -z 1.5 doc/readme/tiny210/images/mmu-page-tables.svg -o doc/readme/tiny210/images/mmu-page-tables.png
import sys
from xml.sax.saxutils import escape as esc

W, H = 1800, 1745
SANS = "'PingFang SC','Noto Sans CJK SC','Helvetica Neue',Arial,sans-serif"
MONO = "Menlo,'SF Mono',Consolas,monospace"
out = []
o = out.append

DRAM, PHYSTOP, ROOTS = 0x20000000, 0x27000000, 0x200D4000
SEC_RAM = lambda a: a | (1 << 12) | (1 << 10) | 2
SEC_NC = lambda a: a | (1 << 12) | (1 << 10) | (1 << 4) | 2
SEC_DEV = lambda a: a | (1 << 10) | (1 << 4) | 2


def text(x, y, s, size=13, fill="#222", mono=False, weight=None, anchor=None, extra=""):
    fam = MONO if mono else SANS
    w = f' font-weight="{weight}"' if weight else ""
    a = f' text-anchor="{anchor}"' if anchor else ""
    o(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" fill="{fill}"{w}{a} xml:space="preserve"{extra}>{esc(s)}</text>')


def rect(x, y, w, h, fill, stroke, sw=1.5, rx=6, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    o(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def arrow(x1, y1, x2, y2, color, dash=None, sw=1.6, curve=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    mid = color.replace("#", "")
    if curve:
        cx, cy = curve
        o(f'<path d="M{x1},{y1} Q{cx},{cy} {x2},{y2}" fill="none" stroke="{color}" stroke-width="{sw}"{d} marker-end="url(#m{mid})"/>')
    else:
        o(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}"{d} marker-end="url(#m{mid})"/>')


COLORS = ["#c0392b", "#1e8449", "#5b3fb5", "#2160a0", "#8e44ad", "#b9770e", "#555555", "#d35400", "#a93d8c"]
o(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
o("<defs>" + "".join(
    f'<marker id="m{c[1:]}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
    f'<path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>' for c in COLORS) + "</defs>")
o(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
text(W / 2, 40, "kvminit() 图解：Tiny210 的两级页表（L1 段 + 进程的 L2 小页）", 26, weight="600", anchor="middle")
text(W / 2, 66, "ARMv7 短描述符，Cortex-A8 无 LPAE：只有 TTBR0 一张 L1（4096 项 × 1 MiB）；内核全部用 1 MiB 段恒等映射，"
     "用户空间经 L2 表映射 4 KiB 小页。数值为实板地址，L2 示例取自 QEMU MMU_TRACE=3 日志", 13, "#666", anchor="middle")

# ------------------------------------------------------------ code panel
cx, cy, cw, ch = 30, 85, 560, 480
rect(cx, cy, cw, ch, "#16213a", "#16213a", rx=6)
text(cx + 14, cy + 22, "kernel/arch/arm/vm.c + mach-s5pv210/devices.c", 12.5, "#cdd6f4", mono=True)
code = [
    ("void kvminit(void) {", None, "#f9e2af"),
    ("  kernel_pagetable = rootalloc();     // ① roots[0], 清零", 0, None),
    ("  for(a = EXTMEM; a < PHYSTOP; a += 1 MiB)    // ② DRAM", 1, None),
    ("    L1[a>>20] = a | (1<<12) | (1<<10) | 2;", 1, None),
    ("  tiny210_map_devices(kernel_pagetable);", 2, None),
    ("}", None, None),
    ("void tiny210_map_devices(p) {", None, "#f9e2af"),
    ("  for(a in GUI arena) p[a>>20] = SECTION_RAM(a); // ③", 2, None),
    ("  for(each MEM resource r)                    // ④", 3, None),
    ("    for(a = r.start & ~0xFFFFF; a <= r.end; a += 1 MiB)", 3, None),
    ("      p[a>>20] = fb ? SECTION_NC(a) : SECTION_DEV(a);", 3, None),
    ("}", None, None),
    ("void kvminithart(void) {", None, "#f9e2af"),
    ("  install(kernel_pagetable);  // ⑤ TTBCR/TTBR0/DACR/TLB", 4, None),
    ("  SCTLR |= 1 (M); isb();      // ⑥ MMU 开", 5, None),
    ("}", None, None),
    ("pagetable_t uvmcreate(void) {          // ⑦ 每个进程", 6, "#f9e2af"),
    ("  p = rootalloc(); memmove(p, kernel_pagetable, 16384);", 6, None),
    ("}", None, None),
    ("pte_t *walk(p, va, alloc) {            // ⑧ 用户页", 7, "#f9e2af"),
    ("  if(p[va>>20] == 0) p[va>>20] = kalloc() | 1;  // L2 表", 7, None),
    ("  return &L2[(va>>12) & 255];  // mappages: *pte = pa|flags|2", 7, None),
    ("}", None, None),
]
band = ["#3a2a2a", "#1f3d2c", "#33294d", "#3d2440", "#4a3a1a", "#1f4030", "#4a3020", "#2a3550"]
y = cy + 44
for line, step, col in code:
    if step is not None:
        o(f'<rect x="{cx + 8}" y="{y - 13}" width="{cw - 16}" height="18" fill="{band[step]}" rx="2"/>')
    text(cx + 16, y, line, 12, col or "#e6e6e6", mono=True)
    y += 19.2

# ------------------------------------------------------------ macro boxes
bx, bw = 615, 1155
rect(bx, 85, bw, 215, "#f4f1fb", "#5b3fb5")
text(bx + 16, 110, "L1 段描述符（section，类型 10）：内核 ②③④ 写的都是它", 15, "#5b3fb5", weight="600")
lines = [
    "idx  = VA >> 20                                   // 4096 项，每项 1 MiB；内核恒等映射所以 VA = PA",
    "desc = (PA & 0xFFF00000) | TEX<<12 | AP<<10 | XN<<4 | C<<3 | B<<2 | 0b10",
    "L1[idx] = desc                                   // MMU 一次查表：PA = desc[31:20] | VA[19:0]",
]
for i, l in enumerate(lines):
    text(bx + 26, 138 + i * 22, l, 12.5, "#333", mono=True)
rows = [
    ("② DRAM / ③ win32k", f"{SEC_RAM(DRAM):08x}", "TEX=001 C=0 B=0 → Normal 不可缓存", "AP=01 仅特权级", "XN=0 可执行"),
    ("④ framebuffer", f"{SEC_NC(0x28000000):08x}", "TEX=001 C=0 B=0 → Normal 不可缓存", "AP=01", "XN=1"),
    ("④ MMIO 设备", f"{SEC_DEV(0xE2900000):08x}", "TEX=000 C=0 B=0 → Strongly-ordered", "AP=01", "XN=1"),
]
for i, r in enumerate(rows):
    yy = 218 + i * 22
    text(bx + 26, yy, r[0], 12.5, "#5b3fb5")
    text(bx + 200, yy, r[1], 12.5, "#c0392b", mono=True)
    text(bx + 300, yy, r[2], 12.5, "#333")
    text(bx + 600, yy, r[3], 12.5, "#333")
    text(bx + 730, yy, r[4], 12.5, "#333")
text(bx + 26, 290, "注释里 SECTION_RAM 写作 \"cached Normal memory\"，但位编码与 DRAM 相同（TEX=001 C=B=0），实际不可缓存；D-cache 也一直关着。",
     11.5, "#888")

rect(bx, 315, bw, 250, "#eef7f0", "#1e8449")
text(bx + 16, 340, "L1 表描述符（类型 01）+ L2 小页（类型 1x）：用户空间 ⑦⑧ 用", 15, "#1e8449", weight="600")
lines = [
    "L1[VA>>20] = L2_pa | 0b01             // walk(): kalloc 一页作 L2 表（只用前 1 KiB：256 项 × 4 字节）",
    "idx2 = (VA >> 12) & 255               // L2 索引",
    "L2[idx2] = PA | PTE_NORMAL | PTE_U | 0b10   // mappages()：小页 4 KiB，bit0 = XN",
    "PA = L2[idx2][31:12] | VA[11:0]       // MMU 两次查表",
]
for i, l in enumerate(lines):
    text(bx + 26, 368 + i * 22, l, 12.5, "#333", mono=True)
text(bx + 26, 466, "QEMU 日志（exec /bin/sh，pid 5）：", 12.5, "#1e8449", weight="600")
logs = ["mm:   L2 table: kalloc pa 46fe0000 -> L1[0] = 46fe0001 (VA 0-fffff)",
        "mm:   page: kalloc pa 46fe1000 -> VA 0 (PTE 46fe1072)",
        "PTE 0x072 = 小页(bit1) · AP=11 用户读写(bit5:4) · TEX=001 Normal 不可缓存(bit6) · XN=0"]
for i, l in enumerate(logs):
    text(bx + 26, 490 + i * 22, l, 12.5, "#c0392b" if i < 2 else "#333", mono=i < 2)

# ------------------------------------------------------------ result diagram
ry, rh = 585, 820
rect(20, ry, W - 40, rh, "#fbfaf7", "#ddd", rx=8)
text(36, ry + 26, "结果：内核 L1 = 145 个段，其余 3951 项为 0（缺页）；每个进程的 L1 = 它的拷贝 + 用户窗口的 L2 表", 16, weight="600")

# VA column
vx, vw, vy0 = 40, 230, ry + 80
text(vx, vy0 - 12, "虚拟地址（TTBR0，整个 4 GiB）", 13, weight="600")
vrows = [  # label, height, color, fill
    ("0xF2000000 / 0xF8000000\nVIC、FIMD", 50, "#c0392b", "#fde2e1"),
    ("0xE0100000 – 0xEC2FFFFF\n外设寄存器", 70, "#c0392b", "#fde2e1"),
    ("0x28400000 – 0xDFFFFFFF\n未映射（缺页）", 60, "#999", "#f2f2f2"),
    ("0x28000000 帧缓冲", 28, "#8e44ad", "#efe1f7"),
    ("0x27000000 win32k", 28, "#8e44ad", "#efe1f7"),
    ("0x20000000 – 0x26FFFFFF\nATAG、内核、kalloc 页池\nVA = PA", 150, "#1e8449", "#e3f4e8"),
    ("0x10000000 – 0x17FFFFFF\nmmap 窗口（用户段）", 50, "#a93d8c", "#f6e0f0"),
    ("0x00000000 – 0x01FFFFFF\n用户代码/数据/栈\n（经 L2 表）", 90, "#d35400", "#fde9cf"),
]
vpos = {}
y = vy0
for lab, h, c, f in vrows:
    rect(vx, y, vw, h, f, c, rx=2)
    for i, part in enumerate(lab.split("\n")):
        text(vx + 10, y + 17 + i * 16, part, 11.5, c if i else "#222", mono=i == 0)
    vpos[lab.split("\n")[0]] = (y, h)
    y += h
text(vx, y + 18, "MAXVA = 0x20000000：以下为用户窗口", 11.5, "#c0392b")

# L1 table
lx, lw = 330, 420
o(f'<rect x="{lx}" y="{vy0 - 40}" width="{lw}" height="{y - vy0 + 40}" rx="6" fill="#fff" stroke="#5b3fb5" stroke-width="2"/>')
o(f'<rect x="{lx}" y="{vy0 - 40}" width="{lw}" height="36" rx="6" fill="#5b3fb5"/>')
text(lx + lw / 2, vy0 - 24, "L1（内核 kernel_pagetable = roots[0]）", 14, "#fff", weight="600", anchor="middle")
text(lx + lw / 2, vy0 - 9, f"bss 静态池 @ {ROOTS:#010x}，16 KiB 对齐 → TTBR0", 11.5, "#e8e2ff", anchor="middle")
l1 = [
    ("[f80]", f"{SEC_DEV(0xF8000000):08x}", "PA F8000000 FIMD", "#c0392b"),
    ("[f20..f23]", f"{SEC_DEV(0xF2000000):08x}…", "PA F2000000 VIC0-3", "#c0392b"),
    ("[e01] [e02] [e25] [e27]", "", "clock gpio pwm wdt", "#c0392b"),
    ("[e29] [eb0] [ec1] [ec2]", f"{SEC_DEV(0xE2900000):08x}", "uart sdhci phy ehci", "#c0392b"),
    ("[284..e00] 等", "00000000", "缺页", "#999"),
    ("[280..283]", f"{SEC_NC(0x28000000):08x}…", "帧缓冲 NC+XN", "#8e44ad"),
    ("[270..27f]", f"{SEC_RAM(0x27000000):08x}…", "win32k", "#8e44ad"),
    ("[26f]", f"{SEC_RAM(0x26F00000):08x}", "PA 26F00000", "#1e8449"),
    ("…", "", "112 段 = 112 MiB", "#1e8449"),
    ("[200]", f"{SEC_RAM(0x20000000):08x}", "PA 20000000 内核", "#1e8449"),
    ("[100..17f]", "00000000", "进程：用户段 AP=11", "#a93d8c"),
    ("[000..01f]", "00000000", "进程：→ L2 表", "#d35400"),
]
y2 = vy0 + 6
l1pos = {}
for idx, val, note, c in l1:
    text(lx + 12, y2 + 14, idx, 12, c, mono=True)
    text(lx + 190, y2 + 14, val, 12, "#333", mono=True)
    text(lx + 290, y2 + 14, note, 11.5, c)
    l1pos[idx] = y2 + 10
    y2 += 37
# PA column
px, pw = 1450, 300
text(px, vy0 - 12, "物理内存 PA（实板）", 13, weight="600")
prows = [
    ("F2000000 VIC · F8000000 FIMD", 50, "#c0392b", "#fde2e1"),
    ("E0100000 – EC2FFFFF 外设", 70, "#c0392b", "#fde2e1"),
    ("28400000 – 3FFFFFFF 未用 DRAM", 60, "#999", "#f2f2f2"),
    ("28000000 帧缓冲 4 MiB", 28, "#8e44ad", "#efe1f7"),
    ("27000000 win32k 16 MiB", 28, "#8e44ad", "#efe1f7"),
    ("20000000 ATAG / 内核 / bss(roots)", 50, "#1e8449", "#e3f4e8"),
    ("2053A000 – 26FFFFFF kalloc 页池\n（L2 表、用户页、内核栈）", 100, "#1e8449", "#d9f2df"),
    ("00000000 – 1FFFFFFF iROM 等，非 DRAM", 140, "#999", "#f2f2f2"),
]
ppos = {}
y = vy0
for lab, h, c, f in prows:
    rect(px, y, pw, h, f, c, rx=2)
    for i, part in enumerate(lab.split("\n")):
        text(px + 10, y + 18 + i * 16, part, 11.5, "#222")
    ppos[lab.split(" ")[0]] = (y, h)
    y += h

# L2 table (process example)
qx, qw, qy = 830, 520, vy0 + 420
o(f'<rect x="{qx}" y="{qy}" width="{qw}" height="290" rx="6" fill="#fff" stroke="#d35400" stroke-width="2"/>')
o(f'<rect x="{qx}" y="{qy}" width="{qw}" height="36" rx="6" fill="#d35400"/>')
text(qx + qw / 2, qy + 16, "L2（进程 /bin/sh 的 L1[000] 所指，kalloc 页）", 14, "#fff", weight="600", anchor="middle")
text(qx + qw / 2, qy + 31, "QEMU 日志：L1[0] = 46fe0001 → L2 @ 46fe0000；只用前 1 KiB", 11.5, "#ffe9d6", anchor="middle")
l2 = [("[0]", "46fe1072", "VA 0000 代码/数据  PA 46fe1000"),
      ("[1]", "46fdf072", "VA 1000 代码/数据  PA 46fdf000"),
      ("[2]", "46fde072", "VA 2000 数据/bss   PA 46fde000"),
      ("[3]", "46fdd052", "VA 3000 保护页（uvmclear 清 PTE_U → AP=01）"),
      ("[4..7]", "46fdc072…", "VA 4000–7FFF 用户栈"),
      ("[8..255]", "00000000", "缺页"),
      ]
y3 = qy + 62
for idx, val, note in l2:
    text(qx + 16, y3, idx, 12, "#d35400", mono=True)
    text(qx + 100, y3, val, 12, "#333", mono=True)
    text(qx + 210, y3, note, 11.5, "#333")
    y3 += 30
text(qx + 16, y3 + 6, "PTE = PA | 0x72：小页 · AP=11 用户读写 · TEX=001 Normal 不可缓存", 11.5, "#d35400")
text(qx + 16, y3 + 26, "保护页：AP 只剩特权级 → 用户访问 Permission fault（栈溢出被抓住）", 11.5, "#888")

# arrows: VA -> L1, L1 -> PA, L1 -> L2, L2 -> PA
def vmid(key):
    y0, h = vpos[key]
    return y0 + h / 2


def pmid(key):
    y0, h = ppos[key]
    return y0 + h / 2


arrow(vx + vw, vmid("0xF2000000 / 0xF8000000"), lx, l1pos["[f80]"], "#c0392b")
arrow(vx + vw, vmid("0xE0100000 – 0xEC2FFFFF"), lx, l1pos["[e29] [eb0] [ec1] [ec2]"], "#c0392b")
arrow(vx + vw, vmid("0x28000000 帧缓冲"), lx, l1pos["[280..283]"], "#8e44ad")
arrow(vx + vw, vmid("0x27000000 win32k"), lx, l1pos["[270..27f]"], "#8e44ad")
arrow(vx + vw, vmid("0x20000000 – 0x26FFFFFF"), lx, l1pos["[200]"], "#1e8449")
arrow(vx + vw, vmid("0x10000000 – 0x17FFFFFF"), lx, l1pos["[100..17f]"], "#a93d8c")
arrow(vx + vw, vmid("0x00000000 – 0x01FFFFFF"), lx, l1pos["[000..01f]"], "#d35400")
# L1 -> PA (sections)
arrow(lx + lw, l1pos["[f80]"], px, pmid("F2000000"), "#c0392b", curve=(1100, l1pos["[f80]"] - 20))
arrow(lx + lw, l1pos["[e29] [eb0] [ec1] [ec2]"], px, pmid("E0100000"), "#c0392b", curve=(1100, l1pos["[e29] [eb0] [ec1] [ec2]"] - 10))
arrow(lx + lw, l1pos["[280..283]"], px, pmid("28000000"), "#8e44ad", curve=(1100, l1pos["[280..283]"] - 40))
arrow(lx + lw, l1pos["[270..27f]"], px, pmid("27000000"), "#8e44ad", curve=(1100, l1pos["[270..27f]"] - 40))
arrow(lx + lw, l1pos["[200]"], px, pmid("20000000"), "#1e8449", curve=(1100, l1pos["[200]"] - 120))
arrow(lx + lw, l1pos["[26f]"], px, pmid("2053A000") - 40, "#1e8449", curve=(1100, l1pos["[26f]"] - 60))
# L1[000] -> L2 table, L2 -> PA kalloc
arrow(lx + lw, l1pos["[000..01f]"], qx, qy + 18, "#d35400", sw=2, curve=(790, l1pos["[000..01f]"]))
arrow(qx + qw, qy + 70, px, pmid("2053A000") + 30, "#d35400", sw=2)
arrow(qx + qw, qy + 160, px, pmid("2053A000") + 45, "#d35400", sw=1.4, dash="5 3")
# mmap section -> framebuffer
arrow(lx + lw, l1pos["[100..17f]"], px, pmid("28000000") + 6, "#a93d8c", dash="5 3",
      curve=(800, pmid("28000000") + 6))
text(qx + 10, qy - 14, "紫色虚线：进程 mmap /dev/fb0（fbtest）时 L1[100..] 写成用户段 → 帧缓冲", 11.5, "#a93d8c")

# ------------------------------------------------------------ bottom boxes
by, bh = 1430, 290
rect(20, by, 880, bh, "#fff7ec", "#d35400")
text(36, by + 28, "⑤⑥ 让 MMU 看到新表：install() + 打开 MMU（MMU 关闭时执行，VA = PA 所以下一条指令照常取）", 15, "#d35400", weight="600")
refresh = [
    ("dsb", "前面写 L1 项的 str 全部完成，页表遍历器可见"),
    ("mcr c2,c0,2  TTBCR = 0", "N = 0：整个 4 GiB 都用 TTBR0（没有 TTBR1）"),
    (f"mcr c2,c0,0  TTBR0 = {ROOTS:#x}", "L1 表物理基址（必须 16 KiB 对齐）"),
    ("mcr c3,c0,0  DACR = 1", "domain 0 = client：按描述符的 AP 位检查权限"),
    ("dsb; mcr c8,c7,0; dsb; isb", "flush_tlb()：TLBIALL 作废全部 TLB 项"),
    ("SCTLR |= M; isb", "MMU 开；随后 setup.c 开 I-cache(I)、分支预测(Z)，D-cache(C) 保持关"),
    ("进程切换：switchuvm()", "只重写 TTBR0 = 进程自己的 L1 + flush_tlb()"),
]
for i, (a, b) in enumerate(refresh):
    text(40, by + 60 + i * 30, a, 12.5, "#333", mono=True)
    text(330, by + 60 + i * 30, b, 12.5, "#333")

rect(920, by, 860, bh, "#fdf0f0", "#c0392b")
text(936, by + 28, "需要注意", 15, "#c0392b", weight="600")
notes = [
    "• 两级而已：Cortex-A8 没有 LPAE，短描述符最多 L1 → L2；内核只用一级（段），用户才用到 L2",
    "• 没有高半区：内核恒等映射在 0x20000000 以上，用户窗口 0–0x1FFFFFFF 与它同在一张表",
    "• L1 根来自 bss 的 roots[256]（4 MiB），不是 kalloc；也决定了最多 256 个地址空间",
    "• L2 表一张只要 1 KiB，但 walk() 每张占一整页 kalloc（浪费 3 KiB）",
    "• 外设寄存器块小于 1 MiB 也占整段；未映射的 VA 访问 → Translation fault（打印 far/dfsr）",
    "• 内核段 AP=01：进程 L1 里看得到这些项，但用户态访问 → Permission fault",
    "• 只映射到 PHYSTOP（DRAM+112 MiB），再加 win32k / 帧缓冲；其余 380 MiB DRAM 不映射",
    "• 启动日志 MMU_TRACE=1 会打印合并后的 L1 段表和 TTBR0/TTBCR/DACR/SCTLR",
]
for i, n in enumerate(notes):
    text(936, by + 60 + i * 28, n, 12.5, "#333")
o("</svg>")
open(sys.argv[1], "w").write("\n".join(out))
