// SPDX-License-Identifier: MIT
#include "minitool/diagnostics/diagnostic_engine.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace minitool::diag {
namespace {

/// Expands tabs to single spaces so the caret line stays aligned with the
/// rendered source line.
[[nodiscard]] std::string detab(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c == '\t' ? ' ' : c);
    }
    return out;
}

}  // namespace

void DiagnosticEngine::report(Diagnostic diagnostic) {
    switch (diagnostic.severity) {
        case Severity::Error:
        case Severity::Fatal:
            ++error_count_;
            break;
        case Severity::Warning:
            ++warning_count_;
            break;
        case Severity::Note:
            break;
    }
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticEngine::error(ErrorCode code, SourceLocation location, std::string message) {
    report(Diagnostic{Severity::Error, code, location, std::move(message), {}, std::nullopt});
}

void DiagnosticEngine::warning(ErrorCode code, SourceLocation location, std::string message) {
    report(Diagnostic{Severity::Warning, code, location, std::move(message), {}, std::nullopt});
}

void DiagnosticEngine::note(SourceLocation location, std::string message) {
    report(Diagnostic{
        Severity::Note, ErrorCode::None, location, std::move(message), {}, std::nullopt});
}

void DiagnosticEngine::clear() noexcept {
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
}

void DiagnosticEngine::renderInto(std::string& out, const Diagnostic& d, bool is_note) const {
    const std::string_view severity = severityName(d.severity);

    if (d.location.valid() && sources_->contains(d.location.file)) {
        out += std::format("{}:{}:{}: {}: {}\n", sources_->name(d.location.file), d.location.line,
                           d.location.column, severity, d.message);
    } else {
        out += std::format("{}: {}\n", severity, d.message);
    }

    const std::string_view raw_line =
        d.location.valid() ? sources_->line(d.location.file, d.location.line) : std::string_view{};
    if (!raw_line.empty() && d.location.column > 0) {
        const std::string line = detab(raw_line);
        const std::string gutter = std::format("{:>5}", d.location.line);
        out += std::format("{} | {}\n", gutter, line);

        // Columns are 1-based; clamp so a stale location can never make us
        // build a caret line longer than the source line.
        const std::size_t col = std::min<std::size_t>(d.location.column - 1U, line.size());
        const std::size_t remaining = line.size() - col;
        const std::size_t span = std::min<std::size_t>(std::max<u32>(d.location.length, 1U),
                                                       std::max<std::size_t>(remaining, 1U));
        std::string caret(col, ' ');
        caret.push_back('^');
        if (span > 1) {
            caret.append(span - 1, '~');
        }
        out += std::format("{} | {}\n", std::string(gutter.size(), ' '), caret);
    }

    if (d.suggestion) {
        out += std::format("  suggestion: {}\n", *d.suggestion);
    }
    for (const Diagnostic& note : d.notes) {
        renderInto(out, note, true);
    }
    (void)is_note;
}

std::string DiagnosticEngine::render(const Diagnostic& diagnostic) const {
    std::string out;
    renderInto(out, diagnostic, false);
    return out;
}

std::string DiagnosticEngine::renderAll() const {
    std::string out;
    for (const Diagnostic& d : diagnostics_) {
        renderInto(out, d, false);
    }
    return out;
}

}  // namespace minitool::diag
