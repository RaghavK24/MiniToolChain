// SPDX-License-Identifier: MIT
//
// Separate compilation: several sources become several objects, and the linker
// is what makes them one program. These are the tests that would catch a
// relocation applied against the wrong section base.

#include <array>
#include <string_view>
#include <vector>

#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

constexpr std::string_view kMain = R"(
.global _start
.extern sum_to
.extern message
.text
_start:
    MOVI R1, 10
    CALL sum_to
    MOV  R5, R14
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 3
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";

constexpr std::string_view kMath = R"(
.global sum_to
.text
sum_to:
    MOVI R14, 0
    MOVI R3, 0
loop:
    CMP  R1, R3
    JLE  done
    ADD  R14, R1
    DEC  R1
    JMP  loop
done:
    RET
)";

constexpr std::string_view kData = R"(
.global message
.rodata
message:
    .asciz "ok\n"
)";

TEST(MultiFile, LinksThreeObjectsIntoOneProgram) {
    const std::array<std::string_view, 3> sources{kMain, kMath, kData};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;

    const testkit::Ran ran = testkit::run(linked.executable);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(5), 55U);  // 10+9+...+1
    EXPECT_EQ(ran.output, "ok\n");
}

TEST(MultiFile, CrossObjectCallsLandInTheRightPlace) {
    const std::array<std::string_view, 3> sources{kMain, kMath, kData};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;

    const executable::SymbolEntry* sum_to = linked.executable.findSymbol("sum_to");
    ASSERT_NE(sum_to, nullptr);
    // The CALL in the first object must resolve to the second object's code,
    // which only works if the PC-relative relocation used the merged address.
    const executable::Segment& text = linked.executable.segments.front();
    const u64 call_address = linked.executable.entry_point + isa::kInstructionSize;
    const std::size_t offset = static_cast<std::size_t>(call_address - text.virtual_address);
    const std::expected<isa::Instruction, isa::DecodeError> call =
        isa::decodeFrom(std::span<const u8>{text.data}.subspan(offset, isa::kInstructionSize));
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->opcode, isa::Opcode::CALL);
    EXPECT_EQ(isa::branchTarget(call_address, call->imm), sum_to->address);
}

TEST(MultiFile, DataInOneObjectIsReachableFromAnother) {
    const std::array<std::string_view, 3> sources{kMain, kMath, kData};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;
    const executable::SymbolEntry* message = linked.executable.findSymbol("message");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->address, linker::kRodataBase);
}

TEST(MultiFile, SectionsFromEveryObjectAreConcatenated) {
    constexpr std::string_view kFirst =
        ".global _start\n.data\n.byte 1, 2\n.text\n_start:\n    HALT\n";
    constexpr std::string_view kSecond =
        ".global other\n.data\n.byte 3, 4\n.text\nother:\n    RET\n";
    const std::array<std::string_view, 2> sources{kFirst, kSecond};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;

    const executable::Segment* data = nullptr;
    for (const executable::Segment& segment : linked.executable.segments) {
        if (segment.name == ".data") {
            data = &segment;
        }
    }
    ASSERT_NE(data, nullptr);
    // The second object's data is aligned to 8 after the first object's.
    ASSERT_EQ(data->virtual_size, 10U);
    EXPECT_EQ(data->data[0], 1U);
    EXPECT_EQ(data->data[1], 2U);
    EXPECT_EQ(data->data[8], 3U);
    EXPECT_EQ(data->data[9], 4U);
}

TEST(MultiFile, LocalLabelsInDifferentObjectsDoNotCollide) {
    // Both objects define `.Lloop`; they are file-scoped, so this must link.
    constexpr std::string_view kFirst = R"(
.global _start
_start:
    MOVI R1, 2
.Lloop:
    DEC R1
    CMP R1, R0
    JG .Lloop
    CALL second
    HALT
)";
    constexpr std::string_view kSecond = R"(
.global second
second:
    MOVI R2, 2
.Lloop:
    DEC R2
    CMP R2, R0
    JG .Lloop
    RET
)";
    const std::array<std::string_view, 2> sources{kFirst, kSecond};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 0U);
    EXPECT_EQ(ran.reg(2), 0U);
}

TEST(MultiFile, AMissingObjectIsALinkErrorNotACrash) {
    const std::array<std::string_view, 1> sources{kMain};
    const testkit::Linked linked = testkit::buildAll(sources);
    ASSERT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("undefined symbol"));
}

TEST(MultiFile, DebugInformationNamesTheRightFile) {
    const std::array<std::string_view, 2> sources{kMain, kMath};
    // This link fails (no `message`), so use a pair that resolves.
    constexpr std::string_view kSimpleMain =
        ".global _start\n.extern helper\n_start:\n    CALL helper\n    HALT\n";
    constexpr std::string_view kHelper = ".global helper\nhelper:\n    NOP\n    RET\n";
    const std::array<std::string_view, 2> pair{kSimpleMain, kHelper};
    static_cast<void>(sources);

    const testkit::Linked linked = testkit::buildAll(pair);
    ASSERT_TRUE(linked.ok) << linked.error;
    ASSERT_EQ(linked.executable.source_files.size(), 2U);
    EXPECT_EQ(linked.executable.source_files[0], "unit0.asm");
    EXPECT_EQ(linked.executable.source_files[1], "unit1.asm");

    const executable::DebugEntry* entry =
        linked.executable.debugEntryFor(linked.executable.entry_point);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->file, 0U);

    const executable::SymbolEntry* helper = linked.executable.findSymbol("helper");
    ASSERT_NE(helper, nullptr);
    const executable::DebugEntry* in_helper = linked.executable.debugEntryFor(helper->address);
    ASSERT_NE(in_helper, nullptr);
    EXPECT_EQ(in_helper->file, 1U);
}

}  // namespace
