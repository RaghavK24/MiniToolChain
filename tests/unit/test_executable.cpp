// SPDX-License-Identifier: MIT
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/common/checksum.hpp"
#include "minitool/executable/executable_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;
using namespace minitool::executable;

/// A minimal but valid image: one executable segment holding a HALT.
Executable sampleExecutable() {
    Executable executable;
    executable.entry_point = 0x10000;
    Segment text;
    text.name = ".text";
    text.type = SegmentType::Text;
    text.flags = SegmentFlags::Read | SegmentFlags::Exec;
    text.virtual_address = 0x10000;
    text.virtual_size = 8;
    text.data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};  // HALT
    executable.segments.push_back(std::move(text));
    executable.symbols.push_back(SymbolEntry{"_start", 0x10000, 8, SymbolKind::Function});
    executable.source_files.emplace_back("main.asm");
    executable.debug_info.push_back(DebugEntry{0x10000, 0, 3, 5});
    return executable;
}

std::vector<u8> serialise(const Executable& executable) {
    std::vector<u8> bytes;
    const std::expected<void, std::string> written = writeExecutableToBuffer(executable, bytes);
    EXPECT_TRUE(written.has_value()) << (written.has_value() ? "" : written.error());
    return bytes;
}

TEST(Executable, RoundTripsThroughBytes) {
    const Executable original = sampleExecutable();
    const std::expected<Executable, std::string> parsed =
        readExecutableFromBuffer(serialise(original));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->entry_point, original.entry_point);
    ASSERT_EQ(parsed->segments.size(), 1U);
    EXPECT_EQ(parsed->segments[0].name, ".text");
    EXPECT_EQ(parsed->segments[0].data, original.segments[0].data);
    EXPECT_EQ(parsed->segments[0].virtual_address, original.segments[0].virtual_address);
    ASSERT_EQ(parsed->symbols.size(), 1U);
    EXPECT_EQ(parsed->symbols[0].name, "_start");
    EXPECT_EQ(parsed->symbols[0].kind, SymbolKind::Function);
    ASSERT_EQ(parsed->debug_info.size(), 1U);
    EXPECT_EQ(parsed->debug_info[0].line, 3U);
    EXPECT_EQ(parsed->source_files, original.source_files);
}

TEST(Executable, SerialisationIsDeterministic) {
    EXPECT_EQ(serialise(sampleExecutable()), serialise(sampleExecutable()));
}

TEST(Executable, StartsWithItsMagic) {
    const std::vector<u8> bytes = serialise(sampleExecutable());
    ASSERT_GE(bytes.size(), kExeHeaderSize);
    EXPECT_EQ(bytes[0], 'M');
    EXPECT_EQ(bytes[1], 'E');
    EXPECT_EQ(bytes[2], 'X');
    EXPECT_EQ(bytes[3], 'E');
}

TEST(Executable, ValidateAcceptsAWellFormedImage) {
    EXPECT_TRUE(validate(sampleExecutable()).has_value());
}

TEST(Executable, ValidateRejectsAnEntryPointOutsideAnySegment) {
    Executable executable = sampleExecutable();
    executable.entry_point = 0x999000;
    const std::expected<void, std::string> valid = validate(executable);
    ASSERT_FALSE(valid.has_value());
    EXPECT_TRUE(valid.error().find("not inside any segment") != std::string::npos);
}

TEST(Executable, ValidateRejectsAnUnalignedEntryPoint) {
    Executable executable = sampleExecutable();
    executable.entry_point = 0x10004;
    EXPECT_FALSE(validate(executable).has_value());
}

TEST(Executable, ValidateRejectsAnEntryPointInDataMemory) {
    Executable executable = sampleExecutable();
    executable.segments[0].flags = SegmentFlags::Read | SegmentFlags::Write;
    const std::expected<void, std::string> valid = validate(executable);
    ASSERT_FALSE(valid.has_value());
    EXPECT_TRUE(valid.error().find("non-executable") != std::string::npos);
}

