// SPDX-License-Identifier: MIT
#pragma once

/// The playground's single page, embedded in the binary.
///
/// It is embedded rather than read from disk so that `minitool serve` works
/// from any working directory and from a copied binary, and so the server has
/// no filesystem surface at all: it can only ever serve this one string. There
/// are no external stylesheets, fonts or scripts, because the toolchain is
/// meant to work with no network (ADR-010).

#include <string_view>

namespace minitool::playground {

/// Split into several literals because MSVC caps a single string literal at
/// 16380 bytes (C2026) and this page is larger. Adjacent string literals are
/// concatenated by the compiler, so `kIndexHtml` is still one contiguous view
/// with no runtime cost. Split points are line boundaries and carry no
/// meaning; add to whichever chunk has room.
inline constexpr std::string_view kIndexHtml =
    R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MiniToolchain Playground</title>
<style>
  :root {
    --bg: #14161a;
    --panel: #1a1d23;
    --panel-2: #21252d;
    --border: #2c313a;
    --text: #d7dce3;
    --dim: #8b95a5;
    --accent: #7aa2f7;
    --ok: #9ece6a;
    --err: #f7768e;
    --warn: #e0af68;
    --mono: ui-monospace, "Cascadia Mono", "SF Mono", Menlo, Consolas, monospace;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font: 14px/1.5 system-ui, -apple-system, Segoe UI, sans-serif;
    display: flex;
    flex-direction: column;
  }

  header {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 14px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    flex-wrap: wrap;
  }
  header h1 {
    font-size: 14px;
    font-weight: 600;
    margin: 0 8px 0 0;
    letter-spacing: .2px;
  }
  header h1 span { color: var(--accent); }
  .spacer { flex: 1; }

  select, button {
    font: inherit;
    color: var(--text);
    background: var(--panel-2);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 6px 10px;
    cursor: pointer;
  }
  select:hover, button:hover { border-color: #3d4452; }
  button.run {
    background: var(--accent);
    border-color: var(--accent);
    color: #10131a;
    font-weight: 600;
    min-width: 104px;
  }
  button.run:disabled { opacity: .55; cursor: default; }
  kbd {
    font: 11px/1 var(--mono);
    color: var(--dim);
    border: 1px solid var(--border);
    border-bottom-width: 2px;
    border-radius: 4px;
    padding: 3px 5px;
  }

  main {
    flex: 1;
    display: grid;
    grid-template-columns: 1fr 1fr;
    min-height: 0;
  }
  @media (max-width: 900px) { main { grid-template-columns: 1fr; } }

  .pane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
  .pane + .pane { border-left: 1px solid var(--border); }
  .pane-head {
    display: flex;
    align-items: center;
    gap: 2px;
    padding: 0 8px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    min-height: 36px;
  }
  .pane-head .label {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: .8px;
    color: var(--dim);
    padding: 0 6px;
  }
  .tab {
    background: none;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    color: var(--dim);
    padding: 8px 10px;
    font-size: 12px;
  }
  .tab.active { color: var(--text); border-bottom-color: var(--accent); }

  /* editor */
  .editor-wrap {
    flex: 1;
    display: flex;
    overflow: auto;
    background: var(--bg);
    min-height: 0;
  }
  .gutter {
    padding: 12px 8px 12px 12px;
    text-align: right;
    color: #4a5364;
    font: 13px/1.55 var(--mono);
    user-select: none;
    white-space: pre;
  }
  #source {
    flex: 1;
    resize: none;
    border: none;
    outline: none;
    background: transparent;
    color: var(--text);
    font: 13px/1.55 var(--mono);
    padding: 12px 12px 12px 4px;
    white-space: pre;
    overflow: hidden;
    min-height: 100%;
  }

  /* output */
  .view { flex: 1; overflow: auto; padding: 12px 14px; min-height: 0; }
  .view pre {
    margin: 0;
    font: 12.5px/1.55 var(--mono);
    white-space: pre-wrap;
    word-break: break-word;
  }
  .empty { color: var(--dim); font-style: italic; }
  .block-label {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: .8px;
    color: var(--dim);
    margin: 0 0 6px;
  }
  .block + .block { margin-top: 18px; padding-top: 16px; border-top: 1px solid var(--border); }
  .diag { color: var(--err); }
  .stdout { color: var(--text); }
  .muted { color: var(--dim); }

