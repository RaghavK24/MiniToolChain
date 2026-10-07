// SPDX-License-Identifier: MIT
#include "minitool/common/checksum.hpp"

#include <array>

namespace minitool {
namespace {

/// Builds the standard reflected CRC-32 table at compile time so that no
/// mutable global state and no lazy initialisation are involved.
constexpr std::array<u32, 256> makeTable() noexcept {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (0xEDB8'8320U ^ (value >> 1U)) : (value >> 1U);
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<u32, 256> kTable = makeTable();

}  // namespace

u32 crc32(std::span<const u8> data) noexcept {
    u32 state = 0xFFFF'FFFFU;
    for (const u8 byte : data) {
        state = kTable[(state ^ byte) & 0xFFU] ^ (state >> 8U);
    }
    return state ^ 0xFFFF'FFFFU;
}

}  // namespace minitool
