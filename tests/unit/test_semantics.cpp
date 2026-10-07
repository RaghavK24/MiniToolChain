// SPDX-License-Identifier: MIT
#include <limits>

#include "minitool/isa/semantics.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using namespace minitool::isa;

u64 valueOf(Opcode opcode, u64 a, u64 b) {
    const std::expected<AluResult, AluError> result = evaluateBinary(opcode, a, b);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? result->value : 0;
}

u64 flagsOf(Opcode opcode, u64 a, u64 b) {
    const std::expected<AluResult, AluError> result = evaluateBinary(opcode, a, b);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? result->flags : 0;
}

TEST(Semantics, ArithmeticMatchesTheSpecification) {
    EXPECT_EQ(valueOf(Opcode::ADD, 40, 2), 42U);
    EXPECT_EQ(valueOf(Opcode::SUB, 40, 2), 38U);
    EXPECT_EQ(valueOf(Opcode::MUL, 6, 7), 42U);
    EXPECT_EQ(valueOf(Opcode::DIV, 42, 5), 8U);
    EXPECT_EQ(valueOf(Opcode::MOD, 42, 5), 2U);
}

TEST(Semantics, DivisionIsSigned) {
    const auto minus_seven = static_cast<u64>(-7);
    EXPECT_EQ(static_cast<i64>(valueOf(Opcode::DIV, minus_seven, 2)), -3);
    EXPECT_EQ(static_cast<i64>(valueOf(Opcode::MOD, minus_seven, 2)), -1);
}

TEST(Semantics, DivisionByZeroIsAnError) {
    EXPECT_EQ(evaluateBinary(Opcode::DIV, 1, 0).error(), AluError::DivisionByZero);
    EXPECT_EQ(evaluateBinary(Opcode::MOD, 1, 0).error(), AluError::DivisionByZero);
}

TEST(Semantics, MostNegativeDividedByMinusOneWrapsInsteadOfTrapping) {
    // The C++ expression INT64_MIN / -1 is undefined; the ISA defines this to
    // wrap, and the implementation must not go through the UB.
    const auto most_negative = static_cast<u64>(std::numeric_limits<i64>::min());
    EXPECT_EQ(valueOf(Opcode::DIV, most_negative, static_cast<u64>(-1)), most_negative);
    EXPECT_EQ(valueOf(Opcode::MOD, most_negative, static_cast<u64>(-1)), 0U);
}

TEST(Semantics, LogicalOperations) {
    EXPECT_EQ(valueOf(Opcode::AND, 0b1100, 0b1010), 0b1000U);
    EXPECT_EQ(valueOf(Opcode::OR, 0b1100, 0b1010), 0b1110U);
    EXPECT_EQ(valueOf(Opcode::XOR, 0b1100, 0b1010), 0b0110U);
    EXPECT_EQ(evaluateUnary(Opcode::NOT, 0).value().value, ~u64{0});
}

TEST(Semantics, ShiftsUseTheLowSixBitsOfTheCount) {
    EXPECT_EQ(valueOf(Opcode::SHL, 1, 4), 16U);
    EXPECT_EQ(valueOf(Opcode::SHR, 16, 4), 1U);
    // A count of 64 is masked to 0, which leaves the value alone.
    EXPECT_EQ(valueOf(Opcode::SHL, 1, 64), 1U);
    EXPECT_EQ(valueOf(Opcode::SHR, 1, 64), 1U);
}

TEST(Semantics, ArithmeticShiftPreservesTheSign) {
    const auto negative = static_cast<u64>(-16);
    EXPECT_EQ(static_cast<i64>(valueOf(Opcode::SAR, negative, 2)), -4);
    EXPECT_EQ(valueOf(Opcode::SHR, negative, 2), negative >> 2U);
    EXPECT_EQ(static_cast<i64>(valueOf(Opcode::SAR, negative, 63)), -1);
}

TEST(Semantics, UnaryOperations) {
    EXPECT_EQ(evaluateUnary(Opcode::INC, 41).value().value, 42U);
    EXPECT_EQ(evaluateUnary(Opcode::DEC, 43).value().value, 42U);
    EXPECT_EQ(static_cast<i64>(evaluateUnary(Opcode::NEG, 42).value().value), -42);
    // NEG of the most negative value wraps to itself, without signed overflow.
    const auto most_negative = static_cast<u64>(std::numeric_limits<i64>::min());
    EXPECT_EQ(evaluateUnary(Opcode::NEG, most_negative).value().value, most_negative);
}

TEST(Semantics, ZeroAndSignFlags) {
    EXPECT_TRUE((flagsOf(Opcode::SUB, 5, 5) & kZeroFlag) != 0);
    EXPECT_TRUE((flagsOf(Opcode::SUB, 4, 5) & kSignFlag) != 0);
    EXPECT_TRUE((flagsOf(Opcode::ADD, 1, 1) & (kZeroFlag | kSignFlag)) == 0);
}

