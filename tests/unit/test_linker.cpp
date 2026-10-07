// SPDX-License-Identifier: MIT
#include <array>
#include <string_view>
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

constexpr std::string_view kMain = R"(
.global _start
.extern add_two
_start:
    MOVI R1, 40
    CALL add_two
    HALT
)";

constexpr std::string_view kLibrary = R"(
.global add_two
add_two:
    MOVI R2, 2
    ADD R1, R2
    RET
)";

TEST(Linker, LinksASingleObject) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    EXPECT_EQ(linked.executable.entry_point, linker::kTextBase);
    ASSERT_EQ(linked.executable.segments.size(), 1U);
    EXPECT_EQ(linked.executable.segments[0].name, ".text");
    EXPECT_TRUE(
        executable::hasFlag(linked.executable.segments[0].flags, executable::SegmentFlags::Exec));
}

TEST(Linker, ResolvesSymbolsAcrossObjects) {
    const std::array<std::string_view, 2> sources{kMain, kLibrary};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;

    const executable::SymbolEntry* add_two = linked.executable.findSymbol("add_two");
    ASSERT_NE(add_two, nullptr);
    // The second object's .text follows the first, aligned to 8.
    EXPECT_EQ(add_two->address, linker::kTextBase + 3 * isa::kInstructionSize);

    const testkit::Ran ran = testkit::run(linked.executable);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 42U);
}

TEST(Linker, LinkOrderDoesNotChangeTheAnswer) {
    const std::array<std::string_view, 2> forward{kMain, kLibrary};
    const std::array<std::string_view, 2> reverse{kLibrary, kMain};
    const testkit::Ran first = testkit::run(testkit::buildAll(forward).executable);
    const testkit::Ran second = testkit::run(testkit::buildAll(reverse).executable);
    EXPECT_EQ(first.reg(1), 42U);
    EXPECT_EQ(second.reg(1), 42U);
}

TEST(Linker, PlacesEachSectionInItsOwnRegion) {
    const testkit::Linked linked = testkit::build(
        ".global _start\n"
        ".rodata\n.byte 1\n"
        ".data\n.byte 2\n"
        ".bss\n.space 16\n"
        ".text\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    ASSERT_EQ(linked.executable.segments.size(), 4U);
    EXPECT_EQ(linked.executable.segments[0].virtual_address, linker::kTextBase);
    EXPECT_EQ(linked.executable.segments[1].virtual_address, linker::kRodataBase);
    EXPECT_EQ(linked.executable.segments[2].virtual_address, linker::kDataBase);
    EXPECT_EQ(linked.executable.segments[3].virtual_address, linker::kBssBase);
    // .bss occupies memory but no file space.
    EXPECT_EQ(linked.executable.segments[3].virtual_size, 16U);
    EXPECT_TRUE(linked.executable.segments[3].data.empty());
    // .rodata must not be writable, .data must not be executable.
    EXPECT_FALSE(
        executable::hasFlag(linked.executable.segments[1].flags, executable::SegmentFlags::Write));
    EXPECT_FALSE(
        executable::hasFlag(linked.executable.segments[2].flags, executable::SegmentFlags::Exec));
}

TEST(Linker, PatchesDataRelocationsToFinalAddresses) {
    const testkit::Linked linked = testkit::build(
        ".global _start\n"
        ".rodata\nmessage:\n    .asciz \"hi\"\n"
        ".data\npointer:\n    .qword message\n"
        ".text\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const executable::Segment& data = linked.executable.segments.at(2);
    EXPECT_EQ(byteorder::load<u64>(data.data), linker::kRodataBase);
}

TEST(Linker, RejectsAnUndefinedSymbol) {
    const testkit::Linked linked =
        testkit::build(".global _start\n_start:\n    CALL nowhere\n    HALT\n");
    ASSERT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("undefined symbol 'nowhere'"));
}

TEST(Linker, RejectsDuplicateGlobalDefinitions) {
    constexpr std::string_view kFirst = ".global thing\n.global _start\n_start:\nthing:\n HALT\n";
    constexpr std::string_view kSecond = ".global thing\nthing:\n    RET\n";
    const std::array<std::string_view, 2> sources{kFirst, kSecond};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("defined in more than one object"));
}

TEST(Linker, LetsAStrongDefinitionOverrideAWeakOne) {
    constexpr std::string_view kWeak = ".weak thing\nthing:\n    MOVI R1, 1\n    RET\n";
    constexpr std::string_view kStrong = ".global thing\nthing:\n    MOVI R1, 2\n    RET\n";
    constexpr std::string_view kMainProgram = ".global _start\n_start:\n    CALL thing\n    HALT\n";
    const std::array<std::string_view, 3> sources{kMainProgram, kWeak, kStrong};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 2U);
}

TEST(Linker, ResolvesAnUnusedWeakReferenceToZero) {
    const testkit::Linked linked =
        testkit::build(".global _start\n.weak optional\n_start:\n    LEA R1, optional\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 0U);
}

TEST(Linker, RequiresAnEntryPoint) {
    const testkit::Assembled assembled = testkit::assemble("nothing:\n    HALT\n");
    ASSERT_TRUE(assembled.ok);
    const std::vector<object::ObjectFile> objects{assembled.object};
    const testkit::Linked linked = testkit::link(objects);
    ASSERT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("entry point '_start' is not defined"));
}

TEST(Linker, HonoursACustomEntryPoint) {
    const testkit::Assembled assembled = testkit::assemble(".global begin\nbegin:\n    HALT\n");
    ASSERT_TRUE(assembled.ok);
    const std::vector<object::ObjectFile> objects{assembled.object};
    linker::LinkOptions options;
    options.entry = "begin";
    const testkit::Linked linked = testkit::link(objects, options);
    ASSERT_TRUE(linked.ok) << linked.error;
    EXPECT_EQ(linked.executable.entry_point, linker::kTextBase);
}

TEST(Linker, RejectsAnEmptyLink) {
    const testkit::Linked linked = testkit::link({});
    ASSERT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("no object files"));
}

TEST(Linker, MergesDebugInformationWithAddresses) {
    const std::array<std::string_view, 2> sources{kMain, kLibrary};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;
    EXPECT_EQ(linked.executable.source_files.size(), 2U);
    EXPECT_FALSE(linked.executable.debug_info.empty());
    // Entries are sorted by address and point into the text segment.
    for (std::size_t i = 1; i < linked.executable.debug_info.size(); ++i) {
        EXPECT_LE(linked.executable.debug_info[i - 1].address,
                  linked.executable.debug_info[i].address);
    }
    EXPECT_EQ(linked.executable.debug_info.front().address, linker::kTextBase);
}

TEST(Linker, ExportsSymbolsSortedByAddress) {
    const std::array<std::string_view, 2> sources{kMain, kLibrary};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;
    for (std::size_t i = 1; i < linked.executable.symbols.size(); ++i) {
        EXPECT_LE(linked.executable.symbols[i - 1].address, linked.executable.symbols[i].address);
    }
}

}  // namespace
