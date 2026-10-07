// SPDX-License-Identifier: MIT
#pragma once

#include <optional>

#include "minitool/common/types.hpp"
#include "minitool/isa/opcode.hpp"
#include "minitool/isa/registers.hpp"

namespace minitool::isa {

/// A decoded instruction. Fields not used by the opcode's format are required
/// to be zero (`Reg::R0` / `0`); encode() rejects anything else rather than
/// silently dropping it, which is what makes decode(encode(i)) == i hold
/// exactly rather than "up to unused bits".
struct Instruction {
    Opcode opcode = Opcode::NOP;
    Reg dst = Reg::R0;
    Reg src = Reg::R0;
    /// Immediate / displacement / syscall number, already sign-extended for the
    /// signed formats and zero-extended for SysImm.
    i64 imm = 0;

    friend constexpr bool operator==(const Instruction&, const Instruction&) = default;

    static constexpr Instruction none(Opcode op) noexcept {
        return Instruction{op, Reg::R0, Reg::R0, 0};
    }

    static constexpr Instruction reg1(Opcode op, Reg reg) noexcept {
        return Instruction{op, reg, Reg::R0, 0};
    }

    static constexpr Instruction reg2(Opcode op, Reg dst, Reg src) noexcept {
        return Instruction{op, dst, src, 0};
    }

    static constexpr Instruction regImm(Opcode op, Reg dst, i64 imm) noexcept {
        return Instruction{op, dst, Reg::R0, imm};
    }

    /// LOAD dst, [base + disp]  /  STORE [dst + disp], src
    static constexpr Instruction mem(Opcode op, Reg dst, Reg base, i64 disp) noexcept {
        return Instruction{op, dst, base, disp};
    }

    static constexpr Instruction jump(Opcode op, i64 displacement) noexcept {
        return Instruction{op, Reg::R0, Reg::R0, displacement};
    }

    static constexpr Instruction syscall(u64 number) noexcept {
        return Instruction{Opcode::SYSCALL, Reg::R0, Reg::R0, static_cast<i64>(number)};
    }
};

/// The format of `opcode`, or nullopt if the opcode is not defined.
[[nodiscard]] inline std::optional<Format> formatOf(Opcode opcode) noexcept {
    const OpcodeInfo* info = findOpcode(opcode);
    if (info == nullptr) {
        return std::nullopt;
    }
    return info->format;
}

}  // namespace minitool::isa
