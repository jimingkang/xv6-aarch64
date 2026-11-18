"""xv6 rpi3-lock (Raspberry Pi 3, AArch64) memory animation, driven by a real
`make MMU_TRACE=3` QEMU log (tools/manim/rpi3-mmu-trace3.log).

Scenes
  MemoryInit   DTB -> memblock memory/reserved, 17 kernel table pages taken
               top-down below EARLYTOP, mem_map at the top of RAM,
               memblock_free_all() handing the gaps to the buddy allocator
  MmuSetup     stage 1: entry.S bootstrap tables (identity + high), MMU on
               and the jump to the high half; stage 2: kvmmake() building the
               TTBR1 tree region by region, TCR/MAIR/SCTLR, kvminithart()
  ProcessExec  fork (sh -> child: COW sharing) then exec("/bin/ls"): L1 root,
               L2 and L3 tables, ELF LOAD pages, ld.so, guard + stack,
               argv/auxv, switch TTBR0, free the old table

Render (from the repository root):
  tools/manim/render_rpi3.sh [l|m|h]   -> doc/readme/rpi3/videos/memory-anim.mp4
Log:  make MMU_TRACE=3 fs.img qemu-sd-image, boot, log in, run `ls /`
      (RPI3_ANIM_LOG overrides the path).
"""
import os
import re
from pathlib import Path

from manim import (
    BLUE, BLUE_E, DOWN, GREEN, GREEN_E, GREY, GREY_B, GREY_D, LEFT, ORANGE,
    PURPLE, RED, RIGHT, UP, WHITE, YELLOW, TEAL, Arrow, Create, CurvedArrow,
    FadeIn, FadeOut, GrowArrow, Indicate, Line, ReplacementTransform,
    Rectangle, Scene, SurroundingRectangle, Text, Transform, VGroup,
    config, ValueTracker, always_redraw, LaggedStart, AnimationGroup, DashedLine,
)

CJK = "Hiragino Sans GB"
MONO = "Menlo"
config.background_color = "#14171c"

LOG = Path(os.environ.get("RPI3_ANIM_LOG", Path(__file__).with_name("rpi3-mmu-trace3.log")))
LINES = LOG.read_text(errors="replace").splitlines()


def find(pattern, start=0):
    rx = re.compile(pattern)
    for i in range(start, len(LINES)):
        m = rx.search(LINES[i])
        if m:
            return m
    raise SystemExit(f"rpi3_memory_anim: '{pattern}' not in {LOG}")


def find_all(pattern, start=0, stop=None):
    rx = re.compile(pattern)
    out = []
    for line in LINES[start:stop]:
        m = rx.search(line)
        if m:
            out.append(m)
    return out


def index_of(pattern, start=0):
    rx = re.compile(pattern)
    for i in range(start, len(LINES)):
        if rx.search(LINES[i]):
            return i
    raise SystemExit(f"rpi3_memory_anim: '{pattern}' not in {LOG}")


def hx(s):
    return int(s, 16)


def T(s, size=24, color=WHITE, mono=False, **kw):
    return Text(s, font=MONO if mono else CJK, font_size=size, color=color, **kw)


def fit(m, w):
    if m.width > w:
        m.scale_to_fit_width(w)
    return m


def title(scene, s, sub=None):
    t = fit(T(s, 32).to_edge(UP, buff=0.2), 13.4)
    grp = VGroup(t)
    if sub:
        grp.add(fit(T(sub, 16, GREY_B).next_to(t, DOWN, buff=0.08), 13.4))
    scene.play(FadeIn(grp, shift=DOWN * 0.2), run_time=0.7)
    return grp


def caption(scene, old, s, color=YELLOW):
    new = fit(T(s, 20, color), 13.6).to_edge(DOWN, buff=0.25)
    if old is None:
        scene.play(FadeIn(new), run_time=0.4)
    else:
        scene.play(ReplacementTransform(old, new), run_time=0.4)
    return new


def box(w, h, col, label=None, size=14, opacity=0.7, mono=False):
    r = Rectangle(width=w, height=h, stroke_color=WHITE, stroke_width=1.1,
                  fill_color=col, fill_opacity=opacity)
    if label is None:
        return r
    t = fit(T(label, size, mono=mono), w - 0.12)
    if t.height > h - 0.06:
        t.scale_to_fit_height(h - 0.06)
    t.move_to(r)
    return VGroup(r, t)


