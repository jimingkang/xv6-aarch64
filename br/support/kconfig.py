#!/usr/bin/env python3
"""Minimal self-contained Kconfig engine for the xv6 Buildroot-like framework.

Supported syntax:
  config / menuconfig, bool, string, prompt, default [if], depends on,
  select [if], choice/endchoice, source (glob), menu/endmenu, if/endif,
  comment, mainmenu, help.
Expressions: SYM, "str", !, &&, ||, (), =, !=.

Commands:
  kconfig.py <Config.in> defconfig <file> <.config>
  kconfig.py <Config.in> olddefconfig <.config>
  kconfig.py <Config.in> savedefconfig <.config> <out>
  kconfig.py <Config.in> menuconfig <.config>
"""
import glob
import os
import re
import shlex
import sys


# ---------------------------------------------------------------- expressions
def tokenize(s):
    toks = []
    i = 0
    while i < len(s):
        c = s[i]
        if c.isspace():
            i += 1
        elif s.startswith("&&", i) or s.startswith("||", i) or s.startswith("!=", i):
            toks.append(s[i:i + 2]); i += 2
        elif c in "!()=":
            toks.append(c); i += 1
        elif c == '"':
            j = i + 1
            while j < len(s) and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            toks.append(("str", s[i + 1:j])); i = j + 1
        else:
            m = re.match(r"[A-Za-z0-9_]+", s[i:])
            if not m:
                raise SyntaxError("bad expression: %r" % s)
            toks.append(("sym", m.group(0))); i += len(m.group(0))
    return toks


def parse_expr(s):
    toks = tokenize(s)
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take():
        t = toks[pos[0]]; pos[0] += 1; return t

    def p_or():
        n = p_and()
        while peek() == "||":
            take(); n = ("or", n, p_and())
        return n

    def p_and():
        n = p_not()
        while peek() == "&&":
            take(); n = ("and", n, p_not())
        return n

    def p_not():
        if peek() == "!":
            take(); return ("not", p_not())
        return p_cmp()

    def p_cmp():
        n = p_atom()
        if peek() in ("=", "!="):
            op = take(); n = (op, n, p_atom())
        return n

    def p_atom():
        t = take()
        if t == "(":
            n = p_or()
            if take() != ")":
                raise SyntaxError("missing ) in %r" % s)
            return n
        if isinstance(t, tuple):
            return t
        raise SyntaxError("unexpected %r in %r" % (t, s))

    n = p_or()
    if peek() is not None:
        raise SyntaxError("trailing tokens in %r" % s)
    return n


# ---------------------------------------------------------------- model
class Sym:
    def __init__(self, name):
        self.name = name
        self.type = None          # "bool" | "string"
        self.prompt = None
        self.defaults = []        # (value_expr_or_str, cond)
        self.deps = []            # list of conds (inherited menus/ifs + depends on)
        self.selects = []         # (target, cond)
        self.help = ""
        self.choice = None
        self.user = None          # user value (str)
        self.value = None
        self.visible = False
        self.loc = ""


class Choice:
    def __init__(self):
        self.prompt = None
        self.members = []
        self.default = None       # (sym name, cond) list
        self.defaults = []
        self.deps = []
        self.help = ""


class Menu:
    def __init__(self, title, kind, deps):
        self.title = title
        self.kind = kind          # "menu" | "comment" | "root"
        self.children = []        # items: Sym | Choice | Menu
        self.deps = deps


