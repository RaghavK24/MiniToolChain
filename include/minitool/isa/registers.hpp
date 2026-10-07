// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string_view>

#include "minitool/common/types.hpp"

namespace minitool::isa {

/// The 16 general-purpose 64-bit registers. The numeric value of each enumerator
/// IS its 4-bit encoding field; this mapping is frozen (docs/isa.md).
enum class Reg : u8 {
    R0 = 0,
    R1 = 1,
    R2 = 2,
    R3 = 3,
    R4 = 4,
    R5 = 5,
    R6 = 6,
    R7 = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
};

inline constexpr unsigned kRegisterCount = 16;
inline constexpr unsigned kRegisterFieldBits = 4;

/// ABI roles (see docs/isa.md §Calling convention).
inline constexpr Reg kFramePointer = Reg::R13;
inline constexpr Reg kReturnValue = Reg::R14;
inline constexpr Reg kReserved = Reg::R15;

[[nodiscard]] constexpr unsigned registerIndex(Reg reg) noexcept {
    return static_cast<unsigned>(reg);
}

/// True if `reg` holds one of the 16 architectural register numbers. A Reg can
/// only be out of range if it was produced by a cast, but encoding validates it
/// anyway rather than truncating silently.
[[nodiscard]] constexpr bool isValidRegister(Reg reg) noexcept {
    return registerIndex(reg) < kRegisterCount;
}

/// Canonical spelling, e.g. "R13". Returns "R?" for an out-of-range value.
[[nodiscard]] std::string_view registerName(Reg reg) noexcept;

/// Parses "R0".."R15" (case-insensitive) and the ABI aliases "FP" (R13) and
/// "RV" (R14). Rejects leading zeros ("R01") and out-of-range numbers ("R19").
[[nodiscard]] std::optional<Reg> parseRegister(std::string_view text) noexcept;

}  // namespace minitool::isa
