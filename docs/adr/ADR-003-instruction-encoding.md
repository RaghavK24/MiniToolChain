# ADR-003: Reserved encoding fields are validated, not ignored

**Status:** Accepted, **frozen**

## Context
`HALT` uses none of the dst, src or immediate fields. A decoder can
either ignore those bits or require them to be zero.

## Decision
Require them to be zero. The decoder rejects any word with a non-zero
reserved field (`ReservedRegisterFieldSet` /
`ReservedImmediateFieldSet`), and the encoder rejects any `Instruction`
whose unused fields are non-zero (`NonCanonicalOperand`).

## Alternatives
* **Ignore reserved bits.** Then `decode(encode(i)) == i` only holds "up
  to unused fields", golden binary comparison needs a normalisation step,
  and a corrupted file silently decodes as a valid program.

## Trade-offs
Slightly stricter than real hardware, which usually ignores such bits.
The gain is that the codec is a genuine bijection between valid words and
canonical instructions, so both round-trip directions are exact
equalities that can be property-tested, and file corruption in a reserved
field is caught at load time rather than executed.

## Consequences
Future opcodes may not repurpose a reserved field of an existing opcode
without a format change and a format version bump.
