// SPDX-License-Identifier: MIT
#include <array>
#include "support/test_framework.hpp"

#include "minitool/common/byte_order.hpp"

namespace {

using namespace minitool;

TEST(ByteOrder, StoresLittleEndianRegardlessOfHost) {
    std::array<u8, 8> buffer{};
    byteorder::store<u64>(buffer, 0x0102'0304'0506'0708ULL);
    const std::array<u8, 8> expected{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    EXPECT_EQ(buffer, expected);
}

TEST(ByteOrder, RoundTripsAllWidths) {
    std::array<u8, 8> buffer{};
    byteorder::store<u16>(buffer, 0xBEEFU);
    EXPECT_EQ(byteorder::load<u16>(buffer), 0xBEEFU);
    byteorder::store<u32>(buffer, 0xDEAD'BEEFU);
    EXPECT_EQ(byteorder::load<u32>(buffer), 0xDEAD'BEEFU);
    byteorder::store<u64>(buffer, 0xFEED'FACE'DEAD'BEEFULL);
    EXPECT_EQ(byteorder::load<u64>(buffer), 0xFEED'FACE'DEAD'BEEFULL);
}

TEST(ByteOrder, SignExtendHandlesBoundaries) {
    EXPECT_EQ(byteorder::signExtend(0x7FFFFFFFFFFFULL, 48), 140737488355327LL);
    EXPECT_EQ(byteorder::signExtend(0x800000000000ULL, 48), -140737488355328LL);
    EXPECT_EQ(byteorder::signExtend(0xFFFFFFFFFFFFULL, 48), -1LL);
    EXPECT_EQ(byteorder::signExtend(0, 48), 0LL);
    EXPECT_EQ(byteorder::signExtend(0xFFFFFFFFFFFFFFFFULL, 64), -1LL);
    EXPECT_EQ(byteorder::signExtend(1, 1), -1LL);
}

TEST(ByteOrder, SignExtendIgnoresBitsAboveWidth) {
    // Upper garbage must not leak into the extended value.
    EXPECT_EQ(byteorder::signExtend(0xFFFF'0000'0000'000AULL, 48), 10LL);
}

TEST(ByteOrder, FitsSignedBoundaries) {
    EXPECT_TRUE(byteorder::fitsSigned(140737488355327LL, 48));
    EXPECT_FALSE(byteorder::fitsSigned(140737488355328LL, 48));
    EXPECT_TRUE(byteorder::fitsSigned(-140737488355328LL, 48));
    EXPECT_FALSE(byteorder::fitsSigned(-140737488355329LL, 48));
    EXPECT_TRUE(byteorder::fitsSigned(INT64_MIN, 64));
}

TEST(ByteOrder, FitsUnsignedBoundaries) {
    EXPECT_TRUE(byteorder::fitsUnsigned(0xFFFFFFFFFFFFULL, 48));
    EXPECT_FALSE(byteorder::fitsUnsigned(0x1000000000000ULL, 48));
    EXPECT_TRUE(byteorder::fitsUnsigned(0xFFFFFFFFFFFFFFFFULL, 64));
}

}  // namespace