  table.regs { border-collapse: collapse; font: 12.5px/1.5 var(--mono); }
  table.regs td { padding: 2px 14px 2px 0; }
  table.regs td.name { color: var(--accent); }
  table.regs td.dec { color: var(--dim); }

  #stdin-row { display: none; padding: 8px 14px; background: var(--panel); border-bottom: 1px solid var(--border); }
  #stdin-row.on { display: block; }
  #stdin {
    width: 100%;
    background: var(--panel-2);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 6px;
    font: 12.5px/1.5 var(--mono);
    padding: 6px 8px;
    resize: vertical;
    min-height: 44px;
  }

  footer {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 7px 14px;
    background: var(--panel);
    border-top: 1px solid var(--border);
    font: 12px/1.4 var(--mono);
    color: var(--dim);
    flex-wrap: wrap;
  }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--dim); display: inline-block; }
  .dot.ok { background: var(--ok); }
  .dot.err { background: var(--err); }
  .dot.busy { background: var(--warn); }
</style>
</head>
<body>

<header>
  <h1>MiniToolchain <span>Playground</span></h1>
  <select id="examples" title="Load an example">
    <option value="">Load example...</option>
  </select>
  <select id="opt" title="Optimization level">
    <option value="0">-O0</option>
    <option value="1">-O1</option>
  </select>
  <button id="toggle-stdin" title="Provide input for the read syscall">stdin</button>
  <div class="spacer"></div>
  <kbd>Ctrl</kbd><kbd>Enter</kbd>
  <button id="run" class="run">Run</button>
</header>

<div id="stdin-row">
  <textarea id="stdin" placeholder="Input for the read syscall (fd 0)"></textarea>
</div>

<main>
  <section class="pane">
    <div class="pane-head"><span class="label">playground.asm</span></div>
    <div class="editor-wrap" id="editor-wrap">
      <div class="gutter" id="gutter">1</div>
      <textarea id="source" spellcheck="false" autocomplete="off"
                autocapitalize="off" autocorrect="off"></textarea>
    </div>
  </section>

  <section class="pane">
    <div class="pane-head">
      <button class="tab active" data-view="output">Output</button>
      <button class="tab" data-view="disasm">Disassembly</button>
      <button class="tab" data-view="regs">Registers</button>
    </div>
    <div class="view" id="view">
      <p class="empty">Press Run to assemble, link and execute.</p>
    </div>
  </section>
</main>

<footer>
  <span><span class="dot" id="dot"></span> <span id="status">ready</span></span>
  <span id="metrics"></span>
</footer>

<script>
"use strict";

