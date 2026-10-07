# ADR-006: No immediate-operand arithmetic instructions

**Status:** Accepted

## Context
The instruction set has no immediate form for arithmetic (`ADD`, `SUB`,
...). But the textbook example of constant folding is written with one:

```
MOVI R1, 10
ADD R1, 20      <- an immediate operand for ADD
```

That instruction does not exist. Either the ISA grows immediate forms, or
folding has to work without them.

## Decision
Arithmetic and logical instructions are **register-register only**
(`Format::Reg2`). Constants are materialised with `MOVI` into a register
first.

The optimizer's constant-folding pass therefore works on tracked register
values:

```
MOVI R1, 10
MOVI R2, 20      ->   MOVI R1, 30
ADD  R1, R2           (when R2 is dead afterwards)
```

which is a strictly more general transformation than folding a literal
operand, and exercises liveness analysis rather than pattern matching.

## Alternatives
* **Add `ADDI`/`SUBI`/... immediate forms.** Costs 7+ opcodes and doubles
  the assembler's operand-form handling, for an ergonomic gain the
  optimizer can deliver instead.
* **Overload `ADD` on operand kind.** One mnemonic, two opcodes. Nicer
  assembly, but the disassembler then prints a mnemonic that does not map
  one-to-one onto an opcode, which weakens the round-trip guarantee.

## Trade-offs
Assembly is more verbose: incrementing by a constant other than 1 costs
two instructions and a scratch register. `INC` and `DEC` cover the common
case. If measurement later shows this dominating code size in real
examples, adding `ADDI` is a clean additive change to a free opcode slot
(`0x28`), but it needs its own ADR and a format version note.

## Consequences
This decision is **reversible but not silently**. Whoever revisits it
must update: `docs/isa.md`, the opcode table, the encoder, the
disassembler, the assembler's operand parsing, and the
`InstructionCountMatchesTheFrozenSpec` test.
