// SPDX-License-Identifier: MIT
#include "minitool/isa/semantics.hpp"

namespace minitool::isa {
namespace {

[[nodiscard]] u64 zeroAndSign(u64 result) noexcept {
    u64 flags = 0;
    if (result == 0) {
        flags |= kZeroFlag;
    }
    if ((result >> 63U) != 0U) {
        flags |= kSignFlag;
    }
    return flags;
}

/// Logical and shift operations define CF and OF explicitly rather than leaving
/// them alone, so that FLAGS is a pure function of the instruction and its
/// inputs (docs/isa.md §5).
[[nodiscard]] AluResult logical(u64 result) noexcept {
    return AluResult{result, zeroAndSign(result), true, true};
}

}  // namespace

u64 computeFlags(u64 result, u64 a, u64 b, bool is_subtraction) noexcept {
    u64 flags = zeroAndSign(result);
    if (is_subtraction) {
        if (a < b) {
            flags |= kCarryFlag;  // borrow
        }
        // Signed overflow: the operands differ in sign and the result takes the
        // sign of the subtrahend.
        if ((((a ^ b) & (a ^ result)) >> 63U) != 0U) {
            flags |= kOverflowFlag;
        }
    } else {
        if (result < a) {
            flags |= kCarryFlag;  // unsigned wrap
        }
        // Signed overflow: equal operand signs, different result sign.
        if (((~(a ^ b) & (a ^ result)) >> 63U) != 0U) {
            flags |= kOverflowFlag;
        }
    }
    return flags;
}

bool isBinaryAlu(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::ADD:
        case Opcode::SUB:
        case Opcode::MUL:
        case Opcode::DIV:
        case Opcode::MOD:
        case Opcode::AND:
        case Opcode::OR:
        case Opcode::XOR:
        case Opcode::SHL:
        case Opcode::SHR:
        case Opcode::SAR:
        case Opcode::CMP:
        case Opcode::TEST:
            return true;
        default:
            return false;
    }
}

bool isUnaryAlu(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::INC:
        case Opcode::DEC:
        case Opcode::NEG:
        case Opcode::NOT:
            return true;
        default:
            return false;
    }
}

std::expected<AluResult, AluError> evaluateBinary(Opcode opcode, u64 a, u64 b) noexcept {
    switch (opcode) {
        case Opcode::ADD: {
            const u64 result = a + b;
            return AluResult{result, computeFlags(result, a, b, false), true, true};
        }
        case Opcode::SUB: {
            const u64 result = a - b;
            return AluResult{result, computeFlags(result, a, b, true), true, true};
        }
        case Opcode::MUL: {
            const u64 result = a * b;
            u64 flags = zeroAndSign(result);
            // CF reports that the true unsigned product needed more than 64
            // bits; OF is always clear for MUL (docs/isa.md §5).
            if (a != 0 && result / a != b) {
                flags |= kCarryFlag;
            }
            return AluResult{result, flags, true, true};
        }
        case Opcode::DIV: {
            if (b == 0) {
                return std::unexpected(AluError::DivisionByZero);
            }
            const auto lhs = static_cast<i64>(a);
            const auto rhs = static_cast<i64>(b);
            // INT64_MIN / -1 overflows in C++; the ISA defines it to wrap to
            // INT64_MIN, which is what the two's-complement hardware result is.
            const u64 result = (rhs == -1) ? (~a + 1U) : static_cast<u64>(lhs / rhs);
            return AluResult{result, zeroAndSign(result), true, true};
        }
        case Opcode::MOD: {
            if (b == 0) {
                return std::unexpected(AluError::DivisionByZero);
            }
            const auto lhs = static_cast<i64>(a);
            const auto rhs = static_cast<i64>(b);
            const u64 result = (rhs == -1) ? 0U : static_cast<u64>(lhs % rhs);
            return AluResult{result, zeroAndSign(result), true, true};
        }
        case Opcode::AND:
            return logical(a & b);
        case Opcode::OR:
            return logical(a | b);
        case Opcode::XOR:
            return logical(a ^ b);
        case Opcode::SHL: {
            const unsigned count = static_cast<unsigned>(b & 63U);
            const u64 result = count == 0 ? a : (a << count);
            u64 flags = zeroAndSign(result);
            if (count != 0 && ((a >> (64U - count)) & 1U) != 0U) {
                flags |= kCarryFlag;  // last bit shifted out
            }
            return AluResult{result, flags, true, true};
        }
        case Opcode::SHR: {
            const unsigned count = static_cast<unsigned>(b & 63U);
            const u64 result = count == 0 ? a : (a >> count);
            u64 flags = zeroAndSign(result);
            if (count != 0 && ((a >> (count - 1U)) & 1U) != 0U) {
                flags |= kCarryFlag;
            }
            return AluResult{result, flags, true, true};
        }
        case Opcode::SAR: {
            const unsigned count = static_cast<unsigned>(b & 63U);
            u64 result = a;
            if (count != 0) {
                // Arithmetic shift built from unsigned operations: the standard
                // leaves signed right shift of a negative value to the
                // implementation before C++20 and we do not rely on it here.
                const u64 sign = (a >> 63U) != 0U ? (~u64{0} << (64U - count)) : 0U;
                result = (a >> count) | sign;
            }
            u64 flags = zeroAndSign(result);
            if (count != 0 && ((a >> (count - 1U)) & 1U) != 0U) {
                flags |= kCarryFlag;
            }
            return AluResult{result, flags, true, true};
        }
        case Opcode::CMP: {
            const u64 result = a - b;
            return AluResult{a, computeFlags(result, a, b, true), false, true};
        }
        case Opcode::TEST: {
            const u64 result = a & b;
            return AluResult{a, zeroAndSign(result), false, true};
        }
        default:
            return std::unexpected(AluError::NotArithmetic);
    }
}

std::expected<AluResult, AluError> evaluateUnary(Opcode opcode, u64 a) noexcept {
    switch (opcode) {
        case Opcode::INC: {
            const u64 result = a + 1U;
            return AluResult{result, computeFlags(result, a, 1, false), true, true};
        }
        case Opcode::DEC: {
            const u64 result = a - 1U;
            return AluResult{result, computeFlags(result, a, 1, true), true, true};
        }
        case Opcode::NEG: {
            const u64 result = ~a + 1U;
            return AluResult{result, computeFlags(result, 0, a, true), true, true};
        }
        case Opcode::NOT:
            // The only ALU instruction that leaves FLAGS alone.
            return AluResult{~a, 0, true, false};
        default:
            return std::unexpected(AluError::NotArithmetic);
    }
}

bool readsFlags(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::JE:
        case Opcode::JNE:
        case Opcode::JG:
        case Opcode::JL:
        case Opcode::JGE:
        case Opcode::JLE:
            return true;
        default:
            return false;
    }
}

bool branchTaken(Opcode opcode, u64 flags) noexcept {
    const bool zero = (flags & kZeroFlag) != 0U;
    const bool sign = (flags & kSignFlag) != 0U;
    const bool overflow = (flags & kOverflowFlag) != 0U;
    switch (opcode) {
        case Opcode::JMP:
        case Opcode::CALL:
            return true;
        case Opcode::JE:
            return zero;
        case Opcode::JNE:
            return !zero;
        case Opcode::JG:
            return !zero && (sign == overflow);
        case Opcode::JL:
            return sign != overflow;
        case Opcode::JGE:
            return sign == overflow;
        case Opcode::JLE:
            return zero || (sign != overflow);
        default:
            return false;
    }
}

}  // namespace minitool::isa
