import json
import os

import gdb


NCPU = int(os.environ.get("XV6_TRACE_NCPU", "4"))
NPROC = int(os.environ.get("XV6_TRACE_NPROC", "8"))
TRACE_PATH = os.environ.get("XV6_TRACE_OUT", "trace/xv6.jsonl")
GDB_REMOTE = os.environ.get("XV6_GDB_REMOTE", "localhost:26000")
STACK_WORDS = int(os.environ.get("XV6_TRACE_STACK_WORDS", "12"))
USER_LAYOUT_PAGES = int(os.environ.get("XV6_TRACE_USER_LAYOUT_PAGES", "32"))
QUIET = os.environ.get("XV6_TRACE_QUIET", "0") == "0"
DEDUP_WINDOW = int(os.environ.get("XV6_TRACE_DEDUP_WINDOW", "1"))
MAX_HITS_PER_BP = int(os.environ.get("XV6_TRACE_MAX_HITS_PER_BP", "0"))
USER_SYMBOLS = os.environ.get("XV6_TRACE_USER_SYMBOLS", "1") != "0"
USER_PROGRAM = os.environ.get("XV6_TRACE_USER_PROGRAM", "ls")
USER_SYMBOL_FILE = os.environ.get("XV6_TRACE_USER_SYMBOL_FILE", "user/_%s" % USER_PROGRAM)
USER_SYM_FILE = os.environ.get("XV6_TRACE_USER_SYM_FILE", "user/%s.sym" % USER_PROGRAM)
USER_BREAK_FUNCTIONS = [
    name.strip()
    for name in os.environ.get(
        "XV6_TRACE_USER_BREAKS",
        "ls,stat,fmtname,open,read,fstat,write",
    ).split(",")
    if name.strip()
]

PGSIZE = 4096
EXTMEM = 0x40000000
PHYSTOP = EXTMEM + 128 * 1024 * 1024
KERNBASE = 0xFFFFFF8000000000
KERNLINK = KERNBASE + EXTMEM
MAXVA = KERNBASE + (1 << 38)
UART0 = KERNBASE + 0x09000000
VIRTIO0 = KERNBASE + 0x0A000000
GICV3 = KERNBASE + 0x08000000
GICV3_REDIST = KERNBASE + 0x080A0000
PTE_VALID = 1
PTE_TABLE = 2
PTE_AF = 1 << 10
PTE_U = 1 << 6
PXMASK = 0x1FF

PROC_STATE = {
    0: "UNUSED",
    1: "USED",
    2: "SLEEPING",
    3: "RUNNABLE",
    4: "RUNNING",
    5: "ZOMBIE",
}

SYSCALL_NAMES = {
    1: "fork",
    2: "exit",
    3: "wait",
    4: "pipe",
    5: "read",
    6: "kill",
    7: "exec",
    8: "fstat",
    9: "chdir",
    10: "dup",
    11: "getpid",
    12: "sbrk",
    13: "sleep",
    14: "uptime",
    15: "open",
    16: "write",
    17: "mknod",
    18: "unlink",
    19: "link",
    20: "mkdir",
    21: "close",
}

os.makedirs(os.path.dirname(TRACE_PATH) or ".", exist_ok=True)
trace_out = open(TRACE_PATH, "w")
last_signatures = {}
breakpoint_hits = {}
stack0_range = None


def to_int(value):
    return int(value)


def to_hex(value):
    return hex(int(value))


def to_hex64(value):
    return hex(int(value) & 0xFFFFFFFFFFFFFFFF)


def safe_eval(expr, default=None):
    try:
        return gdb.parse_and_eval(expr)
    except gdb.error:
        return default


def safe_string(char_array):
    try:
        if int(char_array[0]) == 0:
            return ""
        return char_array.string()
    except Exception:
        return ""


def read_proc(index):
    proc = gdb.parse_and_eval("proc[%d]" % index)
    state = to_int(proc["state"])
    kstack = to_int(proc["kstack"])
    pagetable = to_int(proc["pagetable"])
    trapframe = to_int(proc["trapframe"])
    return {
        "index": index,
        "pid": to_int(proc["pid"]),
        "state": PROC_STATE.get(state, str(state)),
        "kstack": to_hex(kstack),
        "kstack_region": "kstack" if kstack else "zero",
        "sz": to_int(proc["sz"]),
        "pagetable": to_hex(pagetable),
        "pagetable_region": "pagetable" if pagetable else "zero",
        "trapframe": to_hex(trapframe),
        "trapframe_region": "trapframe" if trapframe else "zero",
        "name": safe_string(proc["name"]),
    }


