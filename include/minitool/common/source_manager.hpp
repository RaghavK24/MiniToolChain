// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "minitool/common/types.hpp"

namespace minitool {

/// Handle for a file registered with a SourceManager. `kInvalidFileId` marks a
/// location that did not come from source text (e.g. synthesised code).
using FileId = u32;
inline constexpr FileId kInvalidFileId = 0xFFFF'FFFFU;

/// A span of source text: 1-based line and column, plus a length in bytes used
/// to underline the offending text in diagnostics.
struct SourceLocation {
    FileId file = kInvalidFileId;
    u32 line = 0;
    u32 column = 0;
    u32 length = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return file != kInvalidFileId; }
    friend constexpr bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

/// Owns the text of every source file in a compilation so that SourceLocation
/// stays a small trivially-copyable value. Registered text is never mutated or
/// freed while the manager lives, so returned views stay valid.
class SourceManager {
  public:
    /// Registers a file and returns its id. `text` is copied into the manager.
    FileId addFile(std::string name, std::string text);

    [[nodiscard]] std::string_view name(FileId id) const;
    [[nodiscard]] std::string_view text(FileId id) const;

    /// Returns the 1-based `line` of `id` without its terminator, or an empty
    /// view if the file or line does not exist.
    [[nodiscard]] std::string_view line(FileId id, u32 line) const;

    [[nodiscard]] std::size_t fileCount() const noexcept { return files_.size(); }
    [[nodiscard]] bool contains(FileId id) const noexcept { return id < files_.size(); }

  private:
    struct Entry {
        std::string name;
        std::string text;
        std::vector<std::size_t> line_starts;  // byte offset of each 1-based line
    };
    std::vector<Entry> files_;
};

}  // namespace minitool
