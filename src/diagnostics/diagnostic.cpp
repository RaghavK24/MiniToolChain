// SPDX-License-Identifier: MIT
#include "minitool/diagnostics/diagnostic.hpp"

namespace minitool::diag {

std::string_view severityName(Severity severity) noexcept {
    switch (severity) {
        case Severity::Note:
            return "note";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
        case Severity::Fatal:
            return "fatal error";
    }
    return "unknown";
}

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:
            return "none";
        case ErrorCode::LexicalError:
            return "LEXICAL_ERROR";
        case ErrorCode::ParseError:
            return "PARSE_ERROR";
        case ErrorCode::InvalidRegister:
            return "INVALID_REGISTER";
        case ErrorCode::InvalidOpcode:
            return "INVALID_OPCODE";
        case ErrorCode::InvalidOperand:
            return "INVALID_OPERAND";
        case ErrorCode::IntegerOverflow:
            return "INTEGER_OVERFLOW";
        case ErrorCode::InvalidDirective:
            return "INVALID_DIRECTIVE";
        case ErrorCode::UndefinedSymbol:
            return "UNDEFINED_SYMBOL";
        case ErrorCode::DuplicateSymbol:
            return "DUPLICATE_SYMBOL";
        case ErrorCode::RelocationOverflow:
            return "RELOCATION_OVERFLOW";
        case ErrorCode::InvalidRelocation:
            return "INVALID_RELOCATION";
        case ErrorCode::InvalidSectionReference:
            return "INVALID_SECTION_REFERENCE";
        case ErrorCode::SymbolVisibilityConflict:
            return "SYMBOL_VISIBILITY_CONFLICT";
        case ErrorCode::InvalidObject:
            return "INVALID_OBJECT";
        case ErrorCode::InvalidExecutable:
            return "INVALID_EXECUTABLE";
        case ErrorCode::UnsupportedVersion:
            return "UNSUPPORTED_VERSION";
        case ErrorCode::TruncatedFile:
            return "TRUNCATED_FILE";
        case ErrorCode::InvalidMemoryAccess:
            return "INVALID_MEMORY_ACCESS";
        case ErrorCode::IllegalInstruction:
            return "ILLEGAL_INSTRUCTION";
        case ErrorCode::DivisionByZero:
            return "DIVISION_BY_ZERO";
        case ErrorCode::StackOverflow:
            return "STACK_OVERFLOW";
        case ErrorCode::StackUnderflow:
            return "STACK_UNDERFLOW";
        case ErrorCode::PermissionViolation:
            return "PERMISSION_VIOLATION";
        case ErrorCode::SyscallError:
            return "SYSCALL_ERROR";
        case ErrorCode::IoError:
            return "IO_ERROR";
        case ErrorCode::InternalError:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

}  // namespace minitool::diag
