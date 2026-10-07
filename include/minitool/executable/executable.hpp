// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/common/types.hpp"

/// The in-memory model of a `.mexe` executable: a memory image plus the
/// metadata a debugger needs. The VM loads this and never has to understand
/// object files, sections or relocations (architectural rule 5).
namespace minitool::executable {

inline constexpr std::array<u8, 4> kExeMagic{'M', 'E', 'X', 'E'};
inline constexpr u16 kExeVersion = 1;
inline constexpr u32 kExeHeaderSize = 64;

enum class SegmentType : u8 {
    Text = 0,
    Rodata = 1,
    Data = 2,
    Bss = 3,
};

[[nodiscard]] std::string_view segmentTypeName(SegmentType type) noexcept;
[[nodiscard]] bool isValidSegmentType(u8 value) noexcept;

enum class SegmentFlags : u8 {
    None = 0,
    Read = 1,
    Write = 2,
    Exec = 4,
};

[[nodiscard]] constexpr SegmentFlags operator|(SegmentFlags a, SegmentFlags b) noexcept {
    return static_cast<SegmentFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr SegmentFlags operator&(SegmentFlags a, SegmentFlags b) noexcept {
    return static_cast<SegmentFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}

constexpr SegmentFlags& operator|=(SegmentFlags& a, SegmentFlags b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool hasFlag(SegmentFlags flags, SegmentFlags flag) noexcept {
    return (flags & flag) == flag;
}

[[nodiscard]] std::string flagsToString(SegmentFlags flags);

struct Segment {
    SegmentType type = SegmentType::Text;
    SegmentFlags flags = SegmentFlags::Read;
    std::string name;
    u64 virtual_address = 0;
    /// Bytes occupied in memory. Larger than `data.size()` only for .bss, whose
    /// tail is zero-filled by the loader.
    u64 virtual_size = 0;
    std::vector<u8> data;
};

enum class SymbolKind : u8 {
    None = 0,
    Function = 1,
    Object = 2,
};

/// A name the debugger and disassembler can show. Purely informational: the VM
/// never consults it.
struct SymbolEntry {
    std::string name;
    u64 address = 0;
    u64 size = 0;
    SymbolKind kind = SymbolKind::None;
};

/// Maps a code address back to a source position.
struct DebugEntry {
    u64 address = 0;
    /// Index into Executable::source_files.
    u32 file = 0;
    u32 line = 0;
    u32 column = 0;
};

struct Executable {
    u16 version = kExeVersion;
    u64 entry_point = 0;
    std::vector<Segment> segments;
    std::vector<SymbolEntry> symbols;
    std::vector<DebugEntry> debug_info;
    std::vector<std::string> source_files;

    /// The segment containing `address`, or nullptr.
    [[nodiscard]] const Segment* findSegment(u64 address) const noexcept;
    /// Symbol whose address exactly matches, or nullptr.
    [[nodiscard]] const SymbolEntry* findSymbol(std::string_view name) const noexcept;
    /// The nearest symbol at or before `address` within its size, or nullptr.
    [[nodiscard]] const SymbolEntry* symbolContaining(u64 address) const noexcept;
    /// The debug entry for exactly `address`, or nullptr.
    [[nodiscard]] const DebugEntry* debugEntryFor(u64 address) const noexcept;
};

/// Checks every invariant the loader depends on: no overlapping or wrapping
/// segments, a data-free .bss, and an entry point that is 8-byte aligned and
/// lands inside an executable segment. `minitool verify` is exactly this call.
[[nodiscard]] std::expected<void, std::string> validate(const Executable& executable);

}  // namespace minitool::executable
