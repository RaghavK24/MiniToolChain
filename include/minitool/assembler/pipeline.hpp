// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <string>

#include "minitool/common/source_manager.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/ir/ir.hpp"
#include "minitool/object/object.hpp"
#include "minitool/optimizer/optimizer.hpp"

namespace minitool {

struct AssembleOptions {
    optimizer::OptLevel opt_level = optimizer::OptLevel::O0;
    /// Emit the line table that the debugger uses. Turning it off produces a
    /// smaller object with no other change in behaviour.
    bool emit_debug_info = true;
};

struct AssembleResult {
    object::ObjectFile object;
    optimizer::OptStats stats;
    ir::Module module;
};

/// Runs the whole front end for one file: lex, parse, analyse, lower to IR,
/// optimize, assemble.
///
/// Each stage is available separately; this exists so that the driver and the
/// tests exercise exactly the same sequence, and so there is one place where
/// the order of the pipeline is written down.
[[nodiscard]] std::expected<AssembleResult, std::string> assembleSource(
    const SourceManager& sources, FileId file, diag::DiagnosticEngine& diagnostics,
    const AssembleOptions& options = {});

}  // namespace minitool
