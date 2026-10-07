// SPDX-License-Identifier: MIT
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minitool/common/types.hpp"

namespace minitool {

/// Index of a section within an object file.
using SectionId = u32;
/// The section of a symbol that is referenced but not defined here.
inline constexpr SectionId kUndefinedSection = 0xFFFF'FFFFU;

enum class SymbolBinding : u8 {
    /// Visible only inside this object. Two files may both define one.
    Local = 0,
    /// Visible to the linker; two definitions are an error.
    Global = 1,
    /// A definition a strong one overrides, or a reference that resolves to 0.
    Weak = 2,
    /// Declared here, defined elsewhere.
    Extern = 3,
};

enum class SymbolType : u8 {
    NoType = 0,
    Object = 1,
    Function = 2,
    Section = 3,
};

struct Symbol {
    std::string name;
    SymbolBinding binding = SymbolBinding::Local;
    SymbolType type = SymbolType::NoType;
    /// The section this symbol lives in, or kUndefinedSection.
    SectionId section = kUndefinedSection;
    /// Offset within that section. Only meaningful when `defined`.
    u64 value = 0;
    u64 size = 0;
    bool defined = false;
};

/// The symbols of one object file, in a stable order.
///
/// Order is part of the contract: relocations refer to symbols by index, so
/// inserting a symbol must never renumber an existing one. `addSymbol`
/// therefore replaces in place when a name is already present, and never
/// reorders, which is also what makes object files byte-for-byte reproducible.
class SymbolTable {
  public:
    /// Adds `symbol`, or replaces the existing entry with the same name.
    /// Returns its index either way.
    u32 addSymbol(Symbol symbol) {
        const auto existing = index_.find(symbol.name);
        if (existing != index_.end()) {
            symbols_[existing->second] = std::move(symbol);
            return existing->second;
        }
        const auto position = static_cast<u32>(symbols_.size());
        index_.emplace(symbol.name, position);
        symbols_.push_back(std::move(symbol));
        return position;
    }

    /// Returns the index of `name`, creating an undefined local symbol for it
    /// if this is the first mention. This is how a forward reference gets an
    /// index before anyone knows what it refers to.
    u32 findOrAdd(std::string_view name) {
        const auto existing = index_.find(std::string{name});
        if (existing != index_.end()) {
            return existing->second;
        }
        Symbol symbol;
        symbol.name = std::string{name};
        return addSymbol(std::move(symbol));
    }

    [[nodiscard]] const Symbol* find(std::string_view name) const {
        const auto existing = index_.find(std::string{name});
        return existing == index_.end() ? nullptr : &symbols_[existing->second];
    }

    [[nodiscard]] Symbol* find(std::string_view name) {
        const auto existing = index_.find(std::string{name});
        return existing == index_.end() ? nullptr : &symbols_[existing->second];
    }

    [[nodiscard]] const Symbol& at(u32 index) const { return symbols_.at(index); }
    [[nodiscard]] Symbol& at(u32 index) { return symbols_.at(index); }

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(symbols_.size()); }
    [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }
    [[nodiscard]] std::span<const Symbol> symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::span<Symbol> symbols() noexcept { return symbols_; }

  private:
    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, u32> index_;
};

}  // namespace minitool
