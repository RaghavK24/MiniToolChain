// SPDX-License-Identifier: MIT
//
// Golden tests: the exact bytes of the `.mobj` and `.mexe` files produced from
// fixed sources are checked in, and any change to them has to be deliberate.
//
// This is what stops a refactor from quietly altering a binary format. A
// failure here is not necessarily a bug, but it is always a decision:
//
//     1. if the change is intended, regenerate the fixtures with
//            MINITOOL_UPDATE_GOLDEN=1 build/msvc-release/test_golden_binaries.exe
//        (or `pwsh tools/generate-fixtures.ps1`), and commit the new files
//        alongside a note in docs/development-log.md;
//     2. if it is not, the format changed by accident, and that is the bug.
//
// Fixtures are never silently rewritten by a normal run.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <array>

#include "minitool/disassembler/disassembler.hpp"
#include "minitool/executable/executable_io.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/object/object_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

const std::filesystem::path kFixtures{"tests/fixtures"};

bool updatingFixtures() {
    const char* value = std::getenv("MINITOOL_UPDATE_GOLDEN");
    return value != nullptr && std::string_view{value} != "0";
}

std::vector<u8> readFixture(const std::filesystem::path& path) {
    const std::expected<std::vector<u8>, std::string> bytes = object::readFileBytes(path);
    return bytes.value_or(std::vector<u8>{});
}

/// Compares `actual` with the fixture at `name`, or writes it when updating.
void checkGolden(std::string_view name, const std::vector<u8>& actual) {
    const std::filesystem::path path = kFixtures / name;
    if (updatingFixtures()) {
        std::filesystem::create_directories(kFixtures);
        ASSERT_TRUE(object::writeFileBytes(path, actual).has_value());
        return;
    }
    const std::vector<u8> expected = readFixture(path);
    if (expected.empty()) {
        ADD_FAILURE() << "missing fixture " << path.string()
                      << "; regenerate with MINITOOL_UPDATE_GOLDEN=1";
        return;
    }
    ASSERT_EQ(actual.size(), expected.size())
        << path.string() << " changed size; see the header comment in this file";
    for (std::size_t i = 0; i < actual.size(); ++i) {
        ASSERT_EQ(actual[i], expected[i])
            << path.string() << " differs at byte " << i << "; see the header comment in this file";
    }
}

/// The program the fixtures are built from. It deliberately uses every part of
/// the format: all four sections, each symbol binding, each relocation type,
/// and a line table.
constexpr std::string_view kGoldenSource = R"(
.global _start
.global counter
.extern external_helper
.weak optional_hook

.rodata
message:
    .asciz "golden\n"

.data
counter:
    .qword 0
pointer:
    .qword message + 1

.bss
scratch:
    .space 32

.text
_start:
    MOVI R1, 40
    MOVI R2, 2
    ADD  R1, R2
    LEA  R3, counter
    STORE [R3 + 0], R1
    CALL local_helper
    JMP  finish
local_helper:
    LOAD R4, [R3 + 0]
    INC  R4
    RET
finish:
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 7
    SYSCALL 1
    HALT
)";

/// The same program without the external references, so it can be linked.
constexpr std::string_view kLinkableSource = R"(
.global _start

.rodata
message:
    .asciz "golden\n"

.data
counter:
    .qword 0

.text
_start:
    MOVI R1, 40
    MOVI R2, 2
    ADD  R1, R2
    LEA  R3, counter
    STORE [R3 + 0], R1
    CALL local_helper
    JMP  finish
local_helper:
    LOAD R4, [R3 + 0]
    INC  R4
    RET
finish:
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 7
    SYSCALL 1
    HALT
)";

std::vector<u8> assembleBytes(std::string_view source, optimizer::OptLevel level,
                              std::string name) {
    const testkit::Assembled assembled = testkit::assemble(source, level, std::move(name));
    EXPECT_TRUE(assembled.ok) << assembled.diagnostics << assembled.error;
    std::vector<u8> bytes;
    EXPECT_TRUE(object::writeObjectToBuffer(assembled.object, bytes).has_value());
    return bytes;
}

