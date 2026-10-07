// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "minitool/common/types.hpp"

namespace minitool::isa {

/// Opcode numbers are frozen: they are the on-disk contract of every .mobj and
/// .mexe file ever produced. Append new opcodes in a free slot; never renumber.
enum class Opcode : u8 {
    // System (0x00-0x0F)
    NOP = 0x00,
    HALT = 0x01,
    SYSCALL = 0x02,

    // Data movement (0x10-0x1F)
    MOV = 0x10,
    MOVI = 0x11,
    LOAD = 0x12,
    STORE = 0x13,
    LEA = 0x14,
    PUSH = 0x15,
    POP = 0x16,

    // Arithmetic (0x20-0x2F)
    ADD = 0x20,
    SUB = 0x21,
    MUL = 0x22,
    DIV = 0x23,
    MOD = 0x24,
    INC = 0x25,
    DEC = 0x26,
    NEG = 0x27,

    // Logical (0x30-0x3F)
    AND = 0x30,
    OR = 0x31,
    XOR = 0x32,
    NOT = 0x33,
    SHL = 0x34,
    SHR = 0x35,
    SAR = 0x36,

    // Comparison (0x40-0x4F)
    CMP = 0x40,
    TEST = 0x41,

    // Control flow (0x50-0x5F)
    JMP = 0x50,
    JE = 0x51,
    JNE = 0x52,
    JG = 0x53,
    JL = 0x54,
    JGE = 0x55,
    JLE = 0x56,
    CALL = 0x57,
    RET = 0x58,
};

/// How the low 56 bits of an instruction word are interpreted.
enum class Format : u8 {
    /// No operands. dst, src and imm fields must be zero.
    None,
    /// One register operand, carried in the dst field.
    Reg1,
    /// Two register operands: dst and src.
    Reg2,
    /// One register and a 48-bit signed immediate.
    RegImm,
    /// Two registers and a 48-bit signed displacement: [base + disp].
    Mem,
    /// A 48-bit signed PC-relative displacement.
    Jump,
    /// A 48-bit unsigned immediate with no register operands.
    SysImm,
};

[[nodiscard]] std::string_view formatName(Format format) noexcept;

/// Static metadata for one opcode. `mnemonic` is the canonical uppercase
/// spelling emitted by the disassembler.
struct OpcodeInfo {
    Opcode opcode{};
    std::string_view mnemonic;
    Format format{};
    /// The instruction writes its dst register.
    bool writes_dst = false;
    /// The instruction reads its dst register (e.g. ADD dst, src).
    bool reads_dst = false;
    /// The instruction updates FLAGS.
    bool writes_flags = false;
    /// Control leaves the following instruction unconditionally.
    bool is_terminator = false;
    /// The instruction transfers control (conditionally or not).
    bool is_branch = false;
};

/// Every opcode, in ascending numeric order.
[[nodiscard]] std::span<const OpcodeInfo> allOpcodes() noexcept;

/// Metadata lookup. Returns nullptr if `opcode` is not a defined opcode.
[[nodiscard]] const OpcodeInfo* findOpcode(Opcode opcode) noexcept;

/// Mnemonic lookup, case-insensitive. Returns nullptr if unknown.
[[nodiscard]] const OpcodeInfo* findMnemonic(std::string_view mnemonic) noexcept;

[[nodiscard]] inline bool isValidOpcode(Opcode opcode) noexcept {
    return findOpcode(opcode) != nullptr;
}

/// Canonical mnemonic, or "???" for an unknown opcode.
[[nodiscard]] std::string_view opcodeName(Opcode opcode) noexcept;

/// Number of explicit operands implied by the format.
[[nodiscard]] constexpr unsigned operandCount(Format format) noexcept {
    switch (format) {
        case Format::None:
            return 0;
        case Format::Reg1:
        case Format::Jump:
        case Format::SysImm:
            return 1;
        case Format::Reg2:
        case Format::RegImm:
            return 2;
        case Format::Mem:
            return 2;  // register + memory operand [base + disp]
    }
    return 0;
}

}  // namespace minitool::isa
