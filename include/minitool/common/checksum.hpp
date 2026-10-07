// SPDX-License-Identifier: MIT
#pragma once

#include <span>

#include "minitool/common/types.hpp"

namespace minitool {

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) over `data`.
///
/// Used as the integrity check in the `.mobj` and `.mexe` headers. It detects
/// truncation and casual corruption; it is not a cryptographic guarantee, and
/// the readers still validate every offset and length independently of it.
[[nodiscard]] u32 crc32(std::span<const u8> data) noexcept;

}  // namespace minitool
