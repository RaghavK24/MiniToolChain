// SPDX-License-Identifier: MIT
#include "minitool/executable/executable.hpp"

#include <algorithm>
#include <format>
#include <limits>

#include "minitool/isa/encoding.hpp"

namespace minitool::executable {

std::string_view segmentTypeName(SegmentType type) noexcept {
    switch (type) {
        case SegmentType::Text:
            return "text";
        case SegmentType::Rodata:
            return "rodata";
        case SegmentType::Data:
            return "data";
        case SegmentType::Bss:
            return "bss";
    }
    return "unknown";
}

bool isValidSegmentType(u8 value) noexcept {
    return value <= static_cast<u8>(SegmentType::Bss);
}

std::string flagsToString(SegmentFlags flags) {
    std::string text;
    text.push_back(hasFlag(flags, SegmentFlags::Read) ? 'r' : '-');
    text.push_back(hasFlag(flags, SegmentFlags::Write) ? 'w' : '-');
    text.push_back(hasFlag(flags, SegmentFlags::Exec) ? 'x' : '-');
    return text;
}

const Segment* Executable::findSegment(u64 address) const noexcept {
    for (const Segment& segment : segments) {
        if (address >= segment.virtual_address &&
            address - segment.virtual_address < segment.virtual_size) {
            return &segment;
        }
    }
    return nullptr;
}

const SymbolEntry* Executable::findSymbol(std::string_view name) const noexcept {
    for (const SymbolEntry& symbol : symbols) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const SymbolEntry* Executable::symbolContaining(u64 address) const noexcept {
    const SymbolEntry* best = nullptr;
    for (const SymbolEntry& symbol : symbols) {
        if (symbol.address > address) {
            continue;
        }
        // Prefer the closest preceding symbol; a sized symbol must still cover
        // the address.
        if (symbol.size != 0 && address - symbol.address >= symbol.size) {
            continue;
        }
        if (best == nullptr || symbol.address > best->address) {
            best = &symbol;
        }
    }
    return best;
}

const DebugEntry* Executable::debugEntryFor(u64 address) const noexcept {
    for (const DebugEntry& entry : debug_info) {
        if (entry.address == address) {
            return &entry;
        }
    }
    return nullptr;
}

std::expected<void, std::string> validate(const Executable& executable) {
    if (executable.version != kExeVersion) {
        return std::unexpected(
            std::format("unsupported executable version {}", executable.version));
    }
    if (executable.segments.empty()) {
        return std::unexpected(std::string{"executable has no segments"});
    }

    for (const Segment& segment : executable.segments) {
        if (segment.virtual_size == 0) {
            return std::unexpected(std::format("segment '{}' occupies no memory", segment.name));
        }
        if (segment.virtual_size < segment.data.size()) {
            return std::unexpected(std::format(
                "segment '{}' holds {} bytes of data but claims only {} bytes of memory",
                segment.name, segment.data.size(), segment.virtual_size));
        }
        if (segment.virtual_address > std::numeric_limits<u64>::max() - segment.virtual_size) {
            return std::unexpected(
                std::format("segment '{}' wraps past the end of the address space", segment.name));
        }
        if (segment.type == SegmentType::Bss && !segment.data.empty()) {
            return std::unexpected(std::format("segment '{}' is .bss but carries {} bytes of data",
                                               segment.name, segment.data.size()));
        }
        if (hasFlag(segment.flags, SegmentFlags::Write) &&
            hasFlag(segment.flags, SegmentFlags::Exec)) {
            return std::unexpected(
                std::format("segment '{}' is both writable and executable", segment.name));
        }
    }

    for (std::size_t i = 0; i < executable.segments.size(); ++i) {
        for (std::size_t j = i + 1; j < executable.segments.size(); ++j) {
            const Segment& a = executable.segments[i];
            const Segment& b = executable.segments[j];
            if (a.virtual_address < b.virtual_address + b.virtual_size &&
                b.virtual_address < a.virtual_address + a.virtual_size) {
                return std::unexpected(
                    std::format("segments '{}' and '{}' overlap in memory", a.name, b.name));
            }
        }
    }

    if ((executable.entry_point % isa::kInstructionSize) != 0) {
        return std::unexpected(std::format("entry point 0x{:X} is not {}-byte aligned",
                                           executable.entry_point, isa::kInstructionSize));
    }
    const Segment* entry_segment = executable.findSegment(executable.entry_point);
    if (entry_segment == nullptr) {
        return std::unexpected(
            std::format("entry point 0x{:X} is not inside any segment", executable.entry_point));
    }
    if (!hasFlag(entry_segment->flags, SegmentFlags::Exec)) {
        return std::unexpected(std::format("entry point 0x{:X} is in non-executable segment '{}'",
                                           executable.entry_point, entry_segment->name));
    }

    for (const DebugEntry& entry : executable.debug_info) {
        if (entry.file >= executable.source_files.size()) {
            return std::unexpected(std::format("debug entry names source file {} of {}", entry.file,
                                               executable.source_files.size()));
        }
    }
    return {};
}

}  // namespace minitool::executable
