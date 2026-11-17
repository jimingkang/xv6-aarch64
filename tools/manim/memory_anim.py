"""xv6 Tiny210 (ARM32) memory animation, driven by a real MMU_TRACE=3 log.

Scenes
  MemoryInit    DRAM layout, kernel image, kinit() filling the kalloc freelist
  MmuSetup      kvminit()/kvminithart() on the real Tiny210, laid out like
                doc/readme/tiny210/images/mmu-page-tables.svg: code steps ①-⑧,
                the L1 table filling (sections + descriptor bits), install() and
                SCTLR.M, then a process L1 copy and its L2 table (from the log)
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
    """kvminit()/kvminithart() and a process's L2 table on the real Tiny210,
    following doc/readme/tiny210/images/mmu-page-tables.svg (modelled on
    minios' __create_page_tables diagram): numbered code panel on the left,
    the L1 table filling in the middle, physical memory on the right, the
    descriptor / register explanation underneath; then a process L1 copy
    with its L2 table (values from the MMU_TRACE=3 log)."""

    DRAM, PHYSTOP, ROOTS = 0x20000000, 0x27000000, 0x200D4000

    CODE = [  # text, step (None = plain); leading spaces become NBSP
        ("void kvminit(void) {", None),
        ("  kernel_pagetable = rootalloc(); //①", 1),
        ("  for(a=EXTMEM; a<PHYSTOP; a+=1M) //②", 2),
        ("    L1[a>>20] = a|(1<<12)|(1<<10)|2;", 2),
        ("  tiny210_map_devices(kernel_pagetable);", None),
        ("}", None),
        ("void tiny210_map_devices(p) {", None),
        ("  GUI: p[a>>20] = SECTION_RAM(a); //③", 3),
        ("  for(each MEM resource r)        //④", 4),
        ("    for(a=r.start&~0xFFFFF; a<=r.end;)", 4),
        ("      p[a>>20] = fb ? SECTION_NC(a)", 4),
        ("                    : SECTION_DEV(a);", 4),
        ("}", None),
        ("void kvminithart(void) {", None),
        ("  install(kernel_pagetable);      //⑤", 5),
        ("  SCTLR |= M; isb();              //⑥", 6),
        ("}", None),
        ("uvmcreate(): p = rootalloc();     //⑦", 7),
        ("  memmove(p, kernel_pagetable, 16K);", 7),
        ("walk(p, va, alloc=1):             //⑧", 8),
        ("  if(!p[va>>20]) p[va>>20]=kalloc()|1;", 8),
        ("  return &L2[(va>>12) & 255];", 8),
    ]

    def construct(self):
        self.cap = None
        self.strip = None
        title(self, "2. MMU 初始化：kvminit() / kvminithart()（实板 Tiny210，两级页表）",
              "按 mmu-page-tables.svg：左 = 代码 ①–⑧，中 = L1 表 roots[0]，右 = 物理内存；内核用 L1 段，进程用 L1 → L2")
        self.build_code()
        self.build_l1()
        self.build_pa()
        self.step1()
        self.step2()
        self.step3()
        self.step4()
        self.step56()
        self.step78()
        self.wait(3)

    # ---------------------------------------------------------- helpers
    def say(self, s, color=YELLOW):
        self.cap = caption(self, self.cap, s, color)

    def show_strip(self, *mobs):
        new = VGroup(*mobs).arrange(DOWN, aligned_edge=LEFT, buff=0.1)
        if new.width > 9.4:
            new.scale_to_fit_width(9.4)
        if new.height > 1.55:
            new.scale_to_fit_height(1.55)
        new.move_to([2.15, -2.5, 0])
        if self.strip is not None:
            self.play(FadeOut(self.strip), run_time=0.3)
        self.play(FadeIn(new, shift=UP * 0.15), run_time=0.6)
        self.strip = new

    def bits(self, value, kind):
        tex, ap, xn, c, b = (value >> 12) & 7, (value >> 10) & 3, (value >> 4) & 1, (value >> 3) & 1, (value >> 2) & 1
        fields = [("PA[31:20]", f"{value >> 20:03x}", BLUE), ("TEX", f"{tex:03b}", GREEN), ("AP", f"{ap:02b}", YELLOW),
                  ("XN", f"{xn}", RED), ("C", f"{c}", PURPLE), ("B", f"{b}", PURPLE), ("类型", "10", GREY_B)]
        row = VGroup()
        for name, val, col in fields:
            cell = VGroup(Rectangle(width=0.8, height=0.42, stroke_color=col, fill_color=col, fill_opacity=0.18),
                          T(val, 14, col, mono=True), T(name, 10, GREY_B))
            cell[1].move_to(cell[0])
            cell[2].next_to(cell[0], DOWN, buff=0.02)
            row.add(cell)
        row.arrange(RIGHT, buff=0.04)
        head = T(f"段描述符 {value:08x} = PA基址 | TEX<<12 | AP<<10 | XN<<4 | C<<3 | B<<2 | 0b10  →  {kind}", 13)
        return VGroup(head, row).arrange(DOWN, aligned_edge=LEFT, buff=0.06)

    def hl(self, step):
        anims = []
        for band, s in zip(self.bands, self.band_steps):
            anims.append(band.animate.set_fill(opacity=0.85 if s == step else 0.0))
        self.play(*anims, run_time=0.4)

    # ---------------------------------------------------------- layout
    def build_code(self):
        panel = Rectangle(width=4.25, height=5.75, stroke_width=0, fill_color="#16213a", fill_opacity=1) \
            .move_to([-4.85, -0.05, 0])
        hdr = T("vm.c + mach-s5pv210/devices.c", 11, "#cdd6f4", mono=True) \
            .next_to(panel.get_corner(UP + LEFT), DOWN + RIGHT, buff=0.08)
        lines = VGroup()
        self.bands, self.band_steps = VGroup(), []
        y = panel.get_top()[1] - 0.42
        for txt, step in self.CODE:
            lead = len(txt) - len(txt.lstrip(" "))
            t = T(txt.lstrip(" "), 10.5,
                  "#f9e2af" if step is None and txt.endswith("{") else "#e6e6e6", mono=True)
            if t.width > 4.0 - 0.085 * lead:
                t.scale_to_fit_width(4.0 - 0.085 * lead)
            t.move_to([0, y, 0]).align_to(panel, LEFT).shift(RIGHT * (0.1 + 0.085 * lead))
            if step is not None:
                band = Rectangle(width=4.1, height=0.235, stroke_width=0, fill_color="#3d5a80", fill_opacity=0.0) \
                    .move_to([panel.get_center()[0], y, 0])
                self.bands.add(band)
                self.band_steps.append(step)
            lines.add(t)
            y -= 0.245
        self.play(FadeIn(panel), FadeIn(hdr), FadeIn(self.bands), FadeIn(lines, lag_ratio=0.02), run_time=1.5)

    L1ROWS = [  # key, index text, color
        ("f80", "[f80]", RED), ("f20", "[f20..f23]", RED), ("e01", "[e01][e02][e25][e27]", RED),
        ("e29", "[e29][eb0][ec1][ec2]", RED), ("hole", "[284..e00] 等", GREY),
        ("280", "[280..283]", PURPLE), ("270", "[270..27f]", PURPLE), ("26f", "[26f]", GREEN),
        ("dots", "…  [201..26e]", GREEN), ("200", "[200]", GREEN), ("100", "[100..17f]", "#d080c0"),
        ("000", "[000..01f]", ORANGE),
    ]

    def build_l1(self):
        box = Rectangle(width=5.2, height=4.15, stroke_color="#7a5cf0", stroke_width=2, fill_color="#1b1830",
                        fill_opacity=1).move_to([0.35, 0.55, 0])
        head = Rectangle(width=5.2, height=0.42, stroke_width=0, fill_color="#5b3fb5", fill_opacity=1) \
            .align_to(box, UP).align_to(box, LEFT)
        ht = T(f"L1 = roots[0] @ {self.ROOTS:08x}（TTBR0）", 13, WHITE).move_to(head)
        self.l1box = VGroup(box, head, ht)
        self.l1 = {}
        y = head.get_bottom()[1] - 0.2
        for key, idx, col in self.L1ROWS:
            it = T(idx, 11, col, mono=True).move_to([0, y, 0]).align_to(box, LEFT).shift(RIGHT * 0.1)
            val = T("00000000", 11, GREY_D, mono=True).move_to([box.get_left()[0] + 2.6, y, 0])
            note = T("缺页", 10, GREY_D).move_to([0, y, 0]).align_to(box, RIGHT).shift(LEFT * 0.08)
            self.l1[key] = [it, val, note, y]
            y -= 0.3
        self.play(FadeIn(self.l1box), *[FadeIn(VGroup(*v[:3])) for v in self.l1.values()], run_time=1)

    PAROWS = [  # key, label, height, color
        ("hi", "F2000000 VIC · F8000000 FIMD", 0.42, RED),
        ("io", "E0100000–EC2FFFFF 外设", 0.55, RED),
        ("unused", "28400000–3FFFFFFF 未用 DRAM", 0.42, GREY_D),
        ("fb", "28000000 帧缓冲 4 MiB", 0.3, "#8e5ba8"),
        ("gui", "27000000 win32k 16 MiB", 0.3, PURPLE),
        ("dram", "20000000 ATAG/内核/bss(roots)", 0.45, BLUE_E),
        ("kalloc", "2053A000–26FFFFFF kalloc 页池", 0.85, GREEN_E),
        ("low", "00000000–1FFFFFFF 非 DRAM", 0.7, GREY_D),
    ]

    def build_pa(self):
        y = 2.52
        self.pa = {}
        grp = VGroup(T("物理内存（实板）", 13).move_to([5.25, 2.72, 0]))
        for key, lab, h, col in self.PAROWS:
            r = Rectangle(width=3.3, height=h, stroke_color=GREY, stroke_width=1, fill_color=col, fill_opacity=0.3) \
                .move_to([5.25, y - h / 2, 0])
            t = T(lab, 10.5).move_to(r)
            if t.width > 3.2:
                t.scale_to_fit_width(3.2)
            self.pa[key] = VGroup(r, t)
            grp.add(self.pa[key])
            y -= h
        self.play(FadeIn(grp), run_time=0.8)

    def fill(self, key, value, note, color, pa=None, curve=0.0):
        it, val, nt, y = self.l1[key]
        nv = T(value, 11, color, mono=True).move_to([self.l1box[0].get_left()[0] + 2.6, y, 0])
        nn = T(note, 10, color).move_to(nt).align_to(nt, RIGHT)
        anims = [Transform(val, nv), Transform(nt, nn), Indicate(it, color=color, scale_factor=1.05)]
        self.play(*anims, run_time=0.6)
        if pa is not None:
            start = [self.l1box[0].get_right()[0], y, 0]
            end = self.pa[pa][0].get_left()
            a = CurvedArrow(start, end, angle=curve, color=color, stroke_width=2) if curve else \
                Arrow(start, end, buff=0.04, color=color, stroke_width=2, max_tip_length_to_length_ratio=0.06)
            self.play(Create(a), run_time=0.5)

    # ---------------------------------------------------------- steps
    def step1(self):
        self.hl(1)
        self.say(f"① rootalloc()：从 .bss 的静态池 roots[256][4096] 取 roots[0]（{self.ROOTS:08x}，16 KiB 对齐）并清零")
        self.show_strip(T("L1 表 = 4096 项 × 4 字节 = 16 KiB，覆盖 4 GiB；索引 = VA >> 20，每项管 1 MiB", 15),
                        T("项的低 2 位：00 缺页 · 01 指向 L2 表（进程用）· 10 段，直接映射 1 MiB（内核全部用段）", 15, GREY_B))
        self.play(Indicate(self.pa["dram"], color=ORANGE), run_time=1.2)
        self.wait(3)

    def step2(self):
        self.hl(2)
        self.say("② DRAM 恒等映射：a 从 0x20000000 到 PHYSTOP（+112 MiB），每 MiB 一个段，共 112 项 L1[200..26f]")
        self.show_strip(self.bits(SEC_RAM(self.DRAM), "Normal 不可缓存 · 仅特权级读写 · 可执行"),
                        T("TEX=001 C=0 B=0：Normal 不可缓存（D-cache 关）；AP=01：用户态访问 → Permission fault", 13, GREY_B))
        self.fill("200", f"{SEC_RAM(0x20000000):08x}", "PA 20000000", GREEN, "dram", curve=-0.25)
        self.fill("dots", "…", "112 段", GREEN)
        self.fill("26f", f"{SEC_RAM(0x26F00000):08x}", "PA 26F00000", GREEN, "kalloc", curve=-0.15)
        self.wait(3)

    def step3(self):
        self.hl(3)
        self.say("③ win32k 表面缓存区 16 MiB：SECTION_RAM，属性与 DRAM 相同（注释说 cached，实际不可缓存）")
        self.show_strip(self.bits(SEC_RAM(0x27000000), "win32k：Normal 不可缓存"))
        self.fill("270", f"{SEC_RAM(0x27000000):08x}…", "win32k", PURPLE, "gui")
        self.wait(2.5)

    def step4(self):
        self.hl(4)
        self.say("④ devices.c 每个 MEM 资源 → 覆盖它的 1 MiB 段：帧缓冲用 SECTION_NC，其余 SECTION_DEV")
        self.show_strip(self.bits(SEC_NC(0x28000000), "帧缓冲：Normal 不可缓存 + XN"),
                        self.bits(SEC_DEV(0xE2900000), "UART0：Strongly-ordered + XN"))
        self.fill("280", f"{SEC_NC(0x28000000):08x}…", "帧缓冲 NC+XN", "#b98ad6", "fb")
        self.fill("e01", "e0100412…", "clock gpio pwm wdt", RED)
        self.fill("e29", f"{SEC_DEV(0xE2900000):08x}", "uart sdhci phy ehci", RED, "io")
        self.fill("f20", f"{SEC_DEV(0xF2000000):08x}…", "VIC0-3", RED)
        self.fill("f80", f"{SEC_DEV(0xF8000000):08x}", "FIMD", RED, "hi")
        self.say("MMIO 共 13 段：寄存器块小于 1 MiB 也占整段；Strongly-ordered = 不缓存、不合并、不乱序")
        self.wait(2)
        self.say("结果：内核 L1 共 145 个段（112 + 16 + 4 + 13），其余 3951 项为 0 → 访问即 Translation fault")
        self.play(Indicate(self.l1["hole"][0], color=GREY_B), Indicate(self.pa["unused"], color=GREY_B), run_time=1.2)
        self.wait(2.5)

    def step56(self):
        self.hl(5)
        regs = [("dsb", "写 L1 的 str 全部完成，页表遍历器可见"),
                ("mcr c2,c0,2  TTBCR = 0", "N=0：整个 4 GiB 只用 TTBR0（没有 TTBR1）"),
                (f"mcr c2,c0,0  TTBR0 = {self.ROOTS:#x}", "L1 表物理基址，16 KiB 对齐"),
                ("mcr c3,c0,0  DACR = 1", "domain0 = client：按 AP 位检查"),
                ("dsb; mcr c8,c7,0; dsb; isb", "flush_tlb()：TLBIALL")]
        rows = VGroup(*[VGroup(T(a, 12, "#9cdcfe", mono=True), T(b, 12, GREY_B)).arrange(RIGHT, buff=0.3)
                        for a, b in regs])
        for r in rows:
            r[1].shift(RIGHT * (4.2 - r[0].width))
        rows.arrange(DOWN, aligned_edge=LEFT, buff=0.05)
        self.say("⑤ install()：MMU 还关着；写 TTBCR、TTBR0、DACR，再作废全部 TLB")
        self.show_strip(rows)
        self.wait(3)
        self.hl(6)
        off = T("SCTLR: M=0 I=0 Z=0 C=0    MMU 关", 15, GREY_B, mono=True)
        on = T("SCTLR: M=1 I=1 Z=1 C=0    MMU 开，I-cache/分支预测开，D-cache 关", 15, GREEN, mono=True)
        self.say("⑥ 置 SCTLR.M=1：因为 VA = PA，下一条指令的地址不变，照常取指；之后 setup.c 打开 I-cache 和分支预测")
        self.show_strip(off)
        on.move_to(off).align_to(off, LEFT)
        self.play(ReplacementTransform(off, on), run_time=1.2)
        self.strip = on
        self.play(Indicate(self.l1box[2], color=GREEN))
        self.wait(3)

    def step78(self):
        self.hl(7)
        self.say("⑦ 创建进程：uvmcreate() 再取一个 roots[k]，整张复制内核 L1——内核的 145 个段在每个进程里都在")
        ex = find(r"exec: /bin/sh pid (\d+): inode")
        root = find(r"mm:\s+L1 root roots\[(\d+)\] at (\w+)")
        new_head = T(f"进程 L1 = roots[{root.group(1)}] @ {root.group(2)}（拷贝）", 13, WHITE).move_to(self.l1box[2])
        self.play(self.l1box[1].animate.set_fill(ORANGE), Transform(self.l1box[2], new_head), run_time=1)
        self.show_strip(T("进程切换只写 TTBR0（switchuvm）：内核部分人人相同，用户窗口 L1[000..1ff] 各不相同", 15),
                        T("下面是 exec(\"/bin/sh\") 的真实日志（QEMU，MMU_TRACE=3）", 13, GREY_B))
        self.wait(3)

        self.hl(8)
        l2 = find(r"mm:\s+L2 table: kalloc pa (\w+) -> L1\[(\w+)\] = (\w+)")
        pages = []
        start = next(i for i, l in enumerate(LINES) if f"exec: /bin/sh pid {ex.group(1)}: inode" in l)
        for l in LINES[start:]:
            m = re.search(r"mm:\s+page: kalloc pa (\w+) -> VA (\w+) \(PTE (\w+)\)", l)
            if m:
                pages.append(m.groups())
            if l.startswith(f"exec: /bin/sh pid {ex.group(1)}: sz"):
                break
        self.say(f"⑧ walk()：VA 0 所在 1 MiB 没有 L2 表 → kalloc 一页 {l2.group(1)} 作 L2，"
                 f"L1[0] = {l2.group(3)}（低 2 位 01 = 表描述符）")
        self.fill("000", l2.group(3), "→ L2 表", ORANGE)
        # L2 table replaces the bottom strip.
        if self.strip is not None:
            self.play(FadeOut(self.strip), run_time=0.3)
            self.strip = None
        box = Rectangle(width=6.2, height=1.75, stroke_color=ORANGE, stroke_width=2, fill_color="#2a1a10",
                        fill_opacity=1).move_to([0.4, -2.5, 0])
        bt = T(f"L2 @ {l2.group(1)}（kalloc 页，只用前 1 KiB：256 项）  索引 = (VA>>12) & 255", 12, ORANGE) \
            .next_to(box.get_top(), DOWN, buff=0.06)
        self.play(FadeIn(box), FadeIn(bt))
        self.play(Create(Arrow([self.l1box[0].get_left()[0] + 0.9, self.l1["000"][3] - 0.1, 0],
                               [self.l1box[0].get_left()[0] + 0.9, box.get_top()[1] + 0.02, 0], buff=0.0,
                               color=ORANGE, stroke_width=2, max_tip_length_to_length_ratio=0.3)))
        rows = VGroup()
        for k, (pa, va, pte) in enumerate(pages[:4]):
            rows.add(T(f"L2[{hx(va) >> 12 & 255}] = {pte}    VA {hx(va):05x} → PA {pa}", 11, WHITE, mono=True))
        rows.add(T(f"…  共 {len(pages)} 个 kalloc 页（代码/数据、保护页、栈）", 11, GREY_B))
        rows.arrange(DOWN, aligned_edge=LEFT, buff=0.04).next_to(bt, DOWN, buff=0.08).align_to(bt, LEFT)
        for k, r in enumerate(rows):
            self.play(FadeIn(r, shift=RIGHT * 0.1), run_time=0.45)
            if k == 0:
                self.play(Create(Arrow(box.get_right() + UP * 0.3, self.pa["kalloc"][0].get_left() + DOWN * 0.15,
                                       buff=0.05, color=ORANGE, stroke_width=2)), run_time=0.5)
        note = T("PTE = PA | 0x72：小页 · AP=11 用户读写 · TEX=001 Normal 不可缓存",
                 11, GREY_B).next_to(rows, DOWN, buff=0.05).align_to(rows, LEFT)
        if note.width > 6.0:
            note.scale_to_fit_width(6.0)
        self.play(FadeIn(note))
        self.say("用户地址两次查表：L1[VA>>20] → L2[(VA>>12)&255] → 4 KiB 页；内核地址一次查表（段）")
        self.fill("100", "00000000", "mmap 时：用户段", "#d080c0")
        self.wait(3)


SEC_RAM = lambda a: a | (1 << 12) | (1 << 10) | 2
SEC_NC = lambda a: a | (1 << 12) | (1 << 10) | (1 << 4) | 2
SEC_DEV = lambda a: a | (1 << 10) | (1 << 4) | 2


# ---------------------------------------------------------------- scene 3
class ProcessExec(Scene):
    def where_tables(self, body, cap):
        """Where the page tables live in physical memory: the kernel L1 is
        roots[0] in .bss, a process L1 is a copy in another roots[] slot of
        the same .bss pool, its L2 tables come from the top of the kalloc
        pool (LIFO freelist, kinit freed pages in ascending order)."""
        ttbr = hx(find(r"mmu: TTBR0=(\w+)").group(1))
        bss = find(r"mm: kernel bss\s+(\w+)-(\w+)")
        kin = find(r"mm: kinit: freerange (\w+)-(\w+) -> .*PHYSTOP=(\w+)")
        root = next(re.search(r"L1 root roots\[(\d+)\] at (\w+)", l) for l in body if "L1 root roots" in l)
        l2 = next(re.search(r"L2 table: kalloc pa (\w+) -> L1\[(\w+)\] = (\w+)", l) for l in body if "L2 table" in l)
        pg = next(re.search(r"page: kalloc pa (\w+) -> VA (\w+)", l) for l in body if "page: kalloc" in l)
        k, kaddr = int(root.group(1)), hx(root.group(2))

        # Physical strip (not to scale), high addresses on top.
        x = -4.3
        segs = [("kalloc", f"kalloc 页池  {hx(kin.group(1)):08x}–{hx(kin.group(2)):08x}", 2.6, GREEN_E),
                ("bss", f"内核 .bss  {hx(bss.group(1)):08x}–{hx(bss.group(2)):08x}", 2.3, "#3d5a80"),
                ("img", "内核 code / data / ramdisk", 0.7, BLUE_E)]
        y = 2.2
        R = {}
        grp = VGroup(T("物理内存（QEMU 日志地址）", 14).move_to([x, 2.42, 0]))
        for key, lab, h, col in segs:
            r = Rectangle(width=3.6, height=h, stroke_color=GREY, fill_color=col, fill_opacity=0.35) \
                .move_to([x, y - h / 2, 0])
            t = T(lab, 12).next_to(r.get_top(), DOWN, buff=0.06)
            R[key] = r
            grp.add(r, t)
            y -= h
        phys = T(f"← PHYSTOP {hx(kin.group(3)):08x}", 11, RED).next_to(R["kalloc"].get_corner(UP + RIGHT), RIGHT, buff=0.05)
        grp.add(phys)
        # roots[] pool inside .bss: 16 x 16 slots of 16 KiB.
        pool = VGroup(*[Rectangle(width=0.15, height=0.07, stroke_width=0.4, stroke_color=GREY_D,
                                  fill_color="#22262e", fill_opacity=1) for _ in range(256)])
        pool.arrange_in_grid(rows=16, cols=16, buff=0.012).move_to(R["bss"].get_center() + UP * 0.05)
        plab = T(f"roots 池 @ {ttbr:08x}：256 槽 × 16 KiB = 4 MiB", 10, GREY_B).next_to(pool, DOWN, buff=0.05)
        cap = caption(self, cap, "先看页表放在哪里：所有 L1 都在内核 .bss 的 roots 池里，L2 表和用户页来自 kalloc 页池")
        self.play(FadeIn(grp), run_time=1)
        self.play(FadeIn(pool), FadeIn(plab), run_time=0.8)

        rx = 0.6
        info = VGroup()

        def note(s, col=WHITE, size=15):
            t = T(s, size, col)
            if t.width > 6.3:
                t.scale_to_fit_width(6.3)
            if len(info):
                t.next_to(info[-1], DOWN, buff=0.14, aligned_edge=LEFT)
            else:
                t.move_to([rx, 2.3, 0], aligned_edge=LEFT)
            info.add(t)
            return t

        # Kernel L1 = roots[0].
        self.play(pool[0].animate.set_fill(BLUE, opacity=1))
        n1 = note(f"① 内核 L1 = roots[0] = {ttbr:08x}", BLUE)
        n1b = note("   开机时 kvminit() 写好，TTBR0 指向它；在 .bss 里，不是 kalloc 来的", GREY_B, 12)
        a0 = Arrow(n1.get_left() + LEFT * 0.05, pool[0].get_right(), buff=0.05, color=BLUE, stroke_width=2,
                   max_tip_length_to_length_ratio=0.05)
        cap = caption(self, cap, f"内核 L1 就是 .bss 里的 roots[0]（{ttbr:08x}）；实板上 roots 在 0x200d4000")
        self.play(FadeIn(n1), FadeIn(n1b), GrowArrow(a0))
        self.wait(2)

        # Process L1 = copy into roots[k].
        cap = caption(self, cap, f"uvmcreate()：rootalloc() 在同一个池里找空槽 roots[{k}]，memmove 整张 16 KiB（4096 项）")
        n2 = note(f"② 进程 L1 = roots[{k}] = {kaddr:08x}（= {ttbr:08x} + {k}×0x4000）", ORANGE)
        n2b = note("   memmove(p, kernel_pagetable, 16384)：内核的段项原样复制", GREY_B, 12)
        n2c = note("   进程退出/exec 换表：uvmfree 把 used[k] 清 0，槽还给池子", GREY_B, 12)
        copy = pool[0].copy()
        self.play(FadeIn(n2), FadeIn(n2b), FadeIn(n2c))
        self.play(copy.animate.move_to(pool[k]).set_fill(ORANGE, opacity=1), run_time=1.2)
        self.play(pool[k].animate.set_fill(ORANGE, opacity=1), FadeOut(copy), run_time=0.3)
        for i in range(1, k):
            pool[i].set_fill(GREY_B, opacity=0.8)
        used = T(f"roots[1..{k - 1}]：其他进程的 L1（灰）", 10, GREY_B).next_to(plab, DOWN, buff=0.03)
        self.play(FadeIn(used), GrowArrow(Arrow(n2.get_left() + LEFT * 0.05, pool[k].get_right(), buff=0.05,
                                                color=ORANGE, stroke_width=2, max_tip_length_to_length_ratio=0.05)))
        self.wait(2)

        # L2 from the top of the kalloc pool.
        l2pa = hx(l2.group(1))
        top = R["kalloc"].get_top()[1]
        slot = Rectangle(width=1.5, height=0.28, stroke_color=ORANGE, fill_color=ORANGE, fill_opacity=0.8) \
            .move_to([x + 0.8, top - 0.55, 0])
        stxt = T(f"L2 {l2pa:08x}", 11, WHITE, mono=True).move_to(slot)
        page = Rectangle(width=1.5, height=0.28, stroke_color=GREEN, fill_color=GREEN, fill_opacity=0.8) \
            .next_to(slot, DOWN, buff=0.04)
        ptxt = T(f"页 {hx(pg.group(1)):08x}", 11, WHITE, mono=True).move_to(page)
        cap = caption(self, cap, f"walk()：L1[{l2.group(2)}] 为 0 → kalloc() 取 freelist 表头 → L2 表在 {l2pa:08x}，"
                                 f"紧贴 PHYSTOP 下方")
        n3 = note(f"③ L2 表 = kalloc 页 {l2pa:08x}（只用前 1 KiB）", GREEN)
        n3b = note(f"   roots[{k}][{l2.group(2)}] = {l2.group(3)}：低 2 位 01 = 指向 L2 表", GREY_B, 12)
        n3c = note("   freelist 后进先出，kinit 按地址升序 kfree → 表头是最高地址的页", GREY_B, 12)
        n4 = note(f"④ 用户页同样来自 kalloc：{hx(pg.group(1)):08x} → VA {hx(pg.group(2)):05x} …", GREEN)
        self.play(FadeIn(n3), FadeIn(n3b), FadeIn(n3c), FadeIn(slot), FadeIn(stxt))
        self.play(Create(CurvedArrow(pool[k].get_left(), slot.get_left(), angle=-1.2, color=ORANGE, stroke_width=2)),
                  run_time=1)
        self.play(FadeIn(n4), FadeIn(page), FadeIn(ptxt))
        self.wait(3)
        self.play(*[FadeOut(m) for m in self.mobjects if m is not cap and m is not self._title], run_time=0.6)
        return cap

    def construct(self):
        ex = find(r"exec: /bin/sh pid (\d+): inode (\d+), (\d+) bytes, ELF entry (\w+), (\d+) program headers")
        pid = ex.group(1)
        # Lines between this exec's header and its summary.
        start = next(i for i, l in enumerate(LINES) if l.startswith(f"exec: /bin/sh pid {pid}: inode"))
        end = next(i for i, l in enumerate(LINES) if l.startswith(f"exec: /bin/sh pid {pid}: sz"))
        body = LINES[start + 1:end]
        summ = re.search(r"sz (\w+) sp (\w+) entry (\w+), (\d+) kalloc pages, new L1 root (\w+)", LINES[end])
        fork = find(rf"mm: fork: (\S+) pid (\d+) -> pid {pid}: kstack pa (\w+), L1 root (\w+), copying (\d+)", default=False)

        self._title = title(self, "3. 创建用户进程：fork() + exec(\"/bin/sh\")",
                            f"kalloc 分配页面、建 L2 页表、从文件加载镜像（pid {pid}；每一步对应日志中的一行，MMU_TRACE=3）")
        cap = None
        if fork:
            cap = caption(self, cap, f"fork()：{fork.group(1)} pid {fork.group(2)} → pid {pid}，"
                                     f"kalloc 内核栈 {fork.group(3)}，复制父进程 {fork.group(5)} 个用户页")
            self.wait(1.5)
        cap = self.where_tables(body, cap)

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
