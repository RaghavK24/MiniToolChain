// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "minitool/common/source_manager.hpp"
#include "minitool/common/types.hpp"
#include "minitool/isa/registers.hpp"

/// The parse tree of one assembly file: a flat sequence of statements in source
/// order. It is deliberately syntactic: it records what was written, not what
/// it means. Nothing here knows an opcode number, a section layout or an
/// instruction encoding; that is the job of sema, the IR lowering and the
/// assembler respectively (architectural rule 2).
namespace minitool::ast {

enum class OperandType : u8 {
    /// `R7`, `FP`
    Register,
    /// `10`, `0xFF`, `'a'`, `#4`
    Immediate,
    /// `message`, `message + 8`, `.text`
    Symbol,
    /// `[R2 + 8]`
    Memory,
    /// `"hello"`. Only valid as a directive argument.
    String,
};

[[nodiscard]] std::string_view operandTypeName(OperandType type) noexcept;

struct Operand {
    OperandType type = OperandType::Immediate;
    /// Register operands, and the base register of a memory operand.
    isa::Reg reg = isa::Reg::R0;
    /// Immediate value, memory displacement, or the addend of a symbol
    /// reference (`message + 8` stores 8 here).
    i64 immediate = 0;
    /// Symbol name for OperandType::Symbol.
    std::string symbol;
    /// Decoded bytes for OperandType::String.
    std::string text;
    SourceLocation location;
};

struct InstructionNode {
    /// Spelled exactly as written; case-insensitive lookup happens in sema.
    std::string mnemonic;
    std::vector<Operand> operands;
    SourceLocation location;
};

struct LabelNode {
    std::string name;
    /// True for `.Lname:`: a label that never becomes a global symbol and is
    /// not visible to other objects.
    bool local = false;
    SourceLocation location;
};

enum class DirectiveType : u8 {
    Section,
    Global,
    Extern,
    Weak,
    Byte,
    Word,
    Dword,
    Qword,
    Asciz,
    Align,
    Space,
};

[[nodiscard]] std::string_view directiveName(DirectiveType type) noexcept;

/// Width in bytes emitted by one operand of a data directive, or 0 if the
/// directive is not a data directive.
[[nodiscard]] u32 dataDirectiveWidth(DirectiveType type) noexcept;

struct DirectiveNode {
    DirectiveType type = DirectiveType::Section;
    std::vector<Operand> operands;
    SourceLocation location;
};

using Statement = std::variant<InstructionNode, LabelNode, DirectiveNode>;

struct Program {
    std::vector<Statement> statements;
};

[[nodiscard]] SourceLocation statementLocation(const Statement& statement) noexcept;

}  // namespace minitool::ast