const EXAMPLES = {
  "Hello, World!": `; The smallest program that produces output.
; Demonstrates: the write syscall, string data, exiting cleanly

.section .data
message:
    .asciz "Hello, World!\\n"

.section .text
.global _start

_start:
    MOVI R1, 1              ; fd = stdout
    LEA  R2, message        ; buffer address (filled in by the linker)
    MOVI R3, 14             ; length
    SYSCALL 1               ; write

    MOVI R1, 0              ; exit code
    SYSCALL 0               ; exit
`,

  "Arithmetic": `; Every arithmetic instruction, with the result left in a register.
; Demonstrates: ADD, SUB, MUL, DIV, MOD, INC, DEC, NEG
;
; There is no immediate arithmetic in this ISA: a constant has to be moved
; into a register first. See docs/adr/ADR-006.

.section .text
.global _start

_start:
    MOVI R1, 40
    MOVI R2, 2
    ADD  R1, R2             ; R1 = 42

    MOVI R3, 100
    SUB  R3, R1             ; R3 = 58

    MOVI R4, 6
    MOVI R5, 7
    MUL  R4, R5             ; R4 = 42

    MOVI R6, 84
    MOVI R7, 2
    DIV  R6, R7             ; R6 = 42

    MOVI R8, 100
    MOVI R9, 58
    MOD  R8, R9             ; R8 = 42

    MOVI R10, 41
    INC  R10                ; R10 = 42
    MOVI R11, 43
    DEC  R11                ; R11 = 42
    MOVI R12, 42
    NEG  R12                ; R12 = -42

    HALT
`,

  "Bitwise and shifts": `; Bitwise operations and shifts.
; Demonstrates: AND, OR, XOR, NOT, SHL, SHR, SAR, binary literals

.section .text
.global _start

_start:
    MOVI R1, 0b11001100     ; 204
    MOVI R2, 0b10101010     ; 170

    MOV  R3, R1
    AND  R3, R2             ; R3 = 0b10001000 = 136

    MOV  R4, R1
    OR   R4, R2             ; R4 = 0b11101110 = 238

    MOV  R5, R1
    XOR  R5, R2             ; R5 = 0b01100110 = 102

    MOV  R6, R1
    NOT  R6                 ; R6 = ~204

    MOVI R7, 1
    MOVI R8, 4
    SHL  R7, R8             ; R7 = 1 << 4 = 16

    MOVI R9, 256
    SHR  R9, R8             ; R9 = 256 >> 4 = 16

    ; SAR keeps the sign; SHR does not.
    MOVI R10, 0
    MOVI R11, 32
    SUB  R10, R11           ; R10 = -32
    MOVI R12, 2
    SAR  R10, R12           ; R10 = -8

    HALT
`,

  "Conditionals and jumps": `; Comparison and conditional branching, including signed values.
; Demonstrates: CMP, JE/JNE/JG/JL/JGE/JLE

.section .text
.global _start

_start:
    MOVI R1, 5
    MOVI R2, 0
    SUB  R2, R1             ; R2 = -5

    ; -5 < 5 as a signed comparison
    CMP  R2, R1
    JL   is_less
    MOVI R3, 0              ; not reached
    JMP  check_equal
is_less:
    MOVI R3, 1              ; R3 = 1

check_equal:
    MOVI R4, 42
    MOVI R5, 42
    CMP  R4, R5
    JE   is_equal
    MOVI R6, 0
    JMP  count
is_equal:
    MOVI R6, 1              ; R6 = 1

count:
    ; Count down from 5 using JG.
    MOVI R7, 5
    MOVI R8, 0
loop:
    CMP  R7, R0             ; R0 is still zero
    JLE  done
    INC  R8
    DEC  R7
    JMP  loop
done:
    HALT                    ; R8 = 5
`,

  "The stack": `; The stack: PUSH and POP.
; Demonstrates: stack discipline, last-in first-out ordering

.section .text
.global _start

_start:
    MOVI R1, 111
    MOVI R2, 222
    MOVI R3, 333

    PUSH R1
    PUSH R2
    PUSH R3

    POP  R4                 ; R4 = 333 (last in, first out)
    POP  R5                 ; R5 = 222
    POP  R6                 ; R6 = 111

    HALT
`,

  "Factorial (loops)": `; Iterative factorial - 10! = 3628800, left in R14.
; Demonstrates: loops, CMP, conditional branching, CALL/RET

.section .text
.global _start

_start:
    MOVI R1, 10
    CALL factorial
    MOV  R14, R1            ; R14 = 3628800
    HALT

; factorial(n) with n in R1, result in R1.
factorial:
    MOVI R2, 1              ; accumulator

factorial_loop:
    MOVI R3, 1
    CMP  R1, R3             ; while n > 1
    JLE  factorial_done
    MUL  R2, R1             ; acc *= n
    DEC  R1
    JMP  factorial_loop

factorial_done:
    MOV  R1, R2
    RET
`,

  "Fibonacci (recursion)": `; Recursive Fibonacci - fib(10) = 55, left in R5.
; Demonstrates: recursion, CALL/RET, saving values across a call

.section .text
.global _start

_start:
    MOVI R1, 10
    CALL fib
    MOV  R5, R14            ; R5 = 55
    HALT

; fib(n) with n in R1, result in R14.
fib:
    MOVI R2, 2
    CMP  R1, R2
    JGE  fib_recurse
    MOV  R14, R1            ; fib(0) = 0, fib(1) = 1
    RET

fib_recurse:
    PUSH R1                 ; save n
    MOVI R2, 1
    SUB  R1, R2
    CALL fib                ; fib(n-1)
    POP  R1                 ; restore n

    PUSH R14                ; save fib(n-1)
    PUSH R1
    MOVI R2, 2
    SUB  R1, R2
    CALL fib                ; fib(n-2)
    POP  R1
    POP  R3                 ; R3 = fib(n-1)

    ADD  R14, R3            ; fib(n-1) + fib(n-2)
    RET
`,

  "Arrays in memory": `; Walking an array in memory.
; Demonstrates: .data, LEA, LOAD with a computed address, summing a loop

.section .data
array:
    .qword 5
    .qword 3
    .qword 8
    .qword 1
    .qword 9
length:
    .qword 5

.section .text
.global _start

_start:
    LEA  R1, array          ; R1 = base address
    MOVI R2, 5              ; R2 = element count
    MOVI R3, 0              ; R3 = index
    MOVI R4, 0              ; R4 = running total
    MOVI R5, 0              ; R5 = largest seen

sum_loop:
    CMP  R3, R2
    JGE  sum_done

    ; address = base + index * 8
    MOV  R6, R3
    MOVI R7, 8
    MUL  R6, R7
    ADD  R6, R1
    LOAD R8, [R6 + 0]       ; R8 = array[index]

    ADD  R4, R8             ; total += array[index]

)PAGE"
    R"PAGE(    CMP  R8, R5             ; track the maximum
    JLE  sum_next
    MOV  R5, R8

sum_next:
    INC  R3
    JMP  sum_loop

sum_done:
    HALT                    ; R4 = 26 (total), R5 = 9 (max)
`,

  "Print a number": `; Printing a number, which the ISA gives you no help with.
; Demonstrates: MOD/DIV digit extraction, packing bytes into a register,
; and why STORE being 64-bit shapes the whole routine.

.section .data
newline:
    .asciz "\\n"

.section .bss
numbuf:
    .space 16

.section .text
.global _start

_start:
    MOVI R1, 3628800        ; 10!
    CALL print_number
    MOVI R1, 0
    SYSCALL 0

; print_number(R1): writes R1 in decimal, then a newline.
;
; STORE writes eight bytes at once, so digits cannot be laid down one at a
; time - a second STORE would zero the bytes after it. Instead the digits are
; packed into one register (least significant digit last, which puts the most
; significant digit in the lowest byte) and written with a single STORE.
; That caps this routine at 8 digits, which is enough for what it prints.
print_number:
    MOVI R6, 0              ; packed digits
    MOVI R7, 0              ; digit count
    MOVI R8, 10

    CMP  R1, R0             ; zero has one digit, not none
    JNE  pn_loop
    MOVI R6, 48             ; '0'
    MOVI R7, 1
    JMP  pn_emit

pn_loop:
    MOV  R2, R1
    MOD  R2, R8             ; R2 = n % 10
    MOVI R3, 48
    ADD  R2, R3             ; to ASCII
    MOVI R4, 8
    SHL  R6, R4             ; make room for the next digit
    OR   R6, R2
    INC  R7
    DIV  R1, R8             ; n /= 10
    CMP  R1, R0
    JG   pn_loop

pn_emit:
    LEA   R2, numbuf
    STORE [R2 + 0], R6
    MOVI  R1, 1
    MOV   R3, R7
    SYSCALL 1               ; write(1, numbuf, digits)

    MOVI R1, 1
    LEA  R2, newline
    MOVI R3, 1
    SYSCALL 1
    RET
`,

  "Echo (reads stdin)": `; Echo: reads standard input and writes it straight back.
; Open the "stdin" box in the toolbar and type something before running.
; Demonstrates: the read syscall, .bss buffers, using a returned length

.section .bss
buffer:
    .space 256

.section .text
.global _start

_start:
    MOVI R1, 0              ; fd = stdin
    LEA  R2, buffer
    MOVI R3, 256
    SYSCALL 2               ; read -> R14 = bytes actually read

    MOVI R1, 1              ; fd = stdout
    LEA  R2, buffer
    MOV  R3, R14            ; write back exactly that many bytes
    SYSCALL 1

    MOVI R1, 0
    SYSCALL 0
`,

  "Heap allocation": `; Dynamic allocation through the allocate syscall.
; Demonstrates: syscall 3, storing into heap memory, reading it back

.section .text
.global _start

_start:
    MOVI R1, 64
    SYSCALL 3               ; allocate(64) -> R14 = address
    MOV  R2, R14            ; R2 = our block

    MOVI R3, 12345
    STORE [R2 + 0], R3      ; write into it
    MOVI R4, 678
    STORE [R2 + 8], R4

    LOAD R5, [R2 + 0]       ; R5 = 12345
    LOAD R6, [R2 + 8]       ; R6 = 678
    ADD  R5, R6             ; R5 = 13023

    HALT
`,

  "Optimizer (try -O0 vs -O1)": `; Run this at -O0, then switch to -O1 and run it again.
; Compare the instruction count in the status bar and the Disassembly tab.

.section .text
.global _start

_start:
    MOVI R1, 2
    MOVI R2, 3
    ADD  R1, R2             ; constant-foldable: flags are dead after this

    MOV  R3, R3             ; identity

    PUSH R4
    POP  R4                 ; a push/pop pair that cancels

    MOVI R5, 1
    MOVI R5, 2              ; dead store: R5 is overwritten before it is read

    JMP  done
    MOVI R6, 99             ; unreachable
done:
    HALT
`,

  "Error: bad operand": `; A deliberate mistake: MOV takes two registers.
; The diagnostic points a caret at the operand that is wrong.

.section .text
.global _start

_start:
    MOVI R1, 5
    MOV  R1, 7              ; should be MOVI for an immediate
    HALT
`,

  "Error: divide by zero": `; A deliberate trap. The VM stops the program and reports where,
; rather than crashing the host or producing a nonsense value.

.section .text
.global _start

_start:
    MOVI R1, 42
    MOVI R2, 0
    DIV  R1, R2             ; trap: division by zero
    HALT
`,

  "Error: read-only memory": `; A runtime trap: .rodata is mapped read-only, and the VM enforces it
; rather than letting the write land.

.section .rodata
constant:
    .qword 42

.section .text
.global _start

_start:
    LEA   R1, constant
    LOAD  R2, [R1 + 0]      ; reading is fine: R2 = 42
    MOVI  R3, 99
    STORE [R1 + 0], R3      ; trap: this segment is not writable
    HALT
`
};

