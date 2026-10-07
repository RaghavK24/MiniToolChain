// SPDX-License-Identifier: MIT
#include "minitool/assembler/assembler.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool {
namespace {

using object::ObjectFile;
using object::Relocation;
using object::RelocationType;
using object::Section;
using object::SectionFlags;
using object::SectionType;

[[nodiscard]] u64 alignUp(u64 value, u64 alignment) noexcept {
    if (alignment <= 1) {
        return value;
    }
    const u64 remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

[[nodiscard]] SectionType sectionTypeOf(ir::SectionKind kind) noexcept {
    switch (kind) {
        case ir::SectionKind::Text:
            return SectionType::Text;
        case ir::SectionKind::Rodata:
            return SectionType::Rodata;
        case ir::SectionKind::Data:
            return SectionType::Data;
        case ir::SectionKind::Bss:
            return SectionType::Bss;
    }
    return SectionType::Null;
}

[[nodiscard]] SectionFlags sectionFlagsOf(ir::SectionKind kind) noexcept {
    switch (kind) {
        case ir::SectionKind::Text:
            return SectionFlags::Alloc | SectionFlags::Exec;
        case ir::SectionKind::Rodata:
            return SectionFlags::Alloc;
        case ir::SectionKind::Data:
        case ir::SectionKind::Bss:
            return SectionFlags::Alloc | SectionFlags::Write;
    }
    return SectionFlags::None;
}

/// Sections are emitted in this fixed order whatever order the source used, so
/// that two runs over the same input always produce identical bytes.
constexpr std::array<ir::SectionKind, 4> kSectionOrder{
    ir::SectionKind::Text, ir::SectionKind::Rodata, ir::SectionKind::Data, ir::SectionKind::Bss};

/// The relocation a symbolic instruction operand needs, chosen from the
/// instruction's format: a branch gets a PC-relative displacement, everything
/// else an absolute address.
[[nodiscard]] RelocationType relocationForInstruction(isa::Opcode opcode) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(opcode);
    if (info != nullptr && info->format == isa::Format::Jump) {
        return RelocationType::PCREL48;
    }
    return RelocationType::IMM48;
}

/// Builds the object file for one module.
class AssemblyContext {
  public:
    AssemblyContext(const ir::Module& module, diag::DiagnosticEngine& diagnostics)
        : module_(module), diagnostics_(diagnostics) {}

    std::expected<ObjectFile, std::string> run() {
        collectSections();
        layoutAndDefineSymbols();
        applyBindings();
        emit();
        if (failed_) {
            return std::unexpected(
                std::format("{} error(s) while assembling", diagnostics_.errorCount()));
        }
        return std::move(object_);
    }

  private:
    void error(SourceLocation location, diag::ErrorCode code, std::string message) {
        diagnostics_.error(code, location, std::move(message));
        failed_ = true;
    }

    /// Creates one object section per IR section that carries anything.
    void collectSections() {
        for (const ir::SectionKind kind : kSectionOrder) {
            const ir::Section* source = module_.findSection(kind);
            if (source == nullptr || source->items.empty()) {
                continue;
            }
            Section section;
            section.name = std::string{ir::sectionName(kind)};
            section.type = sectionTypeOf(kind);
            section.flags = sectionFlagsOf(kind);
            section.alignment = source->alignment;
            section.index = static_cast<u32>(object_.sections.size());
            section_index_.emplace(kind, section.index);
            object_.sections.push_back(std::move(section));
            ordered_kinds_.push_back(kind);
        }
        object_.source_files.push_back(module_.source_name);
    }

