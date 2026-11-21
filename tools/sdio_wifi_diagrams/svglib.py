"""Tiny SVG helpers shared by gen_sdio_wifi_diagrams.py (same look as
diagrams/qh_helpers.py: GitHub-ish palette, CJK sans, mono for code)."""
from html import escape

MONO = 'DejaVu Sans Mono, Menlo, monospace'
FONT = 'PingFang SC, Noto Sans CJK SC, WenQuanYi Zen Hei, Microsoft YaHei, sans-serif'

BLUE, BLUE_S = '#ddf4ff', '#0969da'
YEL, YEL_S = '#fff8c5', '#9a6700'
GREY, GREY_S = '#f6f8fa', '#57606a'
GREEN, GREEN_S = '#dafbe1', '#1a7f37'
PURP, PURP_S = '#fbefff', '#8250df'
RED, RED_BG = '#cf222e', '#ffebe9'
ORANGE, ORANGE_S = '#fff1e5', '#bc4c00'
INK, SUB = '#1f2328', '#57606a'

MARKERS = (('a', '#333'), ('ab', BLUE_S), ('ar', RED), ('ag', GREEN_S),
           ('ay', YEL_S), ('ap', PURP_S), ('ao', ORANGE_S), ('as', SUB))


def e(s):
    return escape(str(s), quote=True)


def twidth(s, size, mono=False):
    w = 0
    for ch in str(s):
        if ord(ch) > 0x2e80:
            w += size
        else:
            w += size * (0.6 if mono else 0.56)
    return w


class Svg:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.out = []

    def add(self, s):
        self.out.append(s)

    def text(self, x, y, s, size=13, fill=INK, bold=False, anchor='start', mono=False,
             italic=False, maxw=None):
        if maxw and twidth(s, size, mono) > maxw:
            size = round(size * maxw / twidth(s, size, mono), 1)
        s = str(s)
        n = len(s) - len(s.lstrip(' '))
        if n and not mono:
            s = '\u2002' * n + s[n:]
        elif n:
            s = '\u00a0' * n + s[n:]
        fam = f' font-family="{MONO}"' if mono else ''
        it = ' font-style="italic"' if italic else ''
        self.out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
                        f'font-weight="{"bold" if bold else "normal"}" text-anchor="{anchor}"{fam}{it}>{e(s)}</text>')

    def lines(self, x, y, items, size=12, gap=None, **kw):
        gap = gap or size + 5
        for i, s in enumerate(items):
            self.text(x, y + i * gap, s, size, **kw)
        return len(items) * gap

    def rect(self, x, y, w, h, fill='#fff', stroke='#8c959f', sw=1.2, rx=8, dash=None, opacity=None):
        d = f' stroke-dasharray="{dash}"' if dash else ''
        o = f' fill-opacity="{opacity}"' if opacity else ''
        self.out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
                        f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}{o}/>')

    def line(self, pts, color='#333', marker='a', dash=None, sw=1.8, start_marker=None):
        d = 'M' + ' L'.join(f'{x:.1f},{y:.1f}' for x, y in pts)
        dd = f' stroke-dasharray="{dash}"' if dash else ''
        m = f' marker-end="url(#{marker})"' if marker else ''
        sm = f' marker-start="url(#{start_marker})"' if start_marker else ''
        self.out.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}"{dd}{m}{sm}/>')

    def badge(self, x, y, n, col=BLUE_S, r=10):
        self.out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{col}"/>')
        s = str(n)
        self.text(x, y + 4.3, s, 11.5 if len(s) < 2 else 10, fill='#fff', bold=True, anchor='middle')

    def box(self, x, y, w, h, title, lines=(), fill=BLUE, stroke=BLUE_S, tsize=13.5,
            lsize=11.5, mono_lines=False, center=False, sw=1.4, dash=None, lcolor=SUB):
        self.rect(x, y, w, h, fill, stroke, sw, 7, dash=dash)
        if center:
            self.text(x + w / 2, y + 20, title, tsize, bold=True, anchor='middle', maxw=w - 10)
            for i, l in enumerate(lines):
                self.text(x + w / 2, y + 37 + i * (lsize + 4), l, lsize, fill=lcolor, anchor='middle',
                          mono=mono_lines, maxw=w - 10)
        else:
            self.text(x + 10, y + 19, title, tsize, bold=True, maxw=w - 16)
            for i, l in enumerate(lines):
                self.text(x + 10, y + 36 + i * (lsize + 4), l, lsize, fill=lcolor, mono=mono_lines,
                          maxw=w - 16)

    def save(self, path, title=''):
        head = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}" font-family="{FONT}" font-size="13">']
        if title:
            head.append(f'<title>{e(title)}</title>')
        head.append('<defs>')
        for mid, col in MARKERS:
            head.append(f'<marker id="{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
                        f'markerHeight="7" orient="auto-start-reverse"><path d="M0,0L10,5L0,10z" fill="{col}"/></marker>')
        head.append('</defs>')
        head.append(f'<rect width="{self.w}" height="{self.h}" fill="#fff"/>')
        with open(path, 'w') as f:
            f.write('\n'.join(head + self.out + ['</svg>']) + '\n')


