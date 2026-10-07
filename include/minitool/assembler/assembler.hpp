// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <string>

#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/ir/ir.hpp"
#include "minitool/object/object.hpp"

namespace minitool {

/// Turns an IR module into a relocatable object file.
///
/// Pass 1 walks every section assigning offsets, which defines every label;
/// pass 2 encodes the instructions and emits a relocation wherever a value is
/// still unknown. Splitting it this way is what makes a forward reference work:
/// pass 2 can see labels that appear later in the file.
///
/// The assembler never executes code and never reads or writes files
/// (architectural rule 3); it produces an in-memory ObjectFile which the caller
/// may serialise.
class Assembler {
  public:
    explicit Assembler(diag::DiagnosticEngine& diagnostics) noexcept;

    [[nodiscard]] std::expected<object::ObjectFile, std::string> assemble(const ir::Module& module);

  private:
    diag::DiagnosticEngine& diagnostics_;
};

}  // namespace minitool
