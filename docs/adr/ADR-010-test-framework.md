# ADR-010: A bundled test runner instead of GoogleTest

**Status:** Accepted

## Context

The first build used GoogleTest through CMake's `FetchContent`. That works on a machine
with CMake and network access. It fails on a machine with neither, such
as a Windows box with only the MSVC build tools installed.

It also sits badly with the project's own premise. The toolchain
hand-writes its lexer, parser, encoder and linker rather
than pulling in a framework; downloading sixty thousand lines of test
framework to check nine thousand lines of project is a strange place to
stop.

## Decision

`tests/support/test_framework.hpp` is a ~300-line xUnit runner providing
the subset of the GoogleTest surface this suite uses: `TEST`, `TEST_F`,
`testing::Test`, `EXPECT_`/`ASSERT_` for `EQ NE LT LE GT GE TRUE FALSE
STREQ`, plus `FAIL`, `ADD_FAILURE` and `<<` message streaming.

It is source-compatible with GoogleTest, so every test file
would compile unchanged against the real thing and this decision stays
reversible by replacing one header and one CMake target.

Two implementation details are worth recording, because both are easy to
get subtly wrong:

* **A failed `ASSERT_` must abort the test without throwing from a
  destructor.** The macro expands to a `for` loop whose iteration
  expression calls `Check::finish()`, which reports and then throws
  `TestAborted`. The throw happens in ordinary control flow, never during
  unwinding.
* **`main` lives in `test_main.cpp`, not in the header**, so a test
  binary can span several translation units.

Value printing uses `if constexpr` over `std::formattable`, string
convertibility, an ADL-found `toString`, and enums, so a failed
comparison of two `isa::Instruction`s prints `MOVI R1, 42` rather than
`<unprintable value>`.

## Alternatives

* **Vendor GoogleTest into the repository.** Removes the network
  dependency and adds ~60k lines to the checkout.
* **Keep `FetchContent` and accept the requirement.** Makes "run the
  tests" conditional on network access, a poor property for a suite that
  is meant to be the project's evidence.
* **No framework, just `assert`.** No test names, no continuing past a
  failure, no per-assertion messages, no way to run one test.

## Trade-offs

The suite gives up death tests, parameterised tests, matchers and gmock.
None are used. The two that might eventually be missed
(`INSTANTIATE_TEST_SUITE_P` for the opcode table, matchers for string
assertions) are replaced by plain loops over `allOpcodes()` and by
`find() != npos`.

CMake registers one `add_test` per binary rather than per case, so
`ctest` reports 24 tests where GoogleTest discovery would report 358.
Each binary prints its own per-case results, and `--filter=` selects
within one.

## Consequences

* The project has *no* third-party dependencies, in the build or in the
  tests. `pwsh tools/build.ps1` on a machine with only MSVC builds and
  runs everything.
* If the suite ever needs a real framework feature, the switch is a
  header swap, thanks to the source compatibility.
