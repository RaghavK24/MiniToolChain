// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "minitool/common/source_manager.hpp"
#include "minitool/diagnostics/diagnostic.hpp"

namespace minitool::diag {

/// Collects diagnostics and renders them with source context.
///
/// The engine never writes to stdout/stderr itself and holds no global state;
/// callers decide when and where to print. It borrows a SourceManager, which
/// must outlive the engine.
class DiagnosticEngine {
  public:
    explicit DiagnosticEngine(const SourceManager& sources) noexcept : sources_(&sources) {}

    void report(Diagnostic diagnostic);

    /// Convenience helpers for the common single-message cases.
    void error(ErrorCode code, SourceLocation location, std::string message);
    void warning(ErrorCode code, SourceLocation location, std::string message);
    void note(SourceLocation location, std::string message);

    [[nodiscard]] bool hasErrors() const noexcept { return error_count_ > 0; }
    [[nodiscard]] std::size_t errorCount() const noexcept { return error_count_; }
    [[nodiscard]] std::size_t warningCount() const noexcept { return warning_count_; }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// Renders one diagnostic in the canonical form:
    ///
    ///   main.asm:12:5: error: undefined symbol 'print'
    ///      12 |     CALL print
    ///         |          ^~~~~
    ///   note: ...
    [[nodiscard]] std::string render(const Diagnostic& diagnostic) const;

    /// Renders every collected diagnostic in report order.
    [[nodiscard]] std::string renderAll() const;

    void clear() noexcept;

  private:
    void renderInto(std::string& out, const Diagnostic& diagnostic, bool is_note) const;

    const SourceManager* sources_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
    std::size_t warning_count_ = 0;
};

}  // namespace minitool::diag
