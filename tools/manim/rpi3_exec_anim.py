"""exec("/bin/ls") on rpi3-lock, step by step: how the ELF is read in and
the process's own TTBR0 table is filled (addresses from a MMU_TRACE=3 log).

  manim -qm --media_dir build/manim tools/manim/rpi3_exec_anim.py ExecDetail
  (tools/manim/render_rpi3.sh exec  ->  doc/readme/rpi3/videos/exec-ls.mp4)
"""
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from manim import (
    BLUE, BLUE_E, DOWN, GREEN, GREEN_E, GREY, GREY_B, GREY_D, LEFT, ORANGE,
    RED, RIGHT, UP, WHITE, YELLOW, Arrow, Create, FadeIn, FadeOut, GrowArrow,
    Indicate, ReplacementTransform, Scene, SurroundingRectangle, Transform,
    VGroup, CurvedArrow,
)
from rpi3_memory_anim import LINES, T, box, caption, fit, find, index_of, title

C_TAB = "#c86400"


class ExecDetail(Scene):
    def construct(self):
        title(self, "exec(\"/bin/ls\")：ELF → uvmalloc/walk/mappages → loadseg → switchuvm",
              "pid 19，所有地址取自 make MMU_TRACE=3 的 QEMU 日志")
        e0 = index_of(r"^exec: /bin/ls pid")
        e1 = index_of(r"^exec: /bin/ls pid \d+: sz", e0)
        ev = LINES[e0:e1 + 1]
        J = "\n".join(ev)
        root = re.search(r"L1 root: kalloc pa (\w+)", J).group(1)
        l2 = re.search(r"L2 table: kalloc pa (\w+) -> user L1\[0\] = (\w+)", J)
        l3 = re.search(r"L3 table: kalloc pa (\w+) -> user L2\[0\] = (\w+)", J)
        pages = {int(m.group(3)): (m.group(1), m.group(4)) for m in
                 re.finditer(r"page: kalloc pa (\w+) -> VA (\w+) \(L3\[(\d+)\] = (\w+)", J)}
        reads = re.findall(r"read file off (\w+) -> VA (\w+) \(pa (\w+)\) (\d+) bytes", J)
        fin = re.search(r"sz (\w+) sp (\w+) entry (\w+), (\d+) table pages \+ (\d+) user pages", J)

        # ---- step list on the left
        names = ["1 读 ELF 头 / PT_INTERP", "2 uvmcreate：空 L1", "3 uvmalloc + walk 建表",
                 "4 loadseg 拷文件", "5 装 ld-xv6.so", "6 栈 + 保护页 + argv", "7 switchuvm 换 TTBR0"]
        steps = VGroup(*[T(n, 16, GREY_B) for n in names]).arrange(DOWN, aligned_edge=LEFT, buff=0.32)
        steps.to_edge(LEFT, buff=0.3).shift(DOWN * 0.2)
        self.play(FadeIn(steps), run_time=0.6)
        hl = None

        def step(i):
            nonlocal hl
            new = SurroundingRectangle(steps[i], color=YELLOW, buff=0.06)
            anims = [steps[i].animate.set_color(YELLOW)]
            if hl is None:
                anims.append(Create(new))
            else:
                anims.append(ReplacementTransform(hl, new))
                anims.append(steps[i - 1].animate.set_color(GREY))
            self.play(*anims, run_time=0.4)
            hl = new

        # ---- step 1: the files
        step(0)
        cap = caption(self, None, "namei(\"/bin/ls\") + ilock；ELF 头：EM_AARCH64、ET_DYN（PIE），PT_INTERP = /lib/ld-xv6.so")
        ls_f = VGroup(box(3.0, 0.32, "#5a3a1a", "/bin/ls", 14, mono=True),
                      box(3.0, 0.32, BLUE, "off 0x000 R-X 0x8F9", 12, mono=True),
                      box(3.0, 0.32, BLUE_E, "off 0x900 RW- 0x1A8", 12, mono=True)).arrange(DOWN, buff=0.04)
        ld_f = VGroup(box(3.0, 0.32, "#5a3a1a", "/lib/ld-xv6.so", 14, mono=True),
                      box(3.0, 0.32, ORANGE, "off 0x000 R-X 0xCC2", 12, mono=True),
                      box(3.0, 0.32, "#b9770e", "off 0xCC8 RW- 0xE0", 12, mono=True)).arrange(DOWN, buff=0.04)
        files = VGroup(ls_f, ld_f).arrange(RIGHT, buff=0.3).move_to([1.1, -2.85, 0])
        self.play(FadeIn(ls_f), run_time=0.6)
        self.wait(2.0)

        # ---- step 2: empty L1
        step(1)
        cap = caption(self, cap, f"uvmcreate()：kalloc 一页清零 = 新 L1 {root}；旧 COW 表仍在 TTBR0")
        ttbr = box(2.5, 0.5, "#7a3a12", "TTBR0 = 旧 COW 表", 14).move_to([-1.3, 2.75, 0])
        L1 = box(2.5, 0.6, C_TAB, f"L1 {root}\n[0..511] 无效", 13).move_to([-1.3, 1.6, 0])
        self.play(FadeIn(ttbr), FadeIn(L1, shift=DOWN * 0.2), run_time=0.7)
        self.wait(1.8)

        # ---- step 3: uvmalloc + walk
        step(2)
        cap = caption(self, cap, "uvmalloc 每页：kalloc + 清零 → mappages → walk(alloc=1) 发现 L1[0] 空 → 建 L2")
        L3X, PGX = 1.15, 3.95
        rows = VGroup(*[box(2.1, 0.4, GREY_D, f"[{k}]  无效", 12, opacity=0.3, mono=True) for k in range(9)])
        rows.arrange(DOWN, buff=0.04).move_to([L3X, 0.55, 0])
        slots = [rows[k].get_center() for k in range(9)]
        L2 = box(2.5, 0.6, C_TAB, f"L2 {l2.group(1)}\n[0] 无效", 13).move_to([-1.3, 0.3, 0])
        self.play(FadeIn(L2, shift=DOWN * 0.2), run_time=0.6)
        L1b = box(2.5, 0.6, C_TAB, f"L1 {root}\n[0] = {l2.group(2)}", 13).move_to(L1)
        a12 = Arrow(L1.get_bottom(), L2.get_top(), buff=0.04, stroke_width=3, color=C_TAB)
        self.play(Transform(L1, L1b), GrowArrow(a12), run_time=0.6)
        self.wait(1.0)
        cap = caption(self, cap, f"L2[0] 也空 → 再 kalloc 一页作 L3 {l3.group(1)}")
        l3title = T(f"L3 {l3.group(1)}", 15, C_TAB, mono=True).next_to(rows, UP, buff=0.1)
        L2b = box(2.5, 0.6, C_TAB, f"L2 {l2.group(1)}\n[0] = {l3.group(2)}", 13).move_to(L2)
        a23 = Arrow(L2.get_right(), rows.get_left() + UP * 1.2, buff=0.05, stroke_width=3, color=C_TAB)
        self.play(FadeIn(rows), FadeIn(l3title), Transform(L2, L2b), GrowArrow(a23), run_time=0.8)
        self.wait(1.0)

        pg = {}

        def map_page(k, col, label, rt=0.6):
            pa, pte = pages[k]
            p = box(2.4, 0.4, col, f"{pa}  {label}", 12, opacity=0.8, mono=False).move_to([PGX, slots[k][1], 0])
            r = box(2.1, 0.4, col, f"[{k}] {pte}", 12, opacity=0.55, mono=True).move_to(slots[k])
            ar = Arrow(r.get_right(), p.get_left(), buff=0.04, stroke_width=2, color=GREY_B,
                       max_tip_length_to_length_ratio=0.25)
            self.play(FadeIn(p, shift=LEFT * 0.2), Transform(rows[k], r), GrowArrow(ar), run_time=rt)
            pg[k] = p

        cap = caption(self, cap, "*pte = PA | PTE_NORMAL | PTE_U | AF | V：L3[0]、L3[1] 指向两个清零的新页")
        map_page(0, BLUE, "ls .text")
        map_page(1, BLUE_E, "ls .data")
        self.wait(1.5)

        # ---- step 4: loadseg
        step(3)
        cap = caption(self, cap, "loadseg：uva2ka(新表) 查到 PA，P2V 后经内核 TTBR1 线性映射 readi 写入（新表还没装上）")
        for seg, k, (off, va, pa, n) in ((ls_f[1], 0, reads[0]), (ls_f[2], 1, reads[1])):
            c = seg.copy()
            note = T(f"off {off}\n{n} B", 11, YELLOW, mono=True).next_to(pg[k], RIGHT, buff=0.08)
            self.play(c.animate.scale(0.5).move_to(pg[k]), FadeIn(note), run_time=0.9)
            self.play(FadeOut(c), Indicate(pg[k], color=YELLOW), run_time=0.4)
        self.wait(1.5)

        # ---- step 5: ld.so
        step(4)
        cap = caption(self, cap, "PT_INTERP：在 PGROUNDUP(sz) = 0x2000 处用同样的 loadimage 装 ld-xv6.so，入口 0x2784")
        self.play(FadeIn(ld_f), run_time=0.5)
        map_page(2, ORANGE, "ld.so .text", 0.5)
        map_page(3, "#b9770e", "ld.so .data", 0.5)
        for seg, k in ((ld_f[1], 2), (ld_f[2], 3)):
            c = seg.copy()
            self.play(c.animate.scale(0.5).move_to(pg[k]), run_time=0.7)
            self.play(FadeOut(c), Indicate(pg[k], color=YELLOW), run_time=0.3)

        # ---- step 6: stack
        step(5)
        cap = caption(self, cap, "uvmalloc 5 页（VA 0x4000–0x8FFF），uvmclear 把最低一页的 PTE_U 清掉 → 保护页")
        for k in range(4, 9):
            map_page(k, "#c0582b" if k > 4 else GREY, "保护页" if k == 4 else "栈", 0.35)
        gr = box(2.1, 0.4, GREY, f"[4] {pages[4][1][:-2]}0b  无 U", 12, opacity=0.55, mono=True).move_to(slots[4])
        self.play(Transform(rows[4], gr), run_time=0.5)
        self.wait(1.0)
        cap = caption(self, cap, f"copyout argv / envp / auxv 到栈顶，sp = {fin.group(2)}")
        argv = T("argv/envp/auxv\nsp=8f50", 11, YELLOW).next_to(pg[8], RIGHT, buff=0.08)
        self.play(FadeIn(argv, shift=DOWN * 0.1), run_time=0.6)
        self.wait(1.5)

        # ---- step 7: switch
        step(6)
        cap = caption(self, cap, f"提交：vm->pagetable = 新表，elr = {fin.group(3)}，sp = {fin.group(2)}；uvmsync_icache")
        self.wait(1.5)
        cap = caption(self, cap, f"switchuvm：w_ttbr0_el1({root}) + flush_tlb()；旧表 uvmfree（COW 页只 put_page）")
        ttbr2 = box(2.5, 0.5, "#7a3a12", f"TTBR0 = {root}", 14).move_to(ttbr)
        a01 = Arrow(ttbr.get_bottom(), L1.get_top(), buff=0.04, stroke_width=4, color=RED)
        self.play(Transform(ttbr, ttbr2), GrowArrow(a01), run_time=0.8)
        self.play(Indicate(ttbr, color=RED), run_time=0.8)
        cap = caption(self, cap, f"结果：{fin.group(4)} 个表页 + {fin.group(5)} 个用户页；返回 EL0 后先执行 ld.so，再 mmap 进 libc.so")
        self.wait(3.5)