def proc_address(index):
    return to_int(gdb.parse_and_eval("&proc[%d]" % index))


def read_cpu(index):
    cpu = gdb.parse_and_eval("cpus[%d]" % index)
    return {
        "index": index,
        "proc": to_hex(cpu["proc"]),
        "noff": to_int(cpu["noff"]),
        "intena": to_int(cpu["intena"]),
    }


def current_function():
    try:
        frame = gdb.selected_frame()
        return frame.name() or "?"
    except gdb.error:
        return "?"


def current_pc():
    pc = safe_eval("$pc", 0)
    return to_hex(pc)


def read_ticks():
    ticks = safe_eval("ticks", 0)
    return to_int(ticks)


def read_registers():
    regs = {}
    for name in ["pc", "sp"] + ["x%d" % i for i in range(31)]:
        value = safe_eval("$%s" % name)
        regs[name] = to_hex(value) if value is not None else "0x0"
    return regs


def classify_address(value, current_proc=None):
    if value == 0:
        return "zero"
    stack0 = read_stack0_range()
    if stack0 is not None and stack0[0] <= value < stack0[1]:
        return "kernel-stack"
    if current_proc is not None:
        kstack = int(current_proc["kstack"], 16)
        trapframe = int(current_proc["trapframe"], 16)
        if trapframe <= value < trapframe + 34 * 8:
            return "trapframe"
        if kstack <= value < kstack + PGSIZE:
            return "kstack"
        if 0 < value < current_proc["sz"]:
            return "user"
    if KERNLINK <= value < KERNBASE + PHYSTOP:
        return "kernel"
    if UART0 <= value < UART0 + PGSIZE:
        return "uart"
    if VIRTIO0 <= value < VIRTIO0 + PGSIZE:
        return "virtio"
    if GICV3 <= value < GICV3 + 0x10000 or GICV3_REDIST <= value < GICV3_REDIST + 0xF60000:
        return "gic"
    if KERNBASE <= value < MAXVA:
        return "kernel-va"
    if 0 < value < MAXVA:
        return "user-va"
    return "unknown"


def read_stack0_range():
    global stack0_range
    if stack0_range is not None:
        return stack0_range
    value = safe_eval("&stack0")
    if value is None:
        return None
    start = to_int(value)
    stack0_range = (start, start + NCPU * PGSIZE)
    return stack0_range


def read_stack_owner(regs, procs, current_proc):
    sp = int(regs.get("sp", "0x0"), 16)
    for proc in procs:
        kstack = int(proc["kstack"], 16)
        if kstack <= sp < kstack + PGSIZE:
            return {
                "kind": "process-kstack",
                "label": "proc[%d] kernel stack" % proc["index"],
                "proc": proc["index"],
                "base": proc["kstack"],
                "limit": to_hex(kstack + PGSIZE),
            }

    stack0 = read_stack0_range()
    if stack0 is not None and stack0[0] <= sp < stack0[1]:
        cpu = (sp - stack0[0]) // PGSIZE
        return {
            "kind": "kernel-stack0",
            "label": "kernel stack0 cpu[%d]" % cpu,
            "cpu": cpu,
            "base": to_hex(stack0[0] + cpu * PGSIZE),
            "limit": to_hex(stack0[0] + (cpu + 1) * PGSIZE),
        }

    region = classify_address(sp, current_proc)
    if region in ("kernel", "kernel-va"):
        return {
            "kind": "kernel",
            "label": "kernel stack",
            "base": "?",
            "limit": "?",
        }
    if region in ("user", "user-va"):
        return {
            "kind": "user",
            "label": "user stack",
            "base": "0x0",
            "limit": current_proc["sz"] if current_proc else "?",
        }
    return {
        "kind": region,
        "label": region,
        "base": "?",
        "limit": "?",
    }