# ---------------------------------------------------------------- scene 1
class MemoryInit(Scene):
    def construct(self):
        title(self, "1. 内存初始化：DTB → memblock → 伙伴系统",
              "QEMU raspi3b，全部数值来自 make MMU_TRACE=3 的启动日志")
        src = find(r"memblock: memory map from (\w+) at pa (\w+) \((\d+) bytes\), model \"([^\"]+)\"")
        mem = find(r"\[0\] (0x\w+)-(0x\w+) (\d+) KiB")
        rs = find_all(r"^\s+\[\d\] (0x\w+)-(0x\w+) (\d+) KiB", index_of(r"reserved.cnt"), index_of(r"^mmu: ----"))
        img = find(r"mmu: kernel image PA (\w+)-(\w+)")
        mm = find(r"mmu: page_alloc_init: mem_map pa (\w+), (\d+) struct page x (\d+) bytes = (\d+ \w+)")
        buddy = find_all(r"mmu:   buddy gets PA (\w+)-(\w+) \(([^,]+), (\d+) pages\)")
        ktabs = find_all(r"mmu:     (L\d (?:root|table)): memblock pa (\w+)")

        cap = caption(self, None, f"引导程序把 DTB 地址放在 x0：pa {src.group(2)}，{src.group(4)}")
        # physical bar, not to scale: (name, lo, hi, height, color, kind)
        segs = [
            ("0 页 / armstub", 0x0, 0x7ffff, 0.3, YELLOW),
            ("内核 .text", 0x80000, 0xbcfff, 0.32, BLUE),
            ("data + bss", 0xbd000, hx(img.group(2)), 0.34, BLUE_E),
            ("空闲", 0x1ba000, 0x3eefff, 0.3, GREY_D),
            ("内核页表 68 KiB", 0x3ef000, 0x3fffff, 0.32, ORANGE),
            ("空闲 108 MiB", 0x400000, 0x6ffffff, 0.75, GREY_D),
            ("carve-out 6 MiB", 0x7000000, 0x75fffff, 0.34, PURPLE),
            ("空闲 10 MiB", 0x7600000, 0x7ffffff, 0.3, GREY_D),
            ("DTB 87 KiB", 0x8000000, 0x8015d95, 0.3, YELLOW),
            ("空闲 ≈822 MiB", 0x8016000, 0x3b69ffff, 1.25, GREY_D),
            ("mem_map 9600 KiB", hx(mm.group(1)), 0x3bffffff, 0.34, TEAL),
        ]
        x0, y = -4.6, -3.0
        bars, labs = [], []
        for name, lo, hi, h, col in segs:
            r = box(2.2, h, col, opacity=0.25 if col == GREY_D else 0.75).move_to([x0, y + h / 2, 0])
            lab = VGroup(T(name, 14), T(f"{lo:08x}-{hi:08x}", 12, GREY_B, mono=True)) \
                .arrange(RIGHT, buff=0.15).next_to(r, RIGHT, buff=0.12)
            bars.append(r)
            labs.append(lab)
            y += h
        head = T(f"memory[0] {mem.group(1)[-8:]}-{mem.group(2)[-8:]}  {int(mem.group(3)) // 1024} MiB",
                 16, GREEN, mono=True).next_to(bars[-1], UP, buff=0.1).align_to(bars[0], LEFT)
        self.play(FadeIn(head), LaggedStart(*[Create(b) for b in bars], lag_ratio=0.05), run_time=1.4)

        # reserved list on the right
        rtitle = T("memblock.reserved", 20, ORANGE, mono=True).move_to([3.3, 2.45, 0])
        rows = VGroup(*[T(f"{hx(m.group(1)):08x}-{hx(m.group(2)):08x} {m.group(3)} KiB", 15, mono=True) for m in rs]) \
            .arrange(DOWN, aligned_edge=LEFT, buff=0.14).next_to(rtitle, DOWN, buff=0.2)
        why = ["0 页 + 内核镜像", "17 个内核页表页", "carve-out 窗口", "DTB 本身", "mem_map"]
        whys = VGroup(*[T(w, 13, GREY_B).next_to(rows[i], RIGHT, buff=0.15) for i, w in enumerate(why[:len(rows)])])
        cap = caption(self, cap, "memblock_init()：DTB /memory 给出 RAM；先预留 0 页+内核、carve-out 窗口和 DTB")
        self.play(FadeIn(rtitle), *[FadeIn(labs[i]) for i in (0, 1, 2, 6, 8)],
                  *[bars[i].animate.set_fill(opacity=0.9) for i in (0, 1, 2, 6, 8)], run_time=1.0)
        self.play(FadeIn(rows[0]), FadeIn(whys[0]), FadeIn(rows[2]), FadeIn(whys[2]),
                  FadeIn(rows[3]), FadeIn(whys[3]), run_time=0.8)
        self.wait(1.5)

        # 17 kernel table pages, top-down below EARLYTOP
        cap = caption(self, cap, f"kvminit() 用 memblock_alloc() 自 EARLYTOP(4 MiB) 向下取页：{len(ktabs)} 个页表页")
        kb = bars[4]
        grid = VGroup()
        for i, m in enumerate(ktabs):
            c = ORANGE if m.group(1) != "L1 root" else RED
            g = box(0.66, 0.3, c, m.group(2)[-6:], 10, mono=True)
            grid.add(g)
        grid.arrange_in_grid(rows=3, buff=0.06).move_to([3.7, -0.55, 0])
        self.play(FadeIn(labs[4]), kb.animate.set_fill(opacity=0.9), run_time=0.4)
        self.play(LaggedStart(*[ReplacementTransform(kb.copy().scale(0.2), g) for g in grid], lag_ratio=0.12),
                  run_time=2.4)
        lbl = T("根 0x3FF000 → L2 0x3FE000 → L3 …（红 = L1 根）", 13, GREY_B).next_to(grid, DOWN, buff=0.1)
        self.play(FadeIn(rows[1]), FadeIn(whys[1]), FadeIn(lbl), run_time=0.6)
        self.wait(1.5)

        cap = caption(self, cap, f"page_alloc_init()：mem_map = {mm.group(2)} 个 struct page × {mm.group(3)} B = {mm.group(4)}，放在 RAM 顶端")
        self.play(FadeIn(labs[10]), bars[10].animate.set_fill(opacity=0.9), FadeIn(rows[4]), FadeIn(whys[4]), run_time=0.8)
        self.wait(1.2)

        cap = caption(self, cap, "memblock_free_all()：memory − reserved 的空隙逐段交给伙伴系统 free_area[]")
        free_idx = [3, 5, 7, 9]
        counter = ValueTracker(0)
        num = always_redraw(lambda: VGroup(T("伙伴系统空闲页：", 18), T(f"{int(counter.get_value())}", 26, GREEN, mono=True))
                            .arrange(RIGHT).move_to([3.7, -2.35, 0]))
        self.add(num)
        total = 0
        for i, m in zip(free_idx, buddy):
            total += int(m.group(4))
            self.play(bars[i].animate.set_fill(GREEN_E, opacity=0.8), FadeIn(labs[i]),
                      counter.animate.set_value(total), run_time=0.7)
        mt = find(r"MemTotal:\s+(\d+) kB")
        cap = caption(self, cap, f"共 {total} 页；加上 per-CPU 缓存等即 /proc/meminfo 的 MemTotal {mt.group(1)} kB")
        self.wait(2.5)


