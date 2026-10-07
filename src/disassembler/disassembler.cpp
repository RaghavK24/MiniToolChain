// SPDX-License-Identifier: MIT
#include "minitool/disassembler/disassembler.hpp"

#include <format>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool::disassembler {
namespace {

/// Renders `address` as a symbol reference when one starts exactly there,
/// otherwise as a bare address.
[[nodiscard]] std::string describeTarget(u64 address, const executable::Executable* executable) {
    if (executable != nullptr) {
        for (const executable::SymbolEntry& symbol : executable->symbols) {
            if (symbol.address == address) {
                return std::format("{} (0x{:X})", symbol.name, address);
            }
        }
        const executable::SymbolEntry* containing = executable->symbolContaining(address);
        if (containing != nullptr && containing->address != address) {
            return std::format("{}+{} (0x{:X})", containing->name, address - containing->address,
                               address);
        }
    }
    return std::format("0x{:X}", address);
}

}  // namespace

Disassembler::Disassembler(Options options) noexcept : options_(options) {}

std::string Disassembler::formatInstruction(u64 address, const isa::Instruction& instruction,
                                            const executable::Executable* executable) const {
    std::string text = isa::toString(instruction);
    const isa::OpcodeInfo* info = isa::findOpcode(instruction.opcode);
    if (info != nullptr && info->format == isa::Format::Jump) {
        // A raw displacement is close to useless to a reader; show where the
        // branch actually goes, using the ISA's own target rule.
        const u64 target = isa::branchTarget(address, instruction.imm);
        text = std::format("{:<8}{}", info->mnemonic,
                           options_.show_symbols ? describeTarget(target, executable)
                                                 : std::format("0x{:X}", target));
    }
    return text;
}

std::string Disassembler::disassemble(std::span<const u8> code, u64 base_address,
                                      const executable::Executable* executable) const {
    std::string out;
    for (std::size_t offset = 0; offset + isa::kInstructionSize <= code.size();
         offset += isa::kInstructionSize) {
        const u64 address = base_address + offset;

        if (options_.show_labels && executable != nullptr) {
            for (const executable::SymbolEntry& symbol : executable->symbols) {
                if (symbol.address == address) {
                    out += std::format("\n{:016X} <{}>:\n", address, symbol.name);
                }
            }
        }

        const std::span<const u8> word_bytes = code.subspan(offset, isa::kInstructionSize);
        const u64 word = byteorder::load<u64>(word_bytes);
        const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decode(word);

        out += std::format("{:016X}:  ", address);
        if (options_.show_bytes) {
            out += std::format("{:016X}  ", word);
        }
        if (decoded.has_value()) {
            out += formatInstruction(address, *decoded, executable);
        } else {
            // Not an instruction: show it as data with the reason, rather than
            // inventing a mnemonic for it.
            out += std::format(".qword 0x{:016X}    ; {}", word,
                               isa::decodeErrorName(decoded.error()));
        }
        out += '\n';
    }

    const std::size_t tail = code.size() % isa::kInstructionSize;
    if (tail != 0) {
        out += std::format("{:016X}:  ; {} trailing byte(s) do not form an instruction\n",
                           base_address + code.size() - tail, tail);
    }
    return out;
}

std::string Disassembler::disassemble(const executable::Executable& executable) const {
    std::string out;
    for (const executable::Segment& segment : executable.segments) {
        if (!executable::hasFlag(segment.flags, executable::SegmentFlags::Exec)) {
            continue;
        }
        out += std::format("segment {} at 0x{:X} ({} bytes, {})\n", segment.name,
                           segment.virtual_address, segment.virtual_size,
                           executable::flagsToString(segment.flags));
        out += disassemble(segment.data, segment.virtual_address, &executable);
    }
    if (out.empty()) {
        out = "this executable contains no executable segments\n";
    }
    return out;
}

}  // namespace minitool::disassembler