const $ = (id) => document.getElementById(id);
const sourceEl = $("source");
const gutterEl = $("gutter");
const viewEl = $("view");
const statusEl = $("status");
const metricsEl = $("metrics");
const dotEl = $("dot");
const runBtn = $("run");

let currentView = "output";
let lastReport = null;

/* ---------------------------------------------------------------- editor -- */

function syncGutter() {
  const lines = sourceEl.value.split("\n").length;
  let text = "";
  for (let i = 1; i <= lines; i++) text += i + "\n";
  gutterEl.textContent = text;
  sourceEl.style.height = "auto";
  sourceEl.style.height = sourceEl.scrollHeight + "px";
}

sourceEl.addEventListener("input", syncGutter);

// Tab inserts four spaces instead of leaving the textarea.
sourceEl.addEventListener("keydown", (e) => {
  if (e.key === "Tab") {
    e.preventDefault();
    const start = sourceEl.selectionStart;
    const end = sourceEl.selectionEnd;
    sourceEl.value = sourceEl.value.slice(0, start) + "    " + sourceEl.value.slice(end);
    sourceEl.selectionStart = sourceEl.selectionEnd = start + 4;
    syncGutter();
  }
});

document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    run();
  }
});

/* ----------------------------------------------------------------- tabs -- */

