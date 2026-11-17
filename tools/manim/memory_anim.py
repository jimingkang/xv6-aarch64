"""xv6 Tiny210 (ARM32) memory animation, driven by a real MMU_TRACE=3 log.

Scenes
  MemoryInit    DRAM layout, kernel image, kinit() filling the kalloc freelist
  MmuSetup      kvminit() on the real Tiny210, laid out like memory-layout.svg:
                descriptor format, rootalloc, DRAM / win32k / framebuffer / MMIO
                sections with their descriptor bits, unmapped ranges, install()
                + SCTLR.M, two table walks, per-process L1 copies
  ProcessExec   exec("/bin/sh"): L1 root copy, L2 table, kalloc'd pages,
                readi() of the ELF LOAD segment, user stack, switch TTBR0

Render (from the repository root):
  manim -qm tools/manim/memory_anim.py MemoryInit MmuSetup ProcessExec
  tools/manim/render.sh            # all three + concatenated mp4

The log comes from the QEMU stand-in (DRAM at 0x40000000):
  make -f ports/tiny210/Makefile BUILD=build/tiny210-qemu QEMU_TEST=1 MMU_TRACE=3 qemu
and is kept in tools/manim/qemu-mmu-trace3.log (MEMORY_ANIM_LOG overrides).
"""
import os
import re
from pathlib import Path

from manim import (
    BLUE, BLUE_E, DOWN, GREEN, GREEN_E, GREY, GREY_B, GREY_D, LEFT, ORANGE,
    PURPLE, RED, RIGHT, UP, WHITE, YELLOW, Arrow, Create, CurvedArrow,
    FadeIn, FadeOut, GrowArrow, Indicate, Line, ReplacementTransform,
    Rectangle, Scene, SurroundingRectangle, Text, Transform, VGroup, Write,
    config, ValueTracker, always_redraw, LaggedStart, AnimationGroup,
)

CJK = "Hiragino Sans GB"
MONO = "Menlo"
config.background_color = "#14171c"

LOG = Path(os.environ.get("MEMORY_ANIM_LOG", Path(__file__).with_name("qemu-mmu-trace3.log")))
LINES = LOG.read_text(errors="replace").splitlines()


def find(pattern, default=None):
    rx = re.compile(pattern)
    for line in LINES:
        m = rx.search(line)
        if m:
            return m
    if default is None:
        raise SystemExit(f"memory_anim: '{pattern}' not in {LOG}")
    return default


def hx(s):
    return int(s, 16)


def T(s, size=24, color=WHITE, mono=False, **kw):
    return Text(s, font=MONO if mono else CJK, font_size=size, color=color, **kw)


def title(scene, s, sub=None):
    t = T(s, 32).to_edge(UP, buff=0.2)
    if t.width > 13.4:
        t.scale_to_fit_width(13.4)
    grp = VGroup(t)
    if sub:
        st = T(sub, 16, GREY_B).next_to(t, DOWN, buff=0.08)
        if st.width > 13.4:
            st.scale_to_fit_width(13.4)
        grp.add(st)
    scene.play(FadeIn(grp, shift=DOWN * 0.2), run_time=0.8)
    return grp


def caption(scene, old, s, color=YELLOW):
    new = T(s, 20, color)
    if new.width > 13.6:
        new.scale_to_fit_width(13.6)
    new.to_edge(DOWN, buff=0.25)
    if old is None:
        scene.play(FadeIn(new), run_time=0.5)
    else:
        scene.play(ReplacementTransform(old, new), run_time=0.5)
    return new


