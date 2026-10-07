// SPDX-License-Identifier: MIT
#pragma once

#include <expected>

#include "minitool/common/types.hpp"
#include "minitool/isa/opcode.hpp"

/// The arithmetic and logic semantics of the ISA, as pure functions.
///
/// There is exactly one implementation of "what does ADD do", and both the
/// virtual CPU and the optimizer's constant folder call it. That is deliberate:
/// a constant folder that computes `2 + 2` differently from the machine that
/// runs the un-folded code is the classic way an optimizing toolchain becomes
/// quietly wrong, and sharing the function makes the two impossible to
/// separate. docs/isa.md §5 specifies the flag rules implemented here.
namespace minitool::isa {

/// FLAGS bit positions.
inline constexpr u64 kZeroFlag = u64{1} << 0;
inline constexpr u64 kSignFlag = u64{1} << 1;
inline constexpr u64 kCarryFlag = u64{1} << 2;
inline constexpr u64 kOverflowFlag = u64{1} << 3;
/// Every bit outside this mask reads as zero.
inline constexpr u64 kFlagsMask = kZeroFlag | kSignFlag | kCarryFlag | kOverflowFlag;

enum class AluError : u8 {
    /// DIV or MOD with a zero divisor.
    DivisionByZero,
    /// The opcode is not an ALU operation.
    NotArithmetic,
};

struct AluResult {
    /// The value written to the destination register. Ignored for CMP and TEST,
    /// which only produce flags.
    u64 value = 0;
    u64 flags = 0;
    /// False for CMP and TEST, which leave their operands alone.
    bool writes_value = true;
    /// False for NOT, which leaves FLAGS alone.
    bool writes_flags = true;
};

/// ZF/SF from `result`, plus CF/OF for an addition (`is_subtraction` false) or a
/// subtraction of `b` from `a`.
[[nodiscard]] u64 computeFlags(u64 result, u64 a, u64 b, bool is_subtraction) noexcept;

/// Evaluates a two-operand ALU opcode (ADD, SUB, MUL, DIV, MOD, AND, OR, XOR,
/// SHL, SHR, SAR, CMP, TEST) with `a` as the destination value and `b` as the
/// source value.
[[nodiscard]] std::expected<AluResult, AluError> evaluateBinary(Opcode opcode, u64 a,
                                                                u64 b) noexcept;

/// Evaluates a one-operand ALU opcode (INC, DEC, NEG, NOT) on `a`.
[[nodiscard]] std::expected<AluResult, AluError> evaluateUnary(Opcode opcode, u64 a) noexcept;

/// True if the opcode is handled by evaluateBinary.
[[nodiscard]] bool isBinaryAlu(Opcode opcode) noexcept;
/// True if the opcode is handled by evaluateUnary.
[[nodiscard]] bool isUnaryAlu(Opcode opcode) noexcept;

/// True if `opcode` branches on the state of FLAGS (the conditional jumps).
[[nodiscard]] bool readsFlags(Opcode opcode) noexcept;

/// Evaluates a conditional branch's predicate against `flags`. Returns true for
/// JMP and CALL, which are unconditional, and false for anything that is not a
/// branch.
[[nodiscard]] bool branchTaken(Opcode opcode, u64 flags) noexcept;

}  // namespace minitool::isa
