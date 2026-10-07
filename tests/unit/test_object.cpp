// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/common/checksum.hpp"
#include "minitool/object/object_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;
using namespace minitool::object;

/// A representative object: several sections, symbols of every binding, every
/// relocation type and a line table.
ObjectFile sampleObject() {
    const testkit::Assembled assembled = testkit::assemble(
        ".global _start\n"
        ".extern helper\n"
        ".rodata\n"
        "msg:    .asciz \"hi\"\n"
        ".data\n"
        "slot:   .qword msg\n"
        ".bss\n"
        "buffer: .space 32\n"
        ".text\n"
        "_start:\n"
        "    LEA  R1, msg\n"
        "    CALL helper\n"
        "    JMP  _start\n"
        "    HALT\n");
    return assembled.object;
}

std::vector<u8> serialise(const ObjectFile& object) {
    std::vector<u8> bytes;
    const std::expected<void, std::string> written = writeObjectToBuffer(object, bytes);
    EXPECT_TRUE(written.has_value());
    return bytes;
}

/// Compares everything that must survive a round trip.
void expectEquivalent(const ObjectFile& a, const ObjectFile& b) {
    ASSERT_EQ(a.sections.size(), b.sections.size());
    for (std::size_t i = 0; i < a.sections.size(); ++i) {
        EXPECT_EQ(a.sections[i].name, b.sections[i].name);
        EXPECT_EQ(a.sections[i].type, b.sections[i].type);
        EXPECT_EQ(a.sections[i].alignment, b.sections[i].alignment);
        EXPECT_EQ(a.sections[i].size, b.sections[i].size);
        EXPECT_EQ(a.sections[i].data, b.sections[i].data);
        EXPECT_EQ(a.sections[i].index, b.sections[i].index);
    }
    ASSERT_EQ(a.symbols.size(), b.symbols.size());
    for (u32 i = 0; i < a.symbols.size(); ++i) {
        EXPECT_EQ(a.symbols.at(i).name, b.symbols.at(i).name);
        EXPECT_EQ(a.symbols.at(i).binding, b.symbols.at(i).binding);
        EXPECT_EQ(a.symbols.at(i).type, b.symbols.at(i).type);
        EXPECT_EQ(a.symbols.at(i).defined, b.symbols.at(i).defined);
        EXPECT_EQ(a.symbols.at(i).section, b.symbols.at(i).section);
        EXPECT_EQ(a.symbols.at(i).value, b.symbols.at(i).value);
    }
    ASSERT_EQ(a.relocations.size(), b.relocations.size());
    for (std::size_t i = 0; i < a.relocations.size(); ++i) {
        EXPECT_EQ(a.relocations[i].section, b.relocations[i].section);
        EXPECT_EQ(a.relocations[i].offset, b.relocations[i].offset);
        EXPECT_EQ(a.relocations[i].type, b.relocations[i].type);
        EXPECT_EQ(a.relocations[i].symbol, b.relocations[i].symbol);
        EXPECT_EQ(a.relocations[i].addend, b.relocations[i].addend);
    }
    ASSERT_EQ(a.debug_info.size(), b.debug_info.size());
    for (std::size_t i = 0; i < a.debug_info.size(); ++i) {
        EXPECT_EQ(a.debug_info[i].offset, b.debug_info[i].offset);
        EXPECT_EQ(a.debug_info[i].line, b.debug_info[i].line);
    }
    EXPECT_EQ(a.source_files, b.source_files);
}

TEST(Object, RoundTripsThroughBytes) {
    const ObjectFile original = sampleObject();
    const std::vector<u8> bytes = serialise(original);
    const std::expected<ObjectFile, std::string> parsed = readObjectFromBuffer(bytes);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    expectEquivalent(original, *parsed);
}

TEST(Object, SerialisationIsDeterministic) {
    // Two assemblies of the same source must be byte-identical: no timestamps,
    // no addresses, no iteration order leaking into the file.
    const std::vector<u8> first = serialise(sampleObject());
    const std::vector<u8> second = serialise(sampleObject());
    EXPECT_EQ(first, second);

    // And re-serialising what was read back must reproduce the same bytes.
    const std::expected<ObjectFile, std::string> parsed = readObjectFromBuffer(first);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(serialise(*parsed), first);
}

