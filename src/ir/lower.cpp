// SPDX-License-Identifier: MIT
#include "minitool/ir/lower.hpp"

#include <format>
#include <type_traits>
#include <variant>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool::ir {
namespace {

using ast::Operand;
using ast::OperandType;

/// Carries the lowering state for one file: which section is current, and
/// whether anything has gone wrong.
class Lowerer {
  public:
    Lowerer(std::string source_name, diag::DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {
        module_.source_name = std::move(source_name);
        // Every file starts in .text, exactly like a real assembler, so a
        // fragment with no .section directive still assembles.
        static_cast<void>(module_.sectionFor(SectionKind::Text));
    }

    std::expected<Module, std::string> run(const ast::Program& program) {
        for (const ast::Statement& statement : program.statements) {
            std::visit([this](const auto& node) { lowerStatement(node); }, statement);
        }
        if (failed_) {
            return std::unexpected(
                std::format("{} error(s) lowering to IR", diagnostics_.errorCount()));
        }
        return std::move(module_);
    }

  private:
    void error(SourceLocation location, diag::ErrorCode code, std::string message) {
        diagnostics_.error(code, location, std::move(message));
        failed_ = true;
    }

    Section& current() { return module_.sectionFor(current_kind_); }

    void emit(Item item) { current().items.push_back(std::move(item)); }

    /// Rejects anything that would put initialised bytes in .bss, which by
    /// definition occupies no file space.
    bool requireInitialisableSection(SourceLocation location, std::string_view what) {
        if (current_kind_ == SectionKind::Bss) {
            error(location, diag::ErrorCode::InvalidSectionReference,
                  std::format("{} cannot appear in .bss, which holds no data", what));
            return false;
        }
        return true;
    }

    void lowerStatement(const ast::LabelNode& label) {
        emit(Label{label.name, label.local, label.location});
    }

    void lowerStatement(const ast::DirectiveNode& directive) {
        switch (directive.type) {
            case ast::DirectiveType::Section: {
                const std::optional<SectionKind> kind =
                    sectionKindFromName(directive.operands.at(0).symbol);
                if (!kind.has_value()) {
                    error(directive.location, diag::ErrorCode::InvalidSectionReference,
                          std::format("unknown section '{}'", directive.operands.at(0).symbol));
                    return;
                }
                current_kind_ = *kind;
                static_cast<void>(module_.sectionFor(current_kind_));
                return;
            }
            case ast::DirectiveType::Global:
                for (const Operand& operand : directive.operands) {
                    module_.globals.push_back(operand.symbol);
                }
                return;
            case ast::DirectiveType::Extern:
                for (const Operand& operand : directive.operands) {
                    module_.externs.push_back(operand.symbol);
                }
                return;
            case ast::DirectiveType::Weak:
                for (const Operand& operand : directive.operands) {
                    module_.weaks.push_back(operand.symbol);
                }
                return;
            case ast::DirectiveType::Byte:
            case ast::DirectiveType::Word:
            case ast::DirectiveType::Dword:
            case ast::DirectiveType::Qword:
                lowerData(directive);
                return;
            case ast::DirectiveType::Asciz: {
                if (!requireInitialisableSection(directive.location, ".asciz")) {
                    return;
                }
                const std::string& text = directive.operands.at(0).text;
                Bytes bytes;
                bytes.location = directive.location;
                bytes.data.assign(text.begin(), text.end());
                bytes.data.push_back(0);  // the "z" in .asciz
                emit(std::move(bytes));
                return;
            }
            case ast::DirectiveType::Align:
                emit(Align{static_cast<u64>(directive.operands.at(0).immediate),
                           directive.location});
                return;
            case ast::DirectiveType::Space: {
                const auto size = static_cast<u64>(directive.operands.at(0).immediate);
                const u8 fill = directive.operands.size() > 1
                                    ? static_cast<u8>(directive.operands[1].immediate & 0xFF)
                                    : u8{0};
                if (fill != 0 && !requireInitialisableSection(directive.location,
                                                              ".space with a non-zero fill")) {
                    return;
                }
                emit(Space{size, fill, directive.location});
                return;
            }
        }
    }

    void lowerData(const ast::DirectiveNode& directive) {
        if (!requireInitialisableSection(directive.location,
                                         std::string{ast::directiveName(directive.type)})) {
            return;
        }
        const u32 width = ast::dataDirectiveWidth(directive.type);
        Bytes pending;
        pending.location = directive.location;
        const auto flush = [&] {
            if (!pending.data.empty()) {
                emit(std::move(pending));
                pending = Bytes{};
                pending.location = directive.location;
            }
        };

        for (const Operand& operand : directive.operands) {
            if (operand.type == OperandType::Symbol) {
                // A symbol address has to be patched at link time, so it cannot
                // share a literal byte run.
                flush();
                emit(SymbolValue{width, SymbolRef{operand.symbol, operand.immediate},
                                 operand.location});
                continue;
            }
            const auto value = static_cast<u64>(operand.immediate);
            for (u32 byte = 0; byte < width; ++byte) {
                pending.data.push_back(static_cast<u8>((value >> (8U * byte)) & 0xFFU));
            }
        }
        flush();
    }

    void lowerStatement(const ast::InstructionNode& node) {
        const isa::OpcodeInfo* info = isa::findMnemonic(node.mnemonic);
        if (info == nullptr) {
            error(node.location, diag::ErrorCode::InvalidOpcode,
                  std::format("unknown instruction '{}'", node.mnemonic));
            return;
        }
        if (current_kind_ != SectionKind::Text) {
            error(node.location, diag::ErrorCode::InvalidSectionReference,
                  std::format("instructions may only appear in .text, not in {}",
                              sectionName(current_kind_)));
            return;
        }

        Instruction instruction;
        instruction.location = node.location;
        instruction.machine.opcode = info->opcode;
        const std::vector<Operand>& operands = node.operands;

        switch (info->format) {
            case isa::Format::None:
                break;
            case isa::Format::Reg1:
                instruction.machine.dst = operands.at(0).reg;
                break;
            case isa::Format::Reg2:
                instruction.machine.dst = operands.at(0).reg;
                instruction.machine.src = operands.at(1).reg;
                break;
            case isa::Format::RegImm:
                instruction.machine.dst = operands.at(0).reg;
                setValue(instruction, operands.at(1));
                break;
            case isa::Format::Mem: {
                const bool store = info->opcode == isa::Opcode::STORE;
                const Operand& memory = store ? operands.at(0) : operands.at(1);
                const Operand& reg = store ? operands.at(1) : operands.at(0);
                // In both directions dst is the register named first in the
                // encoding: for LOAD the destination register, for STORE the
                // base register of the memory operand.
                instruction.machine.dst = store ? memory.reg : reg.reg;
                instruction.machine.src = store ? reg.reg : memory.reg;
                instruction.machine.imm = memory.immediate;
                break;
            }
            case isa::Format::Jump:
                setValue(instruction, operands.at(0));
                break;
            case isa::Format::SysImm:
                instruction.machine.imm = operands.at(0).immediate;
                break;
        }
        emit(std::move(instruction));
    }

    /// Fills in either a literal immediate or a symbol reference.
    static void setValue(Instruction& instruction, const Operand& operand) {
        if (operand.type == OperandType::Symbol) {
            instruction.symbol = SymbolRef{operand.symbol, operand.immediate};
            instruction.machine.imm = 0;
        } else {
            instruction.machine.imm = operand.immediate;
        }
    }

    diag::DiagnosticEngine& diagnostics_;
    Module module_;
    SectionKind current_kind_ = SectionKind::Text;
    bool failed_ = false;
};

}  // namespace

std::expected<Module, std::string> lower(const ast::Program& program, std::string source_name,
                                         diag::DiagnosticEngine& diagnostics) {
    Lowerer lowerer(std::move(source_name), diagnostics);
    return lowerer.run(program);
}

}  // namespace minitool::ir
