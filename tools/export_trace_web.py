#!/usr/bin/env python3
import json
import os


TRACE_PATH = os.environ.get("XV6_TRACE_IN", "trace/xv6.jsonl")
OUT_PATH = os.environ.get("XV6_TRACE_HTML", "trace/xv6_trace.html")
MAX_EVENTS = int(os.environ.get("XV6_TRACE_MAX_EVENTS", "300"))

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

TRAPFRAME_NAMES = [
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
    "x29", "x30", "elr", "spsr", "sp",
]


HTML_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>xv6 trace viewer</title>
  <style>
    :root {
      --bg: #0f1115;
      --panel: #171b22;
      --panel2: #11151b;
      --text: #e8edf2;
      --muted: #9aa7b3;
      --line: #303744;
      --user: #25b8a6;
      --kstack: #42b86b;
      --trapframe: #ef6262;
      --kernel: #5f8cff;
      --device: #f2a541;
      --gray: #707984;
      --changed: #f6d365;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 12px 18px;
      border-bottom: 1px solid var(--line);
      background: #12161d;
      position: sticky;
      top: 0;
      z-index: 10;
    }
    h1 { font-size: 17px; margin: 0; font-weight: 650; }
    button, input[type="range"] { accent-color: #5f8cff; }
    button {
      background: #232b36;
      color: var(--text);
      border: 1px solid #394352;
      border-radius: 6px;
      padding: 7px 11px;
      font-weight: 600;
      cursor: pointer;
    }
    button:disabled { opacity: 0.45; cursor: default; }
    .controls { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
    .counter { color: var(--muted); font-variant-numeric: tabular-nums; min-width: 108px; text-align: center; }
    .layout {
      position: relative;
      display: grid;
      grid-template-columns: 300px minmax(360px, 1fr);
      grid-template-rows: minmax(430px, auto) minmax(360px, 1fr);
      gap: 12px;
      padding: 14px;
      min-height: calc(100vh - 58px);
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 10px;
      overflow: hidden;
    }
    .panel h2 {
      margin: 0 0 8px;
      font-size: 13px;
      color: #c8d2dc;
      letter-spacing: 0.02em;
      text-transform: uppercase;
    }
    .left { grid-column: 1; grid-row: 1; display: block; }
    .center { grid-column: 2; grid-row: 1; display: grid; grid-template-rows: auto 1fr; gap: 12px; }
    .memory { grid-column: 1 / 3; grid-row: 2; }
    .summary {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 8px 14px;
      font-size: 13px;
      color: var(--muted);
      margin-bottom: 10px;
    }
    .summary b { color: var(--text); font-weight: 650; }
    .execution-strip {
      border: 1px solid #394352;
      background: #101720;
      border-radius: 6px;
      padding: 8px;
      margin-bottom: 10px;
      font-size: 12px;
      color: var(--muted);
      line-height: 1.35;
    }
    .execution-strip b { color: var(--text); }
    .trap-badge {
      display: inline-block;
      border: 1px solid var(--trapframe);
      color: var(--trapframe);
      border-radius: 999px;
      padding: 1px 7px;
      margin-right: 6px;
      font-size: 11px;
      font-weight: 750;
    }
    .rows { display: flex; flex-direction: column; gap: 6px; }
    .row, .proc, .mem-card {
      border: 1px solid var(--line);
      background: var(--panel2);
      border-radius: 6px;
      padding: 6px 8px;
      font-size: 12px;
      line-height: 1.25;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      position: relative;
    }
    .proc { min-height: 72px; min-width: 190px; }
    .proc-row {
      display: flex;
      gap: 8px;
      overflow-x: auto;
      padding-bottom: 4px;
    }
    .proc .title { font-family: inherit; font-weight: 750; margin-bottom: 4px; }
    .proc .ptr { color: var(--muted); }
    .proc.RUNNING { border-color: var(--kstack); box-shadow: inset 3px 0 0 var(--kstack); }
    .proc.RUNNABLE { border-color: var(--user); box-shadow: inset 3px 0 0 var(--user); }
    .proc.SLEEPING { border-color: var(--device); box-shadow: inset 3px 0 0 var(--device); }
    .proc.ZOMBIE { border-color: var(--trapframe); box-shadow: inset 3px 0 0 var(--trapframe); }
    .changed {
      background: rgba(246, 211, 101, 0.10);
      outline: 1px solid rgba(246, 211, 101, 0.55);
    }
    .region-user { border-color: var(--user); }
    .region-kstack { border-color: var(--kstack); }
    .region-trapframe { border-color: var(--trapframe); }
    .region-kernel, .region-kernel-va, .region-pagetable { border-color: var(--kernel); }
    .region-kernel-stack { border-color: var(--kernel); }
    .region-device, .region-uart, .region-virtio, .region-gic { border-color: var(--device); }
    .region-status { border-color: var(--gray); color: var(--muted); }
    .region-zero, .region-unreadable { border-color: var(--gray); }
    .registers {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 6px;
    }
    .callchain .row { font-size: 12px; }
    .memory-map {
      position: relative;
      display: grid;
      grid-template-columns: 1fr 1fr minmax(520px, 1.9fr) 1.1fr 1fr;
      gap: 12px;
      min-height: 320px;
    }
    .mem-card {
      display: flex;
      flex-direction: column;
      justify-content: flex-start;
      align-items: center;
      text-align: center;
      font-family: inherit;
      font-weight: 650;
    }
    .mem-card small { display: block; color: var(--muted); font-weight: 500; margin-top: 4px; }
    .mem-title {
      font-weight: 750;
      margin-bottom: 7px;
      font-size: 13px;
    }
    .mem-content {
      width: 100%;
      display: grid;
      gap: 5px;
      overflow: hidden;
    }
    .kstack-card {
      border-color: var(--kstack);
      display: grid;
      grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
      grid-template-rows: auto minmax(250px, auto);
      gap: 8px;
      align-items: stretch;
      padding: 10px;
      text-align: left;
    }
    .kstack-card .kstack-title {
      grid-column: 1 / -1;
      color: var(--kstack);
      font-weight: 750;
      font-size: 13px;
      text-align: center;
    }
    .nested {
      display: flex;
      flex-direction: column;
      align-items: stretch;
      justify-content: flex-start;
      border: 1px solid var(--line);
      border-radius: 5px;
      font-size: 12px;
      padding: 8px;
      min-height: 250px;
      overflow: auto;
    }
    .nested h3 {
      margin: 0 0 7px;
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.03em;
      color: #c8d2dc;
    }
    .nested.stack-with-trapframe {
      display: grid;
      grid-template-rows: auto auto minmax(130px, auto);
      gap: 8px;
    }
    .nested.stack { border-color: var(--kstack); }
    .nested.kernel-stack { border-color: var(--kernel); }
    .nested.trapframe { border-color: var(--trapframe); }
    .mini-rows { display: grid; gap: 5px; }
    .mini-row {
      border: 1px solid var(--line);
      border-radius: 5px;
      padding: 4px 5px;
      font-size: 11px;
      line-height: 1.18;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      background: #0f141a;
      position: relative;
    }
    .mini-row strong { color: var(--text); }
    .disasm-row {
      border-color: var(--kernel);
      text-align: left;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .page-row {
      text-align: left;
      border-color: var(--user);
    }
    .user-ref-row {
      text-align: left;
      border-color: var(--user);
      box-shadow: inset 3px 0 0 var(--user);
    }
    .user-code-row {
      text-align: left;
      border-color: var(--user);
      box-shadow: inset 3px 0 0 var(--user);
    }
    .user-section {
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 6px;
      display: grid;
      gap: 5px;
      background: #0f141a;
    }
    .user-section-title {
      color: var(--text);
      font-weight: 750;
      font-size: 11px;
      text-align: left;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    }
    .user-section.text-data { border-color: var(--user); }
    .user-ref-row.stack-ref {
      border-color: var(--kstack);
      box-shadow: inset 3px 0 0 var(--kstack);
    }
    .user-ref-detail {
      color: var(--muted);
      margin-top: 3px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .page-row.stack { border-color: var(--kstack); }
    .page-row.guard { border-color: var(--trapframe); }
    .call-frame {
      border-color: var(--kernel);
      box-shadow: inset 3px 0 0 var(--kernel);
    }
    .stack-word { color: var(--muted); }
    .sp-flow {
      display: grid;
      grid-template-columns: 1fr;
      gap: 4px;
      border: 1px solid var(--changed);
      border-radius: 6px;
      background: rgba(246, 211, 101, 0.08);
      padding: 6px;
    }
    .sp-flow .mini-row {
      border-color: var(--changed);
    }
    .stack-frame-box {
      border: 1px solid var(--kstack);
      border-radius: 6px;
      background: #0d1511;
      padding: 6px;
      display: grid;
      gap: 4px;
    }
    .stack-frame-title {
      color: var(--text);
      font-weight: 750;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 11px;
      line-height: 1.25;
    }
    .stack-frame-meta {
      color: var(--muted);
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 11px;
      line-height: 1.25;
    }
    svg#arrows {
      pointer-events: none;
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      z-index: 5;
    }
    .hint { color: var(--muted); font-size: 12px; }
    @media (max-width: 1100px) {
      .layout {
        grid-template-columns: 1fr;
        grid-template-rows: auto auto auto;
      }
      .left, .center, .memory { grid-column: 1; grid-row: auto; }
      svg#arrows { display: none; }
      .memory-map { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <header>
    <h1>xv6-aarch64 trace viewer</h1>
    <div class="controls">
      <button id="prev">Prev</button>
      <button id="next">Next</button>
      <button id="play">Play</button>
      <input id="slider" type="range" min="0" max="0" value="0">
      <span class="counter" id="counter"></span>
    </div>
  </header>
  <main class="layout" id="layout">
    <svg id="arrows"></svg>
    <section class="left">
      <div class="panel">
        <h2>cpus</h2>
        <div class="rows" id="cpus"></div>
      </div>
    </section>
    <section class="center">
      <div class="panel">
        <h2>event</h2>
        <div class="execution-strip" id="execution"></div>
        <div class="summary" id="summary"></div>
        <div class="hint">Use Next/Prev, arrow keys, or the slider to step through trace events.</div>
      </div>
      <div class="panel callchain">
        <h2>function call chain</h2>
        <div class="rows" id="callchain"></div>
      </div>
    </section>
    <section class="memory panel">
      <h2>memory regions and owned objects</h2>
      <div class="memory-map">
        <div class="mem-card region-user" data-region="user">
          <div class="mem-title">user pages</div>
          <small>TTBR0 lower VA</small>
          <div class="mem-content" id="user-pages-layout"></div>
        </div>
        <div class="mem-card region-pagetable" data-region="pagetable">
          <div class="mem-title">pagetable</div>
          <small>current proc VA -> PA</small>
          <div class="mem-content" id="pagetable-layout"></div>
        </div>
        <div class="mem-card kstack-card" data-region="kstack">
          <div class="kstack-title">kernel stacks</div>
          <div class="nested stack stack-with-trapframe" data-region="stack">
            <h3>live stack / sp</h3>
            <div class="mini-rows" id="live-stack"></div>
            <div class="nested trapframe" data-region="trapframe">
              <h3>trapframe</h3>
              <div class="mini-rows" id="live-trapframe"></div>
            </div>
          </div>
          <div class="nested kernel-stack" data-region="kernel-stack">
            <h3>kernel stack / sp</h3>
            <div class="mini-rows" id="kernel-stack"></div>
          </div>
        </div>
        <div class="mem-card region-kernel" data-region="kernel">
          <div class="mem-title">kernel text/data</div>
          <small>TTBR1 high VA</small>
          <div class="mem-content" id="kernel-disasm"></div>
        </div>
        <div class="mem-card region-device" data-region="device">
          <div class="mem-title">device mmio</div>
        </div>
      </div>
      <h2 style="margin-top: 12px;">proc table</h2>
      <div class="proc-row" id="procs"></div>
    </section>
  </main>
  <script id="trace-data" type="application/json">__TRACE_JSON__</script>
  <script>
    const events = JSON.parse(document.getElementById("trace-data").textContent);
    const keyRegisters = __KEY_REGISTERS__;
    const trapframeNames = __TRAPFRAME_NAMES__;
    let index = 0;
    let timer = null;

    const $ = (id) => document.getElementById(id);
    const esc = (s) => String(s ?? "").replace(/[&<>"']/g, c => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
    }[c]));
    const shortHex = (value) => {
      value = String(value ?? "?");
      return value.length <= 13 ? value : value.slice(0, 6) + ".." + value.slice(-4);
    };
    const hexToBigInt = (value) => {
      try { return BigInt(value || "0x0"); } catch (_) { return 0n; }
    };
    const regionLabel = (region) => {
      if (["uart", "virtio", "gic"].includes(region)) return "device";
      if (region === "user-va") return "user";
      if (region === "kernel-va") return "kernel";
      return region;
    };
    const regionClass = (region) => "region-" + regionLabel(region || "zero");

    function displayTrapField(name) {
      return `uint64 ${name}`;
    }

    function row(text, region, attrs = "", extraClass = "") {
      return `<div class="row ${regionClass(region)} ${extraClass}" ${attrs}>${esc(text)}</div>`;
    }

    function miniRow(text, region, attrs = "", extraClass = "") {
      return `<div class="mini-row ${regionClass(region)} ${extraClass}" ${attrs}>${esc(text)}</div>`;
    }

    function compareHexAddress(a, b) {
      const av = hexToBigInt(a);
      const bv = hexToBigInt(b);
      if (av < bv) return -1;
      if (av > bv) return 1;
      return 0;
    }

    function prevEvent() {
      return index > 0 ? events[index - 1] : null;
    }

    function procByIndex(ev, procIndex) {
      return (ev?.procs || []).find(p => p.index === procIndex);
    }

    function currentProcLabel(proc) {
      return proc ? `proc[${proc.index}] pid=${proc.pid} ${proc.state}` : "none";
    }

    function isTrapEvent(ev) {
      return /trap|irq/i.test(ev.reason || ev.function || "");
    }

    function renderExecution(ev) {
      const prev = prevEvent();
      const regs = ev.registers || {};
      const prevRegs = prev?.registers || {};
      const current = ev.current_proc;
      const previous = prev?.current_proc;
      const procSwitch = currentProcLabel(previous) + " -> " + currentProcLabel(current);
      const spFlow = `${shortHex(prevRegs.sp || "-")} -> ${shortHex(regs.sp || "-")}`;
      const pcFlow = `${shortHex(prevRegs.pc || "-")} -> ${shortHex(regs.pc || ev.pc)}`;
      const trap = isTrapEvent(ev)
        ? `<span class="trap-badge">TRAP/IRQ</span>EL1 handler saves CPU state on kernel stack/trapframe; scheduler may switch PCB.`
        : `Normal kernel execution sample.`;
      $("execution").innerHTML =
        `${trap}<br><b>pc</b> ${esc(pcFlow)} &nbsp; <b>sp</b> ${esc(spFlow)} &nbsp; <b>current proc</b> ${esc(procSwitch)}`;
    }

    function renderSummary(ev) {
      const current = ev.current_proc;
      $("summary").innerHTML = [
        ["break", ev.reason],
        ["func", ev.function],
        ["ticks", ev.ticks],
        ["pc", shortHex(ev.pc)],
        ["current", current ? `proc[${current.index}] pid=${current.pid} ${current.state}` : "none"],
        ["tf/pt", current ? `${shortHex(current.trapframe)} / ${shortHex(current.pagetable)}` : "-"],
      ].map(([k, v]) => `<div><b>${esc(k)}</b> ${esc(v)}</div>`).join("");
    }

    function renderProcs(ev) {
      const prev = prevEvent();
      $("procs").innerHTML = ev.procs.map(p => `
        <div class="proc ${esc(p.state)} ${procChanged(p, procByIndex(prev, p.index)) ? "changed" : ""}">
          <div class="title">proc[${p.index}] pid=${p.pid} ${esc(p.state)} ${esc(p.name || "-")}</div>
          <div class="ptr" data-proc="${p.index}" data-field="trapframe">tf ${esc(shortHex(p.trapframe))}</div>
          <div class="ptr" data-proc="${p.index}" data-field="pagetable">pt ${esc(shortHex(p.pagetable))}</div>
          <div class="ptr" data-proc="${p.index}" data-field="kstack">ks ${esc(shortHex(p.kstack))}</div>
        </div>
      `).join("");
    }

    function procChanged(proc, prevProc) {
      if (!prevProc) return false;
      return proc.pid !== prevProc.pid ||
        proc.state !== prevProc.state ||
        proc.kstack !== prevProc.kstack ||
        proc.trapframe !== prevProc.trapframe ||
        proc.pagetable !== prevProc.pagetable ||
        proc.sz !== prevProc.sz;
    }

    function renderCpus(ev) {
      const prev = prevEvent();
      $("cpus").innerHTML = ev.cpus.map(c => {
        const old = (prev?.cpus || []).find(cpu => cpu.index === c.index);
        const changed = old && old.proc !== c.proc;
        return row(`cpu[${c.index}] proc=${shortHex(c.proc)}`, "kernel", "", changed ? "changed" : "");
      }).join("");
    }

    function renderRegisters(ev) {
      const prev = prevEvent();
      const regs = ev.registers || { pc: ev.pc };
      const prevRegs = prev?.registers || {};
      const pointerRegions = Object.fromEntries((ev.pointers || []).map(p => [p.register, p.region]));
      const html = keyRegisters.map(([label, reg]) => {
        const region = pointerRegions[reg];
        const changed = prev && prevRegs[reg] !== regs[reg];
        return row(
          `${label.padEnd(4)} ${reg.padEnd(3)} ${shortHex(regs[reg] || "0x0")}`,
          region,
          `data-register="${reg}"`,
          changed ? "changed" : ""
        );
      }).join("");
      return `<div class="panel"><h2>key registers</h2><div class="registers">${html}</div></div>`;
    }

    function renderCallchain(ev) {
      const frames = (ev.callchain || []).slice(0, 8);
      $("callchain").innerHTML = frames.length
        ? frames.map(f => row(
            `#${f.level} ${f.function} ${shortHex(f.pc)}`,
            "kernel",
            `data-callchain-level="${f.level}"`
          )).join("")
        : row("callchain unavailable", "unreadable");
    }

    function renderMemoryObjects(ev) {
      const prev = prevEvent();
      const sp = (ev.registers || {}).sp || "0x0";
      const stackOwner = ev.stack_owner || {};
      const stackRows = [
        renderSpFlow(ev, prev),
        ...stackFrameGroups(ev, prev).map(group => renderStackFrameBox(group))
      ];
      if (isKernelOwnedStack(stackOwner)) {
        $("live-stack").innerHTML = miniRow(
          `current sp ${shortHex(sp)} is on ${stackOwner.label || stackOwner.kind}`,
          "kernel-stack"
        );
        $("kernel-stack").innerHTML = [
          miniRow(`${stackOwner.label || "kernel stack"} ${shortHex(stackOwner.base)}..${shortHex(stackOwner.limit)}`, "kernel-stack"),
          ...stackRows,
        ].join("");
      } else {
        $("live-stack").innerHTML = stackRows.join("") || miniRow("stack unavailable", "unreadable");
        $("kernel-stack").innerHTML = miniRow("inactive for this event", "unreadable");
      }

      const tf = ev.trapframe || {};
      const tfRows = trapframeNames
        .filter(name => tf[name])
        .map(name => ({ name, ...tf[name] }))
        .sort((a, b) => compareHexAddress(a.address, b.address));
      $("live-trapframe").innerHTML = tfRows.map(entry => {
        const prevEntry = prev?.trapframe?.[entry.name];
        const changed = prevEntry && prevEntry.value !== entry.value;
        return miniRow(
          `${shortHex(entry.address)}  ${displayTrapField(entry.name).padEnd(7)} ${shortHex(entry.value)}`,
          entry.region,
          `data-trap-field="${entry.name}"`,
          changed ? "changed" : ""
        );
      }).join("") || miniRow("trapframe unavailable", "unreadable");

      $("pagetable-layout").innerHTML = (ev.user_layout || []).slice(0, 14).map(page => {
        const cls = page.region === "user stack" ? " stack" : page.region === "guard" ? " guard" : "";
        return `<div class="mini-row page-row${cls}" data-page-table-va="${esc(page.va)}">${esc(page.region)}<br>VA ${esc(shortHex(page.va))} -> PA ${esc(shortHex(page.pa))}</div>`;
      }).join("") || miniRow("no current user pagetable", "unreadable");

      const userRefs = (ev.user_refs || []).map(ref => {
        const detail = ref.kind === "code"
          ? (ref.lines || []).slice(0, 3).map(line => `<div class="user-ref-detail">${esc(line)}</div>`).join("")
          : (ref.words || []).slice(0, 3).map(word =>
              `<div class="user-ref-detail">${esc(shortHex(word.address))}  ${esc(shortHex(word.value))}</div>`
            ).join("");
        const cls = ref.kind === "stack" ? " stack-ref" : "";
        const title = `${ref.label} ${ref.kind} VA ${shortHex(ref.va)} -> PA ${shortHex(ref.pa)}`;
        return `<div class="mini-row user-ref-row${cls}" data-user-ref-field="${esc(ref.field)}"><strong>${esc(title)}</strong>${detail}</div>`;
      });
      const userCode = (ev.user_disasm || []).map(block => {
        const lines = (block.lines || []).slice(0, 5).map(line =>
          `<div class="user-ref-detail">${esc(line)}</div>`
        ).join("");
        const title = `user text ${block.label} VA ${shortHex(block.address)} -> PA ${shortHex(block.pa)}`;
        return `<div class="mini-row user-code-row" data-user-code-address="${esc(block.address)}"><strong>${esc(title)}</strong>${lines}</div>`;
      });
      const userPages = (ev.user_layout || []).slice(0, 14).map(page => {
        const cls = page.region === "user stack" ? " stack" : page.region === "guard" ? " guard" : "";
        return `<div class="mini-row page-row${cls}" data-user-page-va="${esc(page.va)}">${esc(page.region)}<br>VA ${esc(shortHex(page.va))}<br>PA ${esc(shortHex(page.pa))}</div>`;
      });
      const textDataRows = [...userCode, ...userRefs].join("") || miniRow("no sampled user pc/lr/sp", "unreadable");
      const pageRows = userPages.join("") || miniRow("no current user page mapping", "unreadable");
      $("user-pages-layout").innerHTML = `
        <div class="user-section text-data">
          <div class="user-section-title">text/data</div>
          ${textDataRows}
        </div>
        <div class="user-section">
          <div class="user-section-title">page mapping</div>
          ${pageRows}
        </div>
      `;

      const disasm = (ev.kernel_disasm || []).slice(0, 4);
      $("kernel-disasm").innerHTML = disasm.map(block => {
        const lines = (block.lines || []).slice(0, 3).map(line =>
          `<div class="mini-row disasm-row" data-disasm-address="${esc(block.address)}">${esc(line)}</div>`
        ).join("");
        return `<div class="mini-row disasm-row"><strong>${esc(block.label)}</strong> ${esc(shortHex(block.address))}</div>${lines}`;
      }).join("") || miniRow("no current kernel text pc", "unreadable");
    }

    function isKernelOwnedStack(owner) {
      return owner && ["kernel-stack0", "kernel"].includes(owner.kind);
    }

    function renderSpFlow(ev, prev) {
      const sp = (ev.registers || {}).sp || "0x0";
      const prevSp = (prev?.registers || {}).sp;
      if (!prevSp || prevSp === sp) {
        return miniRow(`sp ${shortHex(sp)}`, "kstack", `data-sp-marker="current" data-stack-sp="1"`);
      }
      const direction = hexToBigInt(sp) < hexToBigInt(prevSp) ? "push" : "pop";
      return `
        <div class="sp-flow" data-sp-flow="1">
          ${miniRow(`prev sp ${shortHex(prevSp)}`, "kstack", `data-sp-marker="prev"`)}
          ${miniRow(`sp ${shortHex(sp)}  ${direction}`, "kstack", `data-sp-marker="current"`, "changed")}
        </div>
      `;
    }

    function stackFrameGroups(ev, prev) {
      const frames = ev.callchain || [];
      const words = ev.stack || [];
      if (!frames.length) {
        return [{ frame: null, words, prev }];
      }
      return frames.map((frame, index) => {
        const low = hexToBigInt(frame.sp);
        const high = index + 1 < frames.length ? hexToBigInt(frames[index + 1].sp) : 0n;
        const frameWords = words.filter(word => {
          const address = hexToBigInt(word.address);
          if (low > 0n && high > 0n) return address >= low && address < high;
          if (low > 0n) return address >= low;
          return false;
        });
        return { frame, words: frameWords, prev };
      });
    }

    function renderStackFrameBox(group) {
      const frame = group.frame;
      const title = frame
        ? `#${frame.level} ${frame.function} ${shortHex(frame.pc)}${frame.file ? "  " + frame.file + ":" + frame.line : ""}`
        : "raw stack";
      const meta = frame
        ? `sp ${shortHex(frame.sp)}  fp ${shortHex(frame.fp)}  lr ${shortHex(frame.lr)}`
        : "";
      const words = group.words.map(w => miniRow(
        `${shortHex(w.address)}  ${shortHex(w.value)}`,
        w.region,
        `data-stack-word="${w.index}"`,
        stackChanged(w, group.prev) + " stack-word"
      )).join("");
      return `
        <div class="stack-frame-box" data-stack-frame-level="${esc(frame?.level ?? "raw")}">
          <div class="stack-frame-title">${esc(title)}</div>
          ${meta ? `<div class="stack-frame-meta">${esc(meta)}</div>` : ""}
          ${words || miniRow("no sampled words in this frame", "unreadable")}
        </div>
      `;
    }

    function stackChanged(word, prev) {
      if (!prev) return "";
      const old = (prev.stack || []).find(w => w.index === word.index);
      return old && (old.address !== word.address || old.value !== word.value) ? "changed" : "";
    }

    function renderCenter(ev) {
      const center = document.querySelector(".center");
      let regPanel = document.getElementById("register-panel");
      if (!regPanel) {
        regPanel = document.createElement("div");
        regPanel.id = "register-panel";
        center.insertBefore(regPanel, center.children[1]);
      }
      regPanel.innerHTML = renderRegisters(ev);
    }

    function endpoint(el, targetEl) {
      const layout = $("layout").getBoundingClientRect();
      const a = el.getBoundingClientRect();
      const b = targetEl.getBoundingClientRect();
      return {
        x1: a.left + a.width / 2 - layout.left,
        y1: a.bottom - layout.top,
        x2: b.left + b.width / 2 - layout.left,
        y2: b.top - layout.top,
      };
    }

    function procPointerEndpoint(el, targetEl) {
      const layout = $("layout").getBoundingClientRect();
      const a = el.getBoundingClientRect();
      const b = targetEl.getBoundingClientRect();
      return {
        x1: a.right - layout.left,
        y1: a.top + a.height / 2 - layout.top,
        x2: b.left + b.width / 2 - layout.left,
        y2: b.bottom - layout.top,
      };
    }

    function arrowSvg(x1, y1, x2, y2, color) {
      const id = "arrowhead-" + color.replace("#", "");
      return `
        <defs><marker id="${id}" markerWidth="8" markerHeight="8" refX="7" refY="3" orient="auto">
          <path d="M0,0 L7,3 L0,6 Z" fill="${color}"></path>
        </marker></defs>
        <line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${color}" stroke-width="1.5" marker-end="url(#${id})" opacity="0.86"></line>`;
    }

    function colorFor(region) {
      const css = getComputedStyle(document.documentElement);
      region = regionLabel(region);
      if (region === "user") return css.getPropertyValue("--user").trim();
      if (region === "kstack" || region === "stack") return css.getPropertyValue("--kstack").trim();
      if (region === "kernel-stack") return css.getPropertyValue("--kernel").trim();
      if (region === "trapframe") return css.getPropertyValue("--trapframe").trim();
      if (region === "device") return css.getPropertyValue("--device").trim();
      return css.getPropertyValue("--kernel").trim();
    }

    function renderArrows(ev) {
      const svg = $("arrows");
      let out = "";
      const emitted = new Set();
      const prevSp = document.querySelector(`[data-sp-marker="prev"]`);
      const currentSp = document.querySelector(`[data-sp-marker="current"]`);
      if (prevSp && currentSp) {
        const e = endpoint(prevSp, currentSp);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor("kstack"));
      }
      const usefulRegs = new Set(["pc", "sp", "x30", "x0", "x1", "x2", "x7"]);
      for (const p of (ev.pointers || [])) {
        if (!usefulRegs.has(p.register)) continue;
        const region = regionLabel(p.region);
        const src = document.querySelector(`[data-register="${CSS.escape(p.register)}"]`);
        const dst = document.querySelector(`[data-region="${CSS.escape(region)}"]`);
        const key = `r:${p.register}:${region}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = endpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor(region));
        if (emitted.size >= 5) break;
      }
      const currentProcIndex = ev.current_proc ? ev.current_proc.index : null;
      for (const p of (ev.proc_pointers || []).filter(p => p.proc === currentProcIndex).slice(0, 4)) {
        let region = regionLabel(p.region);
        if (p.field === "pagetable") region = "pagetable";
        const src = document.querySelector(`[data-proc="${p.proc}"][data-field="${CSS.escape(p.field)}"]`);
        const dst = document.querySelector(`[data-region="${CSS.escape(region)}"]`);
        const key = `p:${p.proc}:${p.field}:${region}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = procPointerEndpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor(region));
      }
      for (const frame of (ev.callchain || []).filter(frame => frame.level === 0).slice(0, 1)) {
        const src = document.querySelector(`[data-callchain-level="${frame.level}"]`);
        const dst = document.querySelector(`[data-stack-frame-level="${frame.level}"]`);
        const key = `frame:${frame.level}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = endpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor("kernel"));
      }
      for (const page of (ev.user_layout || []).slice(0, 14)) {
        const src = document.querySelector(`[data-page-table-va="${CSS.escape(page.va)}"]`);
        const dst = document.querySelector(`[data-user-page-va="${CSS.escape(page.va)}"]`);
        const key = `page:${page.va}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = endpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor(page.region === "user stack" ? "kstack" : "user"));
      }
      for (const word of (ev.stack || []).slice(0, 7)) {
        const region = regionLabel(word.region);
        if (!["kernel", "stack", "kstack", "trapframe"].includes(region)) continue;
        const src = document.querySelector(`[data-stack-word="${word.index}"]`);
        const dstRegion = region === "stack" ? "kstack" : region;
        const dst = document.querySelector(`[data-region="${CSS.escape(dstRegion)}"]`);
        const key = `stack:${word.index}:${dstRegion}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = endpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor(dstRegion));
      }
      for (const ref of (ev.user_refs || [])) {
        if (!["elr", "x29", "x30", "sp"].includes(ref.field)) continue;
        const src = document.querySelector(`[data-trap-field="${CSS.escape(ref.field)}"]`);
        const dst = document.querySelector(`[data-user-ref-field="${CSS.escape(ref.field)}"]`);
        const key = `userref:${ref.field}`;
        if (!src || !dst || emitted.has(key)) continue;
        emitted.add(key);
        const e = endpoint(src, dst);
        out += arrowSvg(e.x1, e.y1, e.x2, e.y2, colorFor(ref.kind === "stack" ? "kstack" : "user"));
      }
      svg.innerHTML = out;
    }

    function render() {
      const ev = events[index];
      $("counter").textContent = `${index + 1} / ${events.length}`;
      $("slider").max = Math.max(events.length - 1, 0);
      $("slider").value = index;
      $("prev").disabled = index === 0;
      $("next").disabled = index === events.length - 1;
      renderSummary(ev);
      renderExecution(ev);
      renderProcs(ev);
      renderCpus(ev);
      renderCenter(ev);
      renderCallchain(ev);
      renderMemoryObjects(ev);
      requestAnimationFrame(() => renderArrows(ev));
    }

    function step(delta) {
      index = Math.max(0, Math.min(events.length - 1, index + delta));
      render();
    }

    $("prev").onclick = () => step(-1);
    $("next").onclick = () => step(1);
    $("slider").oninput = (e) => { index = Number(e.target.value); render(); };
    $("play").onclick = () => {
      if (timer) {
        clearInterval(timer);
        timer = null;
        $("play").textContent = "Play";
        return;
      }
      $("play").textContent = "Pause";
      timer = setInterval(() => {
        if (index >= events.length - 1) {
          clearInterval(timer);
          timer = null;
          $("play").textContent = "Play";
        } else {
          step(1);
        }
      }, 650);
    };
    document.addEventListener("keydown", (e) => {
      if (e.key === "ArrowRight") step(1);
      if (e.key === "ArrowLeft") step(-1);
    });
    window.addEventListener("resize", render);
    render();
  </script>
</body>
</html>
"""


def load_events():
    with open(TRACE_PATH) as trace_file:
        return [json.loads(line) for line in trace_file if line.strip()][:MAX_EVENTS]


def main():
    if not os.path.exists(TRACE_PATH):
        raise SystemExit("trace file not found: %s" % TRACE_PATH)

    events = load_events()
    if not events:
        raise SystemExit("trace file is empty: %s" % TRACE_PATH)

    os.makedirs(os.path.dirname(OUT_PATH) or ".", exist_ok=True)
    trace_json = json.dumps(events).replace("</", "<\\/")
    page = HTML_TEMPLATE
    page = page.replace("__TRACE_JSON__", trace_json)
    page = page.replace("__KEY_REGISTERS__", json.dumps(KEY_REGISTERS))
    page = page.replace("__TRAPFRAME_NAMES__", json.dumps(TRAPFRAME_NAMES))

    with open(OUT_PATH, "w") as out:
        out.write(page)

    print("wrote %s with %d events" % (OUT_PATH, len(events)))


if __name__ == "__main__":
    main()
