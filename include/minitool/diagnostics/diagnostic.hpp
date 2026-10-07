// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/common/source_manager.hpp"
#include "minitool/common/types.hpp"

namespace minitool::diag {

enum class Severity : u8 { Note, Warning, Error, Fatal };

[[nodiscard]] std::string_view severityName(Severity severity) noexcept;

/// Stable machine-readable classification of every failure the toolchain can
/// report. Values are part of the tool's contract: append, never renumber.
enum class ErrorCode : u16 {
    None = 0,

    // Front end
    LexicalError = 100,
    ParseError = 101,
    InvalidRegister = 102,
    InvalidOpcode = 103,
    InvalidOperand = 104,
    IntegerOverflow = 105,
    InvalidDirective = 106,

    // Symbols and relocation
    UndefinedSymbol = 200,
    DuplicateSymbol = 201,
    RelocationOverflow = 202,
    InvalidRelocation = 203,
    InvalidSectionReference = 204,
    SymbolVisibilityConflict = 205,

    // Binary formats
    InvalidObject = 300,
    InvalidExecutable = 301,
    UnsupportedVersion = 302,
    TruncatedFile = 303,

    // Runtime
    InvalidMemoryAccess = 400,
    IllegalInstruction = 401,
    DivisionByZero = 402,
    StackOverflow = 403,
    StackUnderflow = 404,
    PermissionViolation = 405,
    SyscallError = 406,

    // Tooling
    IoError = 500,
    InternalError = 501,
};

[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

/// One rendered message. `notes` are secondary messages attached to the primary
/// one; `suggestion` is a short "did you mean" style hint.
struct Diagnostic {
    Severity severity = Severity::Error;
    ErrorCode code = ErrorCode::None;
    SourceLocation location{};
    std::string message;
    std::vector<Diagnostic> notes;
    std::optional<std::string> suggestion;
};

}  // namespace minitool::diag
