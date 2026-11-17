"""xv6 Tiny210 (ARM32) memory animation, driven by a real MMU_TRACE=3 log.

Scenes
  MemoryInit    DRAM layout, kernel image, kinit() filling the kalloc freelist
  MmuSetup      kvminit(): kernel L1 sections, descriptor bits, MMU registers
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
    def construct(self):
        title(self, "2. MMU 页表建立：kvminit() / kvminithart()",
              "ARMv7 短描述符：一张 L1 表 4096 项，每项映射 1 MiB 段；内核恒等映射 VA = PA")
        ttbr = find(r"mmu: TTBR0=(\w+) .*TTBCR=(\w+) DACR=(\w+)")
        sctlr = find(r"mmu: SCTLR=(\w+): M\(MMU\)=(\d) C\(D-cache\)=(\d) Z\(branch pred\)=(\d) I\(I-cache\)=(\d)")
        runs = []
        for line in LINES:
            m = re.search(r"mmu:\s+L1\[(\w+)\.\.(\w+)\] VA (\w+)-(\w+) -> PA (\w+)\s+(\d+) MiB\s+(.+?) AP=\d\((.+?)\) entry=(\w+)", line)
            if m:
                runs.append(m)

        # 64 x 64 grid of L1 entries; row r = VA r*64 MiB.
        n, s = 64, 0.072
        cells = VGroup(*[Rectangle(width=s, height=s, stroke_width=0.3, stroke_color=GREY_D,
                                   fill_color="#22262e", fill_opacity=1) for _ in range(n * n)])
        cells.arrange_in_grid(rows=n, cols=n, buff=0).move_to([-4.3, -0.45, 0])
        gl = T("kernel L1 表（roots[0]）\n每格 = 1 MiB，每行 = 64 MiB", 16, GREY_B).next_to(cells, UP, buff=0.1)
        cap = caption(self, None, f"rootalloc()：从 bss 里的 roots 池取一张 16 KiB 对齐的 L1 表，地址 {ttbr.group(1)}")
        self.play(FadeIn(cells, lag_ratio=0.0005), FadeIn(gl), run_time=2)

        user = VGroup(*[cells[i] for i in range(0x200)])
        ubox = SurroundingRectangle(user, color=ORANGE, buff=0.01, stroke_width=2)
        ulab = T("橙框 VA 0-1FFFFFFF：用户窗口（每个进程自己的 L2 表）", 13, ORANGE).next_to(cells, DOWN, buff=0.08)
        self.play(Create(ubox), FadeIn(ulab))

        colors = [RED, GREEN, PURPLE, BLUE, YELLOW]
        right = VGroup()
        run_cols = []
        for k, m in enumerate(runs):
            a, b = hx(m.group(1)), hx(m.group(2))
            col = colors[k % len(colors)] if "Strongly" not in m.group(7) else RED
            if "non-cacheable" in m.group(7) and "XN" not in m.group(7):
                col = GREEN
            grp = VGroup(*[cells[i] for i in range(a, b + 1)])
            box = VGroup(
                T(f"L1[{a:03x}..{b:03x}]  VA {hx(m.group(3)):08x} → PA {hx(m.group(5)):08x}", 14, col, mono=True),
                T(f"{m.group(6)} MiB  {m.group(7)}  {m.group(8)}", 14, GREY_B),
            ).arrange(DOWN, aligned_edge=LEFT, buff=0.04)
            right.add(box)
            run_cols.append(col)
        right.arrange(DOWN, aligned_edge=LEFT, buff=0.22)
        right.move_to([0, 1.55, 0]).align_to([-1.3, 0, 0], LEFT)
        for k, m in enumerate(runs):
            a, b = hx(m.group(1)), hx(m.group(2))
            col = run_cols[k]
            grp = VGroup(*[cells[i] for i in range(a, b + 1)])
            what = "外设 MMIO：Strongly-ordered + XN" if "Strongly" in m.group(7) else \
                ("DRAM：Normal 不可缓存，可执行" if "XN" not in m.group(7) else "帧缓冲：Normal 不可缓存 + XN")
            cap = caption(self, cap, f"kvminit()：{what}，写 {b - a + 1} 个段描述符")
            # One filled bar per grid row the run covers (cells are too small
            # to colour one by one at 720p).
            bars = VGroup()
            for row in range(a // n, b // n + 1):
                c0, c1 = max(a, row * n) - row * n, min(b, row * n + n - 1) - row * n
                ul, dr = cells[row * n + c0].get_corner(UP + LEFT), cells[row * n + c1].get_corner(DOWN + RIGHT)
                bars.add(Rectangle(width=dr[0] - ul[0], height=ul[1] - dr[1], stroke_width=0,
                                   fill_color=col, fill_opacity=0.95).move_to((ul + dr) / 2))
            grp = bars
            self.play(FadeIn(bars), FadeIn(right[k]), run_time=1.2)
            self.play(Create(Arrow(grp.get_right(), right[k].get_left(), buff=0.05, stroke_width=2,
                                   color=col, max_tip_length_to_length_ratio=0.05)), run_time=0.6)

        # Descriptor bits of the DRAM section.
        dram = next((m for m in runs if "XN" not in m.group(7)), runs[0])
        e = hx(dram.group(9))
        fields = [("PA[31:20]", f"{e >> 20:03x}", BLUE), ("TEX", f"{(e >> 12) & 7:03b}", GREEN),
                  ("AP", f"{(e >> 10) & 3:02b}", YELLOW), ("XN", f"{(e >> 4) & 1}", RED),
                  ("C B", f"{(e >> 3) & 1} {(e >> 2) & 1}", PURPLE), ("type", "10", GREY_B)]
        cells_d = VGroup()
        for name, val, col in fields:
            c = VGroup(Rectangle(width=1.0, height=0.55, stroke_color=col, fill_color=col, fill_opacity=0.18),
                       T(val, 16, col, mono=True), T(name, 12, GREY_B))
            c[1].move_to(c[0])
            c[2].next_to(c[0], DOWN, buff=0.04)
            cells_d.add(c)
        cells_d.arrange(RIGHT, buff=0.05)
        cells_d.move_to([0, -0.55, 0]).align_to([-1.3, 0, 0], LEFT)
        dl = T(f"段描述符 L1[{hx(dram.group(1)):03x}] = {e:08x}", 16, mono=True).next_to(cells_d, UP, buff=0.12)
        cap = caption(self, cap, "段描述符：PA 基址 | TEX=001,C=0,B=0 → Normal 不可缓存 | AP=01 仅特权级读写 | 类型 10 = 段")
        self.play(FadeIn(dl), LaggedStart(*[FadeIn(c) for c in cells_d], lag_ratio=0.2), run_time=1.5)
        self.wait(1)

        # Registers.
        regs = VGroup(
            T(f"TTBR0 = {hx(ttbr.group(1)):08x}  (L1 表基址)", 16, mono=True),
            T(f"TTBCR = {hx(ttbr.group(2))}  (只用 TTBR0)", 16, mono=True),
            T(f"DACR  = {hx(ttbr.group(3))}  (domain0 = client，按 AP 位检查)", 16, mono=True),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        regs.move_to([0, -1.95, 0]).align_to([-1.3, 0, 0], LEFT)
        cap = caption(self, cap, "kvminithart()：install() 写 TTBCR、TTBR0、DACR，flush TLB")
        self.play(FadeIn(regs, shift=UP * 0.1))
        sc_off = T("SCTLR: M=0 C=0 Z=0 I=0   MMU 关", 18, GREY_B, mono=True).next_to(regs, DOWN, buff=0.12, aligned_edge=LEFT)
        sc_on = T(f"SCTLR={hx(sctlr.group(1)):08x}: M={sctlr.group(2)} C={sctlr.group(3)} "
                  f"Z={sctlr.group(4)} I={sctlr.group(5)}   MMU 开", 18, GREEN, mono=True).move_to(sc_off).align_to(sc_off, LEFT)
        self.play(FadeIn(sc_off))
        cap = caption(self, cap, "置 SCTLR.M=1 打开 MMU；随后开 I-cache 和分支预测，D-cache 保持关闭")
        self.play(ReplacementTransform(sc_off, sc_on), run_time=1.2)
        self.play(Indicate(sc_on, color=GREEN))
        self.wait(2)


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
