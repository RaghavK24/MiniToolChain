# ADR-002: 64-bit fixed-width ISA with 16 registers

**Status:** Accepted, **frozen**

## Context
The ISA determines the object format, the linker's relocation types, the
VM and the disassembler. It has to be frozen before any of them exist.

## Decision
* 64-bit registers and addresses, 16 general-purpose registers.
* Fixed 64-bit instruction width, 8-byte aligned.
* Load/store architecture: only `LOAD` and `STORE` touch memory.
* `R0` is an ordinary register, not a hardwired zero.

## Alternatives
* **Variable-length encoding** (x86-style): realistic, but the decoder
  becomes the hardest part of the project and instruction sizing turns
  pass 1 of the assembler into a fixed-point iteration. Rejected as
  effort spent in the wrong place.
* **32-bit instructions** (ARM/RISC-V-style): a 32-bit word leaves ~20
  bits of immediate, which forces multi-instruction constant
  materialisation and relocation splitting everywhere. Rejected for a
  first implementation.
* **Hardwired zero register**: saves opcodes, but costs one of only 16
  registers and adds a special case to every write path.

## Trade-offs
A 64-bit instruction to encode `NOP` is wasteful; binaries are roughly
twice the size of a real 64-bit ISA's. Bought in exchange: instruction
sizes are known before symbol resolution, so pass 1 of the assembler is a
single linear walk, and `PC` alignment is trivially invariant.

## Consequences
Immediates and displacements are limited to 48 bits. Full 64-bit
constants need `MOVI` + `SHL` + `OR`.
