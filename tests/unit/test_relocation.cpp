// SPDX-License-Identifier: MIT
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/object/relocation.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using namespace minitool::object;

Relocation makeRelocation(RelocationType type, u64 offset, i64 addend = 0) {
    Relocation relocation;
    relocation.type = type;
    relocation.offset = offset;
    relocation.addend = addend;
    return relocation;
}

/// Section data holding one encoded instruction, ready to be patched.
std::vector<u8> instructionBuffer(isa::Opcode opcode) {
    std::vector<u8> bytes(isa::kInstructionSize, 0);
    isa::Instruction instruction;
    instruction.opcode = opcode;
    EXPECT_TRUE(isa::encodeInto(bytes, instruction).has_value());
    return bytes;
}

TEST(Relocation, Abs64StoresTheSymbolAddressPlusAddend) {
    std::vector<u8> data(8, 0);
    const auto applied =
        applyRelocation(data, 0x1000, makeRelocation(RelocationType::ABS64, 0, 16), 0x2000);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(byteorder::load<u64>(data), 0x2010U);
}

TEST(Relocation, Abs32RejectsValuesThatDoNotFit) {
    std::vector<u8> data(4, 0);
    EXPECT_TRUE(applyRelocation(data, 0, makeRelocation(RelocationType::ABS32, 0), 0xFFFF'FFFFU)
                    .has_value());
    EXPECT_EQ(byteorder::load<u32>(data), 0xFFFF'FFFFU);

    const auto overflowed =
        applyRelocation(data, 0, makeRelocation(RelocationType::ABS32, 0), 0x1'0000'0000U);
    ASSERT_FALSE(overflowed.has_value());
    EXPECT_EQ(overflowed.error(), RelocationError::Overflow);
    // A rejected relocation must leave the buffer alone.
    EXPECT_EQ(byteorder::load<u32>(data), 0xFFFF'FFFFU);
}

TEST(Relocation, PcRel32IsRelativeToTheFieldItself) {
    std::vector<u8> data(4, 0);
    // Field at 0x1000, symbol at 0x1100 -> 0x100.
    ASSERT_TRUE(applyRelocation(data, 0x1000, makeRelocation(RelocationType::PCREL32, 0), 0x1100)
                    .has_value());
    EXPECT_EQ(static_cast<i32>(byteorder::load<u32>(data)), 0x100);

    // Backwards references are negative.
    ASSERT_TRUE(applyRelocation(data, 0x1000, makeRelocation(RelocationType::PCREL32, 0), 0x0F00)
                    .has_value());
    EXPECT_EQ(static_cast<i32>(byteorder::load<u32>(data)), -0x100);
}

TEST(Relocation, Imm48WritesIntoTheInstructionField) {
    std::vector<u8> data = instructionBuffer(isa::Opcode::LEA);
    ASSERT_TRUE(applyRelocation(data, 0x1000, makeRelocation(RelocationType::IMM48, 0, 8), 0x200000)
                    .has_value());
    const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decodeFrom(data);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->opcode, isa::Opcode::LEA);
    EXPECT_EQ(decoded->imm, 0x200008);
}

TEST(Relocation, PcRel48MatchesTheIsaBranchRule) {
    std::vector<u8> data = instructionBuffer(isa::Opcode::CALL);
    constexpr u64 kBranchAt = 0x1000;
    constexpr u64 kTarget = 0x1040;
    ASSERT_TRUE(
        applyRelocation(data, kBranchAt, makeRelocation(RelocationType::PCREL48, 0), kTarget)
            .has_value());
    const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decodeFrom(data);
    ASSERT_TRUE(decoded.has_value());
    // The displacement is relative to the *next* instruction, and the VM's own
    // branchTarget must reproduce the address the linker was aiming at.
    EXPECT_EQ(decoded->imm, isa::branchDisplacement(kBranchAt, kTarget));
    EXPECT_EQ(isa::branchTarget(kBranchAt, decoded->imm), kTarget);
}

TEST(Relocation, PcRel48HandlesBackwardBranches) {
    std::vector<u8> data = instructionBuffer(isa::Opcode::JMP);
    ASSERT_TRUE(applyRelocation(data, 0x1080, makeRelocation(RelocationType::PCREL48, 0), 0x1000)
                    .has_value());
    const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decodeFrom(data);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->imm, -0x88);
    EXPECT_EQ(isa::branchTarget(0x1080, decoded->imm), 0x1000U);
}

TEST(Relocation, RejectsOffsetsOutsideTheSection) {
    std::vector<u8> data(8, 0);
    EXPECT_EQ(applyRelocation(data, 0, makeRelocation(RelocationType::ABS64, 4), 0).error(),
              RelocationError::OutOfBounds);
    EXPECT_EQ(applyRelocation(data, 0, makeRelocation(RelocationType::ABS64, 1000), 0).error(),
              RelocationError::OutOfBounds);
    std::vector<u8> small(2, 0);
    EXPECT_EQ(applyRelocation(small, 0, makeRelocation(RelocationType::ABS32, 0), 0).error(),
              RelocationError::OutOfBounds);
}

TEST(Relocation, RejectsAnUnalignedInstructionField) {
    std::vector<u8> data(16, 0);
    EXPECT_EQ(applyRelocation(data, 0, makeRelocation(RelocationType::IMM48, 4), 0).error(),
              RelocationError::UnalignedInstructionField);
}

TEST(Relocation, RejectsAnInstructionFieldThatIsNotAnInstruction) {
    // All-ones is not a valid opcode, so the engine must refuse rather than
    // corrupt whatever the bytes really were.
    const std::vector<u8> garbage(8, 0xFF);
    std::vector<u8> data = garbage;
    EXPECT_EQ(applyRelocation(data, 0, makeRelocation(RelocationType::IMM48, 0), 0).error(),
              RelocationError::NotAnInstruction);
    EXPECT_EQ(data, garbage);
}

TEST(Relocation, RejectsInstructionFieldOverflow) {
    std::vector<u8> data = instructionBuffer(isa::Opcode::LEA);
    const auto applied =
        applyRelocation(data, 0, makeRelocation(RelocationType::IMM48, 0), u64{1} << 47U);
    ASSERT_FALSE(applied.has_value());
    EXPECT_EQ(applied.error(), RelocationError::Overflow);
}

TEST(Relocation, ComputesValuesWithoutApplyingThem) {
    EXPECT_EQ(relocatedValue(RelocationType::ABS64, 0x100, 8, 0x999), 0x108);
    EXPECT_EQ(relocatedValue(RelocationType::PCREL32, 0x100, 0, 0x80), 0x80);
    EXPECT_EQ(relocatedValue(RelocationType::PCREL48, 0x100, 0, 0x80),
              isa::branchDisplacement(0x80, 0x100));
}

TEST(Relocation, NamesAndWidthsAreDefinedForEveryType) {
    for (const RelocationType type :
         {RelocationType::ABS32, RelocationType::ABS64, RelocationType::PCREL32,
          RelocationType::IMM48, RelocationType::PCREL48}) {
        EXPECT_NE(relocationTypeName(type), "UNKNOWN");
        EXPECT_GT(relocationWidth(type), 0U);
        EXPECT_TRUE(isValidRelocationType(static_cast<u8>(type)));
    }
    EXPECT_FALSE(isValidRelocationType(200));
}

}  // namespace
