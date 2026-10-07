// SPDX-License-Identifier: MIT
#pragma once

/// One self-contained "compile this text and run it" job.
///
/// This is what the playground UI is built on, but it knows nothing about HTTP
/// or the UI: it takes source text and returns everything a caller could want
/// to display. Keeping it separate is what makes the interesting behaviour (which
/// stage failed, what the diagnostics say, what the program printed)
/// testable without a socket (architectural rule 5: the front end never
/// depends on a transport).
///
/// Every limit here exists because the input is untrusted: a playground accepts
/// whatever someone types, including a program that never terminates.

#include <array>
#include <string>
#include <string_view>

#include "minitool/common/types.hpp"
#include "minitool/isa/registers.hpp"
#include "minitool/optimizer/optimizer.hpp"

namespace minitool::playground {

/// Longest accepted source text. Generous for anything hand-written; it exists
/// so a request cannot make the server allocate without bound.
inline constexpr std::size_t kMaxSourceBytes = 256 * 1024;

/// Most program output kept. Beyond this the run continues but the extra bytes
/// are dropped and `output_truncated` is set, so a program looping on `write`
/// cannot exhaust memory.
inline constexpr std::size_t kMaxOutputBytes = 256 * 1024;

/// Instruction budget for a playground run. Far below the VM's own default:
/// a browser is waiting, so an endless program has to be cut off in well under
/// a second rather than eventually.
inline constexpr u64 kDefaultPlaygroundBudget = 5'000'000;

/// The largest budget a caller may ask for.
inline constexpr u64 kMaxPlaygroundBudget = 50'000'000;

struct RunRequest {
    std::string source;
    /// Supplied to the program's `read` syscall.
    std::string input;
    optimizer::OptLevel opt_level = optimizer::OptLevel::O0;
    u64 budget = kDefaultPlaygroundBudget;
    /// Also disassemble the linked image. Off makes the report smaller.
    bool want_disassembly = true;
};

/// Where a job stopped. `Finished` means the program ran to completion; every
/// other value names the stage that rejected it, which is what lets the UI say
/// "this is an assembly error" rather than just "it failed".
enum class Stage : u8 {
    Assemble,
    Link,
    Load,
    Execute,
    Finished,
};

[[nodiscard]] std::string_view stageName(Stage stage) noexcept;

struct RunReport {
    /// True only if the program assembled, linked, loaded and ran to a halt.
    bool ok = false;
    Stage stage = Stage::Finished;

    /// Rendered with source context and carets: the same text the CLI prints.
    std::string diagnostics;
    /// A single-line reason when the failure has no source location to point
    /// at (a link or runtime error). Empty when `diagnostics` covers it.
    std::string error;

    /// What the program wrote to fd 1.
    std::string output;
    bool output_truncated = false;

    std::string disassembly;

    u64 exit_code = 0;
    u64 instructions = 0;
    optimizer::OptStats stats;

    /// Final machine state, meaningful once the program has run.
    std::array<u64, isa::kRegisterCount> registers{};
    u64 pc = 0;
    u64 sp = 0;
    u64 flags = 0;
};

/// Assembles, links, loads and runs `request`, capturing everything.
///
/// Never throws for bad input and never blocks indefinitely: a source that is
/// too long, fails to assemble, or never halts all come back as a report.
[[nodiscard]] RunReport runSource(const RunRequest& request);

}  // namespace minitool::playground