# ---------------------------------------------------------------- scene 1
class MemoryInit(Scene):
    def construct(self):
        title(self, "1. 内存初始化：DRAM、内核镜像与 kinit()",
              "QEMU 替身 DRAM 0x40000000（实板 0x20000000，结构相同）—— 数据来自 MMU_TRACE 日志")
        ram = find(r"mm: region (\w+)-(\w+) System RAM")
        code = find(r"mm: kernel code (\w+)-(\w+) \((\d+) KiB")
        data = find(r"mm: kernel data (\w+)-(\w+) \((\d+) KiB")
        rd = find(r"mm: kernel ramdisk (\w+)-(\w+) \((\d+) KiB", default=False)
        bss = find(r"mm: kernel bss\s+(\w+)-(\w+) \((\d+) KiB")
        kin = find(r"mm: kinit: freerange (\w+)-(\w+) -> (\d+) pages \((\d+) KiB\).*PHYSTOP=(\w+)")
        gui = find(r"mm: region (\w+)-(\w+) win32k")
        unused = find(r"mm: region (\w+)-(\w+) Unused")

        # Not to scale: (label, lo, hi, height, color)
        regs = [
            ("ATAG / 零页", ram.group(1), "40007fff", 0.28, YELLOW),
            (f"内核 code {code.group(3)} KiB", code.group(1), code.group(2), 0.36, BLUE),
            (f"内核 data {data.group(3)} KiB", data.group(1), data.group(2), 0.3, BLUE_E),
        ]
        if rd:
            regs.append((f"内嵌根文件系统 {rd.group(3)} KiB", rd.group(1), rd.group(2), 0.42, "#4a6fa5"))
        regs += [
            (f"内核 bss {bss.group(3)} KiB（L1 根池 4 MiB）", bss.group(1), bss.group(2), 0.5, "#3d5a80"),
            (f"kalloc 页池 {int(kin.group(4)) // 1024} MiB", kin.group(1), kin.group(2), 1.7, GREEN_E),
            ("win32k 表面缓存区 16 MiB", gui.group(1), gui.group(2), 0.36, PURPLE),
            ("帧缓冲 4 MiB", "48000000", "483fffff", 0.32, "#8e5ba8"),
            ("未使用 DRAM", unused.group(1), unused.group(2), 0.7, GREY_D),
        ]
        x0, y = -3.4, -3.2
        bar = VGroup()
        labels = VGroup()
        boxes = {}
        for name, lo, hi, h, col in regs:
            r = Rectangle(width=2.1, height=h, stroke_color=WHITE, stroke_width=1.2,
                          fill_color=col, fill_opacity=0.75).move_to([x0, y + h / 2, 0])
            lab = VGroup(T(name, 15), T(f"{hx(lo):08x}-{hx(hi):08x}", 12, GREY_B, mono=True)) \
                .arrange(RIGHT, buff=0.15).next_to(r, RIGHT, buff=0.15)
            bar.add(r)
            labels.add(lab)
            boxes[name.split()[0]] = r
            y += h
        head = T(f"System RAM {hx(ram.group(1)):08x}-{hx(ram.group(2)):08x}", 18, GREEN, mono=True) \
            .next_to(bar, UP, buff=0.12).align_to(bar, LEFT)
        cap = caption(self, None, "BL1 / QEMU 把内核放到 DRAM+0x8000，ATAG 放在 DRAM+0x100，然后跳到 _entry")
        self.play(FadeIn(head), Create(bar[0]), FadeIn(labels[0]), run_time=1)
        n_img = 4 if rd else 3
        self.play(LaggedStart(*[AnimationGroup(Create(bar[i]), FadeIn(labels[i])) for i in range(1, n_img)],
                              lag_ratio=0.5), run_time=2)
        cap = caption(self, cap, ".bss 清零：其中 roots[256][4096] 是 4 MiB 的 L1 页表根池")
        self.play(Create(bar[n_img]), FadeIn(labels[n_img]), run_time=1)

        # kinit
        kbox = bar[n_img + 1]
        cap = caption(self, cap, f"kinit1(end, PHYSTOP)：freerange 把 {kin.group(1)}-{kin.group(2)} 逐页 kfree 进空闲链表")
        self.play(Create(kbox), FadeIn(labels[n_img + 1]), run_time=1)
        phys = hx(kin.group(5))
        pline = Line(kbox.get_corner(UP + LEFT) + LEFT * 0.4, kbox.get_corner(UP + RIGHT) + RIGHT * 0.1,
                     color=RED, stroke_width=4)
        ptxt = T(f"PHYSTOP\n{phys:08x}", 15, RED, mono=True).next_to(pline, LEFT, buff=0.08)
        self.play(Create(pline), FadeIn(ptxt))

        fl_title = T("kmem.freelist", 22, GREEN, mono=True).move_to([4.6, 2.2, 0])
        counter = ValueTracker(0)
        num = always_redraw(lambda: VGroup(
            T("空闲页：", 20),
            T(f"{int(counter.get_value())}", 30, GREEN, mono=True)
        ).arrange(RIGHT).move_to([4.6, 1.6, 0]))
        self.play(FadeIn(fl_title), FadeIn(num))
        lo = hx(kin.group(1))
        pages = VGroup()
        for k in range(6):
            pg = VGroup(Rectangle(width=2.4, height=0.42, stroke_color=GREEN, fill_color=GREEN_E,
                                  fill_opacity=0.6),
                        T(f"page {lo + k * 4096:08x}", 14, mono=True))
            pg[1].move_to(pg[0])
            pages.add(pg)
        pages.arrange(DOWN, buff=0.22).move_to([4.6, -0.8, 0])
        for k, pg in enumerate(pages):
            start = pg.copy().scale(0.3).move_to(kbox.get_center() + UP * (0.8 - 0.3 * k))
            self.play(Transform(start, pg), counter.animate.set_value(k + 1), run_time=0.35)
            self.add(pg)
            self.remove(start)
            if k:
                self.add(Arrow(pages[k].get_top(), pages[k - 1].get_bottom(), buff=0.02, stroke_width=2,
                               max_tip_length_to_length_ratio=0.3, color=GREEN))
        dots = T("…  r->next 链接，新释放的页插在表头", 16, GREY_B).next_to(pages, DOWN, buff=0.2)
        self.play(FadeIn(dots), counter.animate.set_value(int(kin.group(3))), run_time=2.5)
        cap = caption(self, cap, f"共 {kin.group(3)} 页 = {kin.group(4)} KiB = /proc/meminfo 的 MemTotal")
        self.wait(0.8)
        cap = caption(self, cap, "PHYSTOP 以上的 win32k 区、帧缓冲和剩余 DRAM 不交给 kalloc（D-cache 关闭，只用 112 MiB）")
        self.play(LaggedStart(*[AnimationGroup(Create(bar[i]), FadeIn(labels[i]))
                                for i in range(n_img + 2, len(bar))], lag_ratio=0.4), run_time=2)
        self.wait(2)


