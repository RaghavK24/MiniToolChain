// SPDX-License-Identifier: MIT
#include "minitool/object/relocation.hpp"

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"

namespace minitool::object {

std::string_view relocationErrorName(RelocationError error) noexcept {
    switch (error) {
        case RelocationError::OutOfBounds:
            return "relocation offset is outside its section";
        case RelocationError::Overflow:
            return "relocated value does not fit the field";
        case RelocationError::UnalignedInstructionField:
            return "instruction-field relocation is not 8-byte aligned";
        case RelocationError::NotAnInstruction:
            return "instruction-field relocation does not point at an instruction";
    }
    return "unknown relocation error";
}

i64 relocatedValue(RelocationType type, u64 symbol_address, i64 addend, u64 place) noexcept {
    // Every step is unsigned so that no intermediate can overflow signed
    // arithmetic; the two's-complement result is reinterpreted at the end.
    const u64 target = symbol_address + static_cast<u64>(addend);
    switch (type) {
        case RelocationType::ABS32:
        case RelocationType::ABS64:
        case RelocationType::IMM48:
            return static_cast<i64>(target);
        case RelocationType::PCREL32:
            return static_cast<i64>(target - place);
        case RelocationType::PCREL48:
            // Identical to isa::branchDisplacement(place, target); the branch
            // base is defined once, in the ISA layer, and reused here.
            return isa::branchDisplacement(place, target);
    }
    return 0;
}

std::expected<void, RelocationError> applyRelocation(std::span<u8> section_data,
                                                     u64 section_address,
                                                     const Relocation& relocation,
                                                     u64 symbol_address) {
    const u64 width = relocationWidth(relocation.type);
    if (relocation.offset > section_data.size() ||
        width > section_data.size() - relocation.offset) {
        return std::unexpected(RelocationError::OutOfBounds);
    }
    const u64 place = section_address + relocation.offset;
    const i64 value = relocatedValue(relocation.type, symbol_address, relocation.addend, place);
    const std::span<u8> field = section_data.subspan(static_cast<std::size_t>(relocation.offset),
                                                     static_cast<std::size_t>(width));

    switch (relocation.type) {
        case RelocationType::ABS32: {
            if (!byteorder::fitsUnsigned(static_cast<u64>(value), 32)) {
                return std::unexpected(RelocationError::Overflow);
            }
            byteorder::store<u32>(field, static_cast<u32>(value));
            return {};
        }
        case RelocationType::ABS64:
            byteorder::store<u64>(field, static_cast<u64>(value));
            return {};
        case RelocationType::PCREL32: {
            if (!byteorder::fitsSigned(value, 32)) {
                return std::unexpected(RelocationError::Overflow);
            }
            byteorder::store<u32>(field, static_cast<u32>(static_cast<u64>(value) & 0xFFFF'FFFFU));
            return {};
        }
        case RelocationType::IMM48:
        case RelocationType::PCREL48: {
            if ((relocation.offset % isa::kInstructionSize) != 0) {
                return std::unexpected(RelocationError::UnalignedInstructionField);
            }
            if (!byteorder::fitsSigned(value, isa::kImmediateBits)) {
                return std::unexpected(RelocationError::Overflow);
            }
            // Re-decode the instruction rather than blindly OR-ing bits in: it
            // proves the offset really names an instruction, and it keeps the
            // encoder the single authority on field layout (rule 9).
            const std::expected<isa::Instruction, isa::DecodeError> decoded =
                isa::decodeFrom(field);
            if (!decoded.has_value()) {
                return std::unexpected(RelocationError::NotAnInstruction);
            }
            isa::Instruction patched = *decoded;
            patched.imm = value;
            const std::expected<void, isa::EncodeError> encoded = isa::encodeInto(field, patched);
            if (!encoded.has_value()) {
                return std::unexpected(RelocationError::Overflow);
            }
            return {};
        }
    }
    return std::unexpected(RelocationError::OutOfBounds);
}

}  // namespace minitool::object