# ------------------------------------------------------------- byte / bit tables
def byte_table(s, x, y, fields, cell, h=36, sizes=True):
    """fields: (name, nbytes, color[, sub]).  Returns x end."""
    cx = x
    for f in fields:
        name, n, col = f[:3]
        sub = f[3] if len(f) > 3 else (f'{n} B' if sizes else '')
        w = cell * n
        s.add(f'<rect x="{cx:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" fill="{col}" stroke="#57606a"/>')
        s.text(cx + w / 2, y + (15 if sub else 22), name, 11.5, anchor='middle', maxw=w - 4)
        if sub:
            s.text(cx + w / 2, y + 30, sub, 9.5, fill=SUB, anchor='middle', maxw=w - 4)
        cx += w
    return cx


def bit_table(s, x, y, fields, width, nbits=32, numbers=True, show=(0, 7, 8, 9, 15, 16, 25, 26, 27, 28, 30, 31)):
    """fields: (name, hi, lo, color) from MSB down."""
    bw = width / nbits
    if numbers:
        for b in range(nbits):
            if b in show or nbits <= 16:
                s.text(x + (nbits - 1 - b) * bw + bw / 2, y + 10, str(b), 9.5, fill=SUB, anchor='middle')
    for name, hi, lo, col in fields:
        x0 = x + (nbits - 1 - hi) * bw
        w = (hi - lo + 1) * bw
        s.add(f'<rect x="{x0:.1f}" y="{y + 15:.1f}" width="{w:.1f}" height="30" fill="{col}" stroke="#57606a"/>')
        s.text(x0 + w / 2, y + 35, name, 11, anchor='middle', maxw=w - 3)
    return 50