# ---------------------------------------------------------------- scene 2
class MmuSetup(Scene):
    def construct(self):
        title(self, "2. 打开 MMU：两阶段页表",
              "阶段 1 entry.S 用 .bss 里 4 张静态表开 MMU；阶段 2 kvmmake() 建正式 TTBR1 表")
        bs = find(r"bss\s+PA \w+-\w+ \S+ KiB \(stack0 (\w+), boot tables l1entrypgt (\w+) l2entrypgt (\w+) l1kpgt (\w+) l2kpgt (\w+)\)")
        # ---- stage 1
        cap = caption(self, None, "MMU 关：entrypgtinit() 填两组 L1→L2，L2 用 2 MiB 块描述符")
        t0 = box(2.3, 0.6, BLUE_E, f"l1entrypgt {bs.group(2)}", 14, mono=True).move_to([-4.6, 1.6, 0])
        t0b = box(3.6, 0.6, BLUE_E, "l2entrypgt: [0..1] → PA 0–4 MiB", 14).move_to([-0.6, 1.6, 0])
        t1 = box(2.3, 0.6, BLUE, f"l1kpgt {bs.group(4)}", 14, mono=True).move_to([-4.6, 0.2, 0])
        t1b = box(3.6, 1.1, BLUE, "l2kpgt: [0..1] → PA 0–4 MiB\n[64..65] → DTB 0x8000000\n[505] → UART 块 (Device)", 13).move_to([-0.6, 0.0, 0])
        a0 = Arrow(t0.get_right(), t0b.get_left(), buff=0.05, stroke_width=3)
        a1 = Arrow(t1.get_right(), t1b.get_left(), buff=0.05, stroke_width=3)
        l0 = T("TTBR0", 18, GREEN, mono=True).next_to(t0, LEFT, buff=0.15)
        l1 = T("TTBR1", 18, ORANGE, mono=True).next_to(t1, LEFT, buff=0.15)
        self.play(FadeIn(t0), FadeIn(t1), FadeIn(l0), FadeIn(l1), run_time=0.6)
        self.play(GrowArrow(a0), FadeIn(t0b), GrowArrow(a1), FadeIn(t1b), run_time=1.0)
        self.wait(1.5)

        regs = find(r"mmu: TCR_EL1=(\w+):")
        mair = find(r"mmu: MAIR_EL1=(\w+):")
        sct = find(r"mmu: SCTLR_EL1=(\w+):")
        rg = VGroup(T(f"TCR_EL1  = {regs.group(1)}  T0SZ=T1SZ=25 → 39 位, 4K", 14, mono=True),
                    T(f"MAIR_EL1 = {mair.group(1)}  0:Device 1:NC 2:WB", 14, mono=True),
                    T(f"SCTLR_EL1 |= M | C | I  → {sct.group(1)}", 14, mono=True)).arrange(DOWN, aligned_edge=LEFT, buff=0.12)
        rg.move_to([3.9, 1.1, 0])
        fit(rg, 5.6)
        cap = caption(self, cap, "写 TTBR0/TTBR1、TCR、MAIR，tlbi vmalle1，再置 SCTLR.M：MMU 打开")
        self.play(FadeIn(rg, shift=LEFT * 0.2), run_time=0.9)
        self.wait(1.5)

        # PC jump low -> high
        low = T("PC = 0x0000_0000_000800xx（恒等映射取指）", 15, GREEN, mono=True).move_to([0, -1.6, 0])
        high = T("PC = 0xFFFFFF80_000800xx（_start，高地址）", 15, ORANGE, mono=True).move_to([0, -2.4, 0])
        self.play(FadeIn(low), run_time=0.5)
        cap = caption(self, cap, "br x1：跳到链接地址 KERNBASE+PA；恒等映射只为这一刻存在")
        self.play(ReplacementTransform(low.copy(), high), Indicate(t1b), run_time=1.0)
        self.wait(1.5)
        self.play(*[FadeOut(m) for m in (t0, t0b, t1, t1b, a0, a1, l0, l1, rg, low, high)], run_time=0.5)

        # ---- stage 2: kvmmake regions -> tree
        cap = caption(self, cap, "阶段 2：memblock_init(DTB) 后 kvmmake() 逐区建表（页表页来自 memblock）")
        root = box(1.7, 0.55, RED, "L1 根 3ff000", 14).move_to([-5.6, 0, 0])
        l2a = box(2.0, 0.55, ORANGE, "L2 3fe000 (0–1G)", 13).move_to([-3.3, 0.8, 0])
        l2b = box(2.0, 0.55, ORANGE, "L2 3f5000 (1–2G)", 13).move_to([-3.3, -1.6, 0])
        self.play(FadeIn(root), run_time=0.4)
        self.play(FadeIn(l2a), Create(Arrow(root.get_right(), l2a.get_left(), buff=0.05, stroke_width=2.5)), run_time=0.5)
        leaves = [
            ("外设 16 MiB → 8 张 L3, Device", RED, "BCM2837 peripherals"),
            ("ARM local → L2 3f5000 → L3", RED, "ARM local"),
            ("ramdisk / 摄像头 DMA(NC) / 模块区(RWX) → 3 张 L3", PURPLE, "ramdisk"),
            ("第 0 页 + .text(RO, 可执行) → L3 3f0000", BLUE, "page 0"),
            ("data+bss+早期 RAM → L3 3ef000 (RW XN)", BLUE_E, "kernel data"),
            ("线性映射 RAM：475 个 2 MiB 块", GREEN_E, "linear map"),
        ]
        ys = [2.0, -1.6, 1.2, 0.4, -0.4, -1.0]
        made_b = False
        for (txt, col, _), yy in zip(leaves, ys):
            lf = box(5.6, 0.48, col, txt, 14).move_to([2.6, yy, 0])
            if "ARM local" in txt:
                if not made_b:
                    self.play(FadeIn(l2b), Create(Arrow(root.get_right(), l2b.get_left(), buff=0.05, stroke_width=2.5)),
                              run_time=0.4)
                    made_b = True
                lf.move_to([2.6, -1.6, 0])
                src = l2b
            else:
                src = l2a
            self.play(FadeIn(lf, shift=LEFT * 0.2),
                      Create(Arrow(src.get_right(), lf.get_left(), buff=0.05, stroke_width=2, color=GREY_B)),
                      run_time=0.8)
        sm = find(r"mmu: kernel table uses (\d+) table pages \((\d+ \w+)\): (\d+) x 2 MiB blocks, (\d+) x 4 KiB pages")
        cap = caption(self, cap, f"共 {sm.group(1)} 个表页（{sm.group(2)}）：{sm.group(3)} 个 2 MiB 块 + {sm.group(4)} 个 4 KiB 页")
        self.wait(1.5)
        kh = find(r"kvminithart: TTBR1=(\w+)")
        cap = caption(self, cap, f"kvminithart()：TTBR1 = {kh.group(1)}，TTBR0 = 0，flush_tlb() —— 启动表退役")
        self.play(Indicate(root, color=YELLOW), run_time=1.0)
        dm = find(r"DirectMap4k (\d+ \w+), DirectMap2M (\d+ \w+)")
        cap = caption(self, cap, f"/proc/meminfo：DirectMap4k {dm.group(1)}，DirectMap2M {dm.group(2)}")
        self.wait(2.5)


