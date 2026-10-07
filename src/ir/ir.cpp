// SPDX-License-Identifier: MIT
#include "minitool/ir/ir.hpp"

#include <algorithm>

namespace minitool::ir {

std::string_view sectionName(SectionKind kind) noexcept {
    switch (kind) {
        case SectionKind::Text:
            return ".text";
        case SectionKind::Rodata:
            return ".rodata";
        case SectionKind::Data:
            return ".data";
        case SectionKind::Bss:
            return ".bss";
    }
    return ".unknown";
}

std::optional<SectionKind> sectionKindFromName(std::string_view name) noexcept {
    if (name == ".text") {
        return SectionKind::Text;
    }
    if (name == ".rodata") {
        return SectionKind::Rodata;
    }
    if (name == ".data") {
        return SectionKind::Data;
    }
    if (name == ".bss") {
        return SectionKind::Bss;
    }
    return std::nullopt;
}

bool sectionHasData(SectionKind kind) noexcept {
    return kind != SectionKind::Bss;
}

SourceLocation itemLocation(const Item& item) noexcept {
    return std::visit([](const auto& value) { return value.location; }, item);
}

Section& Module::sectionFor(SectionKind kind) {
    const auto existing = std::ranges::find_if(
        sections, [kind](const Section& section) { return section.kind == kind; });
    if (existing != sections.end()) {
        return *existing;
    }
    Section section;
    section.kind = kind;
    section.name = std::string{sectionName(kind)};
    // Code is fetched 8 bytes at a time and data is naturally 8-aligned, so 8
    // is the right default for every section the toolchain creates.
    section.alignment = 8;
    sections.push_back(std::move(section));
    return sections.back();
}

const Section* Module::findSection(SectionKind kind) const noexcept {
    const auto existing = std::ranges::find_if(
        sections, [kind](const Section& section) { return section.kind == kind; });
    return existing == sections.end() ? nullptr : &*existing;
}

std::size_t Module::instructionCount() const noexcept {
    std::size_t count = 0;
    for (const Section& section : sections) {
        for (const Item& item : section.items) {
            if (std::holds_alternative<Instruction>(item)) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace minitool::ir