class Kconfig:
    def __init__(self, top):
        self.syms = {}
        self.order = []
        self.choices = []
        self.root = Menu("Configuration", "root", [])
        self.mainmenu = "Configuration"
        self.srctree = os.path.dirname(os.path.abspath(top))
        self.base = os.getcwd()
        self._parse_file(top, self.root, [])

    def sym(self, name):
        if name not in self.syms:
            self.syms[name] = Sym(name)
        return self.syms[name]

    # ------------------------------------------------------------ parser
    def _lines(self, path):
        with open(path) as f:
            raw = f.read().split("\n")
        out = []
        i = 0
        while i < len(raw):
            line = raw[i]
            ln = i + 1
            while line.endswith("\\") and i + 1 < len(raw):
                i += 1
                line = line[:-1] + raw[i]
            out.append((ln, line))
            i += 1
        return out

    def _parse_file(self, path, menu, deps):
        lines = self._lines(path)
        stack = [(menu, deps)]
        cur = None          # current Sym/Choice being defined
        choice = None
        i = 0
        while i < len(lines):
            ln, line = lines[i]
            i += 1
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            kw, _, rest = s.partition(" ")
            rest = rest.strip()
            parent, pdeps = stack[-1]
            where = "%s:%d" % (path, ln)

            if kw in ("help", "---help---"):
                indent = None
                text = []
                while i < len(lines):
                    l2 = lines[i][1]
                    if not l2.strip():
                        text.append(""); i += 1; continue
                    ind = len(l2) - len(l2.lstrip())
                    if indent is None:
                        indent = ind
                    if ind < indent or ind == 0:
                        break
                    text.append(l2.strip()); i += 1
                if cur is not None:
                    cur.help = "\n".join(text).strip()
                continue

            if kw in ("config", "menuconfig"):
                cur = self.sym(rest)
                cur.loc = where
                if cur.name not in self.order:
                    self.order.append(cur.name)
                cur.deps = list(pdeps)
                if choice is not None:
                    cur.choice = choice
                    choice.members.append(cur)
                else:
                    parent.children.append(cur)
            elif kw == "choice":
                choice = Choice()
                choice.deps = list(pdeps)
                cur = choice
                self.choices.append(choice)
                parent.children.append(choice)
            elif kw == "endchoice":
                choice = None
                cur = None
            elif kw in ("bool", "string"):
                cur.type = kw
                if rest:
                    cur.prompt = self._str(rest)
            elif kw == "prompt":
                cur.prompt = self._str(rest)
            elif kw == "default":
                val, cond = self._split_if(rest)
                cur.defaults.append((val, cond))
            elif kw == "depends":
                expr = rest[3:].strip() if rest.startswith("on ") else rest
                cur.deps.append(parse_expr(expr))
            elif kw == "select":
                tgt, cond = self._split_if(rest)
                cur.selects.append((tgt.strip(), cond))
            elif kw == "source":
                pat = self._str(rest)
                files = sorted(glob.glob(os.path.join(self.base, pat)))
                for fpath in files:
                    self._parse_file(fpath, parent, pdeps)
                cur = None
            elif kw == "menu":
                m = Menu(self._str(rest), "menu", list(pdeps))
                parent.children.append(m)
                stack.append((m, list(pdeps)))
                cur = m
            elif kw == "endmenu":
                stack.pop()
                cur = None
            elif kw == "if":
                e = parse_expr(rest)
                stack.append((parent, pdeps + [e]))
                cur = None
            elif kw == "endif":
                stack.pop()
                cur = None
            elif kw == "comment":
                m = Menu(self._str(rest), "comment", list(pdeps))
                parent.children.append(m)
                cur = m
            elif kw == "mainmenu":
                self.mainmenu = self._str(rest)
            else:
                raise SyntaxError("%s: unknown keyword %r" % (where, kw))

            # "depends on" after "menu"/"comment" applies to that menu's items
            if kw == "depends" and isinstance(cur, Menu) and stack[-1][0] is cur:
                stack[-1] = (cur, cur.deps)

    @staticmethod
    def _str(s):
        s = s.strip()
        if s.startswith('"'):
            return shlex.split(s)[0]
        return s

    @staticmethod
    def _split_if(s):
        m = re.match(r'^(".*?(?<!\\)"|\S+)\s+if\s+(.*)$', s)
        if m:
            return m.group(1), parse_expr(m.group(2))
        return s, None

    # ------------------------------------------------------------ evaluation
    def ev(self, e):
        """Evaluate to a value: 'y'/'n' for booleans, strings for compare."""
        if e is None:
            return "y"
        t = e[0]
        if t == "sym":
            name = e[1]
            if name in ("y", "n"):
                return name
            if name in self.syms:
                v = self.syms[name].value
                return v if v is not None else "n"
            return name if not re.match(r"^[A-Z_][A-Z0-9_]*$", name) else "n"
        if t == "str":
            return e[1]
        if t == "not":
            return "n" if self.truth(e[1]) else "y"
        if t == "and":
            return "y" if self.truth(e[1]) and self.truth(e[2]) else "n"
        if t == "or":
            return "y" if self.truth(e[1]) or self.truth(e[2]) else "n"
        if t == "=":
            return "y" if self.ev(e[1]) == self.ev(e[2]) else "n"
        if t == "!=":
            return "y" if self.ev(e[1]) != self.ev(e[2]) else "n"
        raise ValueError(e)

    def truth(self, e):
        v = self.ev(e)
        return v not in ("n", "")

    def deps_ok(self, deps):
        return all(self.truth(d) for d in deps)

    def _default(self, s):
        for val, cond in s.defaults:
            if self.truth(cond):
                if s.type == "string":
                    return self._str(val) if val.startswith('"') else self.ev(parse_expr(val))
                return "y" if self.truth(parse_expr(val)) else "n"
        return "" if s.type == "string" else "n"

    def evaluate(self):
        for s in self.syms.values():
            if s.value is None:
                s.value = "" if s.type == "string" else "n"
        for _ in range(64):
            before = {n: s.value for n, s in self.syms.items()}
            selected = set()
            for s in self.syms.values():
                if s.value == "y":
                    for tgt, cond in s.selects:
                        if self.truth(cond):
                            selected.add(tgt)
            for ch in self.choices:
                self._eval_choice(ch)
            for s in self.syms.values():
                if s.choice is not None:
                    continue
                s.visible = s.prompt is not None and self.deps_ok(s.deps)
                if s.type == "bool":
                    if s.name in selected:
                        s.value = "y"
                    elif not self.deps_ok(s.deps):
                        s.value = "n"
                    elif s.visible and s.user is not None:
                        s.value = s.user
                    else:
                        s.value = self._default(s)
                elif s.type == "string":
                    if not self.deps_ok(s.deps):
                        s.value = ""
                    elif s.visible and s.user is not None:
                        s.value = s.user
                    else:
                        s.value = self._default(s)
            after = {n: s.value for n, s in self.syms.items()}
            if after == before:
                return
        sys.stderr.write("kconfig: warning: values did not converge\n")

    def _eval_choice(self, ch):
        ok = self.deps_ok(ch.deps)
        vis = [m for m in ch.members if ok and self.deps_ok(m.deps)]
        for m in ch.members:
            m.visible = m in vis
        pick = None
        for m in vis:
            if m.user == "y":
                pick = m
                break
        if pick is None:
            for val, cond in ch.defaults:
                if self.truth(cond):
                    cand = self.syms.get(val.strip())
                    if cand in vis:
                        pick = cand
                        break
        if pick is None and vis:
            pick = vis[0]
        for m in ch.members:
            m.value = "y" if m is pick else "n"

    # ------------------------------------------------------------ I/O
    def load(self, path):
        if not os.path.exists(path):
            return
        for line in open(path):
            line = line.strip()
            m = re.match(r"^# (\w+) is not set$", line)
            if m:
                if m.group(1) in self.syms:
                    self.syms[m.group(1)].user = "n"
                continue
            m = re.match(r"^(\w+)=(.*)$", line)
            if m and m.group(1) in self.syms:
                s = self.syms[m.group(1)]
                v = m.group(2)
                if s.type == "string":
                    v = shlex.split(v)[0] if v.startswith('"') else v
                s.user = v

    def write(self, path):
        out = ["#", "# Automatically generated file; DO NOT EDIT.",
               "# %s" % self.mainmenu, "#"]
        self._write_menu(self.root, out)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write("\n".join(out) + "\n")
        if os.path.exists(path):
            os.replace(path, path + ".old")
        os.replace(tmp, path)

    def _fmt(self, s):
        if s.type == "string":
            return '%s="%s"' % (s.name, s.value.replace("\\", "\\\\").replace('"', '\\"'))
        if s.value == "y":
            return "%s=y" % s.name
        return "# %s is not set" % s.name

    def _write_menu(self, menu, out):
        for it in menu.children:
            if isinstance(it, Sym):
                if self.deps_ok(it.deps) or it.value == "y":
                    out.append(self._fmt(it))
            elif isinstance(it, Choice):
                for m in it.members:
                    if m.visible:
                        out.append(self._fmt(m))
            elif isinstance(it, Menu) and it.kind == "menu":
                if self.deps_ok(it.deps):
                    out.append("")
                    out.append("#")
                    out.append("# %s" % it.title)
                    out.append("#")
                    self._write_menu(it, out)

    def savedefconfig(self, path):
        """Write the minimal set of symbols that differ from defaults."""
        cur = {n: s.value for n, s in self.syms.items()}
        users = {n: s.user for n, s in self.syms.items()}
        needed = []
        for name in self.order:
            s = self.syms[name]
            if s.type is None or not s.visible:
                continue
            for x in self.syms.values():
                x.user = None
                x.value = None
            for n in needed:
                self.syms[n].user = cur[n]
            self.evaluate()
            if self.syms[name].value != cur[name]:
                needed.append(name)
        for n, s in self.syms.items():
            s.user = users[n]
            s.value = None
        self.evaluate()
        with open(path, "w") as f:
            for n in needed:
                f.write(self._fmt(self.syms[n]) + "\n")
        return needed