TEST(Executable, ValidateRejectsOverlappingSegments) {
    Executable executable = sampleExecutable();
    Segment overlapping;
    overlapping.name = ".data";
    overlapping.type = SegmentType::Data;
    overlapping.flags = SegmentFlags::Read | SegmentFlags::Write;
    overlapping.virtual_address = 0x10000;
    overlapping.virtual_size = 8;
    overlapping.data = {1, 2, 3, 4, 5, 6, 7, 8};
    executable.segments.push_back(std::move(overlapping));
    const std::expected<void, std::string> valid = validate(executable);
    ASSERT_FALSE(valid.has_value());
    EXPECT_TRUE(valid.error().find("overlap") != std::string::npos);
}

TEST(Executable, ValidateRejectsWritableExecutableMemory) {
    Executable executable = sampleExecutable();
    executable.segments[0].flags = SegmentFlags::Read | SegmentFlags::Write | SegmentFlags::Exec;
    EXPECT_FALSE(validate(executable).has_value());
}

TEST(Executable, ValidateRejectsBssWithData) {
    Executable executable = sampleExecutable();
    Segment bss;
    bss.name = ".bss";
    bss.type = SegmentType::Bss;
    bss.flags = SegmentFlags::Read | SegmentFlags::Write;
    bss.virtual_address = 0x20000;
    bss.virtual_size = 8;
    bss.data = {1};
    executable.segments.push_back(std::move(bss));
    EXPECT_FALSE(validate(executable).has_value());
}

TEST(Executable, RefusesToWriteAnInvalidImage) {
    Executable executable = sampleExecutable();
    executable.entry_point = 0x999000;
    std::vector<u8> bytes;
    const std::expected<void, std::string> written = writeExecutableToBuffer(executable, bytes);
    ASSERT_FALSE(written.has_value());
    EXPECT_TRUE(written.error().find("refusing") != std::string::npos);
}

TEST(Executable, RejectsTruncatedAndCorruptFiles) {
    const std::vector<u8> bytes = serialise(sampleExecutable());
    EXPECT_FALSE(readExecutableFromBuffer({}).has_value());
    for (const std::size_t length : {std::size_t{4}, std::size_t{40}, bytes.size() - 1}) {
        const std::vector<u8> truncated(bytes.begin(),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(length));
        EXPECT_FALSE(readExecutableFromBuffer(truncated).has_value()) << length;
    }
    std::vector<u8> corrupted = bytes;
    corrupted[bytes.size() - 1] ^= 0xFF;
    EXPECT_FALSE(readExecutableFromBuffer(corrupted).has_value());
}

TEST(Executable, RejectsAnImageThatParsesButCannotLoad) {
    // Build a file whose structure is fine but whose entry point is nowhere, by
    // patching the header and repairing the checksum.
    std::vector<u8> bytes = serialise(sampleExecutable());
    byteorder::store<u64>(std::span<u8>{bytes}.subspan(8, 8), 0xDEAD'0000U);
    const u32 checksum = crc32(std::span<const u8>{bytes}.subspan(kExeHeaderSize));
    byteorder::store<u32>(std::span<u8>{bytes}.subspan(60, 4), checksum);
    const std::expected<Executable, std::string> parsed = readExecutableFromBuffer(bytes);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_TRUE(parsed.error().find("entry point") != std::string::npos) << parsed.error();
}

TEST(Executable, FindsSegmentsSymbolsAndLines) {
    const Executable executable = sampleExecutable();
    EXPECT_NE(executable.findSegment(0x10004), nullptr);
    EXPECT_EQ(executable.findSegment(0x20000), nullptr);
    EXPECT_NE(executable.findSymbol("_start"), nullptr);
    EXPECT_EQ(executable.findSymbol("missing"), nullptr);
    EXPECT_NE(executable.symbolContaining(0x10004), nullptr);
    EXPECT_EQ(executable.symbolContaining(0x10008), nullptr);
    EXPECT_NE(executable.debugEntryFor(0x10000), nullptr);
    EXPECT_EQ(executable.debugEntryFor(0x10008), nullptr);
}

TEST(Executable, WritesAndReadsARealProgram) {
    const testkit::Linked linked =
        testkit::build(".global _start\n_start:\n    MOVI R1, 5\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "minitool_exe_test.mexe";
    ASSERT_TRUE(writeExecutable(linked.executable, path).has_value());
    const std::expected<Executable, std::string> parsed = readExecutable(path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    const testkit::Ran ran = testkit::run(*parsed);
    EXPECT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 5U);
    std::filesystem::remove(path);
}

}  // namespace