# ---------------------------------------------------------------- scene 2
class MmuSetup(Scene):
    """kvminit()/kvminithart() on the real Tiny210, laid out like
    doc/readme/tiny210/images/memory-layout.svg: virtual column (left),
    physical column (right), one row per region, high addresses on top."""

    # Board constants (memlayout.h, devices.c; roots from `nm kernel.elf`).
    DRAM, PHYSTOP, ROOTS = 0x20000000, 0x27000000, 0x200D4000
    DEVICES = [  # name, PA start, size, resource
        ("s5pv210-clock", 0xE0100000, 0x10000, "clock"),
        ("s5pv210-gpio", 0xE0200000, 0x1000, "gpio (+keys)"),
        ("s5pv210-pwm", 0xE2500000, 0x400, "pwm"),
        ("s3c2410-wdt", 0xE2700000, 0x400, "wdt"),
        ("s5pv210-uart", 0xE2900000, 0x400, "uart0"),
        ("s3c-sdhci", 0xEB000000, 0x100000, "hsmmc0"),
        ("s5p-ehci", 0xEC100000, 0x1000, "usb-phy"),
        ("s5p-ehci", 0xEC200000, 0x100000, "ehci"),
        ("s5pv210-vic", 0xF2000000, 0x400000, "vic0-3"),
        ("s3c-fb", 0xF8000000, 0x10000, "fimd"),
    ]

    def construct(self):
        self.cap = None
        title(self, "2. MMU 页表建立：kvminit() / kvminithart()（实板 Tiny210）",
              "按 memory-layout.svg：左 = 虚拟地址，右 = 物理地址；ARMv7 短描述符，L1 每项 1 MiB")
        self.intro_descriptor()
        self.build_columns()
        self.step_root()
        self.step_dram()
        self.step_gui_fb()
        self.step_mmio()
        self.step_unmapped()
        self.step_enable()
        self.step_walk_examples()
        self.step_per_process()
        self.wait(3.5)

    # ---------------------------------------------------------- helpers
    def say(self, s, color=YELLOW):
        self.cap = caption(self, self.cap, s, color)

    def panel(self, *mobs, wait=0.0):
        """Replace the right-hand explanation panel."""
        new = VGroup(*mobs).arrange(DOWN, aligned_edge=LEFT, buff=0.16)
        if new.width > 6.0:
            new.scale_to_fit_width(6.0)
        if new.height > 5.4:
            new.scale_to_fit_height(5.4)
        new.move_to([3.9, -0.27, 0])
        old = getattr(self, "pan", None)
        if old is not None:
            self.play(FadeOut(old), run_time=0.3)
        self.play(FadeIn(new, shift=LEFT * 0.2), run_time=0.6)
        self.pan = new
        if wait:
            self.wait(wait)

    def code(self, *lines):
        return VGroup(*[T(l, 14, "#9cdcfe", mono=True) for l in lines]).arrange(DOWN, aligned_edge=LEFT, buff=0.05)

    def bits(self, value, kind):
        tex, ap, xn, c, b = (value >> 12) & 7, (value >> 10) & 3, (value >> 4) & 1, (value >> 3) & 1, (value >> 2) & 1
        fields = [("PA[31:20]", f"{value >> 20:03x}", BLUE), ("TEX", f"{tex:03b}", GREEN), ("AP", f"{ap:02b}", YELLOW),
                  ("XN", f"{xn}", RED), ("C", f"{c}", PURPLE), ("B", f"{b}", PURPLE), ("类型", "10", GREY_B)]
        row = VGroup()
        for name, val, col in fields:
            cell = VGroup(Rectangle(width=0.78, height=0.48, stroke_color=col, fill_color=col, fill_opacity=0.18),
                          T(val, 15, col, mono=True), T(name, 11, GREY_B))
            cell[1].move_to(cell[0])
            cell[2].next_to(cell[0], DOWN, buff=0.03)
            row.add(cell)
        row.arrange(RIGHT, buff=0.04)
        head = T(f"段描述符 = {value:08x}  →  {kind}", 15, mono=False)
        return VGroup(head, row).arrange(DOWN, aligned_edge=LEFT, buff=0.08)

    # ---------------------------------------------------------- 0. format
    def intro_descriptor(self):
        va = VGroup()
        for name, w, col, sub in [("[31:20]  L1 索引", 2.6, ORANGE, "4096 项 × 1 MiB"),
                                  ("[19:12]  L2 索引", 2.0, GREEN, "256 项 × 4 KiB"),
                                  ("[11:0]  页内偏移", 1.8, BLUE, "")]:
            r = VGroup(Rectangle(width=w, height=0.6, stroke_color=col, fill_color=col, fill_opacity=0.2),
                       T(name, 15, col, mono=True))
            r[1].move_to(r[0])
            if sub:
                r.add(T(sub, 13, GREY_B).next_to(r[0], DOWN, buff=0.05))
            va.add(r)
        va.arrange(RIGHT, buff=0.04, aligned_edge=UP).move_to([0, 1.3, 0])
        lab = T("32 位虚拟地址", 18).next_to(va, UP, buff=0.15)
        kinds = VGroup(
            T("L1 项低 2 位：", 17),
            T("00  缺页（fault）—— 访问即 Data Abort", 16, GREY_B),
            T("01  指向 L2 页表（1 KiB，256 个 4 KiB 小页）—— 用户进程用", 16, GREEN),
            T("10  段（section）—— 直接映射 1 MiB；内核全部用段", 16, ORANGE),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.1).move_to([0, -1.0, 0])
        self.say("ARMv7 短描述符格式：VA 高 12 位查 L1 表；内核只用 1 MiB 段，所以内核页表只有一级")
        self.play(FadeIn(lab), LaggedStart(*[FadeIn(v) for v in va], lag_ratio=0.3), run_time=1.5)
        self.play(FadeIn(kinds, shift=UP * 0.1), run_time=1)
        self.wait(4)
        self.play(FadeOut(VGroup(lab, va, kinds)), run_time=0.5)

    # ---------------------------------------------------------- columns
    def build_columns(self):
        # key, height, color, PA label, mapped kind
        rows = [
            ("hi", 0.5, RED, "F2000000–F800FFFF  VIC、FIMD"),
            ("io", 0.85, RED, "E0100000–EC2FFFFF  外设寄存器"),
            ("gap", 0.34, GREY_D, "40000000–DFFFFFFF  无 DRAM"),
            ("unused", 0.36, GREY_D, "28400000–3FFFFFFF  未使用 DRAM"),
            ("fb", 0.3, "#8e5ba8", "28000000–283FFFFF  帧缓冲"),
            ("gui", 0.3, PURPLE, "27000000–27FFFFFF  win32k"),
            ("kalloc", 0.7, GREEN_E, "2053A000–26FFFFFF  kalloc"),
            ("bss", 0.38, "#3d5a80", "20040000–20539FFF  bss(roots)"),
            ("data", 0.24, BLUE_E, "2002F000–2003FFFF  data"),
            ("code", 0.24, BLUE, "20008000–2002EFFF  code"),
            ("atag", 0.22, YELLOW, "20000000–20007FFF  ATAG"),
            ("low", 1.25, GREY_D, "00000000–1FFFFFFF  iROM 等"),
        ]
        top = 2.55
        self.R = {}
        self.vrow = {}
        cols = VGroup()
        for key, h, col, lab in rows:
            y = top - h / 2
            pr = Rectangle(width=2.6, height=h, stroke_color=GREY, stroke_width=1, fill_color=col, fill_opacity=0.25) \
                .move_to([-0.9, y, 0])
            pt = T(lab, 10, WHITE).move_to(pr)
            if pt.width > 2.5:
                pt.scale_to_fit_width(2.5)
            self.R[key] = VGroup(pr, pt)
            cols.add(self.R[key])
            if key != "low":
                vr = Rectangle(width=2.6, height=h, stroke_color=GREY, stroke_width=1, fill_color="#22262e",
                               fill_opacity=1).move_to([-5.2, y, 0])
                vt = T("缺页", 10, GREY).move_to(vr)
                self.vrow[key] = VGroup(vr, vt)
                cols.add(self.vrow[key])
            top -= h
        # Virtual user window, split like the SVG.
        low = self.R["low"][0]
        y = low.get_top()[1]
        for key, h, lab in [("vgap2", 0.22, "18000000–1FFFFFFF"), ("vmmap", 0.36, "10000000–17FFFFFF mmap 窗口"),
                            ("vgap1", 0.22, "02000000–0FFFFFFF"), ("vuser", 0.45, "00000000–01FFFFFF 用户")]:
            vr = Rectangle(width=2.6, height=h, stroke_color=GREY, stroke_width=1, fill_color="#22262e",
                           fill_opacity=1).move_to([-5.2, y - h / 2, 0])
            vt = T(lab, 10, GREY).move_to(vr)
            self.vrow[key] = VGroup(vr, vt)
            cols.add(self.vrow[key])
            y -= h
        heads = VGroup(T("虚拟地址（L1 索引 = VA>>20）", 14).move_to([-5.2, 2.8, 0]),
                       T("物理地址", 14).move_to([-0.9, 2.8, 0]))
        maxva = Line([-6.6, low.get_top()[1], 0], [0.45, low.get_top()[1], 0], color=RED, stroke_width=2.5)
        mtxt = T("↑ 内核 / ↓ 用户窗口  MAXVA = 0x20000000", 11, RED) \
            .next_to(maxva, DOWN, buff=0.04).align_to(self.R["low"][0], LEFT).shift(RIGHT * 0.05)
        self.count = 0
        self.ctxt = T("已写 L1 段描述符：0 / 4096", 14, GREEN).move_to([3.9, 2.75, 0])
        self.say("开始时（rootalloc 刚返回）整张 L1 表全是 0：任何地址都缺页")
        self.play(FadeIn(heads), FadeIn(cols, lag_ratio=0.03), Create(maxva), FadeIn(mtxt), FadeIn(self.ctxt),
                  run_time=2)
        self.maxva = VGroup(maxva, mtxt)

    def bump(self, n):
        self.count += n
        new = T(f"已写 L1 段描述符：{self.count} / 4096", 14, GREEN).move_to(self.ctxt)
        self.play(Transform(self.ctxt, new), run_time=0.3)

    def map_rows(self, keys, color, label, arrows=True):
        anims = []
        for k in keys:
            vr = self.vrow[k]
            anims.append(vr[0].animate.set_fill(color, opacity=0.55))
            anims.append(Transform(vr[1], T(label(k), 10, WHITE).move_to(vr[0])))
        self.play(*anims, run_time=0.8)
        if arrows:
            self.play(*[GrowArrow(Arrow(self.vrow[k][0].get_right(), self.R[k][0].get_left(), buff=0.03,
                                        stroke_width=2, color=color, max_tip_length_to_length_ratio=0.06))
                        for k in keys], run_time=0.6)

    # ---------------------------------------------------------- steps
    def step_root(self):
        self.panel(
            T("① rootalloc()", 18, ORANGE),
            self.code("kernel_pagetable = rootalloc();",
                      "// roots[256][4096] 在 .bss，16 KiB 对齐"),
            T(f"内核 L1 表 = roots[0] @ {self.ROOTS:08x}", 15),
            T("4096 项 × 4 字节 = 16 KiB，覆盖整个 4 GiB", 15, GREY_B),
        )
        self.say(f"kvminit() 先从 bss 里的静态池取 roots[0]（{self.ROOTS:08x}）做内核 L1 表，并清零")
        self.play(Indicate(self.R["bss"], color=ORANGE), run_time=1.2)
        self.wait(2.5)

    def step_dram(self):
        n = (self.PHYSTOP - self.DRAM) >> 20
        e = self.DRAM | (1 << 12) | (1 << 10) | 2
        self.panel(
            T("② DRAM 恒等映射（vm.c kvminit）", 18, GREEN),
            self.code("for(a = EXTMEM; a < PHYSTOP; a += 1 MiB)",
                      "  L1[a>>20] = a | (1<<12) | (1<<10) | 2;"),
            T(f"L1[200..26f]：VA {self.DRAM:08x}–{self.PHYSTOP - 1:08x} = PA，共 {n} 段", 15),
            self.bits(e, "Normal 不可缓存，可执行，仅特权级读写"),
            T("TEX=001 C=0 B=0：Normal 不可缓存（D-cache 关）", 13, GREY_B),
            T("AP=01：特权级读写，用户态访问会 Permission fault", 13, GREY_B),
        )
        self.say(f"DRAM 从 ATAG 到 PHYSTOP（+112 MiB）逐 MiB 写段描述符：VA = PA，ATAG、内核镜像、kalloc 页池都在这里")
        self.map_rows(["atag", "code", "data", "bss", "kalloc"], GREEN, lambda k: "VA = PA  L1[200..26f]")
        self.bump(n)
        self.wait(3.5)

    def step_gui_fb(self):
        g = 0x27000000 | (1 << 12) | (1 << 10) | 2
        f = 0x28000000 | (1 << 12) | (1 << 10) | (1 << 4) | 2
        self.panel(
            T("③ tiny210_map_devices()：win32k 区和帧缓冲", 18, PURPLE),
            self.code("L1[270..27f] = SECTION_RAM(a)   // win32k 16 MiB",
                      "L1[280..283] = SECTION_NC(a)    // framebuffer 4 MiB"),
            self.bits(g, "win32k：Normal 不可缓存"),
            self.bits(f, "帧缓冲：Normal 不可缓存 + XN"),
            T("帧缓冲是 s3c-fb 的 MEM 资源，名字为 \"framebuffer\" 所以用 NC 属性；", 13, GREY_B),
            T("注释写 SECTION_RAM 为 cached，但 TEX=001 C=B=0 实际不可缓存", 13, GREY_B),
        )
        self.say("PHYSTOP 以上：win32k 区 16 段、帧缓冲 4 段也映射进内核，但它们不归 kalloc 管")
        self.map_rows(["gui"], PURPLE, lambda k: "VA = PA  L1[270..27f]")
        self.map_rows(["fb"], "#8e5ba8", lambda k: "VA = PA  L1[280..283]")
        self.bump(20)
        self.wait(3.5)

    def step_mmio(self):
        e = 0xE2900000 | (1 << 10) | (1 << 4) | 2
        lines = VGroup()
        secs = set()
        for name, pa, size, res in self.DEVICES:
            a, b = pa >> 20, (pa + size - 1) >> 20
            for i in range(a, b + 1):
                secs.add(i)
            idx = f"L1[{a:03x}]" if a == b else f"L1[{a:03x}..{b:03x}]"
            lines.add(T(f"{idx:<14}{pa:08x}  {name} {res}", 12, "#ff8a80", mono=True))
        lines.arrange(DOWN, aligned_edge=LEFT, buff=0.04)
        self.panel(
            T("④ 外设：devices.c 每个 MEM 资源 → 1 MiB 段", 18, RED),
            self.code("for(each MEM resource r)",
                      "  for(a = r.start & ~0xFFFFF; a <= r.end; a += 1 MiB)",
                      "    L1[a>>20] = SECTION_DEV(a);"),
            self.bits(e, "Strongly-ordered + XN（UART0）"),
            lines,
        )
        self.say(f"MMIO：Strongly-ordered（不缓存、不合并、不乱序）+ XN（不可执行），共 {len(secs)} 段；"
                 f"小于 1 MiB 的寄存器块也占整段")
        self.map_rows(["io", "hi"], RED, lambda k: "VA = PA  外设")
        self.bump(len(secs))
        self.wait(5)

    def step_unmapped(self):
        self.panel(
            T("⑤ 其余 L1 项保持 0 = 缺页", 18, GREY_B),
            T("28400000–3FFFFFFF：剩余 380 MiB DRAM，不映射", 15),
            T("40000000–DFFFFFFF：无 DRAM（iRAM 只在 MMU 打开前用）", 15),
            T("00000000–1FFFFFFF：用户窗口，内核表里为空，", 15),
            T("      每个进程在自己的 L1 里用 L2 表填（见第 3 段）", 15, GREY_B),
            T(f"内核 L1 只有 {self.count} 项是段，其余 {4096 - self.count} 项缺页", 15, GREEN),
        )
        self.say("没写的项都是 0：访问这些地址会触发 Translation fault（t113_fault 打印 far/dfsr）")
        self.play(Indicate(self.vrow["unused"], color=GREY_B), Indicate(self.vrow["gap"], color=GREY_B),
                  Indicate(self.vrow["vuser"], color=ORANGE), run_time=1.5)
        self.wait(3.5)

    def step_enable(self):
        regs = [("TTBCR", "0", "N=0：整个 4 GiB 只用 TTBR0"),
                ("TTBR0", f"{self.ROOTS:08x}", "L1 表物理基址（16 KiB 对齐）"),
                ("DACR", "1", "domain0 = client：按 AP 位检查权限"),
                ("TLBIALL", "0", "flush_tlb()：作废旧的 TLB")]
        rows = VGroup(*[T(f"mcr  {n:<8}= {v:<10}  {d}", 13, "#9cdcfe", mono=True) for n, v, d in regs])
        rows.arrange(DOWN, aligned_edge=LEFT, buff=0.08)
        off = T("SCTLR: M=0  I=0  Z=0  C=0    MMU 关（物理地址直接访问）", 14, GREY_B, mono=True)
        on = T("SCTLR: M=1  I=1  Z=1  C=0    MMU 开，I-cache/分支预测开", 14, GREEN, mono=True)
        self.panel(T("⑥ kvminithart() → install()，然后打开 MMU", 18, GREEN), rows, off)
        self.say("install()：先写 TTBCR、TTBR0、DACR，清 TLB；再置 SCTLR.M=1。因为 VA = PA，打开 MMU 后下一条指令照常执行")
        on.move_to(off).align_to(off, LEFT)
        self.wait(2.5)
        self.play(ReplacementTransform(off, on), run_time=1.2)
        self.pan.remove(off)
        self.pan.add(on)
        self.play(Indicate(on, color=GREEN))
        self.wait(2.5)

    def walk(self, va, l1row, desc, pa, note, color):
        steps = VGroup(
            T(f"VA {va:08x}", 16, WHITE, mono=True),
            T(f"→ 索引 VA>>20 = {va >> 20:03x}，读 TTBR0 + {va >> 20:03x}×4 = {self.ROOTS + (va >> 20) * 4:08x}", 13,
              ORANGE, mono=True),
            T(f"→ L1[{va >> 20:03x}] = {desc:08x}  （类型 10 = 段）", 13, color, mono=True),
            T(f"→ PA = {desc & 0xFFF00000:08x} | {va & 0xFFFFF:05x} = {pa:08x}", 13, GREEN, mono=True),
            T(note, 13, GREY_B),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.07)
        return steps

    def step_walk_examples(self):
        uart = 0xE2900020
        d_uart = 0xE2900000 | (1 << 10) | (1 << 4) | 2
        kcode = 0x20008000
        d_code = 0x20000000 | (1 << 12) | (1 << 10) | 2
        self.panel(T("⑦ 硬件查表举例（MMU 开启后）", 18, YELLOW),
                   self.walk(kcode, "code", d_code, kcode, "取内核第一条指令：_entry，可执行、可缓存 I-cache", BLUE),
                   self.walk(uart, "io", d_uart, uart, "写 UART0 UTXH：Strongly-ordered，每次写都直达设备", RED))
        self.say("一次访问只查一级：L1 段描述符给出 1 MiB 物理基址，低 20 位直接作页内偏移")
        self.play(Indicate(self.vrow["code"], color=BLUE), Indicate(self.R["code"], color=BLUE), run_time=1.2)
        self.play(Indicate(self.vrow["io"], color=RED), Indicate(self.R["io"], color=RED), run_time=1.2)
        self.wait(2.5)

    def step_per_process(self):
        self.panel(
            T("⑧ 每个进程的 L1 = 内核 L1 的拷贝 + 自己的用户映射", 18, ORANGE),
            self.code("pagetable_t uvmcreate(void) {",
                      "  p = rootalloc();",
                      "  memmove(p, kernel_pagetable, 16384);",
                      "}"),
            T("• L1[000..1ff]（用户窗口）用 01 型项指向 kalloc 来的 L2 表", 14),
            T("• L1[100..17f]（mmap 窗口）是 AP=11 的用户段，例如 /dev/fb0", 14),
            T("• 内核段 AP=01：用户态看得到项，访问会 Permission fault", 14),
            T("• 切换进程只换 TTBR0（switchuvm），不需要 TTBR1", 14, GREY_B),
        )
        self.say("进程切换只写 TTBR0：所有进程共享同一份内核映射，用户部分各不相同")
        self.play(self.vrow["vuser"][0].animate.set_fill(ORANGE, opacity=0.55),
                  self.vrow["vmmap"][0].animate.set_fill("#a93d8c", opacity=0.55), run_time=1)
        self.play(Transform(self.vrow["vuser"][1], T("用户页 → L2 → kalloc 页", 10).move_to(self.vrow["vuser"][0])),
                  Transform(self.vrow["vmmap"][1], T("mmap 段 → 帧缓冲", 10).move_to(self.vrow["vmmap"][0])))
        a1 = CurvedArrow(self.vrow["vuser"][0].get_right(), self.R["kalloc"][0].get_left() + DOWN * 0.15,
                         angle=0.6, color=ORANGE, stroke_width=2)
        a2 = CurvedArrow(self.vrow["vmmap"][0].get_right(), self.R["fb"][0].get_left(),
                         angle=0.5, color="#a93d8c", stroke_width=2)
        self.play(Create(a1), Create(a2), run_time=1.2)
        self.wait(3.5)