# ---------------------------------------------------------------- scene 3
class ProcessExec(Scene):
    def construct(self):
        title(self, "3. 进程装载：fork + exec(\"/bin/ls\")",
              "TTBR0 每进程一张：L1 根 → L2 → L3 → 用户页（MMU_TRACE=3 逐页日志）")
        e0 = index_of(r"^exec: /bin/ls pid")
        f0 = max(i for i in range(e0) if LINES[i].startswith("mmu: fork: sh"))
        fs = max(i for i in range(f0) if "L1 root: kalloc" in LINES[i])
        fork = find(r"mmu: fork: (\w+) pid (\d+) -> pid (\d+): kstack VA (\w+) pa (\w+), child root pa (\w+), sz (\w+): (\d+) user pages", f0)
        cows = find_all(r"mmu:   cow: VA (\w+) pa (\w+) .*refcount (\d+)\), (.*)$", fs, f0)

        # ---- fork
        cap = caption(self, None, f"fork：{fork.group(1)} pid {fork.group(2)} → pid {fork.group(3)}，"
                                  f"内核栈 VA {fork.group(4)}（TTBR1 的 KSTACK 区）")
        par = T(f"父 pid {fork.group(2)} L3", 16, BLUE).move_to([-4.5, 2.1, 0])
        chi = T(f"子 pid {fork.group(3)} L3（root {fork.group(6)}）", 16, GREEN).move_to([4.2, 2.1, 0])
        ppg = VGroup()
        cpg = VGroup()
        phys = VGroup()
        for k, m in enumerate(cows[:10]):
            p = box(2.4, 0.33, BLUE_E, f"VA {hx(m.group(1)):5x}", 12, mono=True)
            c = box(2.4, 0.33, GREEN_E, f"VA {hx(m.group(1)):5x}", 12, mono=True)
            ph = box(2.2, 0.33, GREY_D, f"pa {m.group(2)}", 12, mono=True)
            ppg.add(p)
            cpg.add(c)
            phys.add(ph)
        for g, x in ((ppg, -4.5), (phys, 0), (cpg, 4.2)):
            g.arrange(DOWN, buff=0.06).move_to([x, -0.1, 0])
        self.play(FadeIn(par), FadeIn(ppg), FadeIn(phys), run_time=0.7)
        lines_p = VGroup(*[Line(ppg[k].get_right(), phys[k].get_left(), color=BLUE, stroke_width=1.5) for k in range(len(ppg))])
        self.play(Create(lines_p), run_time=0.5)
        self.wait(1.0)
        cap = caption(self, cap, "uvmcopy：子表 walk() 建 L2/L3，PTE 指向同一物理页，refcount = 2")
        lines_c = VGroup(*[Line(cpg[k].get_left(), phys[k].get_right(), color=GREEN, stroke_width=1.5) for k in range(len(cpg))])
        self.play(FadeIn(chi), LaggedStart(*[FadeIn(c) for c in cpg], lag_ratio=0.08), Create(lines_c), run_time=1.4)
        tags = VGroup()
        for k, m in enumerate(cows[:10]):
            txt = "RO+COW" if "COW" in m.group(4) else "guard"
            tags.add(T(txt, 11, RED if txt == "RO+COW" else GREY_B, mono=True).next_to(ppg[k], LEFT, buff=0.08))
        self.play(FadeIn(tags), *[p[0].animate.set_fill(RED, opacity=0.35) for p in ppg], run_time=0.7)
        cap = caption(self, cap, f"{fork.group(8)} 个用户页一个没复制：可写页在父子两边都改成只读 + PTE_COW")
        self.wait(2.0)
        self.play(*[FadeOut(m) for m in (par, chi, ppg, cpg, phys, lines_p, lines_c, tags)], run_time=0.5)

        # ---- exec
        ex = find(r"exec: (/bin/ls) pid (\d+): inode (\d+), (\d+) bytes, (\w+) ELF, entry (\w+), (\d+) program headers", e0)
        cap = caption(self, cap, f"exec {ex.group(1)}：inode {ex.group(3)}，{ex.group(4)} 字节，{ex.group(5)}，{ex.group(7)} 个程序头")
        ev = LINES[e0:index_of(r"^exec: /bin/ls pid \d+: sz", e0) + 1]
        root = find(r"L1 root: kalloc pa (\w+)", e0)
        l2 = find(r"L2 table: kalloc pa (\w+) -> user L1\[(\d+)\] = (\w+)", e0)
        l3 = find(r"L3 table: kalloc pa (\w+) -> user L2\[(\d+)\] = (\w+)", e0)
        r_box = box(2.2, 0.55, RED, f"L1 根 {root.group(1)}", 13, mono=True).move_to([-5.5, 1.9, 0])
        l2_box = box(2.2, 0.55, ORANGE, f"L2 {l2.group(1)}", 13, mono=True).move_to([-5.5, 0.7, 0])
        l3_box = box(2.2, 0.55, YELLOW, f"L3 {l3.group(1)}", 13, mono=True).move_to([-5.5, -0.5, 0])
        self.play(FadeIn(r_box), run_time=0.5)
        cap = caption(self, cap, "uvmcreate() 一张空 L1；uvmalloc 第一次 mappages() 时 walk() 建 L2、L3")
        self.play(FadeIn(l2_box), Create(Arrow(r_box.get_bottom(), l2_box.get_top(), buff=0.04, stroke_width=2.5)),
                  run_time=0.6)
        self.play(FadeIn(l3_box), Create(Arrow(l2_box.get_bottom(), l3_box.get_top(), buff=0.04, stroke_width=2.5)), run_time=0.6)

        # L3 slots 0..9 (VA 0x0000..0x9000), physical page column
        slots = VGroup(*[box(1.6, 0.42, GREY_D, f"L3[{k}] VA {k:x}000", 12, opacity=0.25, mono=True) for k in range(10)])
        slots.arrange(UP, buff=0.05).move_to([-2.3, -0.1, 0])
        self.play(FadeIn(slots), Create(Arrow(l3_box.get_right(), slots.get_left(), buff=0.08, stroke_width=2)), run_time=0.6)

        pages = find_all(r"mmu:   page: kalloc pa (\w+) -> VA (\w+) \(L3\[(\d+)\] = (\w+)")
        pages = [m for m in pages if any(m.group(0) in l for l in ev)]
        loads = [re.search(r"LOAD VA (\w+) memsz (\w+) filesz (\w+) file off (\w+) flags (\S+)", l) for l in ev]
        loads = [m for m in loads if m]
        stack = re.search(r"guard page VA (\w+) \(PTE_U cleared\), stack VA (\w+)-(\w+)", "\n".join(ev))
        fin = re.search(r"sz (\w+) sp (\w+) entry (\w+), (\d+) table pages \+ (\d+) user pages, TTBR0 = root (\w+)", "\n".join(ev))

        info = VGroup()
        X0 = 0.9

        def say(s, col=WHITE):
            """Append a log line on the right; returns the animations."""
            t = fit(T(s, 14, col), 6.0)
            anims = []
            info.add(t)
            if len(info) > 11:
                old = info[0]
                info.remove(old)
                anims.append(FadeOut(old))
            info.arrange(DOWN, aligned_edge=LEFT, buff=0.12)
            info.move_to([X0, 2.1, 0], aligned_edge=UP + LEFT)
            anims.append(FadeIn(t))
            return anims

        kind = {0: (BLUE, "ls .text"), 1: (BLUE_E, "ls .data"), 2: (ORANGE, "ld.so .text"),
                3: (ORANGE, "ld.so .data")}
        li = 0
        for m in pages:
            k = int(m.group(3))
            va = hx(m.group(2))
            if k in kind:
                col, nm = kind[k]
            elif stack and va == hx(stack.group(1)):
                col, nm = GREY, "保护页"
            else:
                col, nm = "#c0582b", "用户栈"
            if k == 2 and li < len(loads):
                cap = caption(self, cap, "PT_INTERP：在 PGROUNDUP(sz) = 0x2000 处再装 ld-xv6.so")
            if k == 4:
                cap = caption(self, cap, f"再 uvmalloc 5 页：第一页 uvmclear() 去掉 PTE_U 成保护页，其余 4 页是栈")
            new = box(1.6, 0.42, col, f"{nm}", 12, opacity=0.8).move_to(slots[k])
            an = say(f"kalloc pa {m.group(1)} → VA {va:x}  PTE {m.group(4)}", GREY_B)
            self.play(Transform(slots[k], new), *an, run_time=0.7)
            # after the 2 pages of each image, show the LOAD reads
            if k in (1, 3):
                for _ in range(2):
                    if li < len(loads):
                        L = loads[li]
                        an = say(f"LOAD VA {L.group(1)} filesz {L.group(3)} {L.group(5)} → readi 拷入", YELLOW)
                        li += 1
                        self.play(*an, run_time=0.6)
        cap = caption(self, cap, f"copyout argv/envp/auxv 到栈顶，sp = {fin.group(2)}；elr = ld.so 入口 {fin.group(3)}")
        an = say("PTE 0x…74B：Normal-WB, AP=01(EL0 RW), AF, ISH, 无 XN", RED)
        self.play(*an, Indicate(slots[2]), run_time=0.8)
        self.wait(1.5)
        cap = caption(self, cap, f"提交：vm->pagetable = 新表，switchuvm() 写 TTBR0 = {fin.group(6)}，旧 COW 表 uvmfree")
        self.play(Indicate(r_box, color=YELLOW), run_time=0.8)
        self.wait(1.0)
        cap = caption(self, cap, f"结果：{fin.group(4)} 个表页 + {fin.group(5)} 个用户页；libc.so 之后由 ld.so 在用户态 mmap 进来")
        self.wait(3.0)
