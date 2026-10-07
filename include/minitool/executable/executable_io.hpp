// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "minitool/executable/executable.hpp"

/// Serialisation of the `.mexe` format, specified in docs/executable-format.md.
/// The reader validates the file's structure *and* the loadability of the image
/// it describes before returning it, so a caller that gets an Executable back
/// can load it without further checks.
namespace minitool::executable {

[[nodiscard]] std::expected<void, std::string> writeExecutableToBuffer(const Executable& executable,
                                                                       std::vector<u8>& buffer);

[[nodiscard]] std::expected<Executable, std::string> readExecutableFromBuffer(
    std::span<const u8> buffer);

[[nodiscard]] std::expected<void, std::string> writeExecutable(const Executable& executable,
                                                               const std::filesystem::path& path);

[[nodiscard]] std::expected<Executable, std::string> readExecutable(
    const std::filesystem::path& path);

}  // namespace minitool::executable
