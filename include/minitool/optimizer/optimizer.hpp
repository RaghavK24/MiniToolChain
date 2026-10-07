// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "minitool/common/types.hpp"
#include "minitool/ir/ir.hpp"

namespace minitool::optimizer {

enum class OptLevel : u8 {
    /// Emit exactly what was written.
    O0 = 0,
    /// Local (within a basic block) transformations that preserve observable
    /// behaviour.
    O1 = 1,
};

[[nodiscard]] std::string_view optLevelName(OptLevel level) noexcept;

struct OptStats {
    u32 constants_folded = 0;
    u32 identities_eliminated = 0;
    u32 dead_stores_removed = 0;
    u32 unreachable_removed = 0;
    u32 peepholes_applied = 0;
    std::size_t instructions_before = 0;
    std::size_t instructions_after = 0;

    [[nodiscard]] u32 total() const noexcept {
        return constants_folded + identities_eliminated + dead_stores_removed +
               unreachable_removed + peepholes_applied;
    }
    [[nodiscard]] std::string summary() const;
};

/// Local optimizer over the IR (architectural rule 8).
///
/// It works on basic blocks: a block starts at a label and ends after any
/// branch, HALT or SYSCALL. Because the IR has no addresses yet, deleting or
/// rewriting an instruction cannot invalidate a branch: displacements are
/// computed afterwards, by the assembler, from the labels that survive.
///
/// Two rules keep the transformations honest:
///   * a label is never moved, merged or deleted, so every branch target that
///     existed before still exists;
///   * FLAGS is treated as a real output. An instruction that sets flags is
///     only rewritten into one that does not when the flags are provably dead
///     before the end of the block.
class Optimizer {
  public:
    explicit Optimizer(OptLevel level = OptLevel::O1) noexcept;

    /// Rewrites `module` in place and reports what it did.
    OptStats run(ir::Module& module) const;

  private:
    void optimizeSection(ir::Section& section, OptStats& stats) const;
    /// One pass over one section; returns true if anything changed, so the
    /// caller can iterate to a fixed point.
    bool runPasses(ir::Section& section, OptStats& stats) const;

    OptLevel level_;
};

}  // namespace minitool::optimizer
