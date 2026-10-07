// SPDX-License-Identifier: MIT
#pragma once

#include <span>
#include <string>

#include "minitool/common/types.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/isa/instruction.hpp"

namespace minitool::disassembler {

struct Options {
    /// Print the raw 8-byte word next to each instruction.
    bool show_bytes = true;
    /// Resolve branch targets and addresses to symbol names.
    bool show_symbols = true;
    /// Emit `name:` lines where a symbol starts.
    bool show_labels = true;
};

/// Renders machine code as assembly.
///
/// It decodes through isa::decode, the same function the VM executes with
/// (architectural rule 9), so the disassembler cannot drift from the machine.
/// A word that does not decode is printed as data rather than guessed at.
class Disassembler {
  public:
    explicit Disassembler(Options options = {}) noexcept;

    /// Disassembles `code`, which begins at `base_address`. `executable` is
    /// optional and is used only to resolve names.
    [[nodiscard]] std::string disassemble(std::span<const u8> code, u64 base_address,
                                          const executable::Executable* executable = nullptr) const;

    /// Disassembles every executable segment of `executable`.
    [[nodiscard]] std::string disassemble(const executable::Executable& executable) const;

    /// One line, without the trailing newline.
    [[nodiscard]] std::string formatInstruction(
        u64 address, const isa::Instruction& instruction,
        const executable::Executable* executable = nullptr) const;

  private:
    Options options_;
};

}  // namespace minitool::disassembler
