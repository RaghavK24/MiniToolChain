// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "minitool/object/object.hpp"

/// Serialisation of the `.mobj` format. This layer knows the byte layout and
/// nothing else: it contains no linker logic and no assembler logic
/// (architectural rule 4), and it trusts nothing it reads.
namespace minitool::object {

/// Serialises `object` deterministically: the same object always produces the
/// same bytes, with no timestamps and no identifiers derived from the host.
[[nodiscard]] std::expected<void, std::string> writeObjectToBuffer(const ObjectFile& object,
                                                                   std::vector<u8>& buffer);

/// Parses and fully validates an object image. Every offset, length, index and
/// checksum is checked before it is used; malformed input yields an error, and
/// never an out-of-range read.
[[nodiscard]] std::expected<ObjectFile, std::string> readObjectFromBuffer(
    std::span<const u8> buffer);

[[nodiscard]] std::expected<void, std::string> writeObject(const ObjectFile& object,
                                                           const std::filesystem::path& path);

[[nodiscard]] std::expected<ObjectFile, std::string> readObject(const std::filesystem::path& path);

/// Reads a whole file into memory, reporting a readable error on failure.
[[nodiscard]] std::expected<std::vector<u8>, std::string> readFileBytes(
    const std::filesystem::path& path);

/// Writes `bytes` to `path`, creating or truncating it.
[[nodiscard]] std::expected<void, std::string> writeFileBytes(const std::filesystem::path& path,
                                                              std::span<const u8> bytes);

}  // namespace minitool::object
