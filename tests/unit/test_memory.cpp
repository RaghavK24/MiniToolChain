// SPDX-License-Identifier: MIT
#include <array>
#include <vector>

#include "minitool/vm/memory.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using namespace minitool::vm;

VirtualMemory makeMemory() {
    VirtualMemory memory;
    EXPECT_TRUE(memory.addRegion("text", 0x1000, 0x100, Permission::Read | Permission::Exec));
    EXPECT_TRUE(memory.addRegion("data", 0x2000, 0x100, Permission::Read | Permission::Write));
    EXPECT_TRUE(memory.addRegion("rodata", 0x3000, 0x100, Permission::Read));
    return memory;
}

TEST(Memory, ReadsAndWritesWithinARegion) {
    VirtualMemory memory = makeMemory();
    ASSERT_TRUE(memory.writeU64(0x2000, 0x1122'3344'5566'7788U).has_value());
    EXPECT_EQ(memory.readU64(0x2000).value(), 0x1122'3344'5566'7788U);
    // Little-endian, in memory as well as in files.
    EXPECT_EQ(memory.readByte(0x2000).value(), 0x88U);
    EXPECT_EQ(memory.readByte(0x2007).value(), 0x11U);
}

TEST(Memory, CopiesInitialContents) {
    VirtualMemory memory;
    const std::array<u8, 3> initial{1, 2, 3};
    ASSERT_TRUE(memory.addRegion("data", 0x100, 8, Permission::Read, initial));
    EXPECT_EQ(memory.readByte(0x100).value(), 1U);
    EXPECT_EQ(memory.readByte(0x102).value(), 3U);
    // The rest of the region is zero-filled.
    EXPECT_EQ(memory.readByte(0x103).value(), 0U);
}

TEST(Memory, RejectsUnmappedAddresses) {
    const VirtualMemory memory = makeMemory();
    const MemoryResult<u8> read = memory.readByte(0x9999);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, MemoryErrorKind::Unmapped);
    EXPECT_EQ(read.error().address, 0x9999U);
    EXPECT_FALSE(memory.isMapped(0x9999));
    EXPECT_TRUE(memory.isMapped(0x2000));
}

TEST(Memory, EnforcesPermissions) {
    VirtualMemory memory = makeMemory();
    EXPECT_EQ(memory.writeByte(0x3000, 1).error().kind, MemoryErrorKind::WriteDenied);
    EXPECT_EQ(memory.fetchInstruction(0x2000).error().kind, MemoryErrorKind::ExecuteDenied);
    EXPECT_TRUE(memory.fetchInstruction(0x1000).has_value());
    EXPECT_TRUE(memory.isExecutable(0x1000));
    EXPECT_FALSE(memory.isExecutable(0x2000));
    EXPECT_TRUE(memory.isWritable(0x2000));
    EXPECT_FALSE(memory.isWritable(0x3000));
}

TEST(Memory, RejectsAccessesThatRunOffTheEndOfARegion) {
    VirtualMemory memory = makeMemory();
    // The last byte is readable, but a 64-bit read starting there is not.
    EXPECT_TRUE(memory.readByte(0x20FF).has_value());
    const MemoryResult<u64> read = memory.readU64(0x20FC);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, MemoryErrorKind::CrossesRegionEnd);

    std::vector<u8> buffer(16);
    EXPECT_FALSE(memory.readBytes(0x20F8, buffer).has_value());
    EXPECT_FALSE(memory.writeBytes(0x20F8, buffer).has_value());
}

TEST(Memory, NeverStraddlesTwoRegions) {
    VirtualMemory memory;
    ASSERT_TRUE(memory.addRegion("a", 0x100, 8, Permission::Read | Permission::Write));
    ASSERT_TRUE(memory.addRegion("b", 0x108, 8, Permission::Read | Permission::Write));
    // Adjacent regions are still separate: an access may not span them.
    EXPECT_FALSE(memory.readU64(0x104).has_value());
}

TEST(Memory, RejectsOverlappingAndDegenerateRegions) {
    VirtualMemory memory;
    EXPECT_TRUE(memory.addRegion("a", 0x100, 0x100, Permission::Read));
    EXPECT_FALSE(memory.addRegion("b", 0x180, 0x100, Permission::Read));
    EXPECT_FALSE(memory.addRegion("empty", 0x400, 0, Permission::Read));
    // A region that would wrap the address space is refused.
    EXPECT_FALSE(memory.addRegion("wrap", ~u64{0} - 4, 16, Permission::Read));
}

TEST(Memory, TransfersBlocksOfBytes) {
    VirtualMemory memory = makeMemory();
    const std::array<u8, 4> source{9, 8, 7, 6};
    ASSERT_TRUE(memory.writeBytes(0x2010, source).has_value());
    std::array<u8, 4> destination{};
    ASSERT_TRUE(memory.readBytes(0x2010, destination).has_value());
    EXPECT_EQ(destination, source);
    // Empty transfers are trivially fine, even at an unmapped address.
    EXPECT_TRUE(memory.readBytes(0x9999, std::span<u8>{}).has_value());
}

TEST(Memory, AllocatesFromTheHeapRegion) {
    VirtualMemory memory;
    ASSERT_TRUE(memory.addRegion("heap", 0x5000, 64, Permission::Read | Permission::Write));
    const MemoryResult<u64> first = memory.allocate(1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0x5000U);
    const MemoryResult<u64> second = memory.allocate(8);
    ASSERT_TRUE(second.has_value());
    // Blocks are 8-byte aligned, so a 1-byte request still advances by 8.
    EXPECT_EQ(*second, 0x5008U);
    EXPECT_TRUE(memory.writeU64(*second, 42).has_value());
}

TEST(Memory, ReportsHeapExhaustion) {
    VirtualMemory memory;
    ASSERT_TRUE(memory.addRegion("heap", 0x5000, 16, Permission::Read | Permission::Write));
    EXPECT_TRUE(memory.allocate(16).has_value());
    const MemoryResult<u64> failed = memory.allocate(8);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind, MemoryErrorKind::OutOfMemory);
    // A size that would overflow the rounding is refused, not wrapped.
    EXPECT_FALSE(memory.allocate(~u64{0}).has_value());
}

TEST(Memory, AllocateFailsWithoutAHeap) {
    VirtualMemory memory = makeMemory();
    EXPECT_FALSE(memory.allocate(8).has_value());
}

TEST(Memory, ResetClearsEverything) {
    VirtualMemory memory = makeMemory();
    memory.reset();
    EXPECT_TRUE(memory.regions().empty());
    EXPECT_FALSE(memory.readByte(0x2000).has_value());
}

TEST(Memory, FaultsDescribeThemselves) {
    const MemoryFault fault{MemoryErrorKind::WriteDenied, 0x1234, 8};
    const std::string text = fault.describe();
    EXPECT_TRUE(text.find("not writable") != std::string::npos);
    EXPECT_TRUE(text.find("1234") != std::string::npos);
    EXPECT_EQ(permissionsToString(Permission::Read | Permission::Exec), "r-x");
    EXPECT_EQ(permissionsToString(Permission::None), "---");
}

}  // namespace