for (const tab of document.querySelectorAll(".tab")) {
  tab.addEventListener("click", () => {
    for (const t of document.querySelectorAll(".tab")) t.classList.remove("active");
    tab.classList.add("active");
    currentView = tab.dataset.view;
    render();
  });
}

$("toggle-stdin").addEventListener("click", () => {
  $("stdin-row").classList.toggle("on");
});

/* -------------------------------------------------------------- examples -- */

const exSelect = $("examples");
for (const name of Object.keys(EXAMPLES)) {
  const option = document.createElement("option");
  option.value = name;
  option.textContent = name;
  exSelect.appendChild(option);
}
exSelect.addEventListener("change", () => {
  const chosen = EXAMPLES[exSelect.value];
  if (chosen === undefined) return;
  sourceEl.value = chosen;
  exSelect.value = "";
  syncGutter();
  run();
});

/* ---------------------------------------------------------------- render -- */

function escapeHtml(text) {
  return text.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
}

function block(label, body, cls) {
  return `<div class="block"><p class="block-label">${label}</p>` +
         `<pre class="${cls || ""}">${escapeHtml(body)}</pre></div>`;
}

function hex64(value) {
  return "0x" + BigInt(value).toString(16).padStart(16, "0").toUpperCase();
}

function signed64(value) {
  let n = BigInt(value);
  if (n >= (1n << 63n)) n -= (1n << 64n);
  return n.toString();
}

