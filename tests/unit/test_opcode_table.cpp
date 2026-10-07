// SPDX-License-Identifier: MIT
#include <set>
#include <string>
#include "support/test_framework.hpp"

#include "minitool/isa/opcode.hpp"

namespace {

using namespace minitool::isa;

TEST(OpcodeTable, OpcodeNumbersAreUnique) {
    std::set<unsigned> seen;
    for (const OpcodeInfo& info : allOpcodes()) {
        const unsigned value = static_cast<unsigned>(info.opcode);
        EXPECT_TRUE(seen.insert(value).second) << "duplicate opcode 0x" << std::hex << value;
    }
    EXPECT_EQ(seen.size(), allOpcodes().size());
}

TEST(OpcodeTable, MnemonicsAreUniqueUppercaseAndNonEmpty) {
    std::set<std::string> seen;
    for (const OpcodeInfo& info : allOpcodes()) {
        const std::string mnemonic{info.mnemonic};
        EXPECT_FALSE(mnemonic.empty());
        for (const char c : mnemonic) {
            EXPECT_TRUE(c >= 'A' && c <= 'Z') << mnemonic << " must be uppercase";
        }
        EXPECT_TRUE(seen.insert(mnemonic).second) << "duplicate mnemonic " << mnemonic;
    }
}

TEST(OpcodeTable, IsSortedByOpcodeNumber) {
    unsigned previous = 0;
    bool first = true;
    for (const OpcodeInfo& info : allOpcodes()) {
        const unsigned value = static_cast<unsigned>(info.opcode);
        if (!first) {
            EXPECT_LT(previous, value) << "opcode table must stay ascending";
        }
        previous = value;
        first = false;
    }
}

TEST(OpcodeTable, LookupsAgreeWithTheTable) {
    for (const OpcodeInfo& info : allOpcodes()) {
        const OpcodeInfo* by_opcode = findOpcode(info.opcode);
        ASSERT_NE(by_opcode, nullptr);
        EXPECT_EQ(by_opcode->mnemonic, info.mnemonic);

        const OpcodeInfo* by_name = findMnemonic(info.mnemonic);
        ASSERT_NE(by_name, nullptr) << info.mnemonic;
        EXPECT_EQ(by_name->opcode, info.opcode);

        EXPECT_EQ(opcodeName(info.opcode), info.mnemonic);
        EXPECT_TRUE(isValidOpcode(info.opcode));
    }
}

TEST(OpcodeTable, MnemonicLookupIsCaseInsensitive) {
    EXPECT_EQ(findMnemonic("movi")->opcode, Opcode::MOVI);
    EXPECT_EQ(findMnemonic("MoVi")->opcode, Opcode::MOVI);
    EXPECT_EQ(findMnemonic("syscall")->opcode, Opcode::SYSCALL);
}

TEST(OpcodeTable, UnknownLookupsFailCleanly) {
    EXPECT_EQ(findMnemonic("MOVQ"), nullptr);
    EXPECT_EQ(findMnemonic(""), nullptr);
    EXPECT_EQ(findMnemonic("MOV "), nullptr);
    EXPECT_EQ(findOpcode(static_cast<Opcode>(0xFF)), nullptr);
    EXPECT_FALSE(isValidOpcode(static_cast<Opcode>(0x99)));
    EXPECT_EQ(opcodeName(static_cast<Opcode>(0xFF)), "???");
}

TEST(OpcodeTable, MetadataIsInternallyConsistent) {
    for (const OpcodeInfo& info : allOpcodes()) {
        if (info.is_branch) {
            // Every control transfer is either a Jump-format branch or RET.
            EXPECT_TRUE(info.format == Format::Jump || info.opcode == Opcode::RET) << info.mnemonic;
            EXPECT_FALSE(info.writes_dst) << info.mnemonic;
        }
        if (info.format == Format::None) {
            EXPECT_FALSE(info.writes_dst) << info.mnemonic;
            EXPECT_FALSE(info.reads_dst) << info.mnemonic;
        }
    }
}

TEST(OpcodeTable, InstructionCountMatchesTheFrozenSpec) {
    // docs/isa.md freezes 36 instructions. Adding one is a spec change and must
    // update the documentation and this number together.
    EXPECT_EQ(allOpcodes().size(), 36U);
}

TEST(OpcodeTable, OperandCountMatchesFormat) {
    EXPECT_EQ(operandCount(Format::None), 0U);
    EXPECT_EQ(operandCount(Format::Reg1), 1U);
    EXPECT_EQ(operandCount(Format::Reg2), 2U);
    EXPECT_EQ(operandCount(Format::RegImm), 2U);
    EXPECT_EQ(operandCount(Format::Mem), 2U);
    EXPECT_EQ(operandCount(Format::Jump), 1U);
    EXPECT_EQ(operandCount(Format::SysImm), 1U);
}

}  // namespace