    /// Pass 1: assign an offset to every item and record where each label lands.
    void layoutAndDefineSymbols() {
        for (const ir::SectionKind kind : ordered_kinds_) {
            const ir::Section& source = *module_.findSection(kind);
            const u32 index = section_index_.at(kind);
            u64 offset = 0;
            u64 max_alignment = object_.sections[index].alignment;

            for (const ir::Item& item : source.items) {
                std::visit(
                    [&](const auto& node) {
                        using Node = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<Node, ir::Label>) {
                            defineLabel(node, index, offset);
                        } else if constexpr (std::is_same_v<Node, ir::Instruction>) {
                            offset += isa::kInstructionSize;
                        } else if constexpr (std::is_same_v<Node, ir::Bytes>) {
                            offset += node.data.size();
                        } else if constexpr (std::is_same_v<Node, ir::SymbolValue>) {
                            offset += node.width;
                        } else if constexpr (std::is_same_v<Node, ir::Space>) {
                            offset += node.size;
                        } else {
                            offset = alignUp(offset, node.alignment);
                            max_alignment = std::max(max_alignment, node.alignment);
                        }
                    },
                    item);
            }
            object_.sections[index].size = offset;
            object_.sections[index].alignment = max_alignment;
        }
    }

    void defineLabel(const ir::Label& label, u32 section, u64 offset) {
        const u32 index = object_.symbols.findOrAdd(label.name);
        Symbol& symbol = object_.symbols.at(index);
        if (symbol.defined) {
            // sema catches duplicates within one file; this is the backstop for
            // an IR built programmatically rather than parsed.
            error(label.location, diag::ErrorCode::DuplicateSymbol,
                  std::format("symbol '{}' is defined more than once", label.name));
            return;
        }
        symbol.defined = true;
        symbol.section = section;
        symbol.value = offset;
        // Labels start local; `.global` / `.weak` promote them afterwards.
        symbol.binding = SymbolBinding::Local;
        symbol.type = object_.sections[section].type == SectionType::Text ? SymbolType::Function
                                                                          : SymbolType::Object;
    }

    /// Applies `.global`, `.extern` and `.weak`. A name that is declared but
    /// never defined here stays undefined for the linker to resolve.
    void applyBindings() {
        for (const std::string& name : module_.globals) {
            Symbol& symbol = object_.symbols.at(object_.symbols.findOrAdd(name));
            symbol.binding = SymbolBinding::Global;
        }
        for (const std::string& name : module_.weaks) {
            Symbol& symbol = object_.symbols.at(object_.symbols.findOrAdd(name));
            symbol.binding = SymbolBinding::Weak;
        }
        for (const std::string& name : module_.externs) {
            Symbol& symbol = object_.symbols.at(object_.symbols.findOrAdd(name));
            if (symbol.defined) {
                error(SourceLocation{}, diag::ErrorCode::SymbolVisibilityConflict,
                      std::format("'{}' is declared .extern but also defined in this file", name));
                continue;
            }
            symbol.binding = SymbolBinding::Extern;
        }
    }

    /// Records a reference to `name`, creating an undefined external symbol if
    /// this file never defines it. Returns the symbol table index.
    u32 referenceSymbol(const ir::SymbolRef& reference, SourceLocation location) {
        const u32 index = object_.symbols.findOrAdd(reference.name);
        Symbol& symbol = object_.symbols.at(index);
        if (!symbol.defined && symbol.binding == SymbolBinding::Local) {
            if (reference.name.starts_with(".L")) {
                // A local label is file-scoped by construction: if it is not
                // defined here it can never be resolved, so say so now rather
                // than emitting a relocation no linker can satisfy.
                error(location, diag::ErrorCode::UndefinedSymbol,
                      std::format("local label '{}' is used but never defined", reference.name));
            } else {
                // Like a traditional assembler, an unknown name is assumed to
                // come from another object file.
                symbol.binding = SymbolBinding::Extern;
            }
        }
        return index;
    }