TEST(Object, StartsWithItsMagicAndVersion) {
    const std::vector<u8> bytes = serialise(sampleObject());
    ASSERT_GE(bytes.size(), kObjectHeaderSize);
    EXPECT_EQ(bytes[0], 'M');
    EXPECT_EQ(bytes[1], 'O');
    EXPECT_EQ(bytes[2], 'B');
    EXPECT_EQ(bytes[3], 'J');
    EXPECT_EQ(bytes[4], kObjectVersion);
}

TEST(Object, RejectsAnEmptyOrTruncatedFile) {
    EXPECT_FALSE(readObjectFromBuffer({}).has_value());
    const std::vector<u8> bytes = serialise(sampleObject());
    for (const std::size_t length :
         {std::size_t{1}, std::size_t{32}, bytes.size() / 2, bytes.size() - 1}) {
        const std::vector<u8> truncated(bytes.begin(),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(length));
        EXPECT_FALSE(readObjectFromBuffer(truncated).has_value()) << length;
    }
}

TEST(Object, RejectsBadMagicAndVersion) {
    std::vector<u8> bytes = serialise(sampleObject());
    std::vector<u8> wrong_magic = bytes;
    wrong_magic[1] = 'X';
    EXPECT_TRUE(readObjectFromBuffer(wrong_magic).error().find("magic") != std::string::npos);

    std::vector<u8> wrong_version = bytes;
    wrong_version[4] = 99;
    EXPECT_TRUE(readObjectFromBuffer(wrong_version).error().find("version") != std::string::npos);
}

TEST(Object, DetectsCorruptionWithTheChecksum) {
    const std::vector<u8> bytes = serialise(sampleObject());
    // Flip one bit in the middle of the tables; nothing structural changes, so
    // only the checksum can catch it.
    std::vector<u8> corrupted = bytes;
    corrupted[bytes.size() / 2] ^= 0x01;
    const std::expected<ObjectFile, std::string> parsed = readObjectFromBuffer(corrupted);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_TRUE(parsed.error().find("checksum") != std::string::npos) << parsed.error();
}

TEST(Object, RejectsInconsistentTableOffsets) {
    std::vector<u8> bytes = serialise(sampleObject());
    // Point the string table somewhere impossible and repair the checksum, so
    // the structural checks are what must reject it.
    byteorder::store<u64>(std::span<u8>{bytes}.subspan(32, 8), 0xFFFF'FFFFU);
    const u32 checksum = crc32(std::span<const u8>{bytes}.subspan(kObjectHeaderSize));
    byteorder::store<u32>(std::span<u8>{bytes}.subspan(60, 4), checksum);
    const std::expected<ObjectFile, std::string> parsed = readObjectFromBuffer(bytes);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_TRUE(parsed.error().find("layout") != std::string::npos) << parsed.error();
}

TEST(Object, SurvivesEveryOneByteCorruption) {
    // The contract: malformed input may be rejected, but
    // must never crash or read out of bounds.
    const std::vector<u8> bytes = serialise(sampleObject());
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<u8> corrupted = bytes;
        corrupted[i] = static_cast<u8>(corrupted[i] ^ 0xFF);
        if (readObjectFromBuffer(corrupted).has_value()) {
            ++accepted;
        }
    }
    // Only the checksum field itself can be flipped into another valid file,
    // and even that is astronomically unlikely; the point is that none of these
    // crashed.
    EXPECT_LE(accepted, 1U);
}

TEST(Object, WritesAndReadsAFile) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "minitool_object_test.mobj";
    const ObjectFile original = sampleObject();
    ASSERT_TRUE(writeObject(original, path).has_value());
    const std::expected<ObjectFile, std::string> parsed = readObject(path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    expectEquivalent(original, *parsed);
    std::filesystem::remove(path);

    EXPECT_FALSE(readObject("this_file_does_not_exist.mobj").has_value());
}

TEST(Object, HandlesAnEmptyObject) {
    const ObjectFile empty;
    const std::vector<u8> bytes = serialise(empty);
    const std::expected<ObjectFile, std::string> parsed = readObjectFromBuffer(bytes);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_TRUE(parsed->sections.empty());
    EXPECT_EQ(parsed->symbols.size(), 0U);
}

}  // namespace
