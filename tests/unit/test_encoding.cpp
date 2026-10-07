// SPDX-License-Identifier: MIT
#include <array>
#include "support/test_framework.hpp"

#include "minitool/isa/encoding.hpp"

namespace {

using namespace minitool;
using namespace minitool::isa;

/// Builds a canonical example instruction for `info`, so tests can cover every
/// opcode without hand-writing 36 cases.
Instruction sampleFor(const OpcodeInfo& info) {
    switch (info.format) {
        case Format::None:
            return Instruction::none(info.opcode);
        case Format::Reg1:
            return Instruction::reg1(info.opcode, Reg::R7);
        case Format::Reg2:
            return Instruction::reg2(info.opcode, Reg::R3, Reg::R11);
        case Format::RegImm:
            return Instruction::regImm(info.opcode, Reg::R5, -1234);
        case Format::Mem:
            return Instruction::mem(info.opcode, Reg::R2, Reg::R9, 64);
        case Format::Jump:
            return Instruction::jump(info.opcode, -128);
        case Format::SysImm:
            return Instruction::syscall(3);
    }
    return Instruction::none(Opcode::NOP);
}

TEST(Encoding, LayoutMatchesTheFrozenSpecification) {
    // MOVI R1, 10 -> opcode 0x11, dst 1, src 0, imm 10
    const auto word = encode(Instruction::regImm(Opcode::MOVI, Reg::R1, 10));
    ASSERT_TRUE(word.has_value());
    EXPECT_EQ(*word, 0x1110'0000'0000'000AULL);

    // ADD R1, R2 -> opcode 0x20, dst 1, src 2
    const auto add = encode(Instruction::reg2(Opcode::ADD, Reg::R1, Reg::R2));
    ASSERT_TRUE(add.has_value());
    EXPECT_EQ(*add, 0x2012'0000'0000'0000ULL);

    // HALT -> opcode 0x01, everything else zero
    const auto halt = encode(Instruction::none(Opcode::HALT));
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(*halt, 0x0100'0000'0000'0000ULL);

    // JMP -8 (a self-referencing loop is -16; -8 jumps to itself+0)
    const auto jmp = encode(Instruction::jump(Opcode::JMP, -8));
    ASSERT_TRUE(jmp.has_value());
    EXPECT_EQ(*jmp, 0x5000'FFFF'FFFF'FFF8ULL);
}

TEST(Encoding, RoundTripsEveryOpcode) {
    for (const OpcodeInfo& info : allOpcodes()) {
        const Instruction original = sampleFor(info);
        const auto word = encode(original);
        ASSERT_TRUE(word.has_value()) << info.mnemonic << ": " << encodeErrorName(word.error());
        const auto decoded = decode(*word);
        ASSERT_TRUE(decoded.has_value())
            << info.mnemonic << ": " << decodeErrorName(decoded.error());
        EXPECT_EQ(*decoded, original) << info.mnemonic << " -> " << toString(*decoded);
    }
}

TEST(Encoding, ExtremeImmediatesSurviveRoundTrip) {
    constexpr i64 kMax = (i64{1} << 47) - 1;
    constexpr i64 kMin = -(i64{1} << 47);
    for (const i64 imm : {kMin, kMin + 1, i64{-1}, i64{0}, i64{1}, kMax - 1, kMax}) {
        const Instruction original = Instruction::regImm(Opcode::MOVI, Reg::R15, imm);
        const auto word = encode(original);
        ASSERT_TRUE(word.has_value()) << imm;
        const auto decoded = decode(*word);
        ASSERT_TRUE(decoded.has_value()) << imm;
        EXPECT_EQ(decoded->imm, imm);
    }
}

TEST(Encoding, RejectsImmediatesOutsideThe48BitField) {
    constexpr i64 kMax = (i64{1} << 47) - 1;
    constexpr i64 kMin = -(i64{1} << 47);
    EXPECT_EQ(encode(Instruction::regImm(Opcode::MOVI, Reg::R1, kMax + 1)).error(),
              EncodeError::ImmediateOutOfRange);
    EXPECT_EQ(encode(Instruction::regImm(Opcode::MOVI, Reg::R1, kMin - 1)).error(),
              EncodeError::ImmediateOutOfRange);
    EXPECT_EQ(encode(Instruction::jump(Opcode::CALL, kMax + 1)).error(),
              EncodeError::ImmediateOutOfRange);
    EXPECT_EQ(encode(Instruction::mem(Opcode::LOAD, Reg::R1, Reg::R2, kMin - 1)).error(),
              EncodeError::ImmediateOutOfRange);
}

TEST(Encoding, SyscallNumberIsUnsigned) {
    EXPECT_TRUE(encode(Instruction::syscall(0)).has_value());
    EXPECT_TRUE(encode(Instruction::syscall((u64{1} << 48) - 1U)).has_value());
    EXPECT_EQ(encode(Instruction::syscall(u64{1} << 48)).error(), EncodeError::ImmediateOutOfRange);

    Instruction negative = Instruction::syscall(0);
    negative.imm = -1;
    EXPECT_EQ(encode(negative).error(), EncodeError::ImmediateOutOfRange);

    // A syscall number with the high field bit set stays positive on decode.
    const auto word = encode(Instruction::syscall(u64{1} << 47));
    ASSERT_TRUE(word.has_value());
    const auto decoded = decode(*word);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->imm, i64{1} << 47);
}

TEST(Encoding, RejectsUnknownOpcodes) {
    Instruction bogus;
    bogus.opcode = static_cast<Opcode>(0x7F);
    EXPECT_EQ(encode(bogus).error(), EncodeError::UnknownOpcode);
    EXPECT_EQ(decode(u64{0x7F} << 56).error(), DecodeError::UnknownOpcode);
}

TEST(Encoding, RejectsOutOfRangeRegisters) {
    Instruction instruction = Instruction::reg2(Opcode::ADD, Reg::R1, Reg::R2);
    instruction.src = static_cast<Reg>(200);
    EXPECT_EQ(encode(instruction).error(), EncodeError::InvalidRegister);
}

TEST(Encoding, RejectsNonCanonicalOperandFields) {
    Instruction halt = Instruction::none(Opcode::HALT);
    halt.dst = Reg::R1;
    EXPECT_EQ(encode(halt).error(), EncodeError::NonCanonicalOperand);

    Instruction push = Instruction::reg1(Opcode::PUSH, Reg::R1);
    push.src = Reg::R2;
    EXPECT_EQ(encode(push).error(), EncodeError::NonCanonicalOperand);

    Instruction add = Instruction::reg2(Opcode::ADD, Reg::R1, Reg::R2);
    add.imm = 5;
    EXPECT_EQ(encode(add).error(), EncodeError::NonCanonicalOperand);

    Instruction jump = Instruction::jump(Opcode::JMP, 16);
    jump.dst = Reg::R4;
    EXPECT_EQ(encode(jump).error(), EncodeError::NonCanonicalOperand);
}

TEST(Encoding, DecodeRejectsReservedBitsInsteadOfIgnoringThem) {
    // HALT with a stray dst field.
    EXPECT_EQ(decode(0x0110'0000'0000'0000ULL).error(), DecodeError::ReservedRegisterFieldSet);
    // HALT with a stray immediate.
    EXPECT_EQ(decode(0x0100'0000'0000'0001ULL).error(), DecodeError::ReservedImmediateFieldSet);
    // ADD with a stray immediate.
    EXPECT_EQ(decode(0x2012'0000'0000'00FFULL).error(), DecodeError::ReservedImmediateFieldSet);
    // PUSH with a stray src field.
    EXPECT_EQ(decode(0x1512'0000'0000'0000ULL).error(), DecodeError::ReservedRegisterFieldSet);
}

TEST(Encoding, BufferHelpersAreBoundsChecked) {
    std::array<u8, 8> buffer{};
    ASSERT_TRUE(encodeInto(buffer, Instruction::none(Opcode::NOP)).has_value());
    const std::array<u8, 8> expected{0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(buffer, expected);

    ASSERT_TRUE(encodeInto(buffer, Instruction::regImm(Opcode::MOVI, Reg::R1, 10)).has_value());
    // Little-endian: low byte first.
    EXPECT_EQ(buffer[0], 0x0A);
    EXPECT_EQ(buffer[7], 0x11);

    const auto decoded = decodeFrom(buffer);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->opcode, Opcode::MOVI);
    EXPECT_EQ(decoded->imm, 10);

    std::array<u8, 4> small{};
    EXPECT_EQ(encodeInto(small, Instruction::none(Opcode::NOP)).error(),
              EncodeError::BufferTooSmall);
    EXPECT_EQ(decodeFrom(small).error(), DecodeError::Truncated);
    EXPECT_EQ(decodeFrom(std::span<const u8>{}).error(), DecodeError::Truncated);
}

TEST(Encoding, BranchTargetAndDisplacementAreInverses) {
    constexpr Address kInstruction = 0x0001'0000ULL;
    constexpr Address kTarget = 0x0001'0080ULL;
    const i64 displacement = branchDisplacement(kInstruction, kTarget);
    EXPECT_EQ(displacement, 0x78);  // target - (instruction + 8)
    EXPECT_EQ(branchTarget(kInstruction, displacement), kTarget);

    // Backwards branch.
    const i64 back = branchDisplacement(kTarget, kInstruction);
    EXPECT_EQ(back, -0x88);
    EXPECT_EQ(branchTarget(kTarget, back), kInstruction);

    // A branch to itself is -8, not 0.
    EXPECT_EQ(branchDisplacement(kInstruction, kInstruction), -8);
}

TEST(Encoding, ToStringRendersOperandsReadably) {
    EXPECT_EQ(toString(Instruction::none(Opcode::HALT)), "HALT");
    EXPECT_EQ(toString(Instruction::reg1(Opcode::PUSH, Reg::R1)), "PUSH R1");
    EXPECT_EQ(toString(Instruction::reg2(Opcode::ADD, Reg::R1, Reg::R2)), "ADD R1, R2");
    EXPECT_EQ(toString(Instruction::regImm(Opcode::MOVI, Reg::R1, 10)), "MOVI R1, 10");
    EXPECT_EQ(toString(Instruction::mem(Opcode::LOAD, Reg::R1, Reg::R2, 8)), "LOAD R1, [R2 + 8]");
    EXPECT_EQ(toString(Instruction::mem(Opcode::LOAD, Reg::R1, Reg::R2, -8)), "LOAD R1, [R2 - 8]");
    EXPECT_EQ(toString(Instruction::mem(Opcode::STORE, Reg::R13, Reg::R4, -16)),
              "STORE [R13 - 16], R4");
    EXPECT_EQ(toString(Instruction::syscall(1)), "SYSCALL 1");
}

}  // namespace
