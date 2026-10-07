// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/common/types.hpp"

/// Shared primitives for the `.mobj` and `.mexe` serialisers. Both formats are
/// little-endian tables of fixed-size records plus a string table and a data
/// blob, so both need the same two things: an append-and-patch writer, and a
/// reader in which *every* accessor is bounds-checked and returns an error
/// instead of reading out of range (docs/object-format.md §Validation).
namespace minitool::binary {

/// Appends little-endian words to a byte vector and can patch already-written
/// offsets (used for the header's size and checksum fields, which are only
/// known once the body exists).
class Writer {
  public:
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<u8>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<u8> take() noexcept { return std::move(bytes_); }

    void u8v(u8 value) { bytes_.push_back(value); }

    void u16v(u16 value) { word<u16>(value); }
    void u32v(u32 value) { word<u32>(value); }
    void u64v(u64 value) { word<u64>(value); }
    void i64v(i64 value) { word<u64>(static_cast<u64>(value)); }

    void raw(std::span<const u8> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }

    /// Writes `count` zero bytes, e.g. record padding.
    void pad(std::size_t count) { bytes_.insert(bytes_.end(), count, u8{0}); }

    /// Pads until the buffer length is a multiple of `alignment`.
    void alignTo(std::size_t alignment) {
        while (alignment != 0 && (bytes_.size() % alignment) != 0) {
            bytes_.push_back(0);
        }
    }

    void patchU32(std::size_t offset, u32 value) {
        byteorder::store<u32>(std::span<u8>{bytes_}.subspan(offset, sizeof(u32)), value);
    }

    void patchU64(std::size_t offset, u64 value) {
        byteorder::store<u64>(std::span<u8>{bytes_}.subspan(offset, sizeof(u64)), value);
    }

  private:
    template <byteorder::UnsignedWord T>
    void word(T value) {
        const std::size_t at = bytes_.size();
        bytes_.resize(at + sizeof(T));
        byteorder::store<T>(std::span<u8>{bytes_}.subspan(at, sizeof(T)), value);
    }

    std::vector<u8> bytes_;
};

/// Deduplicating string table. Offset 0 always holds the empty string, so a
/// zero name reference is unambiguously "no name" for both writer and reader.
class StringTable {
  public:
    StringTable() { bytes_.push_back(0); }

    /// Returns the byte offset of `text`, inserting it if new.
    u32 intern(std::string_view text) {
        const auto existing = offsets_.find(std::string{text});
        if (existing != offsets_.end()) {
            return existing->second;
        }
        const auto offset = static_cast<u32>(bytes_.size());
        bytes_.insert(bytes_.end(), text.begin(), text.end());
        bytes_.push_back(0);
        offsets_.emplace(std::string{text}, offset);
        return offset;
    }

    [[nodiscard]] std::span<const u8> bytes() const noexcept { return bytes_; }

  private:
    std::vector<u8> bytes_;
    std::unordered_map<std::string, u32> offsets_;
};

/// A cursor over untrusted bytes. Every read validates its own bounds; the
/// caller never advances a raw pointer.
class Reader {
  public:
    explicit Reader(std::span<const u8> data) noexcept : data_(data) {}

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

    void seek(std::size_t offset) noexcept { position_ = offset; }

    [[nodiscard]] std::expected<u8, std::string> u8v() {
        if (!has(1)) {
            return std::unexpected(shortRead("u8", 1));
        }
        return data_[position_++];
    }

    [[nodiscard]] std::expected<u16, std::string> u16v() { return word<u16>("u16"); }
    [[nodiscard]] std::expected<u32, std::string> u32v() { return word<u32>("u32"); }
    [[nodiscard]] std::expected<u64, std::string> u64v() { return word<u64>("u64"); }

    [[nodiscard]] std::expected<i64, std::string> i64v() {
        const std::expected<u64, std::string> value = word<u64>("i64");
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        return static_cast<i64>(*value);
    }

    [[nodiscard]] std::expected<void, std::string> skip(std::size_t count) {
        if (!has(count)) {
            return std::unexpected(shortRead("padding", count));
        }
        position_ += count;
        return {};
    }

    /// Borrows `count` bytes at the cursor without copying.
    [[nodiscard]] std::expected<std::span<const u8>, std::string> raw(std::size_t count) {
        if (!has(count)) {
            return std::unexpected(shortRead("blob", count));
        }
        const std::span<const u8> result = data_.subspan(position_, count);
        position_ += count;
        return result;
    }

    /// Borrows `count` bytes at an absolute offset, leaving the cursor alone.
    [[nodiscard]] std::expected<std::span<const u8>, std::string> rawAt(std::size_t offset,
                                                                        std::size_t count) const {
        if (offset > data_.size() || count > data_.size() - offset) {
            return std::unexpected(std::format("region [{}, {}) is outside the {}-byte file",
                                               offset, offset + count, data_.size()));
        }
        return data_.subspan(offset, count);
    }

  private:
    template <byteorder::UnsignedWord T>
    [[nodiscard]] std::expected<T, std::string> word(const char* what) {
        if (!has(sizeof(T))) {
            return std::unexpected(shortRead(what, sizeof(T)));
        }
        const T value = byteorder::load<T>(data_.subspan(position_, sizeof(T)));
        position_ += sizeof(T);
        return value;
    }

    [[nodiscard]] bool has(std::size_t count) const noexcept {
        return count <= data_.size() - std::min(position_, data_.size());
    }

    [[nodiscard]] std::string shortRead(std::string_view what, std::size_t count) const {
        const std::size_t remaining = data_.size() - std::min(position_, data_.size());
        return std::format("truncated file: {} bytes of {} needed at offset {}, only {} remain",
                           count, what, position_, remaining);
    }

    std::span<const u8> data_;
    std::size_t position_ = 0;
};

/// Reads the NUL-terminated string at `offset` inside a string table blob.
/// Rejects an offset outside the table and a table whose final string is
/// unterminated, so a corrupt file cannot make the reader walk off the end.
[[nodiscard]] inline std::expected<std::string, std::string> readString(std::span<const u8> table,
                                                                        u32 offset) {
    if (offset >= table.size()) {
        return std::unexpected(std::format("string offset {} is outside the {}-byte string table",
                                           offset, table.size()));
    }
    const std::span<const u8> tail = table.subspan(offset);
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if (tail[i] == 0) {
            return std::string(reinterpret_cast<const char*>(tail.data()), i);
        }
    }
    return std::unexpected(std::format("unterminated string at offset {}", offset));
}

}  // namespace minitool::binary
