// SPDX-License-Identifier: MIT
#include "minitool/ast/ast.hpp"

namespace minitool::ast {

std::string_view operandTypeName(OperandType type) noexcept {
    switch (type) {
        case OperandType::Register:
            return "register";
        case OperandType::Immediate:
            return "immediate";
        case OperandType::Symbol:
            return "symbol";
        case OperandType::Memory:
            return "memory operand";
        case OperandType::String:
            return "string";
    }
    return "operand";
}

std::string_view directiveName(DirectiveType type) noexcept {
    switch (type) {
        case DirectiveType::Section:
            return ".section";
        case DirectiveType::Global:
            return ".global";
        case DirectiveType::Extern:
            return ".extern";
        case DirectiveType::Weak:
            return ".weak";
        case DirectiveType::Byte:
            return ".byte";
        case DirectiveType::Word:
            return ".word";
        case DirectiveType::Dword:
            return ".dword";
        case DirectiveType::Qword:
            return ".qword";
        case DirectiveType::Asciz:
            return ".asciz";
        case DirectiveType::Align:
            return ".align";
        case DirectiveType::Space:
            return ".space";
    }
    return ".unknown";
}

u32 dataDirectiveWidth(DirectiveType type) noexcept {
    switch (type) {
        case DirectiveType::Byte:
            return 1;
        case DirectiveType::Word:
            return 2;
        case DirectiveType::Dword:
            return 4;
        case DirectiveType::Qword:
            return 8;
        default:
            return 0;
    }
}

SourceLocation statementLocation(const Statement& statement) noexcept {
    return std::visit([](const auto& node) { return node.location; }, statement);
}

}  // namespace minitool::ast