std::vector<u8> linkBytes(std::string_view source, optimizer::OptLevel level, std::string name) {
    const testkit::Assembled assembled = testkit::assemble(source, level, std::move(name));
    EXPECT_TRUE(assembled.ok) << assembled.diagnostics << assembled.error;
    const std::vector<object::ObjectFile> objects{assembled.object};
    const testkit::Linked linked = testkit::link(objects);
    EXPECT_TRUE(linked.ok) << linked.error;
    std::vector<u8> bytes;
    EXPECT_TRUE(executable::writeExecutableToBuffer(linked.executable, bytes).has_value());
    return bytes;
}

TEST(Golden, ObjectFileBytes) {
    checkGolden("golden.mobj", assembleBytes(kGoldenSource, optimizer::OptLevel::O0, "golden.asm"));
}

TEST(Golden, ExecutableBytes) {
    checkGolden("golden.mexe", linkBytes(kLinkableSource, optimizer::OptLevel::O0, "golden.asm"));
}

TEST(Golden, OptimizedExecutableBytes) {
    checkGolden("golden-o1.mexe",
                linkBytes(kLinkableSource, optimizer::OptLevel::O1, "golden.asm"));
}

TEST(Golden, TheGoldenProgramStillRuns) {
    // A fixture that no longer works would be a very quiet kind of rot.
    const testkit::Ran ran = testkit::runSource(kLinkableSource);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.output, "golden\n");
    EXPECT_EQ(ran.reg(4), 43U);
}

TEST(Golden, InstructionEncodingsAreFrozen) {
    // Spot-checks of the encoding itself, written out by hand from
    // docs/isa.md §3. If one of these changes, every .mobj ever produced by
    // this toolchain has changed meaning.
    struct Case {
        isa::Instruction instruction;
        u64 word;
    };
    const std::array<Case, 8> cases{{
        {isa::Instruction::none(isa::Opcode::NOP), 0x0000'0000'0000'0000ULL},
        {isa::Instruction::none(isa::Opcode::HALT), 0x0100'0000'0000'0000ULL},
        {isa::Instruction::syscall(1), 0x0200'0000'0000'0001ULL},
        {isa::Instruction::reg2(isa::Opcode::MOV, isa::Reg::R1, isa::Reg::R2),
         0x1012'0000'0000'0000ULL},
        {isa::Instruction::regImm(isa::Opcode::MOVI, isa::Reg::R1, 42), 0x1110'0000'0000'002AULL},
        {isa::Instruction::reg2(isa::Opcode::ADD, isa::Reg::R1, isa::Reg::R2),
         0x2012'0000'0000'0000ULL},
        {isa::Instruction::mem(isa::Opcode::LOAD, isa::Reg::R1, isa::Reg::R2, 8),
         0x1212'0000'0000'0008ULL},
        // The displacement occupies only the low 48 bits; the register nibbles
        // above it stay zero even for a negative value.
        {isa::Instruction::jump(isa::Opcode::JMP, -8), 0x5000'FFFF'FFFF'FFF8ULL},
    }};
    for (const Case& test : cases) {
        const std::expected<u64, isa::EncodeError> encoded = isa::encode(test.instruction);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(*encoded, test.word) << isa::toString(test.instruction);
        const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decode(test.word);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, test.instruction);
    }
}

TEST(Golden, DisassemblyText) {
    // The listing is a user-facing format too, so it is worth pinning.
    const testkit::Linked linked = testkit::build(
        ".global _start\n_start:\n    MOVI R1, 42\n    CALL helper\n    HALT\n"
        "helper:\n    RET\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const std::string listing = disassembler::Disassembler{}.disassemble(linked.executable);
    const std::string expected =
        "segment .text at 0x10000 (32 bytes, r-x)\n"
        "\n"
        "0000000000010000 <_start>:\n"
        "0000000000010000:  111000000000002A  MOVI R1, 42\n"
        "0000000000010008:  5700000000000008  CALL    helper (0x10018)\n"
        "0000000000010010:  0100000000000000  HALT\n"
        "\n"
        "0000000000010018 <helper>:\n"
        "0000000000010018:  5800000000000000  RET\n";
    EXPECT_EQ(listing, expected);
}

}  // namespace
