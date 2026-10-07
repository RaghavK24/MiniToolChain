# ADR-005: `std::expected` for invalid input, assertions for broken invariants

**Status:** Accepted

## Context
The toolchain has two very different failure kinds: a user's malformed
program, and a bug in the toolchain itself. Conflating them produces
either exception-driven control flow for ordinary errors, or crashes on
bad input.

## Decision
* Invalid **input** returns `std::expected<T, E>` with a typed error
  enum, or is reported as a `Diagnostic` when it has a source location.
* Broken **internal invariants** assert.
* Nothing in the core libraries throws for ordinary invalid input, and
  nothing writes to `stderr` on its own.

## Alternatives
* **Exceptions.** Malformed input is expected, not exceptional; and a
  decoder in the VM's inner loop should not need an unwind path.
* **Error codes with out-parameters.** Easy to ignore a return value;
  `[[nodiscard]] std::expected` is not.

## Trade-offs
`std::expected` requires a recent standard library, and error propagation
is more verbose than exceptions. In exchange, every failure path is
visible in the signature and cannot be forgotten silently.

## Consequences
`DiagnosticEngine` collects rather than prints, so the same engine serves
the CLI, the tests, and eventually an LSP-style consumer.
