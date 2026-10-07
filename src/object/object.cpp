// SPDX-License-Identifier: MIT
#include "minitool/object/object.hpp"

namespace minitool::object {

std::string_view sectionTypeName(SectionType type) noexcept {
    switch (type) {
        case SectionType::Null:
            return "null";
        case SectionType::Text:
            return "text";
        case SectionType::Rodata:
            return "rodata";
        case SectionType::Data:
            return "data";
        case SectionType::Bss:
            return "bss";
    }
    return "unknown";
}

bool isValidSectionType(u8 value) noexcept {
    return value <= static_cast<u8>(SectionType::Bss);
}

std::string_view relocationTypeName(RelocationType type) noexcept {
    switch (type) {
        case RelocationType::ABS32:
            return "ABS32";
        case RelocationType::ABS64:
            return "ABS64";
        case RelocationType::PCREL32:
            return "PCREL32";
        case RelocationType::IMM48:
            return "IMM48";
        case RelocationType::PCREL48:
            return "PCREL48";
    }
    return "UNKNOWN";
}

bool isValidRelocationType(u8 value) noexcept {
    return value <= static_cast<u8>(RelocationType::PCREL48);
}

u64 relocationWidth(RelocationType type) noexcept {
    switch (type) {
        case RelocationType::ABS32:
        case RelocationType::PCREL32:
            return 4;
        case RelocationType::ABS64:
        case RelocationType::IMM48:
        case RelocationType::PCREL48:
            // The instruction-field relocations rewrite part of an 8-byte word
            // but must be able to read all of it, so they need 8 bytes present.
            return 8;
    }
    return 0;
}

const Section* ObjectFile::findSection(std::string_view name) const noexcept {
    for (const Section& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

}  // namespace minitool::object
