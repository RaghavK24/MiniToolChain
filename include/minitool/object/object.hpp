// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/assembler/symbol_table.hpp"
#include "minitool/common/types.hpp"

/// The in-memory model of a `.mobj` relocatable object file. The on-disk layout
/// is specified independently in docs/object-format.md (architectural rule 10);
/// these structs are only how the toolchain holds it in memory, and nothing
/// here is written to disk by simply copying bytes.
namespace minitool::object {

/// The first four bytes of every object file, in file order.
inline constexpr std::array<u8, 4> kObjectMagic{'M', 'O', 'B', 'J'};
inline constexpr u16 kObjectVersion = 1;
/// Fixed header size; readers use it to find the first table.
inline constexpr u32 kObjectHeaderSize = 64;

enum class SectionType : u8 {
    Null = 0,
    Text = 1,
    Rodata = 2,
    Data = 3,
    Bss = 4,
};

[[nodiscard]] std::string_view sectionTypeName(SectionType type) noexcept;
[[nodiscard]] bool isValidSectionType(u8 value) noexcept;

enum class SectionFlags : u8 {
    None = 0,
    /// Occupies memory at run time.
    Alloc = 1,
    Write = 2,
    Exec = 4,
};

[[nodiscard]] constexpr SectionFlags operator|(SectionFlags a, SectionFlags b) noexcept {
    return static_cast<SectionFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr SectionFlags operator&(SectionFlags a, SectionFlags b) noexcept {
    return static_cast<SectionFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}

[[nodiscard]] constexpr bool hasFlag(SectionFlags flags, SectionFlags flag) noexcept {
    return (flags & flag) == flag;
}

struct Section {
    std::string name;
    SectionType type = SectionType::Null;
    SectionFlags flags = SectionFlags::None;
    u64 alignment = 1;
    /// Initialised bytes. Empty for .bss.
    std::vector<u8> data;
    /// Bytes occupied at run time; equals data.size() for everything but .bss.
    u64 size = 0;
    u32 index = 0;
};

/// How a relocation combines the symbol address (S), the addend (A) and the
/// address of the patched field (P). See docs/relocation.md.
enum class RelocationType : u8 {
    /// 4 data bytes = S + A, rejected if it does not fit unsigned 32 bits.
    ABS32 = 0,
    /// 8 data bytes = S + A.
    ABS64 = 1,
    /// 4 data bytes = S + A - P, rejected outside signed 32 bits.
    PCREL32 = 2,
    /// The 48-bit immediate field of the instruction at `offset` = S + A.
    /// Emitted for `MOVI`/`LEA` of a symbol.
    IMM48 = 3,
    /// The 48-bit immediate field of the instruction at `offset`
    /// = S + A - (P + 8), matching isa::branchDisplacement exactly.
    /// Emitted for `JMP`/`Jcc`/`CALL` of a label.
    PCREL48 = 4,
};

[[nodiscard]] std::string_view relocationTypeName(RelocationType type) noexcept;
[[nodiscard]] bool isValidRelocationType(u8 value) noexcept;
/// Number of bytes the relocation reads and writes at its offset.
[[nodiscard]] u64 relocationWidth(RelocationType type) noexcept;

struct Relocation {
    /// Index into ObjectFile::sections of the section being patched.
    u32 section = 0;
    /// Byte offset of the patched field within that section. For the
    /// instruction-field types this is the offset of the whole 8-byte word.
    u64 offset = 0;
    RelocationType type = RelocationType::ABS64;
    /// Index into the object's symbol table.
    u32 symbol = 0;
    i64 addend = 0;
};

/// Maps one instruction back to the source line that produced it.
struct DebugEntry {
    u32 section = 0;
    /// Index into ObjectFile::source_files.
    u32 file = 0;
    u64 offset = 0;
    u32 line = 0;
    u32 column = 0;
};

struct ObjectFile {
    u16 version = kObjectVersion;
    std::vector<Section> sections;
    SymbolTable symbols;
    std::vector<Relocation> relocations;
    std::vector<DebugEntry> debug_info;
    std::vector<std::string> source_files;

    [[nodiscard]] const Section* findSection(std::string_view name) const noexcept;
};

}  // namespace minitool::object
