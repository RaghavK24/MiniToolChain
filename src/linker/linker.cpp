// SPDX-License-Identifier: MIT
#include "minitool/linker/linker.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

#include "minitool/object/relocation.hpp"

namespace minitool::linker {
namespace {

using executable::Executable;
using executable::Segment;
using executable::SegmentFlags;
using executable::SegmentType;
using object::ObjectFile;
using object::Relocation;
using object::SectionType;

[[nodiscard]] u64 alignUp(u64 value, u64 alignment) noexcept {
    if (alignment <= 1) {
        return value;
    }
    const u64 remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

/// One output region: everything of a given section type from every input,
/// concatenated in input order.
struct Region {
    SectionType type = SectionType::Text;
    SegmentType segment = SegmentType::Text;
    SegmentFlags flags = SegmentFlags::Read;
    std::string name;
    u64 base = 0;
    u64 size = 0;
    std::vector<u8> data;
    /// True for .bss, which is described but not stored.
    bool zero_filled = false;
};

/// Where one input section ended up: its offset inside the merged region.
struct Placement {
    bool placed = false;
    std::size_t region = 0;
    u64 offset = 0;
};

/// A definition that survived symbol resolution.
struct Definition {
    u64 address = 0;
    u64 size = 0;
    SymbolBinding binding = SymbolBinding::Global;
    SymbolType type = SymbolType::NoType;
    std::size_t object = 0;
};

class LinkContext {
  public:
    LinkContext(std::span<const ObjectFile> objects, const LinkOptions& options,
                diag::DiagnosticEngine& diagnostics)
        : objects_(objects), options_(options), diagnostics_(diagnostics) {}

    std::expected<Executable, std::string> run() {
        if (objects_.empty()) {
            return fail(diag::ErrorCode::InvalidObject, "no object files to link");
        }
        initRegions();
        if (const std::expected<void, std::string> merged = mergeSections(); !merged.has_value()) {
            return std::unexpected(merged.error());
        }
        if (const std::expected<void, std::string> resolved = resolveSymbols();
            !resolved.has_value()) {
            return std::unexpected(resolved.error());
        }
        if (const std::expected<void, std::string> applied = applyRelocations();
            !applied.has_value()) {
            return std::unexpected(applied.error());
        }
        return buildExecutable();
    }

  private:
    [[nodiscard]] std::unexpected<std::string> fail(diag::ErrorCode code, std::string message) {
        diagnostics_.error(code, SourceLocation{}, message);
        return std::unexpected(std::move(message));
    }

    void initRegions() {
        regions_ = {
            Region{SectionType::Text,
                   SegmentType::Text,
                   SegmentFlags::Read | SegmentFlags::Exec,
                   ".text",
                   options_.text_base,
                   0,
                   {},
                   false},
            Region{SectionType::Rodata,
                   SegmentType::Rodata,
                   SegmentFlags::Read,
                   ".rodata",
                   options_.rodata_base,
                   0,
                   {},
                   false},
            Region{SectionType::Data,
                   SegmentType::Data,
                   SegmentFlags::Read | SegmentFlags::Write,
                   ".data",
                   options_.data_base,
                   0,
                   {},
                   false},
            Region{SectionType::Bss,
                   SegmentType::Bss,
                   SegmentFlags::Read | SegmentFlags::Write,
                   ".bss",
                   options_.bss_base,
                   0,
                   {},
                   true},
        };
    }

    [[nodiscard]] std::size_t regionIndexFor(SectionType type) const {
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            if (regions_[i].type == type) {
                return i;
            }
        }
        return regions_.size();
    }

    /// Stage 1: concatenate like-named sections, honouring each input's
    /// alignment requirement.
    std::expected<void, std::string> mergeSections() {
        placements_.resize(objects_.size());
        for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
            const ObjectFile& object = objects_[object_index];
            placements_[object_index].resize(object.sections.size());
            for (std::size_t section_index = 0; section_index < object.sections.size();
                 ++section_index) {
                const object::Section& section = object.sections[section_index];
                if (section.type == SectionType::Null) {
                    continue;
                }
                const std::size_t region_index = regionIndexFor(section.type);
                if (region_index == regions_.size()) {
                    return fail(diag::ErrorCode::InvalidSectionReference,
                                std::format("section '{}' has no output region", section.name));
                }
                Region& region = regions_[region_index];
                const u64 offset = alignUp(region.size, section.alignment);
                if (offset > kRegionSize || section.size > kRegionSize - offset) {
                    return fail(diag::ErrorCode::RelocationOverflow,
                                std::format("{} grows past its {} KiB region", region.name,
                                            kRegionSize / 1024));
                }
                if (!region.zero_filled) {
                    region.data.resize(static_cast<std::size_t>(offset), u8{0});
                    region.data.insert(region.data.end(), section.data.begin(), section.data.end());
                    // A section may declare more memory than initialised data
                    // (only .bss does today, but the format allows it).
                    region.data.resize(static_cast<std::size_t>(offset + section.size), u8{0});
                }
                region.size = offset + section.size;
                placements_[object_index][section_index] = Placement{true, region_index, offset};
            }
        }
        return {};
    }

