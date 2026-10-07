// SPDX-License-Identifier: MIT
#include "minitool/assembler/sema.hpp"

#include <array>
#include <format>
#include <type_traits>
#include <variant>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool {
namespace {

using ast::Operand;
using ast::OperandType;

/// The section names the toolchain knows how to lay out.
constexpr std::array<std::string_view, 4> kSectionNames{".text", ".rodata", ".data", ".bss"};

[[nodiscard]] bool isKnownSection(std::string_view name) noexcept {
    for (const std::string_view known : kSectionNames) {
        if (known == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool fitsInWidth(i64 value, u32 bytes) noexcept {
    const unsigned bits = bytes * 8U;
    if (bits >= 64) {
        return true;
    }
    // A data directive accepts either spelling of the same bit pattern:
    // `.byte 255` and `.byte -1` are both legal and identical.
    return byteorder::fitsSigned(value, bits) ||
           byteorder::fitsUnsigned(static_cast<u64>(value), bits);
}

}  // namespace

SemanticAnalyzer::SemanticAnalyzer(diag::DiagnosticEngine& diagnostics) noexcept
    : diagnostics_(diagnostics) {}

void SemanticAnalyzer::error(diag::ErrorCode code, SourceLocation location, std::string message) {
    diagnostics_.error(code, location, std::move(message));
    valid_ = false;
}

void SemanticAnalyzer::errorWithNote(diag::ErrorCode code, SourceLocation location,
                                     std::string message, SourceLocation note_location,
                                     std::string note) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = code;
    diagnostic.location = location;
    diagnostic.message = std::move(message);
    diag::Diagnostic attached;
    attached.severity = diag::Severity::Note;
    attached.location = note_location;
    attached.message = std::move(note);
    diagnostic.notes.push_back(std::move(attached));
    diagnostics_.report(std::move(diagnostic));
    valid_ = false;
}

bool SemanticAnalyzer::isValueOperand(const Operand& operand) noexcept {
    return operand.type == OperandType::Immediate || operand.type == OperandType::Symbol;
}

bool SemanticAnalyzer::checkSignedRange(const Operand& operand, unsigned bits,
                                        std::string_view what) {
    // A symbol's final value is only known after linking; the linker checks the
    // relocation range instead, and reports RELOCATION_OVERFLOW there.
    if (operand.type != OperandType::Immediate) {
        return true;
    }
    if (byteorder::fitsSigned(operand.immediate, bits)) {
        return true;
    }
    error(diag::ErrorCode::IntegerOverflow, operand.location,
          std::format("{} {} does not fit in {} signed bits", what, operand.immediate, bits));
    return false;
}

bool SemanticAnalyzer::analyze(const ast::Program& program) {
    valid_ = true;
    labels_.clear();
    for (const ast::Statement& statement : program.statements) {
        std::visit(
            [this](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, ast::InstructionNode>) {
                    checkInstruction(node);
                } else if constexpr (std::is_same_v<Node, ast::DirectiveNode>) {
                    checkDirective(node);
                } else {
                    checkLabel(node);
                }
            },
            statement);
    }
    return valid_;
}

void SemanticAnalyzer::checkLabel(const ast::LabelNode& label) {
    const auto [entry, inserted] = labels_.emplace(label.name, label.location);
    if (!inserted) {
        errorWithNote(diag::ErrorCode::DuplicateSymbol, label.location,
                      std::format("duplicate label '{}'", label.name), entry->second,
                      "previously defined here");
    }
}

void SemanticAnalyzer::checkInstruction(const ast::InstructionNode& instruction) {
    const isa::OpcodeInfo* info = isa::findMnemonic(instruction.mnemonic);
    if (info == nullptr) {
        error(diag::ErrorCode::InvalidOpcode, instruction.location,
              std::format("unknown instruction '{}'", instruction.mnemonic));
        return;
    }

    const std::vector<Operand>& operands = instruction.operands;
    const unsigned expected = isa::operandCount(info->format);
    if (operands.size() != expected) {
        error(diag::ErrorCode::InvalidOperand, instruction.location,
              std::format("{} takes {} operand{}, but {} {} given", info->mnemonic, expected,
                          expected == 1 ? "" : "s", operands.size(),
                          operands.size() == 1 ? "was" : "were"));
        return;
    }

    const auto requireKind = [this, info](const Operand& operand, OperandType wanted,
                                          std::string_view described) {
        if (operand.type != wanted) {
            error(diag::ErrorCode::InvalidOperand, operand.location,
                  std::format("{} expects {} here, found {}", info->mnemonic, described,
                              ast::operandTypeName(operand.type)));
            return false;
        }
        return true;
    };

    switch (info->format) {
        case isa::Format::None:
            break;
        case isa::Format::Reg1:
            static_cast<void>(requireKind(operands[0], OperandType::Register, "a register"));
            break;
        case isa::Format::Reg2:
            static_cast<void>(requireKind(operands[0], OperandType::Register, "a register"));
            static_cast<void>(requireKind(operands[1], OperandType::Register, "a register"));
            break;
        case isa::Format::RegImm:
            if (requireKind(operands[0], OperandType::Register, "a register") &&
                !isValueOperand(operands[1])) {
                error(diag::ErrorCode::InvalidOperand, operands[1].location,
                      std::format("{} expects an immediate or a symbol here, found {}",
                                  info->mnemonic, ast::operandTypeName(operands[1].type)));
            } else {
                static_cast<void>(checkSignedRange(operands[1], isa::kImmediateBits, "immediate"));
            }
            break;
        case isa::Format::Mem: {
            // LOAD reads memory into a register; STORE writes a register to
            // memory, so the operand order is mirrored.
            const bool store = info->opcode == isa::Opcode::STORE;
            const Operand& memory = store ? operands[0] : operands[1];
            const Operand& reg = store ? operands[1] : operands[0];
            static_cast<void>(requireKind(reg, OperandType::Register, "a register"));
            if (requireKind(memory, OperandType::Memory, "a '[base + disp]' memory operand")) {
                static_cast<void>(checkSignedRange(memory, isa::kImmediateBits, "displacement"));
            }
            break;
        }
        case isa::Format::Jump:
            if (!isValueOperand(operands[0])) {
                error(diag::ErrorCode::InvalidOperand, operands[0].location,
                      std::format("{} expects a label or a displacement, found {}", info->mnemonic,
                                  ast::operandTypeName(operands[0].type)));
            } else if (checkSignedRange(operands[0], isa::kImmediateBits, "displacement") &&
                       operands[0].type == OperandType::Immediate &&
                       (operands[0].immediate % isa::kInstructionSize) != 0) {
                // A literal jump operand is a byte displacement, and every
                // instruction is 8-byte aligned, so an unaligned one can only
                // be a mistake.
                error(diag::ErrorCode::InvalidOperand, operands[0].location,
                      std::format("branch displacement {} is not a multiple of {}",
                                  operands[0].immediate, isa::kInstructionSize));
            }
            break;
        case isa::Format::SysImm:
            if (operands[0].type != OperandType::Immediate) {
                error(diag::ErrorCode::InvalidOperand, operands[0].location,
                      "SYSCALL expects a literal service number");
            } else if (operands[0].immediate < 0 ||
                       !byteorder::fitsUnsigned(static_cast<u64>(operands[0].immediate),
                                                isa::kImmediateBits)) {
                error(diag::ErrorCode::IntegerOverflow, operands[0].location,
                      std::format("syscall number {} is out of range", operands[0].immediate));
            }
            break;
    }
}

void SemanticAnalyzer::checkDirective(const ast::DirectiveNode& directive) {
    const std::vector<Operand>& operands = directive.operands;
    const std::string_view name = ast::directiveName(directive.type);

    const auto requireOperandCount = [&](std::size_t least, std::size_t most) {
        if (operands.size() < least || operands.size() > most) {
            error(diag::ErrorCode::InvalidDirective, directive.location,
                  least == most ? std::format("{} takes exactly {} operand(s), {} given", name,
                                              least, operands.size())
                                : std::format("{} takes between {} and {} operands, {} given", name,
                                              least, most, operands.size()));
            return false;
        }
        return true;
    };

    switch (directive.type) {
        case ast::DirectiveType::Section: {
            if (!requireOperandCount(1, 1)) {
                return;
            }
            if (operands[0].type != OperandType::Symbol) {
                error(diag::ErrorCode::InvalidDirective, operands[0].location,
                      ".section expects a section name");
                return;
            }
            if (!isKnownSection(operands[0].symbol)) {
                error(diag::ErrorCode::InvalidSectionReference, operands[0].location,
                      std::format("unknown section '{}'; expected .text, .rodata, .data or .bss",
                                  operands[0].symbol));
            }
            break;
        }
        case ast::DirectiveType::Global:
        case ast::DirectiveType::Extern:
        case ast::DirectiveType::Weak:
            if (operands.empty()) {
                error(diag::ErrorCode::InvalidDirective, directive.location,
                      std::format("{} needs at least one symbol name", name));
                return;
            }
            for (const Operand& operand : operands) {
                if (operand.type != OperandType::Symbol || operand.immediate != 0) {
                    error(diag::ErrorCode::InvalidDirective, operand.location,
                          std::format("{} expects plain symbol names", name));
                }
            }
            break;
        case ast::DirectiveType::Byte:
        case ast::DirectiveType::Word:
        case ast::DirectiveType::Dword:
        case ast::DirectiveType::Qword: {
            const u32 width = ast::dataDirectiveWidth(directive.type);
            if (operands.empty()) {
                error(diag::ErrorCode::InvalidDirective, directive.location,
                      std::format("{} needs at least one value", name));
                return;
            }
            for (const Operand& operand : operands) {
                if (!isValueOperand(operand)) {
                    error(diag::ErrorCode::InvalidDirective, operand.location,
                          std::format("{} expects integers or symbol references, found {}", name,
                                      ast::operandTypeName(operand.type)));
                    continue;
                }
                if (operand.type == OperandType::Symbol && width != 8 && width != 4) {
                    error(diag::ErrorCode::InvalidRelocation, operand.location,
                          std::format("a symbol address needs .dword or .qword, not {}", name));
                    continue;
                }
                if (operand.type == OperandType::Immediate &&
                    !fitsInWidth(operand.immediate, width)) {
                    error(diag::ErrorCode::IntegerOverflow, operand.location,
                          std::format("{} does not fit in the {} byte(s) of {}", operand.immediate,
                                      width, name));
                }
            }
            break;
        }
        case ast::DirectiveType::Asciz:
            if (!requireOperandCount(1, 1)) {
                return;
            }
            if (operands[0].type != OperandType::String) {
                error(diag::ErrorCode::InvalidDirective, operands[0].location,
                      ".asciz expects a string literal");
            }
            break;
        case ast::DirectiveType::Align: {
            if (!requireOperandCount(1, 1)) {
                return;
            }
            if (operands[0].type != OperandType::Immediate || operands[0].immediate <= 0) {
                error(diag::ErrorCode::InvalidDirective, operands[0].location,
                      ".align expects a positive integer");
                return;
            }
            const auto value = static_cast<u64>(operands[0].immediate);
            if ((value & (value - 1U)) != 0U) {
                error(diag::ErrorCode::InvalidDirective, operands[0].location,
                      std::format(".align expects a power of two, found {}", value));
            }
            break;
        }
        case ast::DirectiveType::Space: {
            if (!requireOperandCount(1, 2)) {
                return;
            }
            if (operands[0].type != OperandType::Immediate || operands[0].immediate < 0) {
                error(diag::ErrorCode::InvalidDirective, operands[0].location,
                      ".space expects a non-negative size");
            }
            if (operands.size() == 2 && (operands[1].type != OperandType::Immediate ||
                                         !fitsInWidth(operands[1].immediate, 1))) {
                error(diag::ErrorCode::InvalidDirective, operands[1].location,
                      ".space fill value must be a single byte");
            }
            break;
        }
    }
}

}  // namespace minitool
