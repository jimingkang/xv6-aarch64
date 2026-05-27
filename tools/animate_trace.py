import json
import os

from manim import *


TRACE_PATH = os.environ.get("XV6_TRACE_IN", "trace/xv6.jsonl")
MAX_EVENTS = int(os.environ.get("XV6_TRACE_MAX_EVENTS", "60"))
STACK_WORDS = int(os.environ.get("XV6_ANIM_STACK_WORDS", "10"))

KEY_REGISTERS = [
    ("pc", "pc"),
    ("sp", "sp"),
    ("lr", "x30"),
    ("ret", "x0"),
    ("arg0", "x0"),
    ("arg1", "x1"),
    ("arg2", "x2"),
    ("arg3", "x3"),
    ("arg4", "x4"),
    ("arg5", "x5"),
    ("sys", "x7"),
]

TRAPFRAME_FIELDS = [
    ("int64 x29", "x29"),
    ("uint64 x30", "x30"),
    ("uint64 elr", "elr"),
    ("uint64 spsr", "spsr"),
    ("uint64 sp", "sp"),
]


class Xv6Trace(Scene):
    def box(self, text, color=BLUE, width=3.0, height=0.38, font_size=13):
        rect = RoundedRectangle(width=width, height=height, corner_radius=0.04, color=color)
        label = Text(text, font_size=font_size)
        if label.width > width - 0.14:
            label.scale_to_fit_width(width - 0.14)
        return VGroup(rect, label)

    def region_color(self, region):
        return {
            "user": TEAL,
            "user-va": TEAL,
            "kstack": GREEN,
            "trapframe": RED,
            "kernel": BLUE,
            "kernel-va": BLUE,
            "pagetable": BLUE,
            "device": ORANGE,
            "uart": ORANGE,
            "virtio": ORANGE,
            "gic": ORANGE,
            "zero": GRAY,
            "unreadable": DARK_GRAY,
        }.get(region, WHITE)

    def region_label(self, region):
        if region in ("uart", "virtio", "gic"):
            return "device"
        if region == "user-va":
            return "user"
        if region == "kernel-va":
            return "kernel"
        return region

    def proc_color(self, state):
        return {
            "UNUSED": GRAY,
            "USED": BLUE,
            "SLEEPING": ORANGE,
            "RUNNABLE": TEAL,
            "RUNNING": GREEN,
            "ZOMBIE": RED,
        }.get(state, WHITE)

    def short_hex(self, value):
        if value is None:
            return "?"
        if not isinstance(value, str):
            value = str(value)
        if len(value) <= 12:
            return value
        return value[:6] + ".." + value[-4:]

    def make_info(self, event):
        lines = VGroup(
            Text("break: %s" % event["reason"], font_size=15),
            Text("func: %s" % event["function"], font_size=15),
            Text("ticks: %s" % event["ticks"], font_size=15),
            Text("pc: %s" % self.short_hex(event["pc"]), font_size=13),
        )
        lines.arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        return lines

    def make_proc_panel(self, event):
        rows = VGroup()
        row_by_pointer = {}
        for proc in event["procs"]:
            state = proc["state"]
            name = proc["name"] or "-"
            header = Text("proc[%d] pid=%d %s %s" % (
                proc["index"],
                proc["pid"],
                state,
                name,
            ), font_size=9)
            tf = Text("tf %s" % self.short_hex(proc.get("trapframe")), font_size=8)
            pt = Text("pt %s" % self.short_hex(proc.get("pagetable")), font_size=8)
            ks = Text("ks %s" % self.short_hex(proc.get("kstack")), font_size=8)
            body = VGroup(header, tf, pt, ks).arrange(DOWN, aligned_edge=LEFT, buff=0.025)
            rect = RoundedRectangle(width=2.85, height=0.68, corner_radius=0.04, color=self.proc_color(state))
            row = VGroup(rect, body)
            body.move_to(rect).align_to(rect, LEFT).shift(RIGHT * 0.10)
            rows.add(row)
            row_by_pointer[(proc["index"], "trapframe")] = tf
            row_by_pointer[(proc["index"], "pagetable")] = pt
            row_by_pointer[(proc["index"], "kstack")] = ks
        rows.arrange(DOWN, buff=0.05)
        title = Text("proc table", font_size=15)
        panel = VGroup(title, rows).arrange(DOWN, buff=0.10)
        return panel, row_by_pointer

    def make_cpu_panel(self, event):
        rows = VGroup()
        for cpu in event["cpus"]:
            rows.add(self.box(
                "cpu[%d] proc=%s" % (cpu["index"], self.short_hex(cpu["proc"])),
                YELLOW,
                width=2.85,
                height=0.26,
                font_size=9,
            ))
        rows.arrange(DOWN, buff=0.04)
        title = Text("cpus", font_size=15)
        return VGroup(title, rows).arrange(DOWN, buff=0.10)

    def make_register_panel(self, event):
        registers = event.get("registers", {"pc": event.get("pc", "0x0")})
        pointer_regions = {
            pointer["register"]: pointer["region"]
            for pointer in event.get("pointers", [])
        }
        rows = VGroup()
        row_by_register = {}
        for label, reg in KEY_REGISTERS:
            value = self.short_hex(registers.get(reg, "0x0"))
            region = pointer_regions.get(reg)
            color = self.region_color(region) if region else DARK_GRAY
            row = self.box("%-4s %-3s %s" % (label, reg, value), color, width=3.25, height=0.34, font_size=11)
            rows.add(row)
            row_by_register[reg] = row
        rows.arrange(DOWN, buff=0.05)
        title = Text("key registers", font_size=17)
        return VGroup(title, rows).arrange(DOWN, buff=0.12), row_by_register

    def make_callchain_panel(self, event):
        rows = VGroup()
        for frame in event.get("callchain", [])[:7]:
            rows.add(self.box(
                "#%d %s %s" % (
                    frame.get("level", 0),
                    frame.get("function", "?"),
                    self.short_hex(frame.get("pc", "?")),
                ),
                BLUE,
                width=3.25,
                height=0.28,
                font_size=9,
            ))
        if len(rows) == 0:
            rows.add(self.box("callchain unavailable", DARK_GRAY, width=3.25, height=0.28, font_size=9))
        rows.arrange(DOWN, buff=0.04)
        title = Text("function call chain", font_size=17)
        return VGroup(title, rows).arrange(DOWN, buff=0.12)

    def parse_hex(self, value):
        try:
            return int(value, 16)
        except Exception:
            return 0

    def stack_frame_groups(self, event):
        frames = event.get("callchain", [])
        words = event.get("stack", [])[:STACK_WORDS]
        groups = []
        for index, frame in enumerate(frames):
            low = self.parse_hex(frame.get("sp", "0x0"))
            high = self.parse_hex(frames[index + 1].get("sp", "0x0")) if index + 1 < len(frames) else 0
            frame_words = []
            for word in words:
                address = self.parse_hex(word.get("address", "0x0"))
                if low and high and low <= address < high:
                    frame_words.append(word)
                elif low and not high and address >= low:
                    frame_words.append(word)
            groups.append((frame, frame_words))
        if not groups:
            groups.append((None, words))
        return groups

    def stack_frame_box(self, frame, words):
        lines = VGroup()
        if frame is None:
            lines.add(Text("raw stack", font_size=8))
        else:
            location = ""
            if frame.get("file"):
                location = " %s:%s" % (frame.get("file"), frame.get("line", 0))
            lines.add(Text(
                "#%d %s %s%s" % (
                    frame.get("level", 0),
                    frame.get("function", "?"),
                    self.short_hex(frame.get("pc", "?")),
                    location,
                ),
                font_size=7,
            ))
            lines.add(Text(
                "sp %s  fp %s" % (
                    self.short_hex(frame.get("sp", "0x0")),
                    self.short_hex(frame.get("fp", "0x0")),
                ),
                font_size=7,
            ))
        for word in words[:3]:
            lines.add(Text(
                "%s  %s" % (
                    self.short_hex(word.get("address")),
                    self.short_hex(word.get("value")),
                ),
                font_size=7,
            ))
        lines.arrange(DOWN, aligned_edge=LEFT, buff=0.025)
        rect = RoundedRectangle(
            width=3.75,
            height=max(0.34, lines.height + 0.16),
            corner_radius=0.04,
            color=GREEN if frame is not None else DARK_GRAY,
        )
        box = VGroup(rect, lines)
        lines.move_to(rect).align_to(rect, LEFT).shift(RIGHT * 0.08)
        return box

    def stack_sp_box(self, event, prev_event):
        current_sp = event.get("registers", {}).get("sp", "0x0")
        prev_sp = prev_event.get("registers", {}).get("sp", "0x0") if prev_event else current_sp
        if prev_sp == current_sp:
            return self.box(
                "sp %s" % self.short_hex(current_sp),
                GREEN,
                width=3.75,
                height=0.30,
                font_size=10,
            )

        prev_value = self.parse_hex(prev_sp)
        current_value = self.parse_hex(current_sp)
        direction = "push" if current_value < prev_value else "pop"
        labels = VGroup(
            Text("prev sp %s" % self.short_hex(prev_sp), font_size=8),
            Arrow(UP * 0.10, DOWN * 0.10, buff=0, stroke_width=2.0, color=YELLOW),
            Text("sp %s  %s" % (self.short_hex(current_sp), direction), font_size=8),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.03)
        rect = RoundedRectangle(
            width=3.75,
            height=max(0.48, labels.height + 0.16),
            corner_radius=0.04,
            color=YELLOW,
        )
        box = VGroup(rect, labels)
        labels.move_to(rect).align_to(rect, LEFT).shift(RIGHT * 0.08)
        return box

    def make_stack_panel(self, event, prev_event=None):
        rows = VGroup()
        rows.add(self.stack_sp_box(event, prev_event))
        for frame, words in self.stack_frame_groups(event):
            rows.add(self.stack_frame_box(frame, words))
        if len(rows) == 0:
            rows.add(self.box("stack unavailable", DARK_GRAY, width=3.75, height=0.34, font_size=11))
        rows.arrange(DOWN, buff=0.035)
        title = Text("live stack / sp", font_size=18)
        return VGroup(title, rows).arrange(DOWN, buff=0.12)

    def make_trapframe_panel(self, event):
        rows = VGroup()
        trapframe = event.get("trapframe", {})
        for label, name in TRAPFRAME_FIELDS:
            entry = trapframe.get(name, {"value": "?", "region": "unreadable"})
            rows.add(self.box(
                "%-11s %s" % (label, self.short_hex(entry.get("value"))),
                self.region_color(entry.get("region")),
                width=3.75,
                height=0.34,
                font_size=11,
            ))
        rows.arrange(DOWN, buff=0.045)
        title = Text("current proc trapframe", font_size=18)
        return VGroup(title, rows).arrange(DOWN, buff=0.12)

    def make_memory_panel(self):
        user = self.box("user pages\nTTBR0", TEAL, width=1.60, height=0.78, font_size=11)
        pt = self.box("pagetable\npage", BLUE, width=1.60, height=0.78, font_size=11)
        stack = self.box("live stack\nsp", GREEN, width=1.45, height=0.52, font_size=10)
        tf = self.box("trapframe", RED, width=1.45, height=0.52, font_size=10)
        kstack_inner = VGroup(stack, tf).arrange(DOWN, buff=0.06)
        kstack_rect = RoundedRectangle(width=1.75, height=1.22, corner_radius=0.05, color=GREEN)
        kstack = VGroup(kstack_rect, kstack_inner)
        kstack_inner.move_to(kstack_rect)
        kernel = self.box("kernel text/data\nTTBR1", BLUE, width=1.80, height=0.78, font_size=10)
        device = self.box("device mmio", ORANGE, width=1.55, height=0.78, font_size=11)
        regions = VGroup(user, pt, kstack, kernel, device).arrange(RIGHT, buff=0.22)
        title = Text("memory regions and owned objects", font_size=16)
        panel = VGroup(title, regions).arrange(DOWN, buff=0.10)
        boxes = {
            "user": user,
            "user-va": user,
            "pagetable": pt,
            "kstack": kstack,
            "trapframe": tf,
            "kernel": kernel,
            "kernel-va": kernel,
            "device": device,
            "uart": device,
            "virtio": device,
            "gic": device,
        }
        return panel, boxes

    def make_current_proc_text(self, event):
        proc = event.get("current_proc")
        if not proc:
            return Text("current proc: none", font_size=13)
        return Text(
            "current: proc[%d] pid=%d %s  tf=%s  pt=%s" % (
                proc["index"],
                proc["pid"],
                proc["state"],
                self.short_hex(proc["trapframe"]),
                self.short_hex(proc["pagetable"]),
            ),
            font_size=12,
        )

    def make_arrow(self, source, target, color):
        return Arrow(
            source.get_bottom(),
            target.get_top(),
            buff=0.04,
            stroke_width=1.5,
            max_tip_length_to_length_ratio=0.08,
            color=color,
        )

    def make_arrows(self, event, register_rows, proc_pointer_rows, memory_boxes):
        arrows = VGroup()
        emitted = set()
        for pointer in event.get("pointers", []):
            reg = pointer.get("register")
            region = self.region_label(pointer.get("region"))
            if reg not in register_rows or region not in memory_boxes:
                continue
            key = ("reg", reg, region)
            if key in emitted:
                continue
            emitted.add(key)
            arrows.add(self.make_arrow(register_rows[reg], memory_boxes[region], self.region_color(region)))
            if len(arrows) >= 8:
                break

        for pointer in event.get("proc_pointers", [])[:18]:
            proc_key = (pointer.get("proc"), pointer.get("field"))
            region = self.region_label(pointer.get("region"))
            if pointer.get("field") == "pagetable":
                region = "pagetable"
            if proc_key not in proc_pointer_rows or region not in memory_boxes:
                continue
            key = ("proc", proc_key, region)
            if key in emitted:
                continue
            emitted.add(key)
            arrows.add(self.make_arrow(proc_pointer_rows[proc_key], memory_boxes[region], self.region_color(region)))
        return arrows

    def make_event_frame(self, event, prev_event=None):
        proc_panel, proc_pointer_rows = self.make_proc_panel(event)
        proc_panel.to_edge(LEFT).shift(UP * 0.45)

        cpu_panel = self.make_cpu_panel(event)
        cpu_panel.next_to(proc_panel, DOWN, buff=0.20).align_to(proc_panel, LEFT)

        reg_panel, register_rows = self.make_register_panel(event)
        callchain_panel = self.make_callchain_panel(event)
        center = VGroup(reg_panel, callchain_panel).arrange(DOWN, buff=0.20)
        center.move_to(ORIGIN).shift(LEFT * 0.15 + UP * 0.18)

        stack_panel = self.make_stack_panel(event, prev_event)
        trapframe_panel = self.make_trapframe_panel(event)
        right = VGroup(stack_panel, trapframe_panel).arrange(DOWN, buff=0.22)
        right.to_edge(RIGHT).shift(UP * 0.24)

        info = self.make_info(event)
        current = self.make_current_proc_text(event)
        summary = VGroup(info, current).arrange(DOWN, aligned_edge=LEFT, buff=0.06)
        summary.to_edge(UP).shift(DOWN * 0.50)

        memory_panel, memory_boxes = self.make_memory_panel()
        memory_panel.to_edge(DOWN)

        arrows = self.make_arrows(event, register_rows, proc_pointer_rows, memory_boxes)
        return VGroup(proc_panel, cpu_panel, center, right, summary, memory_panel, arrows)

    def construct(self):
        if not os.path.exists(TRACE_PATH):
            self.add(Text("trace file not found: %s" % TRACE_PATH, font_size=26))
            self.wait(1)
            return

        with open(TRACE_PATH) as trace_file:
            events = [json.loads(line) for line in trace_file if line.strip()]

        if not events:
            self.add(Text("trace file is empty", font_size=28))
            self.wait(1)
            return

        events = events[:MAX_EVENTS]
        title = Text("xv6-aarch64 process memory, stack, and trapframe trace", font_size=25)
        title.to_edge(UP)
        frame = self.make_event_frame(events[0])
        self.play(Write(title), FadeIn(frame))

        prev_event = events[0]
        for event in events[1:]:
            self.play(Transform(frame, self.make_event_frame(event, prev_event)), run_time=0.35)
            prev_event = event

        self.wait(1)