def find_current_proc(procs, cpus, regs):
    sp = int(regs.get("sp", "0x0"), 16)

    for proc in procs:
        kstack = int(proc["kstack"], 16)
        if kstack <= sp < kstack + PGSIZE:
            return proc

    proc_to_index = {}
    for proc in procs:
        try:
            proc_to_index[proc_address(proc["index"])] = proc["index"]
        except gdb.error:
            pass

    for cpu in cpus:
        proc_addr = int(cpu["proc"], 16)
        if proc_addr in proc_to_index:
            return procs[proc_to_index[proc_addr]]

    for proc in procs:
        if proc["state"] == "RUNNING":
            return proc

    return None


def read_u64(address):
    try:
        inferior = gdb.selected_inferior()
        data = inferior.read_memory(address, 8).tobytes()
        return int.from_bytes(data, byteorder="little")
    except Exception:
        return None


def p2v(pa):
    return pa + KERNBASE


def pte2pa(pte):
    return pte & 0xFFFFFFFFF000


def px(level, va):
    return (va >> (39 - level * 9)) & PXMASK


def read_user_pte(pagetable, va):
    table = pagetable
    for level in [1, 2]:
        pte = read_u64(table + px(level, va) * 8)
        if pte is None or (pte & PTE_VALID) == 0 or (pte & PTE_TABLE) == 0:
            return None
        table = p2v(pte2pa(pte))
    return read_u64(table + px(3, va) * 8)


def translate_user_va(current_proc, va):
    if current_proc is None:
        return None
    if va <= 0 or va >= current_proc["sz"]:
        return None
    pagetable = int(current_proc["pagetable"], 16)
    if pagetable == 0:
        return None
    pte = read_user_pte(pagetable, va)
    if pte is None or (pte & PTE_VALID) == 0 or (pte & PTE_AF) == 0:
        return None
    return pte2pa(pte) + (va & (PGSIZE - 1))


def classify_user_page(va, sz, mapped):
    if not mapped:
        return "unmapped"
    if sz >= 2 * PGSIZE and va == sz - 2 * PGSIZE:
        return "guard"
    if sz >= PGSIZE and va >= sz - PGSIZE:
        return "user stack"
    return "text/data"