TEST(Semantics, CarryAndOverflowOnAddition) {
    // Unsigned wrap sets CF.
    EXPECT_TRUE((flagsOf(Opcode::ADD, ~u64{0}, 1) & kCarryFlag) != 0);
    // Signed overflow sets OF: INT64_MAX + 1.
    const auto max = static_cast<u64>(std::numeric_limits<i64>::max());
    EXPECT_TRUE((flagsOf(Opcode::ADD, max, 1) & kOverflowFlag) != 0);
    EXPECT_TRUE((flagsOf(Opcode::ADD, 1, 1) & (kCarryFlag | kOverflowFlag)) == 0);
}

TEST(Semantics, BorrowAndOverflowOnSubtraction) {
    EXPECT_TRUE((flagsOf(Opcode::SUB, 1, 2) & kCarryFlag) != 0);
    EXPECT_TRUE((flagsOf(Opcode::SUB, 2, 1) & kCarryFlag) == 0);
    const auto min = static_cast<u64>(std::numeric_limits<i64>::min());
    EXPECT_TRUE((flagsOf(Opcode::SUB, min, 1) & kOverflowFlag) != 0);
}

TEST(Semantics, ComparisonsDoNotWriteAValue) {
    const AluResult compare = evaluateBinary(Opcode::CMP, 5, 5).value();
    EXPECT_FALSE(compare.writes_value);
    EXPECT_TRUE((compare.flags & kZeroFlag) != 0);

    const AluResult test = evaluateBinary(Opcode::TEST, 0b1010, 0b0101).value();
    EXPECT_FALSE(test.writes_value);
    EXPECT_TRUE((test.flags & kZeroFlag) != 0);
}

TEST(Semantics, NotIsTheOnlyAluOperationThatLeavesFlagsAlone) {
    EXPECT_FALSE(evaluateUnary(Opcode::NOT, 0).value().writes_flags);
    EXPECT_TRUE(evaluateUnary(Opcode::NEG, 0).value().writes_flags);
    EXPECT_TRUE(evaluateBinary(Opcode::AND, 0, 0).value().writes_flags);
}

TEST(Semantics, RejectsNonArithmeticOpcodes) {
    EXPECT_EQ(evaluateBinary(Opcode::JMP, 0, 0).error(), AluError::NotArithmetic);
    EXPECT_EQ(evaluateUnary(Opcode::HALT, 0).error(), AluError::NotArithmetic);
    EXPECT_FALSE(isBinaryAlu(Opcode::MOV));
    EXPECT_TRUE(isBinaryAlu(Opcode::ADD));
    EXPECT_TRUE(isUnaryAlu(Opcode::NEG));
}

TEST(Semantics, ConditionalBranchesMatchTheFlagRules) {
    // Signed comparisons: CMP a, b then the branch.
    const u64 equal = flagsOf(Opcode::CMP, 5, 5);
    const u64 less = flagsOf(Opcode::CMP, 3, 5);
    const u64 greater = flagsOf(Opcode::CMP, 7, 5);

    EXPECT_TRUE(branchTaken(Opcode::JE, equal));
    EXPECT_FALSE(branchTaken(Opcode::JE, less));
    EXPECT_TRUE(branchTaken(Opcode::JNE, less));

    EXPECT_TRUE(branchTaken(Opcode::JL, less));
    EXPECT_FALSE(branchTaken(Opcode::JL, greater));
    EXPECT_TRUE(branchTaken(Opcode::JG, greater));
    EXPECT_FALSE(branchTaken(Opcode::JG, equal));
    EXPECT_TRUE(branchTaken(Opcode::JGE, equal));
    EXPECT_TRUE(branchTaken(Opcode::JLE, equal));
    EXPECT_TRUE(branchTaken(Opcode::JLE, less));
}

TEST(Semantics, SignedComparisonWorksAcrossZero) {
    const u64 negative_vs_positive = flagsOf(Opcode::CMP, static_cast<u64>(-1), 1);
    EXPECT_TRUE(branchTaken(Opcode::JL, negative_vs_positive));
    EXPECT_FALSE(branchTaken(Opcode::JG, negative_vs_positive));

    // The case that separates signed from unsigned: the most negative value is
    // less than everything, even though its bit pattern is huge.
    const auto min = static_cast<u64>(std::numeric_limits<i64>::min());
    EXPECT_TRUE(branchTaken(Opcode::JL, flagsOf(Opcode::CMP, min, 1)));
}

TEST(Semantics, UnconditionalBranchesAlwaysTake) {
    EXPECT_TRUE(branchTaken(Opcode::JMP, 0));
    EXPECT_TRUE(branchTaken(Opcode::CALL, 0));
    EXPECT_FALSE(branchTaken(Opcode::NOP, 0));
    EXPECT_FALSE(readsFlags(Opcode::JMP));
    EXPECT_TRUE(readsFlags(Opcode::JE));
}

}  // namespace
