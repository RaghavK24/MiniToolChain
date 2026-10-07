# ADR-011: Memory map and syscall ABI

**Status:** Accepted

## Context

The VM needs an address-space layout and a way to reach the host. A rough
sketch of both (section bases, four syscalls) is easy; the hard part is
pinning down the details a program actually depends on: where the
stack lives, which register carries a return value, what happens when an
allocation fails.

These are contract, not implementation. Every program this toolchain
assembles encodes assumptions about them.

## Decision

### Address space

| Region | Base | Size | Permissions |
|---|---|---|---|
| `.text` | `0x0001_0000` | <= 960 KiB | `r-x` |
| `.rodata` | `0x0010_0000` | <= 960 KiB | `r--` |
| `.data` | `0x0020_0000` | <= 960 KiB | `rw-` |
| `.bss` | `0x0030_0000` | <= 960 KiB | `rw-` |
| heap | `0x1000_0000` | 1 MiB | `rw-` |
| stack | `0x7FFF_0000`, growing down | 1 MiB | `rw-` |

Regions sit 1 MiB apart with a 960 KiB cap, so a section that outgrows
its region is a link error rather than a silent overlap. Nothing is
mapped at address 0, which makes a null dereference a fault instead of a
read of `.text`. No region is both writable and executable, and the
executable validator rejects an image that asks for one.

The stack grows down from a high address, far from the program image, so
an overflow runs into unmapped memory rather than into `.bss`.

### Calling convention

Frozen in `docs/isa.md` §2: `R1`-`R4` carry arguments, `R14` the return
value, `R13` is the callee-saved frame pointer, `R15` is reserved.
`CALL` pushes the address of the following instruction; `RET` pops it.

### Syscalls

`SYSCALL n` encodes the number in the instruction, not in a register, so
a disassembly says what a call does, and a verifier can see every service
a program uses without running it.

| n | Call | Arguments | Result |
|---|---|---|---|
| 0 | `exit` | `R1` = status | halts the machine |
| 1 | `write` | `R1` = fd (1 or 2), `R2` = buffer, `R3` = length | `R14` = bytes written |
| 2 | `read` | `R1` = fd (0), `R2` = buffer, `R3` = length | `R14` = bytes read |
| 3 | `allocate` | `R1` = size | `R14` = address, or 0 on failure |

Two rules make the boundary safe:

* **A transfer is capped at 1 MiB per call.** A program with a wild
  length argument gets an error, not a host-side allocation of 2^63
  bytes.
* **A failed allocation returns 0 rather than trapping.** That is what a
  real allocator does, and it lets the program decide. Every other
  failure (an unknown number, a bad file descriptor, a buffer outside
  mapped memory) traps as `SYSCALL_ERROR`.

The host side lives behind `SyscallProvider`, so no host I/O appears in
the CPU's execution path. The default implementation talks to stdio; the
one the tests use captures output and supplies canned input.

## Alternatives

* **Syscall number in `R0`.** Matches Linux, and makes the number
  invisible to static analysis. The instruction's 48-bit immediate is
  otherwise unused, so there is no reason to spend a register on it.
* **A larger or growable heap.** 1 MiB is enough for the examples and
  keeps a runaway `allocate` loop from taking the host's memory with it.
  Growing it would need `brk`-style semantics and a fragmentation policy
  nothing here needs yet.
* **Stack at the top of the 64-bit space.** More realistic, and it makes
  every address in the debugger sixteen digits of mostly `F`.

## Trade-offs

A 960 KiB per-region cap means this toolchain will not link a large
program. That is the right trade for a project whose largest example is
forty lines: the alternative is a layout algorithm with no test coverage.
Raising it is a one-line change to `linker::kRegionSize` plus a note
here.

## Consequences

* Syscall numbers are contract: append, never renumber.
* `VirtualMachine::kStackTop`, `kHeapBase` and the `linker::k*Base`
  constants are the single source of truth; the docs reference them
  rather than repeating them.
* A program that reaches the instruction budget (`--max-instructions`,
  100 M by default) stops with `BUDGET_EXHAUSTED`. That makes
  "malformed input must never hang" testable for the VM.
