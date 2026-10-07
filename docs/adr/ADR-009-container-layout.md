# ADR-009: Table-of-contents container layout for `.mobj` and `.mexe`

**Status:** Accepted

## Context

Both binary formats need to be written, read, validated and diffed. The
straightforward design is a stream: write each structure one after
another and read them back in the same order. It is simple to write and
unpleasant to validate, because you cannot check that a table fits in the
file until you have already walked everything before it.

The formats also have to be *deterministic* and independently readable:
a reader must not need the writer's source to know where anything is.

## Decision

Both formats use the same shape:

```
+------------------+  fixed 64-byte header: magic, version, counts,
| header           |  table offsets, file size, CRC-32
+------------------+
| fixed-size       |  every record in a table is the same size, so a
| record tables    |  table's extent is one multiplication
+------------------+
| string table     |  NUL-terminated; offset 0 is the empty string
+------------------+
| blob             |  section / segment contents
+------------------+
```

Consequences of that shape, in order of how much they matter:

* **Bounds checking is arithmetic, not iteration.** The reader computes
  every table's start and end from the header, checks them against the
  file size once, and only then reads records. A truncated file is
  rejected before a single record is parsed.
* **Names are interned.** A repeated section name costs four bytes, and a
  string offset is validated against a known table extent.
* **The header is patched, not predicted.** Offsets and the checksum are
  written as zero and filled in once the body exists, so no part of the
  writer has to compute a size in advance.
* **CRC-32 covers everything after the header.** It catches what
  structural validation cannot: a flipped bit inside a field that stays
  structurally legal.

Determinism falls out. Sections are emitted in a canonical order
(`.text`, `.rodata`, `.data`, `.bss`) whatever order the source used,
strings are interned in first-use order, and nothing records a timestamp,
a path from the build machine, or an address from the writer's heap.

## Alternatives

* **A streaming format.** Smaller writer, much weaker reader. Validation
  becomes "read it and hope", while the readers here check everything.
* **Variable-length records with a length prefix.** More compact for
  short names. Every bound then has to be re-derived per record, and the
  reader gains a class of bug that fixed records cannot have.
* **Reuse ELF.** Out of scope for this project, and it would
  replace the exercise with a reading-comprehension task.

## Trade-offs

Fixed records waste a few bytes on padding (a symbol record is 32 bytes
where 26 would do). The reader's simplicity is worth more than the space,
and every padding field is *validated as zero* rather than skipped, so
the slack cannot become an undocumented side channel.

The reserved `flags` word in the object header is likewise rejected when
non-zero. That keeps the door open for a future flag to
mean something, and it means no byte of the header is ignored. An earlier
version accepted any value there, and
`Object.SurvivesEveryOneByteCorruption` caught it: four header bytes
could be flipped with no effect on the parse.

## Consequences

* Adding a field means a new version number and a note here. Appending a
  whole table is easier: a new count-and-offset pair in the reserved
  header space.
* `readObjectFromBuffer` and `readExecutableFromBuffer` are the only
  places that interpret these bytes. `minitool verify` is a thin shell
  over the executable reader, because a successful read *is* a successful
  verification.
* Golden fixtures in `tests/fixtures/` pin the exact bytes, so changing
  the layout means regenerating them.
