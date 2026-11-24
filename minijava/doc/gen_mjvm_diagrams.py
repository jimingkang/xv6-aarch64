#!/usr/bin/env python3
"""Generate mjvm-interp-frame.svg and incblock-stack.svg (same style as ../diagrams/*.svg).

Everything shown is taken from minijava/jvm/mjvm.c (invoke(), monitor_enter/exit,
find_handler) and from `javap -c -v` of the classes mjavac generates for
examples/Counter.java.
"""
import os
from html import escape

W = 1400
MONO = 'DejaVu Sans Mono, Menlo, monospace'
FONT = "WenQuanYi Zen Hei, PingFang SC, Noto Sans CJK SC, sans-serif"

BLUE, BLUE_S = '#ddf4ff', '#0969da'
YEL, YEL_S = '#fff8c5', '#9a6700'
GREY, GREY_S = '#f6f8fa', '#57606a'
GREEN, GREEN_S = '#dafbe1', '#1a7f37'
PURP, PURP_S = '#fbefff', '#8250df'
RED, RED_F = '#cf222e', '#ffebe9'
INK = '#1f2328'

out = []


def e(s):
    return escape(str(s), quote=True)


def text(x, y, s, size=13, fill=INK, bold=False, anchor='start', mono=False):
    fam = f' font-family="{MONO}"' if mono else ''
    out.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" '
               f'font-weight="{"bold" if bold else "normal"}" text-anchor="{anchor}"{fam}>{e(s)}</text>')


def lines(x, y, rows, size=12.5, gap=None, **kw):
    gap = gap or size * 1.5
    for i, r in enumerate(rows):
        text(x, y + i * gap, r, size, **kw)
    return y + len(rows) * gap


def rect(x, y, w, h, fill='#fff', stroke='#8c959f', sw=1, rx=8, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
               f'stroke="{stroke}" stroke-width="{sw}"{d}/>')


def line(points, color='#333', marker='a', dash=None, sw=2):
    d = 'M' + ' L'.join(f'{x},{y}' for x, y in points)
    dd = f' stroke-dasharray="{dash}"' if dash else ''
    m = f' marker-end="url(#{marker})"' if marker else ''
    out.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}"{dd}{m}/>')


def cell(x, y, w, h, label, fill='#fff', stroke=GREY_S, size=11.5, mono=True, col=INK):
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}"/>')
    if label:
        text(x + w / 2, y + h / 2 + size * 0.36, label, size, anchor='middle', mono=mono, fill=col)


def panel(x, y, w, h, title, fill, stroke):
    rect(x, y, w, h, fill=fill, stroke=stroke, sw=1.5, rx=10)
    text(x + 14, y + 24, title, 15, bold=True, fill=stroke)


def begin(h):
    out.clear()
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{h}" viewBox="0 0 {W} {h}" '
               f'font-family="{FONT}" font-size="13">')
    out.append('<defs>')
    for mid, col in (('a', '#333'), ('ab', BLUE_S), ('ar', RED), ('ag', GREEN_S), ('ay', YEL_S), ('ap', PURP_S)):
        out.append(f'<marker id="{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" '
                   f'orient="auto"><path d="M0,0L10,5L0,10z" fill="{col}"/></marker>')
    out.append('</defs>')
    out.append(f'<rect width="{W}" height="{h}" fill="#fff"/>')


def save(path):
    out.append('</svg>')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


