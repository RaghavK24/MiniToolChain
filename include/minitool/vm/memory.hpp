// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/common/types.hpp"

namespace minitool::vm {

enum class Permission : u8 {
    None = 0,
    Read = 1,
    Write = 2,
    Exec = 4,
};

[[nodiscard]] constexpr Permission operator|(Permission a, Permission b) noexcept {
    return static_cast<Permission>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr Permission operator&(Permission a, Permission b) noexcept {
    return static_cast<Permission>(static_cast<u8>(a) & static_cast<u8>(b));
}

constexpr Permission& operator|=(Permission& a, Permission b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool hasPermission(Permission permissions, Permission wanted) noexcept {
    return (permissions & wanted) == wanted;
}

[[nodiscard]] std::string permissionsToString(Permission permissions);

/// Why a memory access failed. Each maps to a distinct runtime error, because
/// "your program touched address 0" and "your program wrote to .rodata" want
/// very different explanations.
enum class MemoryErrorKind : u8 {
    Unmapped,
    ReadDenied,
    WriteDenied,
    ExecuteDenied,
    /// The access started inside a region but ran off its end.
    CrossesRegionEnd,
    /// The heap has no room left.
    OutOfMemory,
};

struct MemoryFault {
    MemoryErrorKind kind = MemoryErrorKind::Unmapped;
    u64 address = 0;
    u64 size = 0;

    [[nodiscard]] std::string describe() const;
};

template <typename T>
using MemoryResult = std::expected<T, MemoryFault>;

/// A flat, byte-addressable address space made of named, permissioned regions.
///
/// There is no paging and no address translation: a region is a base, a size,
/// a permission set and a byte vector. Every access is bounds- and
/// permission-checked, so no program running on the VM, however malformed,
/// can read or write host memory it should not.
class VirtualMemory {
  public:
    struct Region {
        std::string name;
        u64 base = 0;
        u64 size = 0;
        Permission permissions = Permission::None;
        std::vector<u8> data;
    };

    /// Maps `size` bytes at `base`, copying `initial` into the start of the
    /// region and zeroing the rest. Returns false if the range would overlap an
    /// existing region or wrap the address space.
    bool addRegion(std::string name, u64 base, u64 size, Permission permissions,
                   std::span<const u8> initial = {});

    [[nodiscard]] MemoryResult<u8> readByte(u64 address) const;
    [[nodiscard]] MemoryResult<u64> readU64(u64 address) const;
    [[nodiscard]] MemoryResult<void> writeByte(u64 address, u8 value);
    [[nodiscard]] MemoryResult<void> writeU64(u64 address, u64 value);
    [[nodiscard]] MemoryResult<void> readBytes(u64 address, std::span<u8> out) const;
    [[nodiscard]] MemoryResult<void> writeBytes(u64 address, std::span<const u8> data);

    /// Reads 8 bytes with execute permission rather than read permission.
    [[nodiscard]] MemoryResult<u64> fetchInstruction(u64 address) const;

    /// Bump-allocates from the region named "heap". Returns the address of the
    /// block, which is 8-byte aligned.
    [[nodiscard]] MemoryResult<u64> allocate(u64 size);

    [[nodiscard]] bool isMapped(u64 address) const noexcept;
    [[nodiscard]] bool isExecutable(u64 address) const noexcept;
    [[nodiscard]] bool isWritable(u64 address) const noexcept;
    [[nodiscard]] std::span<const Region> regions() const noexcept { return regions_; }
    /// The region containing `address`, or nullptr.
    [[nodiscard]] const Region* regionAt(u64 address) const noexcept;

    void reset();

  private:
    /// Validates an access of `size` bytes at `address` against `needed`.
    [[nodiscard]] MemoryResult<const Region*> checkAccess(u64 address, u64 size,
                                                          Permission needed) const;

    std::vector<Region> regions_;
    u64 heap_next_ = 0;
};

}  // namespace minitool::vm
