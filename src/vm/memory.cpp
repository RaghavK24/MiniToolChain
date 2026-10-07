// SPDX-License-Identifier: MIT
#include "minitool/vm/memory.hpp"

#include <algorithm>
#include <format>
#include <limits>

#include "minitool/common/byte_order.hpp"

namespace minitool::vm {
namespace {

[[nodiscard]] std::string_view kindName(MemoryErrorKind kind) noexcept {
    switch (kind) {
        case MemoryErrorKind::Unmapped:
            return "address is not mapped";
        case MemoryErrorKind::ReadDenied:
            return "region is not readable";
        case MemoryErrorKind::WriteDenied:
            return "region is not writable";
        case MemoryErrorKind::ExecuteDenied:
            return "region is not executable";
        case MemoryErrorKind::CrossesRegionEnd:
            return "access runs past the end of its region";
        case MemoryErrorKind::OutOfMemory:
            return "the heap is exhausted";
    }
    return "invalid access";
}

}  // namespace

std::string permissionsToString(Permission permissions) {
    std::string text;
    text.push_back(hasPermission(permissions, Permission::Read) ? 'r' : '-');
    text.push_back(hasPermission(permissions, Permission::Write) ? 'w' : '-');
    text.push_back(hasPermission(permissions, Permission::Exec) ? 'x' : '-');
    return text;
}

std::string MemoryFault::describe() const {
    return std::format("{} (address 0x{:016X}, {} byte{})", kindName(kind), address, size,
                       size == 1 ? "" : "s");
}

bool VirtualMemory::addRegion(std::string name, u64 base, u64 size, Permission permissions,
                              std::span<const u8> initial) {
    if (size == 0 || base > std::numeric_limits<u64>::max() - size) {
        return false;
    }
    for (const Region& region : regions_) {
        if (base < region.base + region.size && region.base < base + size) {
            return false;
        }
    }
    Region region;
    region.name = std::move(name);
    region.base = base;
    region.size = size;
    region.permissions = permissions;
    region.data.assign(static_cast<std::size_t>(size), u8{0});
    const std::size_t copied = std::min<std::size_t>(initial.size(), region.data.size());
    std::copy_n(initial.begin(), copied, region.data.begin());
    if (region.name == "heap") {
        heap_next_ = base;
    }
    regions_.push_back(std::move(region));
    return true;
}

void VirtualMemory::reset() {
    regions_.clear();
    heap_next_ = 0;
}

const VirtualMemory::Region* VirtualMemory::regionAt(u64 address) const noexcept {
    for (const Region& region : regions_) {
        if (address >= region.base && address - region.base < region.size) {
            return &region;
        }
    }
    return nullptr;
}

MemoryResult<const VirtualMemory::Region*> VirtualMemory::checkAccess(u64 address, u64 size,
                                                                      Permission needed) const {
    // Precondition: size > 0. Every caller either passes a nonzero constant
    // (1 or sizeof(u64)) or has already short-circuited an empty access
    // before reaching here (readBytes, writeBytes). That makes the pointer
    // this function returns on success unconditionally non-null, provable
    // from this function's own body alone: regionAt's null check is the only
    // early return, and `region` is never reassigned after it. A `size == 0`
    // special case used to return a null pointer on success instead, which
    // meant every caller's safety depended on the optimizer tracing constant
    // arguments back through this call to rule that branch out -- GCC 14
    // managed to, GCC 13 did not, and the difference showed up as a
    // -Wnull-dereference false positive in writeByte/writeU64/writeBytes
    // that was toolchain-version-dependent.
    const Region* region = regionAt(address);
    if (region == nullptr) {
        return std::unexpected(MemoryFault{MemoryErrorKind::Unmapped, address, size});
    }
    const u64 offset = address - region->base;
    if (size > region->size - offset) {
        return std::unexpected(MemoryFault{MemoryErrorKind::CrossesRegionEnd, address, size});
    }
    if (!hasPermission(region->permissions, needed)) {
        MemoryErrorKind kind = MemoryErrorKind::ReadDenied;
        if (needed == Permission::Write) {
            kind = MemoryErrorKind::WriteDenied;
        } else if (needed == Permission::Exec) {
            kind = MemoryErrorKind::ExecuteDenied;
        }
        return std::unexpected(MemoryFault{kind, address, size});
    }
    return region;
}

MemoryResult<u8> VirtualMemory::readByte(u64 address) const {
    const MemoryResult<const Region*> region = checkAccess(address, 1, Permission::Read);
    if (!region.has_value()) {
        return std::unexpected(region.error());
    }
    return (*region)->data[static_cast<std::size_t>(address - (*region)->base)];
}

MemoryResult<u64> VirtualMemory::readU64(u64 address) const {
    const MemoryResult<const Region*> region = checkAccess(address, sizeof(u64), Permission::Read);
    if (!region.has_value()) {
        return std::unexpected(region.error());
    }
    const std::size_t offset = static_cast<std::size_t>(address - (*region)->base);
    return byteorder::load<u64>(std::span<const u8>{(*region)->data}.subspan(offset, sizeof(u64)));
}

MemoryResult<u64> VirtualMemory::fetchInstruction(u64 address) const {
    const MemoryResult<const Region*> region = checkAccess(address, sizeof(u64), Permission::Exec);
    if (!region.has_value()) {
        return std::unexpected(region.error());
    }
    const std::size_t offset = static_cast<std::size_t>(address - (*region)->base);
    return byteorder::load<u64>(std::span<const u8>{(*region)->data}.subspan(offset, sizeof(u64)));
}

MemoryResult<void> VirtualMemory::writeByte(u64 address, u8 value) {
    const MemoryResult<const Region*> checked = checkAccess(address, 1, Permission::Write);
    if (!checked.has_value()) {
        return std::unexpected(checked.error());
    }
    // Reuse the region checkAccess already located rather than searching again:
    // a second, independent lookup gives the optimizer no way to see that it
    // cannot fail, which is what produced a false-positive null-dereference
    // warning under GCC 14 at -O3.
    Region* region = const_cast<Region*>(*checked);
    region->data[static_cast<std::size_t>(address - region->base)] = value;
    return {};
}

MemoryResult<void> VirtualMemory::writeU64(u64 address, u64 value) {
    const MemoryResult<const Region*> checked =
        checkAccess(address, sizeof(u64), Permission::Write);
    if (!checked.has_value()) {
        return std::unexpected(checked.error());
    }
    Region* region = const_cast<Region*>(*checked);
    const std::size_t offset = static_cast<std::size_t>(address - region->base);
    byteorder::store<u64>(std::span<u8>{region->data}.subspan(offset, sizeof(u64)), value);
    return {};
}

MemoryResult<void> VirtualMemory::readBytes(u64 address, std::span<u8> out) const {
    if (out.empty()) {
        return {};
    }
    const MemoryResult<const Region*> region = checkAccess(address, out.size(), Permission::Read);
    if (!region.has_value()) {
        return std::unexpected(region.error());
    }
    const std::size_t offset = static_cast<std::size_t>(address - (*region)->base);
    std::copy_n((*region)->data.begin() + static_cast<std::ptrdiff_t>(offset), out.size(),
                out.begin());
    return {};
}

MemoryResult<void> VirtualMemory::writeBytes(u64 address, std::span<const u8> data) {
    if (data.empty()) {
        return {};
    }
    const MemoryResult<const Region*> checked =
        checkAccess(address, data.size(), Permission::Write);
    if (!checked.has_value()) {
        return std::unexpected(checked.error());
    }
    Region* region = const_cast<Region*>(*checked);
    const std::size_t offset = static_cast<std::size_t>(address - region->base);
    std::copy(data.begin(), data.end(), region->data.begin() + static_cast<std::ptrdiff_t>(offset));
    return {};
}

MemoryResult<u64> VirtualMemory::allocate(u64 size) {
    const Region* heap = regionAt(heap_next_);
    if (heap == nullptr || heap->name != "heap") {
        return std::unexpected(MemoryFault{MemoryErrorKind::OutOfMemory, heap_next_, size});
    }
    // Keep every block 8-byte aligned so that allocated memory can hold u64s
    // without the program having to think about it.
    const u64 rounded = (size + 7U) & ~u64{7};
    if (rounded < size) {  // the rounding itself overflowed
        return std::unexpected(MemoryFault{MemoryErrorKind::OutOfMemory, heap_next_, size});
    }
    const u64 end = heap->base + heap->size;
    if (heap_next_ > end - rounded) {
        return std::unexpected(MemoryFault{MemoryErrorKind::OutOfMemory, heap_next_, size});
    }
    const u64 block = heap_next_;
    heap_next_ += rounded;
    return block;
}

bool VirtualMemory::isMapped(u64 address) const noexcept {
    return regionAt(address) != nullptr;
}

bool VirtualMemory::isExecutable(u64 address) const noexcept {
    const Region* region = regionAt(address);
    return region != nullptr && hasPermission(region->permissions, Permission::Exec);
}

bool VirtualMemory::isWritable(u64 address) const noexcept {
    const Region* region = regionAt(address);
    return region != nullptr && hasPermission(region->permissions, Permission::Write);
}

}  // namespace minitool::vm
