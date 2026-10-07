// SPDX-License-Identifier: MIT
#include "minitool/isa/encoding.hpp"

#include <format>

#include "minitool/common/byte_order.hpp"

namespace minitool::isa {
namespace {

/// Per-format description of which fields carry meaning.
struct FieldUse {
    bool uses_dst = false;
    bool uses_src = false;
    bool uses_imm = false;
    bool imm_signed = true;
};

[[nodiscard]] constexpr FieldUse fieldsOf(Format format) noexcept {
    switch (format) {
        case Format::None:
            return {false, false, false, true};
        case Format::Reg1:
            return {true, false, false, true};
        case Format::Reg2:
            return {true, true, false, true};
        case Format::RegImm:
            return {true, false, true, true};
        case Format::Mem:
            return {true, true, true, true};
        case Format::Jump:
            return {false, false, true, true};
        case Format::SysImm:
            return {false, false, true, false};
    }
    return {};
}

}  // namespace

std::string_view encodeErrorName(EncodeError error) noexcept {
    switch (error) {
        case EncodeError::UnknownOpcode:
            return "unknown opcode";
        case EncodeError::InvalidRegister:
            return "invalid register";
        case EncodeError::ImmediateOutOfRange:
            return "immediate out of range";
        case EncodeError::NonCanonicalOperand:
            return "non-canonical operand field";
        case EncodeError::BufferTooSmall:
            return "buffer too small";
    }
    return "unknown encode error";
}

std::string_view decodeErrorName(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::UnknownOpcode:
            return "unknown opcode";
        case DecodeError::ReservedRegisterFieldSet:
            return "reserved register field set";
        case DecodeError::ReservedImmediateFieldSet:
            return "reserved immediate field set";
        case DecodeError::Truncated:
            return "truncated instruction";
    }
    return "unknown decode error";
}

std::expected<u64, EncodeError> encode(const Instruction& instruction) noexcept {
    const OpcodeInfo* info = findOpcode(instruction.opcode);
    if (info == nullptr) {
        return std::unexpected(EncodeError::UnknownOpcode);
    }
    const FieldUse fields = fieldsOf(info->format);

    if (!isValidRegister(instruction.dst) || !isValidRegister(instruction.src)) {
        return std::unexpected(EncodeError::InvalidRegister);
    }
    if (!fields.uses_dst && instruction.dst != Reg::R0) {
        return std::unexpected(EncodeError::NonCanonicalOperand);
    }
    if (!fields.uses_src && instruction.src != Reg::R0) {
        return std::unexpected(EncodeError::NonCanonicalOperand);
    }
    if (!fields.uses_imm && instruction.imm != 0) {
        return std::unexpected(EncodeError::NonCanonicalOperand);
    }

    u64 imm_bits = 0;
    if (fields.uses_imm) {
        if (fields.imm_signed) {
            if (!byteorder::fitsSigned(instruction.imm, kImmediateBits)) {
                return std::unexpected(EncodeError::ImmediateOutOfRange);
            }
        } else {
            if (instruction.imm < 0 ||
                !byteorder::fitsUnsigned(static_cast<u64>(instruction.imm), kImmediateBits)) {
                return std::unexpected(EncodeError::ImmediateOutOfRange);
            }
        }
        imm_bits = static_cast<u64>(instruction.imm) & kImmediateMask;
    }

    const u64 word = (static_cast<u64>(instruction.opcode) << kOpcodeShift) |
                     (static_cast<u64>(registerIndex(instruction.dst)) << kDstShift) |
                     (static_cast<u64>(registerIndex(instruction.src)) << kSrcShift) | imm_bits;
    return word;
}

std::expected<Instruction, DecodeError> decode(u64 word) noexcept {
    const auto opcode = static_cast<Opcode>(static_cast<u8>((word >> kOpcodeShift) & 0xFFU));
    const OpcodeInfo* info = findOpcode(opcode);
    if (info == nullptr) {
        return std::unexpected(DecodeError::UnknownOpcode);
    }
    const FieldUse fields = fieldsOf(info->format);

    const u64 dst_bits = (word >> kDstShift) & kRegisterMask;
    const u64 src_bits = (word >> kSrcShift) & kRegisterMask;
    const u64 imm_bits = word & kImmediateMask;

    if (!fields.uses_dst && dst_bits != 0) {
        return std::unexpected(DecodeError::ReservedRegisterFieldSet);
    }
    if (!fields.uses_src && src_bits != 0) {
        return std::unexpected(DecodeError::ReservedRegisterFieldSet);
    }
    if (!fields.uses_imm && imm_bits != 0) {
        return std::unexpected(DecodeError::ReservedImmediateFieldSet);
    }

    Instruction instruction;
    instruction.opcode = opcode;
    instruction.dst = static_cast<Reg>(static_cast<u8>(dst_bits));
    instruction.src = static_cast<Reg>(static_cast<u8>(src_bits));
    if (fields.uses_imm) {
        instruction.imm = fields.imm_signed ? byteorder::signExtend(imm_bits, kImmediateBits)
                                            : static_cast<i64>(imm_bits);
    }
    return instruction;
}

std::expected<void, EncodeError> encodeInto(std::span<u8> out,
                                            const Instruction& instruction) noexcept {
    if (out.size() < kInstructionSize) {
        return std::unexpected(EncodeError::BufferTooSmall);
    }
    const std::expected<u64, EncodeError> word = encode(instruction);
    if (!word.has_value()) {
        return std::unexpected(word.error());
    }
    byteorder::store<u64>(out, *word);
    return {};
}

std::expected<Instruction, DecodeError> decodeFrom(std::span<const u8> in) noexcept {
    if (in.size() < kInstructionSize) {
        return std::unexpected(DecodeError::Truncated);
    }
    return decode(byteorder::load<u64>(in));
}

std::string toString(const Instruction& instruction) {
    const OpcodeInfo* info = findOpcode(instruction.opcode);
    if (info == nullptr) {
        return std::format(".word 0x{:02X}...", static_cast<unsigned>(instruction.opcode));
    }
    const std::string_view name = info->mnemonic;
    switch (info->format) {
        case Format::None:
            return std::string{name};
        case Format::Reg1:
            return std::format("{} {}", name, registerName(instruction.dst));
        case Format::Reg2:
            return std::format("{} {}, {}", name, registerName(instruction.dst),
                               registerName(instruction.src));
        case Format::RegImm:
            return std::format("{} {}, {}", name, registerName(instruction.dst), instruction.imm);
        case Format::Mem: {
            const char sign = instruction.imm < 0 ? '-' : '+';
            // Negate through u64 so INT64_MIN cannot overflow.
            const u64 magnitude = instruction.imm < 0 ? (~static_cast<u64>(instruction.imm) + 1U)
                                                      : static_cast<u64>(instruction.imm);
            if (instruction.opcode == Opcode::STORE) {
                return std::format("{} [{} {} {}], {}", name, registerName(instruction.dst), sign,
                                   magnitude, registerName(instruction.src));
            }
            return std::format("{} {}, [{} {} {}]", name, registerName(instruction.dst),
                               registerName(instruction.src), sign, magnitude);
        }
        case Format::Jump:
            return std::format("{} .{}{}", name, instruction.imm < 0 ? "" : "+", instruction.imm);
        case Format::SysImm:
            return std::format("{} {}", name, instruction.imm);
    }
    return std::string{name};
}

}  // namespace minitool::isa