# ======================================================================
# 1. interpreter loop + frame layout
# ======================================================================
def diagram_interp(path):
    H = 1520
    begin(H)
    text(W / 2, 34, 'mjvm 解释器：invoke() 的 C 栈帧布局 + 取指/分派循环 + 异常路径', 22, bold=True, anchor='middle')
    text(W / 2, 58, '以 Worker.run() → c.incBlock() 为例，快照时刻：incBlock 执行到 pc=6 monitorenter 之前'
         '（代码见 minijava/jvm/mjvm.c）', 13, fill=GREY_S, anchor='middle')

    # ---------------- left: C stack ----------------
    LX, LW = 24, 640
    panel(LX, 78, LW, 1110, '① C 栈（一个 Java 线程 = 一个 xv6 内核线程，栈向下长）', GREY, GREY_S)
    text(LX + 14, 120, 'n_thread_start(): vm_alloc(512KB) 作栈 → plat_thread_start → thread_clone(thread_main, nt, stack)',
         11.5, fill=GREY_S)

    # Thread struct
    y = 136
    rect(LX + 16, y, 300, 186, fill=PURP, stroke=PURP_S, sw=1.5)
    text(LX + 28, y + 22, 'Thread *t（每线程一个，mjvm.c:119）', 13, bold=True, fill=PURP_S)
    lines(LX + 28, y + 46, ['id        = 2   (thin lock 里记它)',
                            'park      = sync_create(SEM)  ParkEvent',
                            'exc       = 待抛异常 (Object*)',
                            'depth     = invoke 递归深度 (≤600)',
                            'bytecodes = 已执行字节码数',
                            'entry     = Worker.run()V',
                            'next      = 监视器队列链接'], 11.5, gap=19, mono=True)
    rect(LX + 330, y, 294, 186, fill='#fff', stroke=GREY_S)
    text(LX + 342, y + 22, 'thread_main(t)', 13, bold=True)
    lines(LX + 342, y + 46, ['args[0].a = t->obj;',
                             'rc = invoke(t, t->entry, args, &r);',
                             'rc < 0 → print_exception',
                             'thread_finish(t): 锁 t->obj,',
                             '  alive = 0; notifyAll → join() 醒',
                             'plat_thread_exit() → texit'], 11.5, gap=21, mono=True)
    line([(LX + 316, y + 90), (LX + 330, y + 90)], color=PURP_S, marker='ap', sw=1.5)

    def frame(y, title, sub, locals_, stack_, sp_idx, regs, h, hl_stack=None):
        rect(LX + 16, y, LW - 32, h, fill='#fff', stroke=BLUE_S, sw=1.8)
        rect(LX + 16, y, LW - 32, 30, fill=BLUE, stroke=BLUE_S, sw=1.8, rx=8)
        text(LX + 28, y + 20, title, 13.5, bold=True, fill=BLUE_S, mono=True)
        text(LX + LW - 28, y + 20, sub, 11.5, fill=GREY_S, anchor='end')
        # locals
        cx, cy = LX + 30, y + 58
        text(cx, cy - 8, f'Slot locals[max_locals+1]   ({len(locals_)} 个 8 字节槽)', 12, bold=True)
        cw = 118
        for i, (lab, val, fill) in enumerate(locals_):
            cell(cx + i * cw, cy, cw, 22, f'[{i}]', fill='#eaeef2', size=10.5)
            cell(cx + i * cw, cy + 22, cw, 28, val, fill=fill, size=11)
        # stack
        sy = cy + 80
        text(cx, sy - 8, f'Slot stack[max_stack+2]   ({len(stack_)} 个槽)  ← 操作数栈，向右长', 12, bold=True)
        for i, (val, fill) in enumerate(stack_):
            cell(cx + i * cw, sy, cw, 22, f'[{i}]', fill='#eaeef2', size=10.5)
            cell(cx + i * cw, sy + 22, cw, 28, val, fill=fill, size=11)
        spx = cx + sp_idx * cw + 10
        line([(spx, sy + 86), (spx, sy + 54)], color=RED, marker='ar', sw=2)
        text(spx + 8, sy + 84, 'sp（指向下一个空槽）', 11.5, fill=RED)
        # registers
        ry = sy + 112
        for i, r in enumerate(regs):
            text(cx + (i % 2) * 300, ry + (i // 2) * 20, r, 11.5, mono=True)
        return cx, cy, sy, cw

    # Worker.run frame
    fy = 340
    cx, cy, sy, cw = frame(
        fy, 'invoke(t, Worker.run, args, &r)', 'max_locals=3 max_stack=2',
        [('this', 'Worker@…', YEL), ('i', 'i = k', '#fff'), ('', '0', '#fff'), ('', '(+1 备用)', '#f6f8fa')],
        [('Counts@… (=c)', GREEN), ('', '#fff'), ('', '#fff'), ('', '#f6f8fa')], 1,
        ['pc  = 31   ipc = 28', 'op  = 0xb6 invokevirtual #31',
         'cls = Worker  cp = Worker.cp', 'syncobj = 0（run 非 synchronized）'], 320)
    text(cx, fy + 302, '28: invokevirtual #31 → args0 = sp - argslots(1) = &stack[0]', 11.5, fill=BLUE_S, mono=True)


    # incBlock frame
    fy2 = 726
    cx2, cy2, sy2, cw2 = frame(
        fy2, 'invoke(t, Counts.incBlock, args0, &r)', 'max_locals=3 max_stack=3',
        [('this', 'Counts@…', GREEN), ('', 'lock 对象', YEL), ('', '(handler 用 e)', '#fff'), ('', '(+1 备用)', '#f6f8fa')],
        [('lock 对象', YEL), ('', '#fff'), ('', '#fff'), ('', '#fff'), ('', '#f6f8fa')], 1,
        ['pc  = 7    ipc = 6', 'op  = 0xc2 monitorenter',
         'cls = Counts  cp = Counts.cp', 'code = m->code (28 B)',
         'syncobj = 0（同步块不靠它）', 'ret → 调用者的 Slot r'], 326)
    # arrow args0 -> callee locals[0]
    line([(cx, sy + 36), (LX + 22, sy + 36), (LX + 22, cy2 + 36), (cx, cy2 + 36)], color=GREEN_S, marker='ag', sw=2)
    text(LX + 34, fy + 344, 'memcpy(locals, args0, argslots)：实参从调用者的操作数栈复制成被调者 locals[0..]', 11.5, fill=GREEN_S)
    text(LX + 34, fy + 362, '调用者 sp 先退回 args0；返回后 *sp++ = r 把返回值压回调用者栈', 11.5, fill=GREEN_S)

    # notes under frames
    ny = 1080
    lines(LX + 24, ny, [
        '· locals / stack 都是 C 的 VLA（变长数组），活在 invoke() 自己的 C 栈帧里：',
        '  Java 调用一层 = C 递归一层；栈帧大小 ≈ 8×(max_locals+max_stack+3) 字节 + C 寄存器保存区。',
        '· Slot 是 8 字节 union {j,i,a,p}：int/引用 占 1 槽，long/double 占 2 槽（PUSHJ: sp += 2）。',
        '· 本地方法 (m->native)：直接 m->native(t, args, ret)，不建 Java 帧。',
        '· 深度 > 600 → StackOverflowError（16KB 用户栈不够，所以 Java 线程跑在 512KB 的 clone 栈上）。',
    ], 12, gap=21)

    # ---------------- right: loop ----------------
    RX, RW = 684, 692
    panel(RX, 78, RW, 1110, '② invoke() 的执行流程（mjvm.c:1536–1920）', '#fff', INK)
    bx, bw = RX + 24, RW - 48

    def box(y, h, title, rows, fill, stroke, size=11.5, gap=18):
        rect(bx, y, bw, h, fill=fill, stroke=stroke, sw=1.5)
        text(bx + 12, y + 20, title, 13, bold=True, fill=stroke)
        lines(bx + 12, y + 42, rows, size, gap=gap, mono=True)
        return y + h

    y = 112
    y1 = box(y, 66, '入口：native？栈溢出？', ['if(m->native){ m->native(t,args,ret); return t->exc ? -1 : 0; }',
                                            'if(++t->depth > 600) → throw StackOverflowError, return -1'], GREY, GREY_S)
    line([(bx + bw / 2, y1), (bx + bw / 2, y1 + 16)])
    y2 = box(y1 + 16, 84, '建帧', ['Slot locals[m->max_locals+1]; Slot stack[m->max_stack+2];',
                                  'Slot *sp = stack;  pc = 0;  cp = cls->cp;  code = m->code;',
                                  'memset(locals,0,…); memcpy(locals, args, 8*m->argslots);'], BLUE, BLUE_S)
    line([(bx + bw / 2, y2), (bx + bw / 2, y2 + 16)])
    y3 = box(y2 + 16, 66, 'ACC_SYNCHRONIZED 方法：VM 自己加锁（incSafe 走这里）',
             ['syncobj = (flags & ACC_STATIC) ? cls->mirror : locals[0].a;',
              'monitor_enter(t, syncobj);   // static 锁 Class 对象'], YEL, YEL_S)
    line([(bx + bw / 2, y3), (bx + bw / 2, y3 + 16)])

    # loop
    ly = y3 + 16
    LH = 470
    rect(bx, ly, bw, LH, fill=GREEN, stroke=GREEN_S, sw=2)
    text(bx + 12, ly + 22, 'for(;;){  ipc = pc;  op = U1();  t->bytecodes++;  switch(op){ … }  }', 13, bold=True,
         fill=GREEN_S, mono=True)
    text(bx + 12, ly + 42, 'ipc = 本条指令地址（跳转偏移、异常表查找都用它）；U1()/U2() 从 code[pc++] 取操作数',
         11.5, fill=GREEN_S)
    cases = [
        ('aload_n / iload_n', '(sp++)->j = locals[n].j'),
        ('astore_n / istore_n', 'locals[n].j = (--sp)->j'),
        ('dup / iconst_1 / iadd', '*sp = sp[-1]; sp++   PUSHI(POPI()+POPI())'),
        ('getfield #idx', 'f = resolve_field(t,cls,idx)  ← cp[idx].resolved 缓存'),
        ('', 'o = POPA(); NULLCHK(o); push FIELDS(o)[f->index]   volatile→LDAR'),
        ('putfield #idx', 'v = POP; o = POPA(); FIELDS(o)[f->index] = v   volatile→STLR'),
        ('goto / if_icmp*', 'pc = ipc + S2()'),
        ('invokevirtual #idx', 'rm = resolve_method; target = recv->cls->vtable[rm->vindex]'),
        ('', 'sp = args0; invoke(t,target,args0,&r) ← C 递归；<0 → goto exception'),
        ('monitorenter', 'o = POPA(); NULLCHK(o); monitor_enter(t, o)'),
        ('monitorexit', 'o = POPA(); monitor_exit(t, o)   不是 owner → IMSE'),
        ('athrow / THROW()', 't->exc = e;  goto exception'),
        ('ireturn / return', '*ret = *--sp;  syncobj? monitor_exit;  depth--;  return 0'),
    ]
    cy0 = ly + 58
    for i, (op, act) in enumerate(cases):
        yy = cy0 + i * 29
        fill = '#fff'
        if op.startswith('monitor'):
            fill = YEL
        elif op.startswith('athrow'):
            fill = RED_F
        elif op.startswith('ireturn'):
            fill = BLUE
        if op:
            cell(bx + 12, yy, 168, 26, op, fill=fill, size=11)
        cell(bx + 180, yy, bw - 192, 26, '', fill=fill)
        text(bx + 188, yy + 17, act, 11, mono=True)
    text(bx + 12, ly + LH - 14, 'break; → continue（回到循环顶，取下一条）', 12, bold=True, fill=GREEN_S)
    # loop-back arrow
    line([(bx + bw, ly + LH - 18), (bx + bw + 12, ly + LH - 18), (bx + bw + 12, ly + 16), (bx + bw, ly + 16)],
         color=GREEN_S, marker='ag', sw=2)

    # return arrow out
    ret_y = cy0 + 12 * 29 + 13
    # exception box
    ey = ly + LH + 22
    EH = 210
    rect(bx, ey, bw, EH, fill=RED_F, stroke=RED, sw=1.8)
    text(bx + 12, ey + 22, 'exception:   (athrow、THROW()、被调者 invoke 返回 -1 都跳到这里)', 13, bold=True, fill=RED)
    lines(bx + 12, ey + 46, ['e = t->exc;  h = find_handler(t, m, ipc, e);',
                             '  // 遍历 m->exc[]: start ≤ ipc < end 且 (type==0 "any" 或 instanceof)'],
          11.5, gap=18, mono=True)
    rect(bx + 12, ey + 86, bw / 2 - 20, 112, fill='#fff', stroke=GREEN_S)
    text(bx + 22, ey + 106, '找到 handler (h ≥ 0)', 12.5, bold=True, fill=GREEN_S)
    lines(bx + 22, ey + 128, ['t->exc = 0;', 'sp = stack;   // 清空操作数栈', 'PUSHA(e); pc = h;',
                              'continue;   → 回到循环'], 11.5, gap=17, mono=True)
    rect(bx + bw / 2, ey + 86, bw / 2 - 12, 112, fill='#fff', stroke=RED)
    text(bx + bw / 2 + 10, ey + 106, '本方法没有 handler', 12.5, bold=True, fill=RED)
    lines(bx + bw / 2 + 10, ey + 128, ['record_frame(e, m, ipc); // 栈轨迹', 'if(syncobj) monitor_exit(t, syncobj);',
                                       'depth--;  return -1;',
                                       '→ 调用者 goto exception，继续找'], 11.5, gap=17, mono=True)
    # arrow from athrow case to exception
    line([(bx - 8, cy0 + 11 * 29 + 13), (bx - 16, cy0 + 11 * 29 + 13), (bx - 16, ey + 20), (bx, ey + 20)],
         color=RED, marker='ar', sw=2)
    # handler arrow back to loop
    line([(bx + 12, ey + 160), (bx + 4, ey + 160), (bx + 4, ly + 30), (bx + 12, ly + 30)],
         color=GREEN_S, marker='ag', sw=1.6, dash='5,4')

    # ---------------- bottom: heap ----------------
    HY = 1206
    panel(24, HY, W - 48, H - HY - 20, '③ 帧里的指针指向的堆数据（vm_alloc 分配，无 GC）', '#fff', INK)
    # Method
    mx, my = 44, HY + 44
    rect(mx, my, 390, 236, fill=BLUE, stroke=BLUE_S, sw=1.5)
    text(mx + 12, my + 22, 'Method  Counts.incBlock()V   (mjvm.c:61)', 13, bold=True, fill=BLUE_S)
    lines(mx + 12, my + 46, ['flags      = 0x0001 PUBLIC (incSafe: 0x0021)',
                             'max_stack  = 3     max_locals = 3',
                             'code_len   = 28    code → aload_0 getfield …',
                             'argslots   = 1 (this)   rettype = V',
                             'vindex     = vtable 下标',
                             'nexc = 2   exc[]:',
                             '  {start=7,  end=19, handler=22, type=0 any}',
                             '  {start=22, end=25, handler=22, type=0 any}',
                             'native     = 0'], 11.5, gap=19, mono=True)
    # Class
    kx = mx + 406
    rect(kx, my, 420, 236, fill=PURP, stroke=PURP_S, sw=1.5)
    text(kx + 12, my + 22, 'Class  Counts   (mjvm.c:85)', 13, bold=True, fill=PURP_S)
    lines(kx + 12, my + 46, ['cp[] : CPEntry {tag, a, b, v, resolved}',
                             '  #15 Fieldref Counts.lock  → resolved = Field*',
                             '  #28 Fieldref blockSafe    → resolved = Field*',
                             '  首次 resolve_field/method 后缓存（acquire/release）',
                             'vtable[] : Method*，invokevirtual 按 vindex 取',
                             'ninst = 5 个实例槽     statics[] = 静态字段',
                             'mirror → java.lang.Class 对象',
                             '         static synchronized / 类初始化锁 锁它',
                             'state / init_thread : JVMS §5.5 初始化状态'], 11.5, gap=19, mono=True)
    # Objects
    ox = kx + 436
    rect(ox, my, W - 44 - ox, 236, fill=GREEN, stroke=GREEN_S, sw=1.5)
    text(ox + 12, my + 22, 'Object 布局：头 16 字节 + FIELDS(o)[]', 13, bold=True, fill=GREEN_S)
    names = ['cls', 'lock', '[0] racy', '[1] vol', '[2] safe', '[3] blkSafe', '[4] lock']
    widths = [50, 54, 64, 56, 60, 80, 62]
    xx = ox + 14
    for nme, wd in zip(names, widths):
        cell(xx, my + 36, wd, 26, nme, fill='#fff' if nme not in ('cls', 'lock') else '#eaeef2', size=10.5)
        xx += wd
    text(ox + 14, my + 78, 'Counts@…（this）', 11.5, fill=GREEN_S)
    line([(xx - 32, my + 62), (xx - 32, my + 96), (ox + 60, my + 96), (ox + 60, my + 110)], color=GREEN_S,
         marker='ag', sw=1.5)
    cell(ox + 14, my + 112, 52, 26, 'cls', fill='#eaeef2', size=10.5)
    cell(ox + 66, my + 112, 120, 26, 'lock (mark word)', fill=YEL, size=10.5)
    text(ox + 196, my + 130, 'java.lang.Object（锁对象，无字段）', 11.5)
    lines(ox + 14, my + 162, ['lock 字的三种状态（低 2 位是标签）:',
                              '  0                               无锁',
                              '  (tid<<32)|(rec<<2)|01           thin：CAS 一次 (LDAXR/STXR)',
                              '  ObjectMonitor* | 10             inflated：owner/rec/entry/wait 队列'],
          11.2, gap=17, mono=True)

    save(path)


# ======================================================================
# 2. incBlock operand stack timeline
# ======================================================================
def diagram_incblock(path):
    rows = [
        # pc, insn, what mjvm does, stack after, L1, L2, lockword, kind
        ('0', 'aload_0', '(sp++)->j = locals[0].j', ['this'], '', '', '0 (无锁)', 'n'),
        ('1', 'getfield #15', 'Field lock:Object；o=POPA()，压 FIELDS(o)[4]', ['lock'], '', '', '0', 'n'),
        ('4', 'dup', '*sp = sp[-1]; sp++', ['lock', 'lock'], '', '', '0', 'n'),
        ('5', 'astore_1', 'locals[1] = 弹出值（锁对象另存一份）', ['lock'], 'lock', '', '0', 'n'),
        ('6', 'monitorenter', 'o=POPA(); monitor_enter(t,o)：CAS 0 → THIN', [], 'lock', '', 'T2 rec=1 |01', 'm'),
        ('7', 'aload_0', '受保护区间 [7,19) 开始', ['this'], 'lock', '', 'T2 rec=1', 'p'),
        ('8', 'aload_0', '', ['this', 'this'], 'lock', '', 'T2 rec=1', 'p'),
        ('9', 'getfield #28', 'Field blockSafe:I；弹 this，压 v', ['this', 'v'], 'lock', '', 'T2 rec=1', 'p'),
        ('12', 'iconst_1', 'PUSHI(1)   ← 栈深 3 = max_stack', ['this', 'v', '1'], 'lock', '', 'T2 rec=1', 'p'),
        ('13', 'iadd', 'PUSHI(POPI() + POPI())', ['this', 'v+1'], 'lock', '', 'T2 rec=1', 'p'),
        ('14', 'putfield #28', 'v=POP; o=POPA(); FIELDS(o)[3] = v+1', [], 'lock', '', 'T2 rec=1', 'p'),
        ('17', 'aload_1', '取回 astore_1 存的锁对象', ['lock'], 'lock', '', 'T2 rec=1', 'p'),
        ('18', 'monitorexit', 'monitor_exit：CAS THIN → 0（release, STLXR）', [], 'lock', '', '0 (无锁)', 'm'),
        ('19', 'goto 27', 'pc = ipc + 8   跳过 handler', [], 'lock', '', '0', 'n'),
        ('27', 'return', 'syncobj==0；depth--；return 0', [], '', '', '0', 'r'),
    ]
    exc_rows = [
        ('—', '(异常)', '7..18 中任一条抛出 → goto exception；find_handler(ipc∈[7,19)) = 22',
         ['e'], 'lock', '', 'T2 rec=1', 'x'),
        ('22', 'astore_2', 'locals[2] = e（sp = stack 后只压了 e）', [], 'lock', 'e', 'T2 rec=1', 'x'),
        ('23', 'aload_1', '', ['lock'], 'lock', 'e', 'T2 rec=1', 'x'),
        ('24', 'monitorexit', '异常路径也解锁；这里再抛 → 仍命中 [22,25)', [], 'lock', 'e', '0 (无锁)', 'm'),
        ('25', 'aload_2', '', ['e'], 'lock', 'e', '0', 'x'),
        ('26', 'athrow', 't->exc = e；ipc=26 不在任何区间 → return -1', [], 'lock', 'e', '0', 'x'),
    ]
    RH = 34
    top = 252
    H = top + 40 + RH * len(rows) + 60 + RH * len(exc_rows) + 250
    begin(H)
    text(W / 2, 34, 'Counts.incBlock() 逐条字节码：操作数栈 / 局部变量 / 锁字的变化', 22, bold=True, anchor='middle')
    text(W / 2, 58, 'mjavac 生成，javap -c -v 输出：stack=3, locals=3, args_size=1；'
         'synchronized (lock) { blockSafe = blockSafe + 1; }', 13, fill=GREY_S, anchor='middle')

    # exception table + legend
    rect(24, 76, 620, 132, fill=YEL, stroke=YEL_S, sw=1.5)
    text(38, 100, 'Exception table（Code 属性里，find_handler 按顺序扫）', 14, bold=True, fill=YEL_S)
    hdr = ['from', 'to', 'target', 'type']
    for i, hname in enumerate(hdr):
        cell(40 + i * 90, 112, 90, 24, hname, fill='#eaeef2', size=11.5)
    for r, vals in enumerate((['7', '19', '22', 'any'], ['22', '25', '22', 'any'])):
        for i, v in enumerate(vals):
            cell(40 + i * 90, 136 + r * 24, 90, 24, v, fill='#fff', size=11.5)
    lines(410, 128, ['to 不含：[7,19) 是同步块体', '[22,25) 罩住 handler 自己的', 'astore/aload/monitorexit，',
                     '保证锁一定被释放（javac 惯例）'], 11.5, gap=18)

    rect(664, 76, 712, 132, fill=GREY, stroke=GREY_S, sw=1.2)
    text(678, 100, '怎么读', 14, bold=True, fill=GREY_S)
    lines(678, 124, ['· 操作数栈：从左（stack[0]）向右长，画的是该指令执行之后的状态，红线 = sp',
                     '· locals[0] 恒为 this（调用者 memcpy 进来），只画 [1] [2]',
                     '· 锁字 = lock 对象的 mark word：T2 rec=1 |01 = (线程id 2 << 32)|(1 << 2)|THIN（Worker 线程，main 是 1）',
                     '· 黄底 = 加/解锁指令；绿底 = 受保护区间；红底 = 异常 handler 路径'], 12, gap=20)

    cols = [(24, 54, 'pc'), (78, 130, '指令'), (208, 430, 'mjvm 做了什么（mjvm.c invoke()）'),
            (638, 300, '执行后操作数栈 stack[0..2]'), (938, 110, 'locals[1]'), (1048, 110, 'locals[2]'),
            (1158, 218, 'lock 对象的锁字')]

    def header(y):
        for x, w, name in cols:
            cell(x, y, w, 30, name, fill='#eaeef2', size=12, mono=False)

    def row(y, r):
        pc, insn, what, st, l1, l2, lw, kind = r
        fill = {'m': YEL, 'p': GREEN, 'x': RED_F, 'r': BLUE}.get(kind, '#fff')
        for x, w, _ in cols:
            out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{RH}" fill="{fill}" stroke="#d0d7de"/>')
        text(24 + 46, y + 22, pc, 12.5, anchor='end', mono=True)
        text(86, y + 22, insn, 12.5, mono=True, bold=kind == 'm', fill=YEL_S if kind == 'm' else INK)
        text(216, y + 22, what, 11.5)
        sx = 646
        for i in range(3):
            v = st[i] if i < len(st) else ''
            cfill = {'this': GREEN, 'lock': YEL, 'e': RED_F}.get(v, '#fff' if v else '#f6f8fa')
            cell(sx + i * 92, y + 5, 92, RH - 10, v, fill=cfill, size=11.5)
        spx = sx + len(st) * 92
        line([(spx, y + 2), (spx, y + RH - 2)], color=RED, marker=None, sw=2.5)
        for x, v in ((938, l1), (1048, l2)):
            if v:
                cfill = {'lock': YEL, 'e': RED_F}[v]
                cell(x + 10, y + 5, 90, RH - 10, v, fill=cfill, size=11.5)
        text(1166, y + 22, lw, 12, mono=True, fill=YEL_S if 'T2' in lw else GREY_S, bold='T2' in lw and kind == 'm')

    y = top
    text(24, y - 10, '正常路径', 15, bold=True)
    header(y)
    y += 30
    for r in rows:
        row(y, r)
        y += RH
    # bracket for protected range
    p0 = top + 30 + 5 * RH
    p1 = top + 30 + 13 * RH
    line([(18, p0 + 2), (12, p0 + 2), (12, p1 - 2), (18, p1 - 2)], color=GREEN_S, marker=None, sw=2)

    y += 50
    text(24, y - 10, '异常路径（例如 getfield 抛 NullPointerException，或 handler 里 monitorexit 抛 IMSE）', 15,
         bold=True, fill=RED)
    header(y)
    y += 30
    for r in exc_rows:
        row(y, r)
        y += RH

    # bottom notes
    y += 24
    rect(24, y, W - 48, 196, fill='#fff', stroke=GREY_S)
    text(38, y + 24, '对照：另外三个方法', 14, bold=True)
    lines(38, y + 50, [
        'incRacy()     0 aload_0  1 aload_0  2 getfield racy  5 iconst_1  6 iadd  7 putfield racy  10 return'
        '    stack=3 locals=1，getfield…putfield 之间别的线程可插入 → 丢更新',
        'incVolatile() 字节码完全一样，只是字段 flags=0x0041 ACC_VOLATILE：mjvm 的 getfield 用 __atomic_load(ACQUIRE)=LDAR，'
        'putfield 用 SEQ_CST store=STLR',
        '              → 每次读写都可见且有序，但 读-加-写 仍不是原子的，照样丢更新',
        'incSafe()     字节码也一样，方法 flags=0x0021 (PUBLIC|SYNCHRONIZED)：没有 monitorenter/monitorexit 指令，'
        'invoke() 入口 monitor_enter(locals[0])，',
        '              每个 return 前 monitor_exit；异常逃出时在 exception: 末尾 monitor_exit(syncobj)。',
        'Worker.run    28: invokevirtual #31 incBlock → resolve_method → recv->cls->vtable[vindex] → invoke() 递归；'
        '异常从 incBlock 返回 -1 后在 run 的表里继续找。',
    ], 11.5, gap=22, mono=False)
    save(path)


if __name__ == '__main__':
    d = os.path.dirname(os.path.abspath(__file__))
    diagram_interp(os.path.join(d, 'mjvm-interp-frame.svg'))
    diagram_incblock(os.path.join(d, 'incblock-stack.svg'))
    print('ok')
