// SPDX-License-Identifier: MIT
//
// Property tests for the instruction codec. These use a fixed seed so a failure
// is always reproducible; the seed is printed on failure.

#include <array>
#include <random>
#include "support/test_framework.hpp"

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"

namespace {

using namespace minitool;
using namespace minitool::isa;

constexpr u64 kSeed = 0x00C0'FFEEULL;
constexpr i64 kImmMax = (i64{1} << 47) - 1;
constexpr i64 kImmMin = -(i64{1} << 47);

Reg randomRegister(std::mt19937_64& rng) {
    std::uniform_int_distribution<unsigned> dist(0, kRegisterCount - 1);
    return static_cast<Reg>(static_cast<u8>(dist(rng)));
}

i64 randomSignedImmediate(std::mt19937_64& rng) {
    std::uniform_int_distribution<i64> dist(kImmMin, kImmMax);
    return dist(rng);
}

Instruction randomInstruction(std::mt19937_64& rng) {
    const std::span<const OpcodeInfo> table = allOpcodes();
    std::uniform_int_distribution<std::size_t> pick(0, table.size() - 1);
    const OpcodeInfo& info = table[pick(rng)];
    switch (info.format) {
        case Format::None:
            return Instruction::none(info.opcode);
        case Format::Reg1:
            return Instruction::reg1(info.opcode, randomRegister(rng));
        case Format::Reg2:
            return Instruction::reg2(info.opcode, randomRegister(rng), randomRegister(rng));
        case Format::RegImm:
            return Instruction::regImm(info.opcode, randomRegister(rng),
                                       randomSignedImmediate(rng));
        case Format::Mem:
            return Instruction::mem(info.opcode, randomRegister(rng), randomRegister(rng),
                                    randomSignedImmediate(rng));
        case Format::Jump:
            return Instruction::jump(info.opcode, randomSignedImmediate(rng));
        case Format::SysImm: {
            std::uniform_int_distribution<u64> dist(0, (u64{1} << 48) - 1U);
            return Instruction::syscall(dist(rng));
        }
    }
    return Instruction::none(Opcode::NOP);
}

// Property: decode(encode(i)) == i for every canonical instruction.
TEST(EncodingProperties, DecodeUndoesEncode) {
    std::mt19937_64 rng(kSeed);
    for (int i = 0; i < 200'000; ++i) {
        const Instruction original = randomInstruction(rng);
        const auto word = encode(original);
        ASSERT_TRUE(word.has_value())
            << "seed=" << kSeed << " iteration=" << i << " " << toString(original);
        const auto decoded = decode(*word);
        ASSERT_TRUE(decoded.has_value())
            << "seed=" << kSeed << " iteration=" << i << " word=" << *word;
        ASSERT_EQ(*decoded, original) << "seed=" << kSeed << " iteration=" << i;
    }
}

// Property: encode(decode(w)) == w for every word that decodes. Together with
// the previous test this makes the codec a bijection between valid words and
// canonical instructions -- there is exactly one encoding per instruction.
TEST(EncodingProperties, EncodeUndoesDecodeForAcceptedWords) {
    std::mt19937_64 rng(kSeed + 1);
    std::size_t accepted = 0;
    for (int i = 0; i < 200'000; ++i) {
        // Bias the opcode byte towards defined opcodes, otherwise almost every
        // random word is rejected and the property is never exercised.
        const std::span<const OpcodeInfo> table = allOpcodes();
        std::uniform_int_distribution<std::size_t> pick(0, table.size() - 1);
        const u64 opcode_bits = static_cast<u64>(table[pick(rng)].opcode) << kOpcodeShift;
        const u64 payload = rng() & ((u64{1} << kOpcodeShift) - 1U);
        const u64 word = opcode_bits | payload;

        const auto decoded = decode(word);
        if (!decoded.has_value()) {
            continue;  // reserved-field violations are expected and fine
        }
        ++accepted;
        const auto reencoded = encode(*decoded);
        ASSERT_TRUE(reencoded.has_value()) << "seed=" << kSeed << " word=" << word;
        ASSERT_EQ(*reencoded, word) << "seed=" << kSeed << " iteration=" << i;
    }
    EXPECT_GT(accepted, 1000U) << "the property barely exercised anything";
}

// Property: no 64-bit word can crash, hang or trip a sanitizer in the decoder.
TEST(EncodingProperties, DecoderNeverMisbehavesOnArbitraryInput) {
    std::mt19937_64 rng(kSeed + 2);
    for (int i = 0; i < 500'000; ++i) {
        const u64 word = rng();
        const auto decoded = decode(word);
        if (decoded.has_value()) {
            // Anything accepted must be a defined opcode with in-range registers.
            EXPECT_TRUE(isValidOpcode(decoded->opcode));
            EXPECT_TRUE(isValidRegister(decoded->dst));
            EXPECT_TRUE(isValidRegister(decoded->src));
            (void)toString(*decoded);
        }
    }
}

// Property: exactly the defined opcodes accept an all-zero payload.
TEST(EncodingProperties, OnlyDefinedOpcodesDecode) {
    std::size_t accepted = 0;
    for (unsigned byte = 0; byte < 256; ++byte) {
        const u64 word = static_cast<u64>(byte) << kOpcodeShift;
        const auto decoded = decode(word);
        const bool defined = isValidOpcode(static_cast<Opcode>(static_cast<u8>(byte)));
        EXPECT_EQ(decoded.has_value(), defined) << "opcode byte 0x" << std::hex << byte;
        if (decoded.has_value()) {
            ++accepted;
        } else {
            EXPECT_EQ(decoded.error(), DecodeError::UnknownOpcode);
        }
    }
    EXPECT_EQ(accepted, allOpcodes().size());
}

// Property: the byte-buffer helpers agree with the word-level codec.
TEST(EncodingProperties, ByteBufferRoundTripMatchesWordRoundTrip) {
    std::mt19937_64 rng(kSeed + 3);
    std::array<u8, kInstructionSize> buffer{};
    for (int i = 0; i < 50'000; ++i) {
        const Instruction original = randomInstruction(rng);
        ASSERT_TRUE(encodeInto(buffer, original).has_value());
        const auto decoded = decodeFrom(buffer);
        ASSERT_TRUE(decoded.has_value());
        ASSERT_EQ(*decoded, original);

        const auto word = encode(original);
        ASSERT_TRUE(word.has_value());
        EXPECT_EQ(byteorder::load<u64>(buffer), *word);
    }
}

// Property: branch displacement and target are exact inverses across the range.
TEST(EncodingProperties, BranchArithmeticIsInvertible) {
    std::mt19937_64 rng(kSeed + 4);
    std::uniform_int_distribution<u64> addr(0, 0x0000'FFFF'FFFF'FFFFULL);
    for (int i = 0; i < 100'000; ++i) {
        const Address from = addr(rng) & ~u64{7};
        const Address to = addr(rng) & ~u64{7};
        const i64 displacement = branchDisplacement(from, to);
        ASSERT_EQ(branchTarget(from, displacement), to);
    }
}

}  // namespace
