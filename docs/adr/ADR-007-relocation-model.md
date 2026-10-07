# ADR-007: Relocation types, including two for instruction fields

**Status:** Accepted

## Context

The usual starting point is three relocation types: `ABS64`, `ABS32`
and `PCREL32`. All three describe a patch applied to a *run of bytes* at
a given offset, which is the model every byte-oriented format uses.

The MiniToolchain ISA does not work that way. An instruction is a single
64-bit word, and the value a relocation needs to patch is the 48-bit
immediate field inside it, sharing the word with the opcode and two
register nibbles (`docs/isa.md` §3). None of the three byte-oriented
types can express that:

* `PCREL32` at the instruction's offset would write four bytes over the
  low half of the immediate field. For a *positive* displacement that
  happens to produce the right bits; for a negative one it leaves bits
  32-47 as zero, and the sign extension the decoder performs then yields
  a completely different address. It would work in testing and fail on
  the first backward branch past 4 GiB or, worse, quietly work while
  meaning something else.
* The standard formula, `symbol + addend - place`, also disagrees with
  the frozen branch rule. `isa::branchTarget` defines the base as the
  address of the instruction *after* the branch, so a displacement
  computed against the branch itself is off by exactly 8.

## Decision

Five relocation types. The three byte-oriented ones keep their meaning and
their numbers, and two more describe instruction fields:

| Value | Name | Width | Value stored |
|---|---|---|---|
| 0 | `ABS32` | 4 bytes | `S + A`, rejected outside unsigned 32 bits |
| 1 | `ABS64` | 8 bytes | `S + A` |
| 2 | `PCREL32` | 4 bytes | `S + A - P`, rejected outside signed 32 bits |
| 3 | `IMM48` | instruction | `S + A` into the 48-bit immediate field |
| 4 | `PCREL48` | instruction | `S + A - (P + 8)` into the immediate field |

`S` is the symbol's final address, `A` the addend, `P` the address of the
patched field.

`PCREL48` is defined as `isa::branchDisplacement(P, S + A)`, and
the implementation calls that function rather than restating the
arithmetic: the branch rule lives in one place and the linker borrows
it.

The instruction-field types do not write bits directly. They decode the
word, replace the immediate, and re-encode it through `isa::encodeInto`.
That costs a decode per relocation and buys three things: the offset is
proven to name a real instruction, the range check is the encoder's own,
and the field layout stays known to one module (architectural
rule 9).

## Alternatives

* **Only the three standard types, with `PCREL32` reinterpreted to mean
  the instruction field.** Keeps the count at three by making a name lie
  about its width. A future reader who sees `PCREL32` in a hex dump
  would reasonably expect four bytes.
* **A single generic "patch these bits" relocation** carrying an offset,
  a shift and a mask. More expressive, and much harder to validate: every
  file would then be able to describe a patch that straddles a field
  boundary, and the reader would have to reject those by hand.
* **Make the assembler resolve branches within a section and emit no
  relocation.** Works for local labels, but the moment a `CALL` crosses
  an object file the linker needs the relocation anyway, so it only moves
  the problem.

## Trade-offs

Two extra types mean two more paths through `applyRelocation`, and a
`.mobj` produced by this toolchain will not be readable by a tool that
implements only the standard three. Since no such tool exists, and the
format is versioned, that cost is theoretical.

The decode-and-re-encode approach makes relocation processing slower than
a masked OR. At the measured link throughput (see `docs/performance.md`)
this is far from the bottleneck.

## Consequences

* `RelocationType` values are on-disk contract: append, never renumber.
* Overflow is always an error. `RELOCATION_OVERFLOW` is reported with the
  symbol name, the section, and the offset; nothing is ever truncated to
  fit.
* A failed relocation leaves the section bytes untouched, so a link that
  fails halfway cannot emit a half-patched image.