# ---------------------------------------------------------------- scene 3
class ProcessExec(Scene):
    def construct(self):
        ex = find(r"exec: /bin/sh pid (\d+): inode (\d+), (\d+) bytes, ELF entry (\w+), (\d+) program headers")
        pid = ex.group(1)
        # Lines between this exec's header and its summary.
        start = next(i for i, l in enumerate(LINES) if l.startswith(f"exec: /bin/sh pid {pid}: inode"))
        end = next(i for i, l in enumerate(LINES) if l.startswith(f"exec: /bin/sh pid {pid}: sz"))
        body = LINES[start + 1:end]
        summ = re.search(r"sz (\w+) sp (\w+) entry (\w+), (\d+) kalloc pages, new L1 root (\w+)", LINES[end])
        fork = find(rf"mm: fork: (\S+) pid (\d+) -> pid {pid}: kstack pa (\w+), L1 root (\w+), copying (\d+)", default=False)

        title(self, "3. 创建用户进程：fork() + exec(\"/bin/sh\")",
              f"kalloc 分配页面、建 L2 页表、从文件加载镜像（pid {pid}；每一步对应日志中的一行，MMU_TRACE=3）")
        cap = None
        if fork:
            cap = caption(self, cap, f"fork()：{fork.group(1)} pid {fork.group(2)} → pid {pid}，"
                                     f"kalloc 内核栈 {fork.group(3)}，复制父进程 {fork.group(5)} 个用户页")
            self.wait(1.5)

        # File (left), process VA (middle), kalloc pool (right).
        fsize = int(ex.group(3))
        file_bar = Rectangle(width=0.9, height=5.4, stroke_color=BLUE, fill_color=BLUE_E, fill_opacity=0.35) \
            .move_to([-5.9, -0.55, 0])
        flab = T(f"/bin/sh\ninode {ex.group(2)}\n{fsize} 字节", 15, BLUE).next_to(file_bar, UP, buff=0.1)
        ehdr = Rectangle(width=0.9, height=0.25, stroke_width=0, fill_color=YELLOW, fill_opacity=0.8) \
            .align_to(file_bar, UP).align_to(file_bar, LEFT)
        ehl = T("ELF 头 + phdr", 12, YELLOW).next_to(ehdr, RIGHT, buff=0.08)
        cap = caption(self, cap, f"exec：namei 找到 inode {ex.group(2)}，readi 读 ELF 头：入口 {ex.group(4)}，"
                                 f"{ex.group(5)} 个 program header")
        self.play(FadeIn(file_bar), FadeIn(flab), FadeIn(ehdr), FadeIn(ehl))

        va_slots = {}
        va_col = VGroup()
        npages = int(summ.group(1), 16) // 4096
        for k in range(npages):
            r = Rectangle(width=1.9, height=0.5, stroke_color=GREY, fill_color="#22262e", fill_opacity=1)
            lab = T(f"VA {k * 4096:05x}", 13, GREY_B, mono=True).move_to(r)
            va_slots[k * 4096] = VGroup(r, lab)
            va_col.add(va_slots[k * 4096])
        va_col.arrange(UP, buff=0.06).move_to([-1.9, -0.55, 0])
        vtitle = T(f"进程虚拟地址\n(sz = {summ.group(1)})", 15).next_to(va_col, UP, buff=0.1)

        pool_title = T("从 kalloc 空闲链表取出的物理页", 15, GREEN).move_to([4.7, 2.25, 0])
        pool = VGroup()
        self.play(FadeIn(va_col), FadeIn(vtitle), FadeIn(pool_title))

        l1 = None
        l2 = None

        def take_page(pa, label, color=GREEN_E):
            box = VGroup(Rectangle(width=3.3, height=0.36, stroke_color=GREEN, fill_color=color, fill_opacity=0.55),
                         T(f"pa {pa}  {label}", 13, mono=True))
            box[1].move_to(box[0])
            if len(pool):
                box.next_to(pool[-1], DOWN, buff=0.06)
            else:
                box.move_to([4.7, 1.85, 0])
            pool.add(box)
            return box

        for line in body:
            m = re.search(r"L1 root roots\[(\d+)\] at (\w+)", line)
            if m:
                l1 = VGroup(Rectangle(width=2.3, height=0.9, stroke_color=ORANGE, fill_color=ORANGE, fill_opacity=0.15),
                            T(f"新 L1 根 roots[{m.group(1)}]\n{m.group(2)}", 13, ORANGE))
                l1[1].move_to(l1[0])
                l1.move_to([1.2, 1.6, 0])
                cap = caption(self, cap, "uvmcreate()：取一张新 L1 根，并整张复制内核 L1（16 KiB），所以内核映射在每个进程里都在")
                self.play(FadeIn(l1, shift=DOWN * 0.2))
                continue
            m = re.search(r"exec:\s+LOAD VA (\w+) memsz (\w+) filesz (\w+) file off (\w+)", line)
            if m:
                off, filesz = hx(m.group(4)), hx(m.group(3))
                y0 = file_bar.get_top()[1] - 5.4 * off / fsize
                h = 5.4 * filesz / fsize
                seg = Rectangle(width=0.9, height=h, stroke_width=0, fill_color=GREEN, fill_opacity=0.6) \
                    .move_to([file_bar.get_center()[0], y0 - h / 2, 0])
                sl = T(f"LOAD 段\noff {m.group(4)}\nfilesz {m.group(3)}\nmemsz {m.group(2)}", 12, GREEN) \
                    .next_to(ehl, DOWN, buff=0.12, aligned_edge=LEFT)
                cap = caption(self, cap, f"program header：LOAD 段装到 VA {m.group(1)}，文件里 {m.group(3)} 字节，内存 {m.group(2)} 字节")
                self.play(FadeIn(seg), FadeIn(sl))
                self.seg, self.segl = seg, sl
                continue
            m = re.search(r"L2 table: kalloc pa (\w+) -> L1\[(\w+)\] = (\w+)", line)
            if m:
                box = take_page(m.group(1), "L2 页表", ORANGE)
                cap = caption(self, cap, f"walk()：VA 所在 1 MiB 还没有 L2 表 → kalloc 一页作 L2 表，"
                                         f"L1[{m.group(2)}] = {m.group(3)}")
                self.play(FadeIn(box, shift=LEFT * 0.3))
                if l1 is not None:
                    self.play(Create(Arrow(l1.get_bottom(), box.get_left(), buff=0.05, stroke_width=2,
                                           color=ORANGE, max_tip_length_to_length_ratio=0.1)), run_time=0.5)
                continue
            m = re.search(r"mm:\s+page: kalloc pa (\w+) -> VA (\w+) \(PTE (\w+)\)", line)
            if m:
                va = hx(m.group(2))
                box = take_page(m.group(1), f"→ VA {va:05x}")
                slot = va_slots.get(va)
                cap = caption(self, cap, f"uvmalloc()：kalloc 页 {m.group(1)}，清零，L2 PTE {m.group(3)} 映射到 VA {va:05x}")
                self.play(FadeIn(box, shift=LEFT * 0.3), run_time=0.4)
                if slot is not None:
                    self.play(slot[0].animate.set_fill(GREEN_E, opacity=0.8),
                              Create(Arrow(slot.get_right(), box.get_left(), buff=0.05, stroke_width=1.5,
                                           color=GREEN, max_tip_length_to_length_ratio=0.05)), run_time=0.45)
                continue
            m = re.search(r"exec:\s+readi inode (\d+) off (\w+) -> VA (\w+) \(pa (\w+)\) (\d+) bytes", line)
            if m:
                va = hx(m.group(3))
                slot = va_slots.get(va)
                off, nbytes = hx(m.group(2)), int(m.group(5))
                y0 = file_bar.get_top()[1] - 5.4 * off / fsize
                h = max(0.08, 5.4 * nbytes / fsize)
                chunk = Rectangle(width=0.9, height=h, stroke_color=YELLOW, fill_color=YELLOW, fill_opacity=0.7) \
                    .move_to([file_bar.get_center()[0], y0 - h / 2, 0])
                cap = caption(self, cap, f"loadseg()：readi(inode {m.group(1)}, off {m.group(2)}, {nbytes} 字节) "
                                         f"直接读进物理页 {m.group(4)}（VA {va:05x}）")
                self.play(FadeIn(chunk), run_time=0.3)
                if slot is not None:
                    fly = chunk.copy()
                    self.play(fly.animate.move_to(slot).set(width=1.9, height=0.5), run_time=0.8)
                    self.play(slot[0].animate.set_fill(BLUE, opacity=0.85), FadeOut(fly), run_time=0.3)
                    slot[1].become(T(f"VA {va:05x} 代码/数据", 12, WHITE).move_to(slot[0]))
                self.play(FadeOut(chunk), run_time=0.2)
                continue
            m = re.search(r"stack: guard page VA (\w+) .*stack VA (\w+)-(\w+)", line)
            if m:
                g = hx(m.group(1))
                if g in va_slots:
                    gs = va_slots[g]
                    cap = caption(self, cap, f"栈：VA {m.group(2)}-{m.group(3)}；下面的 {g:05x} 是保护页（uvmclear 去掉 PTE_U）")
                    self.play(gs[0].animate.set_fill(RED, opacity=0.6),
                              Transform(gs[1], T(f"VA {g:05x} 保护页", 12).move_to(gs[0])))
                    for a in range(hx(m.group(2)), hx(m.group(3)) + 1, 4096):
                        if a in va_slots:
                            st = va_slots[a]
                            self.play(Transform(st[1], T(f"VA {a:05x} 栈", 12).move_to(st[0])), run_time=0.25)
                continue

        cap = caption(self, cap, f"提交：switchuvm() 把新 L1 写进 TTBR0；本次 exec 取了 {summ.group(4)} 个 kalloc 页，旧地址空间由 uvmfree 归还")
        ttbr = T(f"TTBR0 ← {summ.group(5)}    PC = entry {summ.group(3)}    SP = {summ.group(2)}", 18, GREEN, mono=True) \
            .move_to([0.6, -2.95, 0])
        self.play(FadeIn(ttbr, shift=UP * 0.2))
        self.play(Indicate(ttbr, color=GREEN))
        self.wait(2.5)
