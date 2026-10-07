// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <span>
#include <string>

#include "minitool/common/types.hpp"
#include "minitool/object/object.hpp"

namespace minitool::object {

/// Why a relocation could not be applied. Overflow is a first-class outcome:
/// the engine never truncates a value to make it fit.
enum class RelocationError : u8 {
    OutOfBounds,
    Overflow,
    UnalignedInstructionField,
    NotAnInstruction,
};

[[nodiscard]] std::string_view relocationErrorName(RelocationError error) noexcept;

/// The value a relocation wants to store, before range checking.
///
///   ABS32/ABS64/IMM48   S + A
///   PCREL32             S + A - P
///   PCREL48             S + A - (P + 8)     (isa::branchDisplacement)
///
/// `place` is the run-time address of the patched field; for the instruction
/// field types it is the address of the instruction itself.
[[nodiscard]] i64 relocatedValue(RelocationType type, u64 symbol_address, i64 addend,
                                 u64 place) noexcept;

/// Applies one relocation to `section_data`, whose byte 0 lives at
/// `section_address`. Validates bounds and range before writing anything, so a
/// failed relocation leaves the buffer untouched.
[[nodiscard]] std::expected<void, RelocationError> applyRelocation(std::span<u8> section_data,
                                                                   u64 section_address,
                                                                   const Relocation& relocation,
                                                                   u64 symbol_address);

}  // namespace minitool::object
