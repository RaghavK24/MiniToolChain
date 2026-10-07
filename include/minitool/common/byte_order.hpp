// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <span>

#include "minitool/common/types.hpp"

/// All MiniToolchain binary formats are little-endian, independently of the
/// host byte order (see docs/adr/ADR-004-endianness.md). These helpers are the
/// ONLY sanctioned way to move integers in and out of byte buffers; no code may
/// reinterpret_cast a byte pointer to a wider integer type.
namespace minitool::byteorder {

template <typename T>
concept UnsignedWord = std::same_as<T, u16> || std::same_as<T, u32> || std::same_as<T, u64>;

/// Writes `value` little-endian into the first sizeof(T) bytes of `out`.
/// Precondition: out.size() >= sizeof(T).
template <UnsignedWord T>
constexpr void store(std::span<u8> out, T value) noexcept {
    // Widen first: shifting a u16 would promote to int and reintroduce
    // signedness questions that -Wsign-conversion rightly complains about.
    const u64 wide = static_cast<u64>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[i] = static_cast<u8>((wide >> (8U * i)) & 0xFFU);
    }
}

/// Reads a little-endian T from the first sizeof(T) bytes of `in`.
/// Precondition: in.size() >= sizeof(T).
template <UnsignedWord T>
[[nodiscard]] constexpr T load(std::span<const u8> in) noexcept {
    u64 wide = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        wide |= static_cast<u64>(in[i]) << (8U * i);
    }
    return static_cast<T>(wide);
}

/// Sign-extends the low `bits` of `value` to a full 64-bit signed integer.
/// Precondition: 0 < bits <= 64.
[[nodiscard]] constexpr i64 signExtend(u64 value, unsigned bits) noexcept {
    if (bits >= 64) {
        return static_cast<i64>(value);
    }
    const u64 mask = (u64{1} << bits) - 1U;
    const u64 sign = u64{1} << (bits - 1U);
    const u64 truncated = value & mask;
    // Unsigned arithmetic only: no signed overflow, no implementation-defined
    // narrowing.
    return static_cast<i64>((truncated ^ sign) - sign);
}

/// True if `value` is representable in `bits` bits of two's complement.
[[nodiscard]] constexpr bool fitsSigned(i64 value, unsigned bits) noexcept {
    if (bits >= 64) {
        return true;
    }
    const i64 min = -(i64{1} << (bits - 1U));
    const i64 max = (i64{1} << (bits - 1U)) - 1;
    return value >= min && value <= max;
}

/// True if `value` is representable in `bits` bits unsigned.
[[nodiscard]] constexpr bool fitsUnsigned(u64 value, unsigned bits) noexcept {
    if (bits >= 64) {
        return true;
    }
    return value < (u64{1} << bits);
}

}  // namespace minitool::byteorder
