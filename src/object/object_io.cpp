// SPDX-License-Identifier: MIT
#include "minitool/object/object_io.hpp"

#include <algorithm>
#include <format>
#include <fstream>

#include "minitool/common/binary.hpp"
#include "minitool/common/checksum.hpp"

namespace minitool::object {
namespace {

// Record sizes are fixed so that a reader can bounds-check a whole table with
// one multiplication. See docs/object-format.md for the field layouts.
constexpr u64 kSectionRecordSize = 48;
constexpr u64 kSymbolRecordSize = 32;
constexpr u64 kRelocationRecordSize = 32;
constexpr u64 kDebugRecordSize = 24;
constexpr u64 kSourceRecordSize = 4;

// Byte offsets of the header fields that are patched after the body is built.
constexpr std::size_t kStringOffsetField = 32;
constexpr std::size_t kStringSizeField = 40;
constexpr std::size_t kBlobOffsetField = 48;
constexpr std::size_t kFileSizeField = 56;
constexpr std::size_t kChecksumField = 60;

[[nodiscard]] std::string outOfRange(std::string_view what, u64 value, u64 limit) {
    return std::format("invalid object file: {} {} exceeds {}", what, value, limit);
}

}  // namespace

std::expected<void, std::string> writeObjectToBuffer(const ObjectFile& object,
                                                     std::vector<u8>& buffer) {
    if (object.sections.size() > 0xFFFF'FFFFU || object.relocations.size() > 0xFFFF'FFFFU ||
        object.debug_info.size() > 0xFFFF'FFFFU || object.source_files.size() > 0xFFFF'FFFFU) {
        return std::unexpected(std::string{"object is too large to serialise"});
    }

    binary::StringTable strings;
    binary::Writer header;
    header.raw(kObjectMagic);
    header.u16v(kObjectVersion);
    header.u16v(static_cast<u16>(kObjectHeaderSize));
    header.u32v(0);  // flags, reserved
    header.u32v(static_cast<u32>(object.sections.size()));
    header.u32v(object.symbols.size());
    header.u32v(static_cast<u32>(object.relocations.size()));
    header.u32v(static_cast<u32>(object.debug_info.size()));
    header.u32v(static_cast<u32>(object.source_files.size()));
    header.u64v(0);  // string table offset, patched below
    header.u64v(0);  // string table size
    header.u64v(0);  // section data offset
    header.u32v(0);  // file size
    header.u32v(0);  // checksum

    binary::Writer tables;
    binary::Writer blob;

    for (const Section& section : object.sections) {
        const u64 data_offset = blob.size();
        blob.raw(section.data);
        tables.u32v(strings.intern(section.name));
        tables.u8v(static_cast<u8>(section.type));
        tables.u8v(static_cast<u8>(section.flags));
        tables.pad(2);
        tables.u64v(section.alignment);
        tables.u64v(data_offset);
        tables.u64v(section.data.size());
        tables.u64v(section.size);
        tables.u32v(section.index);
        tables.pad(4);
    }

    for (const Symbol& symbol : object.symbols.symbols()) {
        tables.u32v(strings.intern(symbol.name));
        tables.u8v(static_cast<u8>(symbol.binding));
        tables.u8v(static_cast<u8>(symbol.type));
        tables.u8v(symbol.defined ? 1U : 0U);
        tables.pad(1);
        tables.u32v(symbol.section);
        tables.pad(4);
        tables.u64v(symbol.value);
        tables.u64v(symbol.size);
    }

    for (const Relocation& relocation : object.relocations) {
        tables.u32v(relocation.section);
        tables.u32v(relocation.symbol);
        tables.u64v(relocation.offset);
        tables.i64v(relocation.addend);
        tables.u8v(static_cast<u8>(relocation.type));
        tables.pad(7);
    }

    for (const DebugEntry& entry : object.debug_info) {
        tables.u32v(entry.section);
        tables.u32v(entry.file);
        tables.u64v(entry.offset);
        tables.u32v(entry.line);
        tables.u32v(entry.column);
    }

    for (const std::string& file : object.source_files) {
        tables.u32v(strings.intern(file));
    }

    const std::span<const u8> string_bytes = strings.bytes();
    const u64 string_offset = kObjectHeaderSize + tables.size();
    const u64 blob_offset = string_offset + string_bytes.size();
    const u64 file_size = blob_offset + blob.size();
    if (file_size > 0xFFFF'FFFFU) {
        return std::unexpected(std::string{"object exceeds the 4 GiB file size limit"});
    }

    header.patchU64(kStringOffsetField, string_offset);
    header.patchU64(kStringSizeField, string_bytes.size());
    header.patchU64(kBlobOffsetField, blob_offset);
    header.patchU32(kFileSizeField, static_cast<u32>(file_size));

    buffer.clear();
    buffer.reserve(static_cast<std::size_t>(file_size));
    const std::vector<u8>& header_bytes = header.bytes();
    buffer.insert(buffer.end(), header_bytes.begin(), header_bytes.end());
    const std::vector<u8>& table_bytes = tables.bytes();
    buffer.insert(buffer.end(), table_bytes.begin(), table_bytes.end());
    buffer.insert(buffer.end(), string_bytes.begin(), string_bytes.end());
    const std::vector<u8>& blob_bytes = blob.bytes();
    buffer.insert(buffer.end(), blob_bytes.begin(), blob_bytes.end());

    // The checksum covers everything after the header, so it is stable under
    // the header patching above.
    const u32 checksum = crc32(std::span<const u8>{buffer}.subspan(kObjectHeaderSize));
    byteorder::store<u32>(std::span<u8>{buffer}.subspan(kChecksumField, sizeof(u32)), checksum);
    return {};
}

std::expected<ObjectFile, std::string> readObjectFromBuffer(std::span<const u8> buffer) {
    if (buffer.size() < kObjectHeaderSize) {
        return std::unexpected(
            std::format("not an object file: {} bytes is shorter than the {}"
                        "-byte header",
                        buffer.size(), kObjectHeaderSize));
    }
    binary::Reader reader(buffer);
    const std::expected<std::span<const u8>, std::string> magic = reader.raw(kObjectMagic.size());
    if (!magic.has_value()) {
        return std::unexpected(magic.error());
    }
    if (!std::equal(magic->begin(), magic->end(), kObjectMagic.begin())) {
        return std::unexpected(std::string{"not an object file: bad magic"});
    }

    const auto version = reader.u16v();
    const auto header_size = reader.u16v();
    const auto flags = reader.u32v();
    const auto section_count = reader.u32v();
    const auto symbol_count = reader.u32v();
    const auto relocation_count = reader.u32v();
    const auto debug_count = reader.u32v();
    const auto source_count = reader.u32v();
    const auto string_offset = reader.u64v();
    const auto string_size = reader.u64v();
    const auto blob_offset = reader.u64v();
    const auto file_size = reader.u32v();
    const auto checksum = reader.u32v();
    if (!version.has_value() || !header_size.has_value() || !flags.has_value() ||
        !section_count.has_value() || !symbol_count.has_value() || !relocation_count.has_value() ||
        !debug_count.has_value() || !source_count.has_value() || !string_offset.has_value() ||
        !string_size.has_value() || !blob_offset.has_value() || !file_size.has_value() ||
        !checksum.has_value()) {
        return std::unexpected(std::string{"truncated object header"});
    }

    if (*version != kObjectVersion) {
        return std::unexpected(std::format(
            "unsupported object version {} (this build understands {})", *version, kObjectVersion));
    }
    if (*header_size != kObjectHeaderSize) {
        return std::unexpected(std::format("unsupported object header size {}", *header_size));
    }
    if (*flags != 0) {
        // The field is reserved. Rejecting a non-zero value keeps the door open
        // for a future flag to mean something, and means no byte of the header
        // is ignored.
        return std::unexpected(std::format("unknown object flags 0x{:08X}", *flags));
    }
    if (*file_size != buffer.size()) {
        return std::unexpected(
            std::format("object claims to be {} bytes but is {}", *file_size, buffer.size()));
    }
    const u32 actual = crc32(buffer.subspan(kObjectHeaderSize));
    if (actual != *checksum) {
        return std::unexpected(
            std::format("object checksum mismatch: header says 0x{:08X}, contents hash to 0x{:08X}",
                        *checksum, actual));
    }

    // Table extents: each is validated against the file before it is read.
    const u64 sections_at = kObjectHeaderSize;
    const u64 symbols_at = sections_at + u64{*section_count} * kSectionRecordSize;
    const u64 relocations_at = symbols_at + u64{*symbol_count} * kSymbolRecordSize;
    const u64 debug_at = relocations_at + u64{*relocation_count} * kRelocationRecordSize;
    const u64 sources_at = debug_at + u64{*debug_count} * kDebugRecordSize;
    const u64 tables_end = sources_at + u64{*source_count} * kSourceRecordSize;
    if (tables_end > buffer.size() || *string_offset != tables_end ||
        *string_offset + *string_size > buffer.size() ||
        *blob_offset != *string_offset + *string_size || *blob_offset > buffer.size()) {
        return std::unexpected(std::string{"object table layout is inconsistent"});
    }

    const std::expected<std::span<const u8>, std::string> string_table = reader.rawAt(
        static_cast<std::size_t>(*string_offset), static_cast<std::size_t>(*string_size));
    if (!string_table.has_value()) {
        return std::unexpected(string_table.error());
    }
    const std::span<const u8> blob = buffer.subspan(static_cast<std::size_t>(*blob_offset));

    ObjectFile object;
    object.version = *version;

    reader.seek(static_cast<std::size_t>(sections_at));
    for (u32 i = 0; i < *section_count; ++i) {
        Section section;
        const auto name_offset = reader.u32v();
        const auto type = reader.u8v();
        const auto section_flags = reader.u8v();
        if (!reader.skip(2).has_value()) {
            return std::unexpected(std::string{"truncated section table"});
        }
        const auto alignment = reader.u64v();
        const auto data_offset = reader.u64v();
        const auto data_size = reader.u64v();
        const auto memory_size = reader.u64v();
        const auto index = reader.u32v();
        if (!reader.skip(4).has_value() || !name_offset.has_value() || !type.has_value() ||
            !section_flags.has_value() || !alignment.has_value() || !data_offset.has_value() ||
            !data_size.has_value() || !memory_size.has_value() || !index.has_value()) {
            return std::unexpected(std::string{"truncated section table"});
        }
        if (!isValidSectionType(*type)) {
            return std::unexpected(std::format("invalid section type {}", *type));
        }
        if (*alignment == 0 || (*alignment & (*alignment - 1U)) != 0U) {
            return std::unexpected(
                std::format("section alignment {} is not a power of two", *alignment));
        }
        if (*data_offset > blob.size() || *data_size > blob.size() - *data_offset) {
            return std::unexpected(
                outOfRange("section data range", *data_offset + *data_size, blob.size()));
        }
        if (*memory_size < *data_size) {
            return std::unexpected(
                std::string{"section memory size is smaller than its initialised data"});
        }
        const std::expected<std::string, std::string> name =
            binary::readString(*string_table, *name_offset);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        section.name = *name;
        section.type = static_cast<SectionType>(*type);
        section.flags = static_cast<SectionFlags>(*section_flags);
        section.alignment = *alignment;
        const std::span<const u8> data = blob.subspan(static_cast<std::size_t>(*data_offset),
                                                      static_cast<std::size_t>(*data_size));
        section.data.assign(data.begin(), data.end());
        section.size = *memory_size;
        section.index = *index;
        object.sections.push_back(std::move(section));
    }

    reader.seek(static_cast<std::size_t>(symbols_at));
    for (u32 i = 0; i < *symbol_count; ++i) {
        const auto name_offset = reader.u32v();
        const auto binding = reader.u8v();
        const auto type = reader.u8v();
        const auto defined = reader.u8v();
        const bool padded = reader.skip(1).has_value();
        const auto section_index = reader.u32v();
        const bool padded2 = reader.skip(4).has_value();
        const auto value = reader.u64v();
        const auto size = reader.u64v();
        if (!padded || !padded2 || !name_offset.has_value() || !binding.has_value() ||
            !type.has_value() || !defined.has_value() || !section_index.has_value() ||
            !value.has_value() || !size.has_value()) {
            return std::unexpected(std::string{"truncated symbol table"});
        }
        if (*binding > static_cast<u8>(SymbolBinding::Extern)) {
            return std::unexpected(std::format("invalid symbol binding {}", *binding));
        }
        if (*type > static_cast<u8>(SymbolType::Section)) {
            return std::unexpected(std::format("invalid symbol type {}", *type));
        }
        if (*section_index != kUndefinedSection && *section_index >= *section_count) {
            return std::unexpected(
                outOfRange("symbol section index", *section_index, *section_count));
        }
        const std::expected<std::string, std::string> name =
            binary::readString(*string_table, *name_offset);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        if (object.symbols.find(*name) != nullptr) {
            // Symbol indices are positional, so a duplicate name would silently
            // renumber every relocation that follows it.
            return std::unexpected(std::format("duplicate symbol '{}' in symbol table", *name));
        }
        Symbol symbol;
        symbol.name = *name;
        symbol.binding = static_cast<SymbolBinding>(*binding);
        symbol.type = static_cast<SymbolType>(*type);
        symbol.defined = *defined != 0;
        symbol.section = *section_index;
        symbol.value = *value;
        symbol.size = *size;
        static_cast<void>(object.symbols.addSymbol(std::move(symbol)));
    }

    reader.seek(static_cast<std::size_t>(relocations_at));
    for (u32 i = 0; i < *relocation_count; ++i) {
        const auto section_index = reader.u32v();
        const auto symbol_index = reader.u32v();
        const auto offset = reader.u64v();
        const auto addend = reader.i64v();
        const auto type = reader.u8v();
        const bool padded = reader.skip(7).has_value();
        if (!padded || !section_index.has_value() || !symbol_index.has_value() ||
            !offset.has_value() || !addend.has_value() || !type.has_value()) {
            return std::unexpected(std::string{"truncated relocation table"});
        }
        if (!isValidRelocationType(*type)) {
            return std::unexpected(std::format("invalid relocation type {}", *type));
        }
        if (*section_index >= *section_count) {
            return std::unexpected(
                outOfRange("relocation section index", *section_index, *section_count));
        }
        if (*symbol_index >= *symbol_count) {
            return std::unexpected(
                outOfRange("relocation symbol index", *symbol_index, *symbol_count));
        }
        const Section& target = object.sections[*section_index];
        const u64 width = relocationWidth(static_cast<RelocationType>(*type));
        if (*offset > target.data.size() || width > target.data.size() - *offset) {
            return std::unexpected(
                std::format("relocation at offset {} does not fit in section '{}' ({} bytes)",
                            *offset, target.name, target.data.size()));
        }
        Relocation relocation;
        relocation.section = *section_index;
        relocation.symbol = *symbol_index;
        relocation.offset = *offset;
        relocation.addend = *addend;
        relocation.type = static_cast<RelocationType>(*type);
        object.relocations.push_back(relocation);
    }

    reader.seek(static_cast<std::size_t>(debug_at));
    for (u32 i = 0; i < *debug_count; ++i) {
        const auto section_index = reader.u32v();
        const auto file_index = reader.u32v();
        const auto offset = reader.u64v();
        const auto line = reader.u32v();
        const auto column = reader.u32v();
        if (!section_index.has_value() || !file_index.has_value() || !offset.has_value() ||
            !line.has_value() || !column.has_value()) {
            return std::unexpected(std::string{"truncated debug table"});
        }
        if (*section_index >= *section_count) {
            return std::unexpected(
                outOfRange("debug section index", *section_index, *section_count));
        }
        if (*file_index >= *source_count) {
            return std::unexpected(outOfRange("debug file index", *file_index, *source_count));
        }
        object.debug_info.push_back(
            DebugEntry{*section_index, *file_index, *offset, *line, *column});
    }

    reader.seek(static_cast<std::size_t>(sources_at));
    for (u32 i = 0; i < *source_count; ++i) {
        const auto name_offset = reader.u32v();
        if (!name_offset.has_value()) {
            return std::unexpected(std::string{"truncated source file table"});
        }
        const std::expected<std::string, std::string> name =
            binary::readString(*string_table, *name_offset);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        object.source_files.push_back(*name);
    }

    return object;
}

std::expected<std::vector<u8>, std::string> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}' for reading", path.string()));
    }
    std::vector<u8> bytes;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::unexpected(std::format("cannot determine the size of '{}'", path.string()));
    }
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            return std::unexpected(std::format("failed while reading '{}'", path.string()));
        }
    }
    return bytes;
}

std::expected<void, std::string> writeFileBytes(const std::filesystem::path& path,
                                                std::span<const u8> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}' for writing", path.string()));
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return std::unexpected(std::format("failed while writing '{}'", path.string()));
    }
    return {};
}

std::expected<void, std::string> writeObject(const ObjectFile& object,
                                             const std::filesystem::path& path) {
    std::vector<u8> buffer;
    const std::expected<void, std::string> serialised = writeObjectToBuffer(object, buffer);
    if (!serialised.has_value()) {
        return serialised;
    }
    return writeFileBytes(path, buffer);
}

std::expected<ObjectFile, std::string> readObject(const std::filesystem::path& path) {
    const std::expected<std::vector<u8>, std::string> bytes = readFileBytes(path);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    std::expected<ObjectFile, std::string> object = readObjectFromBuffer(*bytes);
    if (!object.has_value()) {
        return std::unexpected(std::format("{}: {}", path.string(), object.error()));
    }
    return object;
}

}  // namespace minitool::object