function renderRegisters(r) {
  let html = '<div class="block"><table class="regs">';
  for (let i = 0; i < r.registers.length; i++) {
    html += `<tr><td class="name">R${i}</td><td>${hex64(r.registers[i])}</td>` +
            `<td class="dec">${signed64(r.registers[i])}</td></tr>`;
  }
  html += `<tr><td class="name">PC</td><td>${hex64(r.pc)}</td><td></td></tr>`;
  html += `<tr><td class="name">SP</td><td>${hex64(r.sp)}</td><td></td></tr>`;
  html += `<tr><td class="name">FLAGS</td><td>${hex64(r.flags)}</td><td></td></tr>`;
  html += "</table></div>";
  return html;
}

function render() {
  const r = lastReport;
  if (!r) {
    viewEl.innerHTML = '<p class="empty">Press Run to assemble, link and execute.</p>';
    return;
  }

  if (currentView === "disasm") {
    viewEl.innerHTML = r.disassembly
      ? block("linked image", r.disassembly)
      : '<p class="empty">No disassembly: the program did not get as far as linking.</p>';
    return;
  }

  if (currentView === "regs") {
    viewEl.innerHTML = r.ran
      ? renderRegisters(r)
      : '<p class="empty">No machine state: the program did not run.</p>';
    return;
  }

  let html = "";
  if (r.diagnostics) html += block("diagnostics", r.diagnostics, "diag");
  if (r.error) html += block(r.stage + " error", r.error, "diag");
  if (r.output) {
    html += block("program output", r.output + (r.output_truncated ? "\n[output truncated]" : ""),
                  "stdout");
  }
  if (r.ok && !r.output) html += block("program output", "(no output)", "muted");
  if (r.stats && r.stats.total > 0) {
    html += block("optimizer", r.stats.summary, "muted");
  }
  viewEl.innerHTML = html || '<p class="empty">Nothing to show.</p>';
}

/* ------------------------------------------------------------------- run -- */

function setStatus(text, kind) {
  statusEl.textContent = text;
  dotEl.className = "dot" + (kind ? " " + kind : "");
}

async function run() {
  runBtn.disabled = true;
  setStatus("running...", "busy");
  metricsEl.textContent = "";

  const params = new URLSearchParams({
    opt: $("opt").value,
    stdin: $("stdin").value
  });

  try {
    const response = await fetch("/api/run?" + params.toString(), {
      method: "POST",
      headers: { "Content-Type": "text/plain; charset=utf-8" },
      body: sourceEl.value
    });
    if (!response.ok) throw new Error("server returned " + response.status);
    lastReport = await response.json();
  } catch (err) {
    lastReport = {
      ok: false, stage: "server", error: String(err && err.message ? err.message : err),
      diagnostics: "", output: "", disassembly: "", ran: false
    };
  }

  const r = lastReport;
  if (r.ok) {
    setStatus("finished - exit " + r.exit_code, "ok");
  } else {
    setStatus(r.stage + " failed", "err");
  }
  metricsEl.textContent = r.ran
    ? `${r.instructions} instructions executed`
    : "";

  runBtn.disabled = false;
  render();
}

runBtn.addEventListener("click", run);

/* ----------------------------------------------------------------- start -- */

sourceEl.value = EXAMPLES["Hello, World!"];
syncGutter();
run();
</script>
</body>
</html>
)PAGE";

}  // namespace minitool::playground
