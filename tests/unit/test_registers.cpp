// SPDX-License-Identifier: MIT
#include "support/test_framework.hpp"

#include "minitool/isa/registers.hpp"

namespace {

using namespace minitool::isa;

TEST(Registers, NamesMatchIndices) {
    EXPECT_EQ(registerName(Reg::R0), "R0");
    EXPECT_EQ(registerName(Reg::R13), "R13");
    EXPECT_EQ(registerName(Reg::R15), "R15");
    for (unsigned i = 0; i < kRegisterCount; ++i) {
        const auto reg = static_cast<Reg>(static_cast<minitool::u8>(i));
        EXPECT_EQ(registerIndex(reg), i);
        EXPECT_TRUE(isValidRegister(reg));
    }
}

TEST(Registers, ParsesEveryCanonicalName) {
    for (unsigned i = 0; i < kRegisterCount; ++i) {
        const auto reg = static_cast<Reg>(static_cast<minitool::u8>(i));
        const auto parsed = parseRegister(registerName(reg));
        ASSERT_TRUE(parsed.has_value()) << "failed to parse " << registerName(reg);
        EXPECT_EQ(*parsed, reg);
    }
}

TEST(Registers, ParseIsCaseInsensitiveAndSupportsAbiAliases) {
    EXPECT_EQ(parseRegister("r7"), Reg::R7);
    EXPECT_EQ(parseRegister("R7"), Reg::R7);
    EXPECT_EQ(parseRegister("fp"), kFramePointer);
    EXPECT_EQ(parseRegister("RV"), kReturnValue);
}

TEST(Registers, RejectsMalformedNames) {
    EXPECT_FALSE(parseRegister("").has_value());
    EXPECT_FALSE(parseRegister("R").has_value());
    EXPECT_FALSE(parseRegister("R16").has_value());
    EXPECT_FALSE(parseRegister("R19").has_value());
    EXPECT_FALSE(parseRegister("R99").has_value());
    EXPECT_FALSE(parseRegister("R123").has_value());
    EXPECT_FALSE(parseRegister("R01").has_value());
    EXPECT_FALSE(parseRegister("R1x").has_value());
    EXPECT_FALSE(parseRegister("X1").has_value());
    EXPECT_FALSE(parseRegister("R-1").has_value());
    EXPECT_FALSE(parseRegister("SP").has_value());  // SP is special, not general purpose
}

TEST(Registers, OutOfRangeValueIsReportedNotTruncated) {
    const auto bogus = static_cast<Reg>(200);
    EXPECT_FALSE(isValidRegister(bogus));
    EXPECT_EQ(registerName(bogus), "R?");
}

}  // namespace
