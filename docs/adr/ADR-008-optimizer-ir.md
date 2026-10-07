# ADR-008: The optimizer works on an addressless IR, not on encoded instructions

**Status:** Accepted

## Context

Architectural rule 8 says the optimizer works on the IR rather than on raw
binary. That leaves open what the IR is, and the obvious choice, a
`std::vector<isa::Instruction>`, is wrong in a way that is easy to miss.

Consider deleting one instruction from such a vector:

```
0x00  MOVI R1, 1
0x08  NOP            <- deleted
0x10  JMP  -0x18     <- displacement computed against 0x10
```

Every branch whose displacement was computed against a later address is
now wrong, and nothing in the data structure records that. The same
applies to symbol values: a label's offset is a byte count into the
section, and removing an instruction changes it.

An optimizer built on that representation is not "sometimes buggy". It is
*structurally* unable to be correct, because the information needed to
fix up the branches has already been thrown away.

## Decision

The IR (`include/minitool/ir/ir.hpp`) is a per-section list of items:

```
Item = Instruction | Label | Bytes | SymbolValue | Space | Align
```

with two properties that make transformation safe:

1. **No addresses.** An item knows its position in the list, not its
   offset. Offsets are assigned once, by the assembler's first pass,
   after every transformation has run.
2. **Symbolic branches.** A branch names a `Label`, it does not carry a
   displacement. Inserting or deleting an instruction cannot invalidate
   it, because there is no displacement yet to invalidate.

Two rules keep the passes safe:

* **A label is never moved, merged or deleted.** Labels are the only
  entry points to a basic block, so every branch target that existed
  before a pass still exists after it. Unreachable *code* is deleted;
  the label that preceded it is not.
* **FLAGS is an output like any other.** An instruction that sets flags
  may only be rewritten into one that does not (folding `ADD` into
  `MOVI`, say) when the flags are provably dead.

## Flag liveness

"Provably dead" is a dataflow question, not a peephole one. The first
implementation answered it conservatively (dead only if a later
instruction *in the same block* overwrites FLAGS first), which was sound
but useless: a block usually ends before it rewrites its own flags, so
folding almost never fired.

The optimizer now builds the section's control-flow graph (blocks split
at labels and after branches, `HALT`, `RET` and `SYSCALL`) and solves
backward liveness for the single bit that matters:

```
live_out(B) = unknown_successor(B) OR any live_in(S) for successors S
live_in(B)  = B reads FLAGS before writing it
              OR (live_out(B) AND B never writes FLAGS)
```

iterated to a fixed point so that loops converge. A successor that cannot
be determined (a `CALL`, a literal displacement, or a branch to a label
defined in another file) sets `unknown_successor` and forces the
conservative answer.

## Alternatives

* **A vector of `isa::Instruction` plus a side table of fixups.** This is
  the same information, worse organised: every pass has to remember to
  update the side table, and forgetting is silent.
* **Re-run the assembler after each pass to recompute displacements.**
  Correct, and it makes each pass quadratic in program size while hiding
  the invariant inside an unrelated component.
* **Do not fold flag-setting instructions at all.** Sound, simple, and
  gives up most of the constant folding on an ISA where nearly every ALU
  operation writes flags.

## Trade-offs

The IR costs a lowering step (`ir::lower`) and a second representation to
keep in sync with the AST. In exchange, the optimizer's correctness
argument is a paragraph rather than a case analysis, and
`OptimizerProperties.OptimizationPreservesObservableBehaviour`, which
runs randomly generated programs at both levels and compares every
register, can actually be expected to pass.

Liveness is computed per section, not across the whole program, so a flag
value that a caller expects a callee to leave behind is treated as live.
That is conservative in the right direction.

## Consequences

* Every optimization is local to a section and to a basic block. Global
  transformations (inlining, cross-block scheduling) would need call-graph
  information the IR does not carry, and a new ADR.
* `docs/optimizer.md` states each pass's precondition. A new pass must
  state its own, and must come with a semantic-equivalence test.
* The optimizer never changes what a program does to make a number look
  better. It is measured by `instructions executed`, and held to
  `output`, `exit code` and every register matching exactly.