# ---------------------------------------------------------------- menuconfig
def menuconfig(kc, path):
    stack = [kc.root]
    dirty = False
    while True:
        kc.evaluate()
        menu = stack[-1]
        rows = []

        def collect(m):
            for it in m.children:
                if isinstance(it, Sym):
                    if it.visible:
                        rows.append(it)
                elif isinstance(it, Choice):
                    if kc.deps_ok(it.deps) and any(x.visible for x in it.members):
                        rows.append(it)
                elif isinstance(it, Menu):
                    if kc.deps_ok(it.deps):
                        rows.append(it)
        collect(menu)
        print("\n=== %s ===" % (" > ".join(m.title for m in stack)))
        for idx, it in enumerate(rows, 1):
            if isinstance(it, Sym):
                if it.type == "bool":
                    print("%3d  [%s] %s" % (idx, "*" if it.value == "y" else " ", it.prompt))
                else:
                    print("%3d  (%s) %s" % (idx, it.value, it.prompt))
            elif isinstance(it, Choice):
                sel = next((m for m in it.members if m.value == "y"), None)
                print("%3d  %s (%s)  --->" % (idx, it.prompt, sel.prompt if sel else "-"))
            elif it.kind == "comment":
                print("      *** %s ***" % it.title)
            else:
                print("%3d  %s  --->" % (idx, it.title))
        print("\n  <N> 切换/进入   ?N 帮助   b 返回   s 保存   q 退出")
        try:
            cmd = input("> ").strip()
        except EOFError:
            cmd = "q"
        if cmd == "q":
            if dirty:
                try:
                    ans = input("保存配置到 %s? [Y/n] " % path).strip().lower()
                except EOFError:
                    ans = "y"
                if ans in ("", "y", "yes"):
                    kc.write(path)
                    print("已保存 %s" % path)
            return
        if cmd == "s":
            kc.write(path)
            dirty = False
            print("已保存 %s" % path)
            continue
        if cmd == "b":
            if len(stack) > 1:
                stack.pop()
            continue
        helpq = cmd.startswith("?")
        num = cmd[1:] if helpq else cmd
        if not num.isdigit() or not (1 <= int(num) <= len(rows)):
            continue
        it = rows[int(num) - 1]
        if helpq:
            if isinstance(it, Sym):
                print("\n%s (%s)\n%s\n" % (it.name, it.loc, it.help or "(无帮助)"))
            elif isinstance(it, Choice):
                print("\n%s\n" % (it.help or "(无帮助)"))
            continue
        if isinstance(it, Menu):
            if it.kind == "menu":
                stack.append(it)
        elif isinstance(it, Choice):
            vis = [m for m in it.members if m.visible]
            for j, m in enumerate(vis, 1):
                print("  %d (%s) %s" % (j, "X" if m.value == "y" else " ", m.prompt))
            try:
                c = input("选择> ").strip()
            except EOFError:
                c = ""
            if c.isdigit() and 1 <= int(c) <= len(vis):
                for m in it.members:
                    m.user = "y" if m is vis[int(c) - 1] else "n"
                dirty = True
        elif it.type == "bool":
            it.user = "n" if it.value == "y" else "y"
            dirty = True
        else:
            try:
                v = input("%s [%s]: " % (it.prompt, it.value))
            except EOFError:
                v = ""
            if v != "":
                it.user = v
                dirty = True


# ---------------------------------------------------------------- main
def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    top, cmd = argv[1], argv[2]
    kc = Kconfig(top)
    if cmd == "defconfig":
        kc.load(argv[3])
        kc.evaluate()
        kc.write(argv[4])
        print("配置已写入 %s（来自 %s）" % (argv[4], argv[3]))
    elif cmd == "olddefconfig":
        kc.load(argv[3])
        kc.evaluate()
        kc.write(argv[3])
    elif cmd == "savedefconfig":
        kc.load(argv[3])
        kc.evaluate()
        kc.savedefconfig(argv[4])
        print("最小配置已写入 %s" % argv[4])
    elif cmd == "menuconfig":
        kc.load(argv[3])
        kc.evaluate()
        menuconfig(kc, argv[3])
    else:
        sys.stderr.write("unknown command %s\n" % cmd)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
