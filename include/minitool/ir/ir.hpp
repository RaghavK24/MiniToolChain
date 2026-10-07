// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "minitool/common/source_manager.hpp"
#include "minitool/common/types.hpp"
#include "minitool/isa/instruction.hpp"

/// The intermediate representation the optimizer works on (architectural rule
/// 8), sitting between the AST and the assembler:
///
///     AST  ->  IR  ->  optimizer  ->  IR  ->  assembler  ->  .mobj
///
/// The IR is *addressless*: an item knows its order inside a section but not
/// its offset, and a branch names a label rather than carrying a displacement.
/// That is exactly what makes optimization safe: inserting or deleting an
/// instruction cannot invalidate a branch, because no displacement exists yet.
/// Offsets are assigned once, in the assembler's first pass, after every
/// transformation has run.
namespace minitool::ir {

enum class SectionKind : u8 { Text, Rodata, Data, Bss };

[[nodiscard]] std::string_view sectionName(SectionKind kind) noexcept;
[[nodiscard]] std::optional<SectionKind> sectionKindFromName(std::string_view name) noexcept;
/// True if the section occupies file space (everything except .bss).
[[nodiscard]] bool sectionHasData(SectionKind kind) noexcept;

/// A reference to a symbol plus a constant addend: `message + 8`.
struct SymbolRef {
    std::string name;
    i64 addend = 0;

    friend bool operator==(const SymbolRef&, const SymbolRef&) = default;
};

/// One machine instruction. `machine.imm` holds the literal immediate when
/// `symbol` is empty, and is ignored (the addend lives in the SymbolRef) when
/// it is set: the value is only known after linking.
struct Instruction {
    isa::Instruction machine;
    std::optional<SymbolRef> symbol;
    SourceLocation location;

    [[nodiscard]] bool isSymbolic() const noexcept { return symbol.has_value(); }
};

/// Defines a symbol at the current offset of the enclosing section.
struct Label {
    std::string name;
    /// `.Lname` labels never leave the object file.
    bool local = false;
    SourceLocation location;
};

/// Literal bytes emitted by `.byte`, `.word`, `.asciz`, ...
struct Bytes {
    std::vector<u8> data;
    SourceLocation location;
};

/// A symbol address stored as data (`.qword message + 8`). Becomes an ABS32 or
/// ABS64 relocation.
struct SymbolValue {
    u32 width = 8;
    SymbolRef symbol;
    SourceLocation location;
};

/// `.space n [, fill]`: n bytes of `fill`.
struct Space {
    u64 size = 0;
    u8 fill = 0;
    SourceLocation location;
};

/// `.align n`: pad the section to an n-byte boundary.
struct Align {
    u64 alignment = 1;
    SourceLocation location;
};

using Item = std::variant<Instruction, Label, Bytes, SymbolValue, Space, Align>;

[[nodiscard]] SourceLocation itemLocation(const Item& item) noexcept;

struct Section {
    SectionKind kind = SectionKind::Text;
    std::string name;
    u64 alignment = 8;
    std::vector<Item> items;
};

/// Everything one assembly file contributes to an object file.
struct Module {
    std::string source_name;
    std::vector<Section> sections;
    /// Names named by `.global` / `.extern` / `.weak`, in source order.
    std::vector<std::string> globals;
    std::vector<std::string> externs;
    std::vector<std::string> weaks;

    /// Returns the section with `kind`, creating it if this is its first use.
    Section& sectionFor(SectionKind kind);
    [[nodiscard]] const Section* findSection(SectionKind kind) const noexcept;
    /// Total instruction count across every section, for -O reporting.
    [[nodiscard]] std::size_t instructionCount() const noexcept;
};

}  // namespace minitool::ir
