# ADR-001: Implementation language is C++23

**Status:** Accepted

## Context
The project needs explicit control over binary layout, no hidden
allocation in hot paths, and a credible systems-programming signal.

## Decision
C++23, built with CMake, targeting GCC 13+ and Clang 17+.

## Alternatives
* **Rust**: better safety story, but the project's teaching goal is to
  handle unsafe binary parsing correctly *by construction*, which C++
  forces you to confront explicitly.
* **C**: no RAII, no `std::expected`, no `std::span`; more manual error
  plumbing for no architectural benefit.
* **Zig**: good fit, smaller ecosystem, weaker tooling for static
  analysis and sanitizers.

## Trade-offs
C++23 support is uneven. `<print>` is missing on GCC 13, so console
output goes through `minitool/common/print.hpp` instead. `std::expected`
is available and is used as the core error channel.

## Consequences
Every target compiles with `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Wold-style-cast -Werror`, and the test suite
runs under ASan and UBSan.