def read_user_layout(current_proc):
    if current_proc is None:
        return []
    pagetable = int(current_proc["pagetable"], 16)
    sz = current_proc["sz"]
    if pagetable == 0 or sz == 0:
        return []

    pages = []
    limit = min((sz + PGSIZE - 1) // PGSIZE, USER_LAYOUT_PAGES)
    for page_index in range(limit):
        va = page_index * PGSIZE
        pte = read_user_pte(pagetable, va)
        mapped = pte is not None and (pte & PTE_VALID) == PTE_VALID and (pte & PTE_AF) != 0
        pages.append({
            "va": to_hex(va),
            "pa": to_hex(pte2pa(pte)) if mapped else "-",
            "pte": to_hex(pte) if pte is not None else "-",
            "mapped": mapped,
            "region": classify_user_page(va, sz, mapped),
            "user": bool(pte and (pte & PTE_U)),
        })

    if ((sz + PGSIZE - 1) // PGSIZE) > USER_LAYOUT_PAGES:
        pages.append({
            "va": "...",
            "pa": "...",
            "pte": "...",
            "mapped": False,
            "region": "truncated",
            "user": False,
        })
    return pages


def read_stack(regs, current_proc):
    sp = int(regs.get("sp", "0x0"), 16)
    words = []
    for index in range(STACK_WORDS):
        address = sp + index * 8
        value = read_u64(address)
        words.append({
            "index": index,
            "address": to_hex(address),
            "value": to_hex(value) if value is not None else "?",
            "region": classify_address(value, current_proc) if value is not None else "unreadable",
        })
    return words


TRAPFRAME_FIELDS = [
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30",
    "elr", "spsr", "sp",
]


def read_trapframe(current_proc):
    if current_proc is None or int(current_proc["trapframe"], 16) == 0:
        return {}
    base = int(current_proc["trapframe"], 16)
    trapframe = gdb.parse_and_eval("*(struct trapframe *)%s" % current_proc["trapframe"])
    fields = {}
    for index, field in enumerate(TRAPFRAME_FIELDS):
        try:
            value = to_int(trapframe[field])
            region = "status" if field == "spsr" else classify_address(value, current_proc)
            fields[field] = {
                "address": to_hex(base + index * 8),
                "value": to_hex(value),
                "region": region,
            }
        except Exception:
            fields[field] = {
                "address": to_hex(base + index * 8),
                "value": "?",
                "region": "unreadable",
            }
    return fields


def read_pointers(regs, current_proc):
    pointers = []
    for name, raw_value in regs.items():
        value = int(raw_value, 16)
        region = classify_address(value, current_proc)
        if region not in ("zero", "unknown"):
            pointers.append({
                "register": name,
                "value": raw_value,
                "region": region,
            })
    return pointers


def read_callchain(limit=8):
    chain = []
    try:
      frame = gdb.selected_frame()
    except gdb.error:
      return chain

    level = 0
    while frame is not None and level < limit:
        try:
            sal = frame.find_sal()
            chain.append({
                "level": level,
                "function": frame.name() or "?",
                "pc": to_hex(frame.pc()),
                "sp": frame_register_hex(frame, "sp"),
                "fp": frame_register_hex(frame, "x29"),
                "lr": frame_register_hex(frame, "x30"),
                "file": sal.symtab.filename if sal.symtab else "",
                "line": sal.line,
            })
            frame = frame.older()
            level += 1
        except Exception:
            break
    return chain


def frame_register_hex(frame, name):
    try:
        return to_hex64(frame.read_register(name))
    except Exception:
        return "0x0"


def disassemble(address, count=5):
    if address == 0:
        return []
    try:
        output = gdb.execute("x/%di 0x%x" % (count, address), to_string=True)
    except gdb.error:
        return []
    lines = []
    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue
        lines.append(line)
    return lines


def read_user_disasm(va, count=4):
    # This works when the target MMU context has the process TTBR0 loaded.
    return disassemble(va, count)


def read_user_stack_words(va, count=4):
    words = []
    if va == 0:
        return words
    for index in range(count):
        address = va + index * 8
        value = read_u64(address)
        words.append({
            "address": to_hex(address),
            "value": to_hex(value) if value is not None else "?",
        })
    return words


def read_user_refs(current_proc, trapframe):
    refs = []
    if current_proc is None or not trapframe:
        return refs

    targets = [
        ("elr", "pc", "code"),
        ("x30", "lr", "code"),
        ("x29", "fp", "stack"),
        ("sp", "sp", "stack"),
    ]
    seen = set()
    for field, label, kind in targets:
        entry = trapframe.get(field)
        if not entry:
            continue
        try:
            va = int(entry["value"], 16)
        except Exception:
            continue
        if va == 0 or (field, va) in seen:
            continue
        seen.add((field, va))
        pa = translate_user_va(current_proc, va)
        region = classify_address(va, current_proc)
        ref = {
            "field": field,
            "label": label,
            "kind": kind,
            "va": to_hex(va),
            "pa": to_hex(pa) if pa is not None else "-",
            "region": region,
            "mapped": pa is not None,
            "lines": [],
            "words": [],
        }
        if kind == "code":
            ref["lines"] = read_user_disasm(va, 4) if pa is not None else []
        else:
            ref["words"] = read_user_stack_words(va, 4) if pa is not None else []
        refs.append(ref)
    return refs


def read_register_int(regs, name):
    try:
        return int(regs.get(name, "0x0"), 16)
    except Exception:
        return 0


def read_trapframe_int(trapframe, field):
    try:
        return int(trapframe[field]["value"], 16)
    except Exception:
        return 0


def pointer_detail(value, current_proc):
    return {
        "value": to_hex(value),
        "region": classify_address(value, current_proc),
    }


def read_syscall_details(current_proc, trapframe):
    num = read_trapframe_int(trapframe, "x7")
    args = []
    for index in range(6):
        field = "x%d" % index
        value = read_trapframe_int(trapframe, field)
        args.append({
            "index": index,
            "register": field,
            "value": to_hex(value),
            "region": classify_address(value, current_proc),
        })
    return {
        "kind": "syscall",
        "num": num,
        "name": SYSCALL_NAMES.get(num, "unknown"),
        "args": args,
        "return_register": "x0",
        "return_value": trapframe.get("x0", {}).get("value", "0x0"),
    }


def read_walkaddr_details(regs, current_proc):
    pagetable = read_register_int(regs, "x0")
    va = read_register_int(regs, "x1")
    return {
        "kind": "walkaddr",
        "pagetable": pointer_detail(pagetable, current_proc),
        "va": pointer_detail(va, current_proc),
        "indexes": {
            "level1": px(1, va),
            "level2": px(2, va),
            "level3": px(3, va),
        },
    }


def read_kalloc_details(regs, current_proc):
    freelist = safe_eval("kmem.freelist", 0)
    freelist_value = to_int(freelist) if freelist is not None else 0
    return {
        "kind": "kalloc",
        "freelist": pointer_detail(freelist_value, current_proc),
    }


def read_kfree_details(regs, current_proc):
    va = read_register_int(regs, "x0")
    return {
        "kind": "kfree",
        "va": pointer_detail(va, current_proc),
    }


def read_return_details(reason, regs, current_proc):
    value = read_register_int(regs, "x0")
    return {
        "kind": reason,
        "return_register": "x0",
        "return_value": pointer_detail(value, current_proc),
    }


def read_event_details(reason, regs, current_proc, trapframe):
    function = current_function()
    name = reason.split(":", 1)[0]
    if reason.endswith(":return"):
        return read_return_details(reason, regs, current_proc)
    if reason.startswith("user:"):
        parts = reason.split(":")
        raw_function = parts[2] if len(parts) > 2 else function
        user_function, _, user_address = raw_function.partition("@")
        return {
            "kind": "user",
            "program": parts[1] if len(parts) > 1 else "",
            "function": user_function,
            "address": user_address,
        }
    if name == "syscall" or function == "syscall":
        return read_syscall_details(current_proc, trapframe)
    #if name == "walkaddr" or function == "walkaddr":
    #    return read_walkaddr_details(regs, current_proc)
    #if name == "kalloc" or function == "kalloc":
    #    return read_kalloc_details(regs, current_proc)
    #if name == "kfree" or function == "kfree":
    #    return read_kfree_details(regs, current_proc)
    return {"kind": "breakpoint"}


def read_kernel_disasm(regs, callchain):
    entries = []
    seen = set()
    targets = [("pc", int(regs.get("pc", "0x0"), 16))]
    for frame in callchain[:6]:
        try:
            targets.append(("#%d %s" % (frame["level"], frame["function"]), int(frame["pc"], 16)))
        except Exception:
            pass
    for label, address in targets:
        if address < KERNBASE:
            continue
        if address in seen:
            continue
        seen.add(address)
        entries.append({
            "label": label,
            "address": to_hex(address),
            "lines": disassemble(address, 4),
        })
    return entries


def read_current_user_disasm(regs, current_proc, count=5):
    if current_proc is None:
        return []
    pc = int(regs.get("pc", "0x0"), 16)
    if pc == 0 or pc >= KERNBASE:
        return []
    pa = translate_user_va(current_proc, pc)
    return [{
        "label": "pc",
        "address": to_hex(pc),
        "pa": to_hex(pa) if pa is not None else "-",
        "mapped": pa is not None,
        "lines": read_user_disasm(pc, count) if pa is not None else disassemble(pc, count),
    }]


def read_proc_pointers(procs):
    pointers = []
    for proc in procs:
        for field in ["kstack", "pagetable", "trapframe"]:
            value = proc[field]
            if value == "0x0":
                continue
            pointers.append({
                "proc": proc["index"],
                "field": field,
                "value": value,
                "region": proc.get("%s_region" % field, "unknown"),
            })
    return pointers


def snapshot(reason):
    regs = read_registers()
    cpus = [read_cpu(i) for i in range(NCPU)]
    procs = [read_proc(i) for i in range(NPROC)]
    current_proc = find_current_proc(procs, cpus, regs)
    callchain = read_callchain()
    trapframe = read_trapframe(current_proc)
    data = {
        "reason": reason,
        "function": current_function(),
        "pc": regs["pc"],
        "ticks": read_ticks(),
        "cpus": cpus,
        "procs": procs,
        "registers": regs,
        "current_proc": current_proc,
        "stack_owner": read_stack_owner(regs, procs, current_proc),
        "stack": read_stack(regs, current_proc),
        "trapframe": trapframe,
        "pointers": read_pointers(regs, current_proc),
        "proc_pointers": read_proc_pointers(procs),
        "callchain": callchain,
        "kernel_disasm": read_kernel_disasm(regs, callchain),
        "user_disasm": read_current_user_disasm(regs, current_proc),
        "user_layout": read_user_layout(current_proc),
        "user_refs": read_user_refs(current_proc, trapframe),
        "details": read_event_details(reason, regs, current_proc, trapframe),
    }

    signature = (
        reason,
        data["pc"],
        data["ticks"],
        data["current_proc"]["pid"] if data["current_proc"] else -1,
        data["registers"].get("sp", "0x0"),
    )
    history = last_signatures.setdefault(reason, [])
    if DEDUP_WINDOW > 0 and signature in history:
        return False
    history.append(signature)
    del history[:-DEDUP_WINDOW]

    trace_out.write(json.dumps(data) + "\n")
    trace_out.flush()
    if not QUIET:
        print("[trace] %s %s pc=%s ticks=%d" % (
            data["reason"],
            data["function"],
            data["pc"],
            data["ticks"],
        ))
    return True


class TraceBreakpoint(gdb.Breakpoint):
    def __init__(self, spec):
        super().__init__(spec)
        self.spec = spec

    def stop(self):
        breakpoint_hits[self.spec] = breakpoint_hits.get(self.spec, 0) + 1
        if MAX_HITS_PER_BP > 0 and breakpoint_hits[self.spec] > MAX_HITS_PER_BP:
            return False
        snapshot(self.spec)
        return False


class UserProgramBreakpoint(gdb.Breakpoint):
    def __init__(self, program, function, address):
        spec = "*0x%x" % address
        super().__init__(spec)
        self.program = program
        self.function = function
        self.address = address
        self.spec = "user:%s:%s@0x%x" % (program, function, address)

    def stop(self):
        regs = read_registers()
        cpus = [read_cpu(i) for i in range(NCPU)]
        procs = [read_proc(i) for i in range(NPROC)]
        current_proc = find_current_proc(procs, cpus, regs)
        if current_proc is None or current_proc.get("name") != self.program:
            return False

        breakpoint_hits[self.spec] = breakpoint_hits.get(self.spec, 0) + 1
        if MAX_HITS_PER_BP > 0 and breakpoint_hits[self.spec] > MAX_HITS_PER_BP:
            return False
        snapshot(self.spec)
        return False


def read_user_symbols(sym_path):
    symbols = {}
    try:
        with open(sym_path) as sym_file:
            for raw in sym_file:
                parts = raw.split()
                if len(parts) != 2:
                    continue
                try:
                    symbols[parts[1]] = int(parts[0], 16)
                except ValueError:
                    pass
    except OSError as error:
        print("[trace] skip user symbols: %s" % error)
    return symbols


def setup_user_program_breakpoints():
    if not USER_SYMBOLS:
        return
    if not os.path.exists(USER_SYMBOL_FILE):
        print("[trace] skip user symbol file %s: not found" % USER_SYMBOL_FILE)
        return

    try:
        gdb.execute("set confirm off", to_string=True)
        gdb.execute("add-symbol-file %s 0" % USER_SYMBOL_FILE, to_string=True)
        print("[trace] loaded user symbols: %s at 0x0" % USER_SYMBOL_FILE)
    except gdb.error as error:
        print("[trace] skip add-symbol-file %s: %s" % (USER_SYMBOL_FILE, error))

    symbols = read_user_symbols(USER_SYM_FILE)
    for function in USER_BREAK_FUNCTIONS:
        address = symbols.get(function)
        if address is None:
            print("[trace] skip user breakpoint %s: symbol not found" % function)
            continue
        try:
            UserProgramBreakpoint(USER_PROGRAM, function, address)
        except gdb.error as error:
            print("[trace] skip user breakpoint %s: %s" % (function, error))


BREAKPOINTS = [
    "scheduler",
    "switchuvm",
    "switchkvm",
    "usertrap",
    "userirq",
    "kernelirq",
    "syscall",
    "allocproc",
    "fork",
    "exit",
    "kalloc",
    "kfree",
    "walkaddr",
]


for breakpoint in BREAKPOINTS:
    try:
        TraceBreakpoint(breakpoint)
    except gdb.error as error:
        print("[trace] skip breakpoint %s: %s" % (breakpoint, error))

setup_user_program_breakpoints()

gdb.execute("target remote %s" % GDB_REMOTE)
gdb.execute("continue")
