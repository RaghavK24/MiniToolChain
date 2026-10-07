// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <string>

#include "minitool/ast/ast.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/ir/ir.hpp"

namespace minitool::ir {

/// Lowers a validated AST into the IR: resolves mnemonics to opcodes, splits
/// statements into sections, turns directives into data items and marks the
/// operands that still need a relocation.
///
/// `program` is expected to have passed SemanticAnalyzer; lowering re-reports
/// anything it still cannot represent rather than assuming. `source_name` is
/// recorded for the debug line table.
[[nodiscard]] std::expected<Module, std::string> lower(const ast::Program& program,
                                                       std::string source_name,
                                                       diag::DiagnosticEngine& diagnostics);

}  // namespace minitool::ir