    /// The final address of a symbol defined in `object`.
    [[nodiscard]] std::expected<u64, std::string> addressOf(std::size_t object_index,
                                                            const Symbol& symbol) {
        const ObjectFile& object = objects_[object_index];
        if (symbol.section >= object.sections.size()) {
            return fail(diag::ErrorCode::InvalidSectionReference,
                        std::format("symbol '{}' names section {} of {}", symbol.name,
                                    symbol.section, object.sections.size()));
        }
        const Placement& placement = placements_[object_index][symbol.section];
        if (!placement.placed) {
            return fail(
                diag::ErrorCode::InvalidSectionReference,
                std::format("symbol '{}' lives in a section that was not laid out", symbol.name));
        }
        const Region& region = regions_[placement.region];
        return region.base + placement.offset + symbol.value;
    }

    /// Stage 2: build the global symbol table and diagnose conflicts.
    std::expected<void, std::string> resolveSymbols() {
        for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
            const ObjectFile& object = objects_[object_index];
            for (const Symbol& symbol : object.symbols.symbols()) {
                if (!symbol.defined) {
                    continue;
                }
                const std::expected<u64, std::string> address = addressOf(object_index, symbol);
                if (!address.has_value()) {
                    return std::unexpected(address.error());
                }
                if (symbol.binding == SymbolBinding::Local) {
                    locals_.push_back(Definition{*address, symbol.size, symbol.binding, symbol.type,
                                                 object_index});
                    local_names_.push_back(symbol.name);
                    continue;
                }
                const auto existing = definitions_.find(symbol.name);
                if (existing != definitions_.end()) {
                    const bool both_strong = existing->second.binding == SymbolBinding::Global &&
                                             symbol.binding == SymbolBinding::Global;
                    if (both_strong) {
                        return fail(diag::ErrorCode::DuplicateSymbol,
                                    std::format("symbol '{}' is defined in more than one object",
                                                symbol.name));
                    }
                    // A strong definition wins over a weak one; two weak
                    // definitions resolve to the first seen, which keeps the
                    // link deterministic for a given input order.
                    if (existing->second.binding == SymbolBinding::Weak &&
                        symbol.binding == SymbolBinding::Global) {
                        existing->second = Definition{*address, symbol.size, symbol.binding,
                                                      symbol.type, object_index};
                    }
                    continue;
                }
                definitions_.emplace(symbol.name, Definition{*address, symbol.size, symbol.binding,
                                                             symbol.type, object_index});
            }
        }

