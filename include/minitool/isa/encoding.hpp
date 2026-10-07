// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "minitool/common/types.hpp"
#include "minitool/isa/instruction.hpp"

/// Instruction encoding. This is the single canonical implementation shared by
/// the assembler, the disassembler and the VM (architectural rule 9).
///
///   63        56 55    52 51    48 47                                       0
///  +------------+--------+--------+-------------------------------------------+
///  |   opcode   |  dst   |  src   |            immediate / displacement        |
///  +------------+--------+--------+-------------------------------------------+
///
/// Every instruction is exactly 8 bytes and is stored little-endian in files
/// and in VM memory.
namespace minitool::isa {

inline constexpr unsigned kInstructionSize = 8;
inline constexpr unsigned kOpcodeShift = 56;
inline constexpr unsigned kDstShift = 52;
inline constexpr unsigned kSrcShift = 48;
inline constexpr unsigned kImmediateBits = 48;
inline constexpr u64 kImmediateMask = (u64{1} << kImmediateBits) - 1U;
inline constexpr u64 kRegisterMask = 0xFU;

enum class EncodeError : u8 {
    UnknownOpcode,
    InvalidRegister,
    ImmediateOutOfRange,
    /// A field unused by this instruction's format was non-zero.
    NonCanonicalOperand,
    /// The destination buffer is smaller than kInstructionSize.
    BufferTooSmall,
};

enum class DecodeError : u8 {
    UnknownOpcode,
    /// A register field must be zero for this format but was not.
    ReservedRegisterFieldSet,
    /// The immediate field must be zero for this format but was not.
    ReservedImmediateFieldSet,
    /// Fewer than kInstructionSize bytes were available.
    Truncated,
};

[[nodiscard]] std::string_view encodeErrorName(EncodeError error) noexcept;
[[nodiscard]] std::string_view decodeErrorName(DecodeError error) noexcept;

/// Encodes one instruction into its 64-bit word.
[[nodiscard]] std::expected<u64, EncodeError> encode(const Instruction& instruction) noexcept;

/// Decodes one 64-bit word. Every rejected word produces an error; no input
/// bit pattern can cause undefined behaviour.
[[nodiscard]] std::expected<Instruction, DecodeError> decode(u64 word) noexcept;

/// Encodes into an 8-byte little-endian buffer.
[[nodiscard]] std::expected<void, EncodeError> encodeInto(std::span<u8> out,
                                                          const Instruction& instruction) noexcept;

/// Decodes from a little-endian byte buffer of at least kInstructionSize bytes.
[[nodiscard]] std::expected<Instruction, DecodeError> decodeFrom(std::span<const u8> in) noexcept;

/// Branch semantics are frozen here so that the assembler, linker, disassembler
/// and VM cannot disagree: a displacement is relative to the address of the
/// instruction *following* the branch.
[[nodiscard]] constexpr Address branchTarget(Address instruction_address,
                                             i64 displacement) noexcept {
    return instruction_address + kInstructionSize + static_cast<u64>(displacement);
}

/// Inverse of branchTarget(). The caller must range-check the result before
/// encoding it (see byteorder::fitsSigned).
[[nodiscard]] constexpr i64 branchDisplacement(Address instruction_address,
                                               Address target) noexcept {
    return static_cast<i64>(target - (instruction_address + kInstructionSize));
}

/// Human-readable rendering of one instruction, used by the disassembler and by
/// test failure messages, e.g. "LOAD R1, [R2 + 8]".
[[nodiscard]] std::string toString(const Instruction& instruction);

}  // namespace minitool::isa