    /// Pass 2: encode everything and emit relocations.
    void emit() {
        for (const ir::SectionKind kind : ordered_kinds_) {
            const ir::Section& source = *module_.findSection(kind);
            const u32 index = section_index_.at(kind);
            Section& section = object_.sections[index];
            const bool holds_data = ir::sectionHasData(kind);
            std::vector<u8>& bytes = section.data;
            bytes.reserve(static_cast<std::size_t>(section.size));

            for (const ir::Item& item : source.items) {
                std::visit(
                    [&](const auto& node) {
                        using Node = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<Node, ir::Label>) {
                            // Already placed in pass 1.
                        } else if constexpr (std::is_same_v<Node, ir::Instruction>) {
                            emitInstruction(node, index, bytes);
                        } else if constexpr (std::is_same_v<Node, ir::Bytes>) {
                            bytes.insert(bytes.end(), node.data.begin(), node.data.end());
                        } else if constexpr (std::is_same_v<Node, ir::SymbolValue>) {
                            emitSymbolValue(node, index, bytes);
                        } else if constexpr (std::is_same_v<Node, ir::Space>) {
                            bytes.insert(bytes.end(), static_cast<std::size_t>(node.size),
                                         node.fill);
                        } else {
                            const u64 padded = alignUp(bytes.size(), node.alignment);
                            bytes.resize(static_cast<std::size_t>(padded), u8{0});
                        }
                    },
                    item);
            }

            if (!holds_data) {
                // .bss keeps its size but occupies no file space.
                if (std::ranges::any_of(bytes, [](u8 byte) { return byte != 0; })) {
                    error(SourceLocation{}, diag::ErrorCode::InternalError,
                          "non-zero bytes were emitted into .bss");
                }
                bytes.clear();
            } else if (bytes.size() != section.size) {
                error(SourceLocation{}, diag::ErrorCode::InternalError,
                      std::format("section '{}' sized {} in pass 1 but emitted {} bytes",
                                  section.name, section.size, bytes.size()));
            }
        }
    }

    void emitInstruction(const ir::Instruction& instruction, u32 section, std::vector<u8>& bytes) {
        const u64 offset = bytes.size();
        isa::Instruction machine = instruction.machine;
        if (instruction.isSymbolic()) {
            // The final value is unknown; encode a zero placeholder and let the
            // linker patch the field.
            machine.imm = 0;
            Relocation relocation;
            relocation.section = section;
            relocation.offset = offset;
            relocation.type = relocationForInstruction(machine.opcode);
            relocation.symbol = referenceSymbol(*instruction.symbol, instruction.location);
            relocation.addend = instruction.symbol->addend;
            object_.relocations.push_back(relocation);
        }

        bytes.resize(bytes.size() + isa::kInstructionSize);
        const std::expected<void, isa::EncodeError> encoded = isa::encodeInto(
            std::span<u8>{bytes}.subspan(static_cast<std::size_t>(offset), isa::kInstructionSize),
            machine);
        if (!encoded.has_value()) {
            error(instruction.location, diag::ErrorCode::InvalidOperand,
                  std::format("cannot encode {}: {}", isa::opcodeName(machine.opcode),
                              isa::encodeErrorName(encoded.error())));
            return;
        }
        object_.debug_info.push_back(object::DebugEntry{
            section, 0, offset, instruction.location.line, instruction.location.column});
    }

    void emitSymbolValue(const ir::SymbolValue& value, u32 section, std::vector<u8>& bytes) {
        Relocation relocation;
        relocation.section = section;
        relocation.offset = bytes.size();
        relocation.type = value.width == 4 ? RelocationType::ABS32 : RelocationType::ABS64;
        relocation.symbol = referenceSymbol(value.symbol, value.location);
        relocation.addend = value.symbol.addend;
        object_.relocations.push_back(relocation);
        bytes.insert(bytes.end(), value.width, u8{0});
    }

    const ir::Module& module_;
    diag::DiagnosticEngine& diagnostics_;
    ObjectFile object_;
    std::unordered_map<ir::SectionKind, u32> section_index_;
    std::vector<ir::SectionKind> ordered_kinds_;
    bool failed_ = false;
};

}  // namespace

Assembler::Assembler(diag::DiagnosticEngine& diagnostics) noexcept : diagnostics_(diagnostics) {}

std::expected<ObjectFile, std::string> Assembler::assemble(const ir::Module& module) {
    AssemblyContext context(module, diagnostics_);
    return context.run();
}

}  // namespace minitool
