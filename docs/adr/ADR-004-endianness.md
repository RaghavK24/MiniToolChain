# ADR-004: Little-endian everywhere, via explicit byte loops

**Status:** Accepted, **frozen**

## Context
Binary output must be identical on every host, and untrusted files must
be parsed without undefined behaviour.

## Decision
Every integer in a `.mobj`, a `.mexe`, or VM memory is little-endian.
Conversion goes exclusively through `minitool::byteorder::load` /
`store`, which loop over bytes and shift.

Casting a `const u8*` to a wider integer pointer is forbidden anywhere in
the codebase: it is an alignment and strict-aliasing violation on
untrusted input.

## Alternatives
* `memcpy` into a value plus a host-endianness swap: correct, but hides
  the byte order behind a build-time condition.
* `std::byteswap` / `std::endian`: fine for the swap, but still needs
  the same care about alignment when reading files.

## Trade-offs
The byte loop is marginally slower than a single load on
little-endian hosts. It compiles to the same instruction at `-O2` in
practice, and correctness on unaligned untrusted buffers is worth more
than the difference regardless.

## Consequences
Output is byte-identical across hosts, so golden binary tests mean the
same thing everywhere.
