// SPDX-License-Identifier: MIT
#include "minitool/executable/executable_io.hpp"

#include <algorithm>
#include <format>

#include "minitool/common/binary.hpp"
#include "minitool/common/checksum.hpp"
#include "minitool/object/object_io.hpp"

namespace minitool::executable {
namespace {

constexpr u64 kSegmentRecordSize = 40;
constexpr u64 kSymbolRecordSize = 24;
constexpr u64 kDebugRecordSize = 24;
constexpr u64 kSourceRecordSize = 4;

constexpr std::size_t kStringOffsetField = 32;
constexpr std::size_t kStringSizeField = 40;
constexpr std::size_t kBlobOffsetField = 48;
constexpr std::size_t kFileSizeField = 56;
constexpr std::size_t kChecksumField = 60;

}  // namespace

std::expected<void, std::string> writeExecutableToBuffer(const Executable& executable,
                                                         std::vector<u8>& buffer) {
    // Never write an image the loader would refuse: a bad executable should be
    // a link error, not a mysterious failure at run time.
    const std::expected<void, std::string> valid = validate(executable);
    if (!valid.has_value()) {
        return std::unexpected(
            std::format("refusing to write an invalid executable: {}", valid.error()));
    }

    binary::StringTable strings;
    binary::Writer header;
    header.raw(kExeMagic);
    header.u16v(kExeVersion);
    header.u16v(static_cast<u16>(kExeHeaderSize));
    header.u64v(executable.entry_point);
    header.u32v(static_cast<u32>(executable.segments.size()));
    header.u32v(static_cast<u32>(executable.symbols.size()));
    header.u32v(static_cast<u32>(executable.debug_info.size()));
    header.u32v(static_cast<u32>(executable.source_files.size()));
    header.u64v(0);  // string table offset
    header.u64v(0);  // string table size
    header.u64v(0);  // segment data offset
    header.u32v(0);  // file size
    header.u32v(0);  // checksum

    binary::Writer tables;
    binary::Writer blob;

    for (const Segment& segment : executable.segments) {
        const u64 data_offset = blob.size();
        blob.raw(segment.data);
        tables.u32v(strings.intern(segment.name));
        tables.u8v(static_cast<u8>(segment.type));
        tables.u8v(static_cast<u8>(segment.flags));
        tables.pad(2);
        tables.u64v(segment.virtual_address);
        tables.u64v(segment.virtual_size);
        tables.u64v(data_offset);
        tables.u64v(segment.data.size());
    }

    for (const SymbolEntry& symbol : executable.symbols) {
        tables.u32v(strings.intern(symbol.name));
        tables.u8v(static_cast<u8>(symbol.kind));
        tables.pad(3);
        tables.u64v(symbol.address);
        tables.u64v(symbol.size);
    }

    for (const DebugEntry& entry : executable.debug_info) {
        tables.u64v(entry.address);
        tables.u32v(entry.file);
        tables.u32v(entry.line);
        tables.u32v(entry.column);
        tables.pad(4);
    }

    for (const std::string& file : executable.source_files) {
        tables.u32v(strings.intern(file));
    }

    const std::span<const u8> string_bytes = strings.bytes();
    const u64 string_offset = kExeHeaderSize + tables.size();
    const u64 blob_offset = string_offset + string_bytes.size();
    const u64 file_size = blob_offset + blob.size();
    if (file_size > 0xFFFF'FFFFU) {
        return std::unexpected(std::string{"executable exceeds the 4 GiB file size limit"});
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

    const u32 checksum = crc32(std::span<const u8>{buffer}.subspan(kExeHeaderSize));
    byteorder::store<u32>(std::span<u8>{buffer}.subspan(kChecksumField, sizeof(u32)), checksum);
    return {};
}

std::expected<Executable, std::string> readExecutableFromBuffer(std::span<const u8> buffer) {
    if (buffer.size() < kExeHeaderSize) {
        return std::unexpected(
            std::format("not an executable: {} bytes is shorter than the {}-byte header",
                        buffer.size(), kExeHeaderSize));
    }
    binary::Reader reader(buffer);
    const std::expected<std::span<const u8>, std::string> magic = reader.raw(kExeMagic.size());
    if (!magic.has_value()) {
        return std::unexpected(magic.error());
    }
    if (!std::equal(magic->begin(), magic->end(), kExeMagic.begin())) {
        return std::unexpected(std::string{"not an executable: bad magic"});
    }

    const auto version = reader.u16v();
    const auto header_size = reader.u16v();
    const auto entry_point = reader.u64v();
    const auto segment_count = reader.u32v();
    const auto symbol_count = reader.u32v();
    const auto debug_count = reader.u32v();
    const auto source_count = reader.u32v();
    const auto string_offset = reader.u64v();
    const auto string_size = reader.u64v();
    const auto blob_offset = reader.u64v();
    const auto file_size = reader.u32v();
    const auto checksum = reader.u32v();
    if (!version.has_value() || !header_size.has_value() || !entry_point.has_value() ||
        !segment_count.has_value() || !symbol_count.has_value() || !debug_count.has_value() ||
        !source_count.has_value() || !string_offset.has_value() || !string_size.has_value() ||
        !blob_offset.has_value() || !file_size.has_value() || !checksum.has_value()) {
        return std::unexpected(std::string{"truncated executable header"});
    }
    if (*version != kExeVersion) {
        return std::unexpected(
            std::format("unsupported executable version {} (this build understands {})", *version,
                        kExeVersion));
    }
    if (*header_size != kExeHeaderSize) {
        return std::unexpected(std::format("unsupported executable header size {}", *header_size));
    }
    if (*file_size != buffer.size()) {
        return std::unexpected(
            std::format("executable claims to be {} bytes but is {}", *file_size, buffer.size()));
    }
    const u32 actual = crc32(buffer.subspan(kExeHeaderSize));
    if (actual != *checksum) {
        return std::unexpected(std::format(
            "executable checksum mismatch: header says 0x{:08X}, contents hash to 0x{:08X}",
            *checksum, actual));
    }

    const u64 segments_at = kExeHeaderSize;
    const u64 symbols_at = segments_at + u64{*segment_count} * kSegmentRecordSize;
    const u64 debug_at = symbols_at + u64{*symbol_count} * kSymbolRecordSize;
    const u64 sources_at = debug_at + u64{*debug_count} * kDebugRecordSize;
    const u64 tables_end = sources_at + u64{*source_count} * kSourceRecordSize;
    if (tables_end > buffer.size() || *string_offset != tables_end ||
        *string_offset + *string_size > buffer.size() ||
        *blob_offset != *string_offset + *string_size || *blob_offset > buffer.size()) {
        return std::unexpected(std::string{"executable table layout is inconsistent"});
    }

    const std::expected<std::span<const u8>, std::string> string_table = reader.rawAt(
        static_cast<std::size_t>(*string_offset), static_cast<std::size_t>(*string_size));
    if (!string_table.has_value()) {
        return std::unexpected(string_table.error());
    }
    const std::span<const u8> blob = buffer.subspan(static_cast<std::size_t>(*blob_offset));

    Executable executable;
    executable.version = *version;
    executable.entry_point = *entry_point;

    reader.seek(static_cast<std::size_t>(segments_at));
    for (u32 i = 0; i < *segment_count; ++i) {
        const auto name_offset = reader.u32v();
        const auto type = reader.u8v();
        const auto flags = reader.u8v();
        const bool padded = reader.skip(2).has_value();
        const auto virtual_address = reader.u64v();
        const auto virtual_size = reader.u64v();
        const auto data_offset = reader.u64v();
        const auto data_size = reader.u64v();
        if (!padded || !name_offset.has_value() || !type.has_value() || !flags.has_value() ||
            !virtual_address.has_value() || !virtual_size.has_value() || !data_offset.has_value() ||
            !data_size.has_value()) {
            return std::unexpected(std::string{"truncated segment table"});
        }
        if (!isValidSegmentType(*type)) {
            return std::unexpected(std::format("invalid segment type {}", *type));
        }
        if (*data_offset > blob.size() || *data_size > blob.size() - *data_offset) {
            return std::unexpected(
                std::format("segment data range [{}, {}) is outside the {}-byte image",
                            *data_offset, *data_offset + *data_size, blob.size()));
        }
        const std::expected<std::string, std::string> name =
            binary::readString(*string_table, *name_offset);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        Segment segment;
        segment.name = *name;
        segment.type = static_cast<SegmentType>(*type);
        segment.flags = static_cast<SegmentFlags>(*flags);
        segment.virtual_address = *virtual_address;
        segment.virtual_size = *virtual_size;
        const std::span<const u8> data = blob.subspan(static_cast<std::size_t>(*data_offset),
                                                      static_cast<std::size_t>(*data_size));
        segment.data.assign(data.begin(), data.end());
        executable.segments.push_back(std::move(segment));
    }

    reader.seek(static_cast<std::size_t>(symbols_at));
    for (u32 i = 0; i < *symbol_count; ++i) {
        const auto name_offset = reader.u32v();
        const auto kind = reader.u8v();
        const bool padded = reader.skip(3).has_value();
        const auto address = reader.u64v();
        const auto size = reader.u64v();
        if (!padded || !name_offset.has_value() || !kind.has_value() || !address.has_value() ||
            !size.has_value()) {
            return std::unexpected(std::string{"truncated executable symbol table"});
        }
        if (*kind > static_cast<u8>(SymbolKind::Object)) {
            return std::unexpected(std::format("invalid symbol kind {}", *kind));
        }
        const std::expected<std::string, std::string> name =
            binary::readString(*string_table, *name_offset);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        executable.symbols.push_back(
            SymbolEntry{*name, *address, *size, static_cast<SymbolKind>(*kind)});
    }

    reader.seek(static_cast<std::size_t>(debug_at));
    for (u32 i = 0; i < *debug_count; ++i) {
        const auto address = reader.u64v();
        const auto file = reader.u32v();
        const auto line = reader.u32v();
        const auto column = reader.u32v();
        const bool padded = reader.skip(4).has_value();
        if (!padded || !address.has_value() || !file.has_value() || !line.has_value() ||
            !column.has_value()) {
            return std::unexpected(std::string{"truncated debug table"});
        }
        if (*file >= *source_count) {
            return std::unexpected(
                std::format("debug entry names source file {} of {}", *file, *source_count));
        }
        executable.debug_info.push_back(DebugEntry{*address, *file, *line, *column});
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
        executable.source_files.push_back(*name);
    }

    // The structural checks above prove the file parses; this proves the image
    // it describes is loadable.
    const std::expected<void, std::string> valid = validate(executable);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    return executable;
}

std::expected<void, std::string> writeExecutable(const Executable& executable,
                                                 const std::filesystem::path& path) {
    std::vector<u8> buffer;
    const std::expected<void, std::string> serialised = writeExecutableToBuffer(executable, buffer);
    if (!serialised.has_value()) {
        return serialised;
    }
    return object::writeFileBytes(path, buffer);
}

std::expected<Executable, std::string> readExecutable(const std::filesystem::path& path) {
    const std::expected<std::vector<u8>, std::string> bytes = object::readFileBytes(path);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    std::expected<Executable, std::string> executable = readExecutableFromBuffer(*bytes);
    if (!executable.has_value()) {
        return std::unexpected(std::format("{}: {}", path.string(), executable.error()));
    }
    return executable;
}

}  // namespace minitool::executable
