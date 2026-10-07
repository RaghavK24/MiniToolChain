// SPDX-License-Identifier: MIT
#include "minitool/isa/opcode.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace minitool::isa {
namespace {

// Fields: opcode, mnemonic, format, writes_dst, reads_dst, writes_flags,
//         is_terminator, is_branch
constexpr std::array<OpcodeInfo, 36> kOpcodes{{
    {Opcode::NOP, "NOP", Format::None, false, false, false, false, false},
    {Opcode::HALT, "HALT", Format::None, false, false, false, true, false},
    {Opcode::SYSCALL, "SYSCALL", Format::SysImm, false, false, false, false, false},

    {Opcode::MOV, "MOV", Format::Reg2, true, false, false, false, false},
    {Opcode::MOVI, "MOVI", Format::RegImm, true, false, false, false, false},
    {Opcode::LOAD, "LOAD", Format::Mem, true, false, false, false, false},
    {Opcode::STORE, "STORE", Format::Mem, false, true, false, false, false},
    {Opcode::LEA, "LEA", Format::RegImm, true, false, false, false, false},
    {Opcode::PUSH, "PUSH", Format::Reg1, false, true, false, false, false},
    {Opcode::POP, "POP", Format::Reg1, true, false, false, false, false},

    {Opcode::ADD, "ADD", Format::Reg2, true, true, true, false, false},
    {Opcode::SUB, "SUB", Format::Reg2, true, true, true, false, false},
    {Opcode::MUL, "MUL", Format::Reg2, true, true, true, false, false},
    {Opcode::DIV, "DIV", Format::Reg2, true, true, true, false, false},
    {Opcode::MOD, "MOD", Format::Reg2, true, true, true, false, false},
    {Opcode::INC, "INC", Format::Reg1, true, true, true, false, false},
    {Opcode::DEC, "DEC", Format::Reg1, true, true, true, false, false},
    {Opcode::NEG, "NEG", Format::Reg1, true, true, true, false, false},

    {Opcode::AND, "AND", Format::Reg2, true, true, true, false, false},
    {Opcode::OR, "OR", Format::Reg2, true, true, true, false, false},
    {Opcode::XOR, "XOR", Format::Reg2, true, true, true, false, false},
    {Opcode::NOT, "NOT", Format::Reg1, true, true, false, false, false},
    {Opcode::SHL, "SHL", Format::Reg2, true, true, true, false, false},
    {Opcode::SHR, "SHR", Format::Reg2, true, true, true, false, false},
    {Opcode::SAR, "SAR", Format::Reg2, true, true, true, false, false},

    {Opcode::CMP, "CMP", Format::Reg2, false, true, true, false, false},
    {Opcode::TEST, "TEST", Format::Reg2, false, true, true, false, false},

    {Opcode::JMP, "JMP", Format::Jump, false, false, false, true, true},
    {Opcode::JE, "JE", Format::Jump, false, false, false, false, true},
    {Opcode::JNE, "JNE", Format::Jump, false, false, false, false, true},
    {Opcode::JG, "JG", Format::Jump, false, false, false, false, true},
    {Opcode::JL, "JL", Format::Jump, false, false, false, false, true},
    {Opcode::JGE, "JGE", Format::Jump, false, false, false, false, true},
    {Opcode::JLE, "JLE", Format::Jump, false, false, false, false, true},
    {Opcode::CALL, "CALL", Format::Jump, false, false, false, false, true},
    {Opcode::RET, "RET", Format::None, false, false, false, true, true},
}};

[[nodiscard]] char upper(char c) noexcept {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

}  // namespace

std::string_view formatName(Format format) noexcept {
    switch (format) {
        case Format::None:
            return "none";
        case Format::Reg1:
            return "reg1";
        case Format::Reg2:
            return "reg2";
        case Format::RegImm:
            return "regimm";
        case Format::Mem:
            return "mem";
        case Format::Jump:
            return "jump";
        case Format::SysImm:
            return "sysimm";
    }
    return "unknown";
}

std::span<const OpcodeInfo> allOpcodes() noexcept {
    return kOpcodes;
}

const OpcodeInfo* findOpcode(Opcode opcode) noexcept {
    for (const OpcodeInfo& info : kOpcodes) {
        if (info.opcode == opcode) {
            return &info;
        }
    }
    return nullptr;
}

const OpcodeInfo* findMnemonic(std::string_view mnemonic) noexcept {
    for (const OpcodeInfo& info : kOpcodes) {
        if (info.mnemonic.size() != mnemonic.size()) {
            continue;
        }
        const bool equal = std::equal(mnemonic.begin(), mnemonic.end(), info.mnemonic.begin(),
                                      [](char a, char b) { return upper(a) == b; });
        if (equal) {
            return &info;
        }
    }
    return nullptr;
}

std::string_view opcodeName(Opcode opcode) noexcept {
    const OpcodeInfo* info = findOpcode(opcode);
    return info != nullptr ? info->mnemonic : std::string_view{"???"};
}

}  // namespace minitool::isa