# ------------------------------------------------------------- sequence diagrams
class Seq:
    """Lifelines + numbered messages.  rows: list of tuples
       ('m', a, b, label, kind)   kind: 'call' | 'ret' | 'hw' | 'irq' | 'air' | 'err'
       ('self', a, label)
       ('note', a, b, text_lines, color)
       ('sect', title)
       ('gap', px)
    """
    KIND = {'call': ('#333', 'a', None), 'ret': (SUB, 'as', '5 4'), 'hw': (BLUE_S, 'ab', None),
            'irq': (RED, 'ar', None), 'air': (PURP_S, 'ap', '7 4'), 'data': (GREEN_S, 'ag', None),
            'warn': (ORANGE_S, 'ao', None)}

    def __init__(self, s, lanes, top, x0=40, x1=None, numbered=True):
        self.s, self.lanes, self.top = s, lanes, top
        x1 = x1 or s.w - 40
        n = len(lanes)
        step = (x1 - x0) / n
        self.x = {}
        for i, (key, title, sub, fill, stroke) in enumerate(lanes):
            cx = x0 + step * i + step / 2
            self.x[key] = cx
        self.step = step
        self.n = 0
        self.numbered = numbered
        self.y = top + 64

    def heads(self):
        s = self.s
        for key, title, sub, fill, stroke in self.lanes:
            cx = self.x[key]
            w = min(self.step - 14, 190)
            s.rect(cx - w / 2, self.top, w, 48, fill, stroke, 1.4, 7)
            s.text(cx, self.top + 20, title, 13, bold=True, anchor='middle', maxw=w - 8)
            s.text(cx, self.top + 37, sub, 10.5, fill=SUB, anchor='middle', maxw=w - 8, mono=True)

    def run(self, rows):
        s = self.s
        body = []
        saved = s.out
        s.out = body
        for r in rows:
            t = r[0]
            if t == 'm':
                _, a, b, label, kind = r
                self.msg(a, b, label, kind)
            elif t == 'self':
                self.selfmsg(r[1], r[2], r[3] if len(r) > 3 else 'call')
            elif t == 'note':
                self.note(*r[1:])
            elif t == 'sect':
                self.sect(r[1])
            elif t == 'gap':
                self.y += r[1]
        s.out = saved
        # lifelines below everything
        for key, *_ in self.lanes:
            cx = self.x[key]
            s.add(f'<path d="M{cx:.1f},{self.top + 48} V{self.y + 10:.1f}" stroke="#afb8c1" '
                  f'stroke-width="1.2" stroke-dasharray="4 4"/>')
        s.out.extend(body)
        return self.y + 20

    def _label(self, x, y, label, anchor, maxw):
        parts = label.split('\n')
        wmax = max(twidth(p, 11.5) for p in parts)
        if x + wmax > self.s.w - 16:
            x = max(30, self.s.w - 16 - wmax)
        for i, p in enumerate(parts):
            self.s.text(x, y - 6 - (len(parts) - 1 - i) * 15, p, 11.5, anchor=anchor,
                        maxw=self.s.w - 16 - x, fill=INK if i == 0 else SUB)
        return len(parts)

    def msg(self, a, b, label, kind='call'):
        s = self.s
        col, mk, dash = self.KIND[kind]
        nl = label.count('\n') + 1
        self.y += 12 + 15 * nl
        xa, xb = self.x[a], self.x[b]
        y = self.y
        d = 1 if xb > xa else -1
        s.line([(xa + d * 3, y), (xb - d * 3, y)], col, mk, dash, 1.6)
        lx = min(xa, xb) + 22
        if self.numbered:
            self.n += 1
            s.badge(min(xa, xb) + 11, y - 9 - (nl - 1) * 7.5, self.n, col if kind != 'ret' else SUB, r=8.5)
        self._label(lx + 4, y, label, 'start', 0)
        self.y += 6

    def selfmsg(self, a, label, kind='call'):
        s = self.s
        col, mk, dash = self.KIND[kind]
        nl = label.count('\n') + 1
        self.y += 8
        xa = self.x[a]
        y = self.y
        h = max(18, 15 * nl + 2)
        s.line([(xa + 2, y), (xa + 26, y), (xa + 26, y + h), (xa + 4, y + h)], col, mk, dash, 1.5)
        if self.numbered:
            self.n += 1
            s.badge(xa + 40, y + 2, self.n, col, r=8.5)
        parts = label.split('\n')
        wmax = max(twidth(p, 11.5) for p in parts)
        left = xa + 54 + wmax > self.s.w - 16
        for i, p in enumerate(parts):
            if left:
                s.text(xa - 10, y + 6 + i * 15, p, 11.5, fill=INK if i == 0 else SUB, anchor='end')
            else:
                s.text(xa + 54, y + 6 + i * 15, p, 11.5, fill=INK if i == 0 else SUB, maxw=self.s.w - xa - 70)
        self.y += h + 8

    def note(self, a, b, lines, color='y'):
        s = self.s
        fill, stroke = {'y': (YEL, YEL_S), 'g': (GREEN, GREEN_S), 'r': (RED_BG, RED),
                        'b': (BLUE, BLUE_S), 'p': (PURP, PURP_S), 'o': (ORANGE, ORANGE_S)}[color]
        xa, xb = sorted((self.x[a], self.x[b]))
        xa -= self.step * 0.45
        xb += self.step * 0.45
        need = max(twidth(l, 11.5) for l in lines) + 24
        if xb - xa < need:
            xb = min(self.s.w - 20, xa + need)
            xa = min(xa, xb - need)
        self.y += 10
        h = 10 + 16 * len(lines)
        s.rect(xa, self.y, xb - xa, h, fill, stroke, 1, 5)
        for i, l in enumerate(lines):
            s.text(xa + 10, self.y + 17 + i * 16, l, 11.5, fill=INK if i == 0 else SUB, maxw=xb - xa - 20)
        self.y += h + 4

    def sect(self, title):
        s = self.s
        self.y += 20
        x0 = self.x[self.lanes[0][0]] - self.step / 2 + 4
        x1 = self.x[self.lanes[-1][0]] + self.step / 2 - 4
        s.rect(x0, self.y - 15, x1 - x0, 24, '#eaeef2', '#d0d7de', 1, 4)
        s.text(x0 + 10, self.y + 2, title, 13, bold=True, fill=INK)
        self.y += 10
