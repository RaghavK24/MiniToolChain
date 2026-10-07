// SPDX-License-Identifier: MIT
#include "support/test_framework.hpp"

#include "minitool/diagnostics/diagnostic_engine.hpp"

namespace {

using namespace minitool;
using namespace minitool::diag;

TEST(Diagnostics, CountsBySeverity) {
    SourceManager sources;
    DiagnosticEngine engine(sources);
    engine.error(ErrorCode::UndefinedSymbol, {}, "boom");
    engine.warning(ErrorCode::IntegerOverflow, {}, "hmm");
    engine.note({}, "context");
    EXPECT_TRUE(engine.hasErrors());
    EXPECT_EQ(engine.errorCount(), 1U);
    EXPECT_EQ(engine.warningCount(), 1U);
    EXPECT_EQ(engine.diagnostics().size(), 3U);
    engine.clear();
    EXPECT_FALSE(engine.hasErrors());
    EXPECT_TRUE(engine.diagnostics().empty());
}

TEST(Diagnostics, RendersCaretUnderTheOffendingText) {
    SourceManager sources;
    const FileId id = sources.addFile("main.asm", "_start:\n    CALL print\n    HALT\n");
    DiagnosticEngine engine(sources);
    engine.error(ErrorCode::UndefinedSymbol, SourceLocation{id, 2, 10, 5},
                 "undefined symbol 'print'");

    const std::string text = engine.renderAll();
    EXPECT_NE(text.find("main.asm:2:10: error: undefined symbol 'print'"), std::string::npos);
    EXPECT_NE(text.find("    2 |     CALL print"), std::string::npos);
    EXPECT_NE(text.find("^~~~~"), std::string::npos);
}

TEST(Diagnostics, RendersNotesAndSuggestions) {
    SourceManager sources;
    const FileId id = sources.addFile("main.asm", "MOVI R19, 1\n");
    DiagnosticEngine engine(sources);
    Diagnostic d{Severity::Error,
                 ErrorCode::InvalidRegister,
                 SourceLocation{id, 1, 6, 3},
                 "invalid register 'R19'",
                 {},
                 "did you mean 'R9'?"};
    d.notes.push_back(Diagnostic{
        Severity::Note, ErrorCode::None, {}, "registers are R0 through R15", {}, std::nullopt});
    engine.report(std::move(d));

    const std::string text = engine.renderAll();
    EXPECT_NE(text.find("suggestion: did you mean 'R9'?"), std::string::npos);
    EXPECT_NE(text.find("note: registers are R0 through R15"), std::string::npos);
}

TEST(Diagnostics, RendersWithoutSourceWhenLocationIsInvalid) {
    SourceManager sources;
    DiagnosticEngine engine(sources);
    engine.error(ErrorCode::IoError, {}, "cannot open 'missing.asm'");
    EXPECT_EQ(engine.renderAll(), "error: cannot open 'missing.asm'\n");
}

TEST(Diagnostics, ClampsOutOfRangeLocationsInsteadOfOverrunning) {
    SourceManager sources;
    const FileId id = sources.addFile("main.asm", "ADD\n");
    DiagnosticEngine engine(sources);
    // Column and length both point past the end of a 3-character line.
    engine.error(ErrorCode::InvalidOperand, SourceLocation{id, 1, 99, 40}, "missing operand");
    const std::string text = engine.renderAll();
    EXPECT_NE(text.find("main.asm:1:99"), std::string::npos);
    EXPECT_NE(text.find('^'), std::string::npos);
}

TEST(Diagnostics, ErrorCodeNamesAreStable) {
    EXPECT_EQ(errorCodeName(ErrorCode::RelocationOverflow), "RELOCATION_OVERFLOW");
    EXPECT_EQ(errorCodeName(ErrorCode::DivisionByZero), "DIVISION_BY_ZERO");
    EXPECT_EQ(severityName(Severity::Fatal), "fatal error");
}

}  // namespace