        // Every reference must now resolve, or the program has a hole in it.
        for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
            const ObjectFile& object = objects_[object_index];
            for (const Symbol& symbol : object.symbols.symbols()) {
                if (symbol.defined || definitions_.contains(symbol.name)) {
                    continue;
                }
                if (symbol.binding == SymbolBinding::Weak) {
                    // An unresolved weak reference is defined to be address 0,
                    // which a program can test for.
                    definitions_.emplace(symbol.name, Definition{0, 0, SymbolBinding::Weak,
                                                                 symbol.type, object_index});
                    continue;
                }
                return fail(diag::ErrorCode::UndefinedSymbol,
                            std::format("undefined symbol '{}' referenced by {}", symbol.name,
                                        sourceNameOf(object_index)));
            }
        }
        return {};
    }

    [[nodiscard]] std::string sourceNameOf(std::size_t object_index) const {
        const ObjectFile& object = objects_[object_index];
        return object.source_files.empty() ? std::format("object #{}", object_index)
                                           : object.source_files.front();
    }

    /// Stage 3: patch every reference now that every address is known.
    std::expected<void, std::string> applyRelocations() {
        for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
            const ObjectFile& object = objects_[object_index];
            for (const Relocation& relocation : object.relocations) {
                if (relocation.section >= object.sections.size() ||
                    relocation.symbol >= object.symbols.size()) {
                    return fail(diag::ErrorCode::InvalidRelocation,
                                "relocation refers to a section or symbol that does not exist");
                }
                const Placement& placement = placements_[object_index][relocation.section];
                if (!placement.placed) {
                    return fail(diag::ErrorCode::InvalidRelocation,
                                "relocation targets a section that was not laid out");
                }
                Region& region = regions_[placement.region];
                if (region.zero_filled) {
                    return fail(diag::ErrorCode::InvalidRelocation,
                                std::format("{} cannot hold a relocation", region.name));
                }

                const Symbol& symbol = object.symbols.at(relocation.symbol);
                u64 symbol_address = 0;
                if (symbol.defined && symbol.binding == SymbolBinding::Local) {
                    const std::expected<u64, std::string> address = addressOf(object_index, symbol);
                    if (!address.has_value()) {
                        return std::unexpected(address.error());
                    }
                    symbol_address = *address;
                } else {
                    const auto definition = definitions_.find(symbol.name);
                    if (definition == definitions_.end()) {
                        return fail(diag::ErrorCode::UndefinedSymbol,
                                    std::format("undefined symbol '{}'", symbol.name));
                    }
                    symbol_address = definition->second.address;
                }

                // Relocations are expressed against their own section; shift
                // them to the merged region before applying.
                Relocation merged = relocation;
                merged.offset = placement.offset + relocation.offset;
                const std::expected<void, object::RelocationError> applied =
                    object::applyRelocation(region.data, region.base, merged, symbol_address);
                if (!applied.has_value()) {
                    const bool overflow = applied.error() == object::RelocationError::Overflow;
                    return fail(overflow ? diag::ErrorCode::RelocationOverflow
                                         : diag::ErrorCode::InvalidRelocation,
                                std::format("{} relocation for '{}' at {} + 0x{:X}: {}",
                                            object::relocationTypeName(relocation.type),
                                            symbol.name, region.name, merged.offset,
                                            object::relocationErrorName(applied.error())));
                }
            }
        }
        return {};
    }

    /// Stage 4: assemble the loadable image and its debug metadata.
    std::expected<Executable, std::string> buildExecutable() {
        Executable executable;

        const auto entry = definitions_.find(options_.entry);
        if (entry == definitions_.end()) {
            return fail(
                diag::ErrorCode::UndefinedSymbol,
                std::format("entry point '{}' is not defined in any object", options_.entry));
        }
        executable.entry_point = entry->second.address;

        for (const Region& region : regions_) {
            if (region.size == 0) {
                continue;
            }
            Segment segment;
            segment.name = region.name;
            segment.type = region.segment;
            segment.flags = region.flags;
            segment.virtual_address = region.base;
            segment.virtual_size = region.size;
            if (!region.zero_filled) {
                segment.data = region.data;
            }
            executable.segments.push_back(std::move(segment));
        }

        for (const auto& [name, definition] : definitions_) {
            executable.symbols.push_back(executable::SymbolEntry{
                name, definition.address, definition.size, symbolKind(definition.type)});
        }
        if (options_.keep_local_symbols) {
            for (std::size_t i = 0; i < locals_.size(); ++i) {
                // A local name may repeat across objects; keep the first so the
                // debugger still has something to show without inventing names.
                if (definitions_.contains(local_names_[i])) {
                    continue;
                }
                executable.symbols.push_back(
                    executable::SymbolEntry{local_names_[i], locals_[i].address, locals_[i].size,
                                            symbolKind(locals_[i].type)});
            }
        }
        std::ranges::sort(executable.symbols, [](const executable::SymbolEntry& a,
                                                 const executable::SymbolEntry& b) {
            return a.address == b.address ? a.name < b.name : a.address < b.address;
        });

        buildDebugInfo(executable);

        const std::expected<void, std::string> valid = executable::validate(executable);
        if (!valid.has_value()) {
            return fail(diag::ErrorCode::InvalidExecutable, valid.error());
        }
        return executable;
    }

    static executable::SymbolKind symbolKind(SymbolType type) noexcept {
        switch (type) {
            case SymbolType::Function:
                return executable::SymbolKind::Function;
            case SymbolType::Object:
                return executable::SymbolKind::Object;
            default:
                return executable::SymbolKind::None;
        }
    }

    void buildDebugInfo(Executable& executable) {
        std::unordered_map<std::string, u32> file_index;
        for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
            const ObjectFile& object = objects_[object_index];
            for (const object::DebugEntry& entry : object.debug_info) {
                if (entry.section >= object.sections.size() ||
                    entry.file >= object.source_files.size()) {
                    continue;  // already rejected by the object reader
                }
                const Placement& placement = placements_[object_index][entry.section];
                if (!placement.placed) {
                    continue;
                }
                const std::string& file = object.source_files[entry.file];
                const auto known = file_index.find(file);
                u32 index = 0;
                if (known == file_index.end()) {
                    index = static_cast<u32>(executable.source_files.size());
                    executable.source_files.push_back(file);
                    file_index.emplace(file, index);
                } else {
                    index = known->second;
                }
                const u64 address =
                    regions_[placement.region].base + placement.offset + entry.offset;
                executable.debug_info.push_back(
                    executable::DebugEntry{address, index, entry.line, entry.column});
            }
        }
        std::ranges::sort(executable.debug_info,
                          [](const executable::DebugEntry& a, const executable::DebugEntry& b) {
                              return a.address < b.address;
                          });
    }

    std::span<const ObjectFile> objects_;
    LinkOptions options_;
    diag::DiagnosticEngine& diagnostics_;
    std::vector<Region> regions_;
    std::vector<std::vector<Placement>> placements_;
    /// Ordered so that the executable's symbol table is deterministic.
    std::map<std::string, Definition> definitions_;
    std::vector<Definition> locals_;
    std::vector<std::string> local_names_;
};

}  // namespace

Linker::Linker(diag::DiagnosticEngine& diagnostics) noexcept : diagnostics_(diagnostics) {}

std::expected<Executable, std::string> Linker::link(std::span<const ObjectFile> objects,
                                                    const LinkOptions& options) {
    LinkContext context(objects, options, diagnostics_);
    return context.run();
}

}  // namespace minitool::linker
