// SPDX-License-Identifier: MIT
#include "minitool/vm/syscall.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <vector>

namespace minitool::vm {
namespace {

/// Argument registers, per the calling convention in docs/isa.md §2.
constexpr std::size_t kArg1 = 1;
constexpr std::size_t kArg2 = 2;
constexpr std::size_t kArg3 = 3;
constexpr std::size_t kReturnValue = 14;

/// A single call may not move more than this much data, so that a program with
/// a wild length argument fails cleanly instead of trying to allocate 2^63
/// bytes on the host.
constexpr u64 kMaxTransfer = 1U << 20U;

}  // namespace

std::string_view syscallName(u64 number) noexcept {
    switch (static_cast<SyscallNumber>(number)) {
        case SyscallNumber::Exit:
            return "exit";
        case SyscallNumber::Write:
            return "write";
        case SyscallNumber::Read:
            return "read";
        case SyscallNumber::Allocate:
            return "allocate";
    }
    return "unknown";
}

std::expected<void, std::string> DefaultSyscallProvider::invoke(u64 number,
                                                                SyscallContext& context) {
    switch (static_cast<SyscallNumber>(number)) {
        case SyscallNumber::Exit:
            context.halt = true;
            context.exit_code = context.registers[kArg1];
            return {};
        case SyscallNumber::Write:
            return doWrite(context);
        case SyscallNumber::Read:
            return doRead(context);
        case SyscallNumber::Allocate:
            return doAllocate(context);
    }
    return std::unexpected(std::format("unknown syscall {}", number));
}

std::expected<void, std::string> DefaultSyscallProvider::doWrite(SyscallContext& context) {
    const u64 fd = context.registers[kArg1];
    const u64 address = context.registers[kArg2];
    const u64 length = context.registers[kArg3];
    if (fd != 1 && fd != 2) {
        return std::unexpected(std::format("write: unsupported file descriptor {}", fd));
    }
    if (length > kMaxTransfer) {
        return std::unexpected(
            std::format("write: length {} exceeds the {}-byte limit", length, kMaxTransfer));
    }
    std::vector<u8> bytes(static_cast<std::size_t>(length));
    const MemoryResult<void> read = context.memory.readBytes(address, bytes);
    if (!read.has_value()) {
        return std::unexpected(std::format("write: {}", read.error().describe()));
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (capture) {
        (fd == 1 ? output : error_output).append(text);
    } else {
        std::FILE* stream = fd == 1 ? stdout : stderr;
        std::fwrite(text.data(), 1, text.size(), stream);
        std::fflush(stream);
    }
    context.registers[kReturnValue] = length;
    return {};
}

std::expected<void, std::string> DefaultSyscallProvider::doRead(SyscallContext& context) {
    const u64 fd = context.registers[kArg1];
    const u64 address = context.registers[kArg2];
    const u64 length = context.registers[kArg3];
    if (fd != 0) {
        return std::unexpected(std::format("read: unsupported file descriptor {}", fd));
    }
    if (length > kMaxTransfer) {
        return std::unexpected(
            std::format("read: length {} exceeds the {}-byte limit", length, kMaxTransfer));
    }
    std::vector<u8> bytes;
    if (capture) {
        const std::size_t count =
            std::min<std::size_t>(static_cast<std::size_t>(length), input.size());
        bytes.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(count));
        input.erase(0, count);
    } else {
        bytes.resize(static_cast<std::size_t>(length));
        const std::size_t count = std::fread(bytes.data(), 1, bytes.size(), stdin);
        bytes.resize(count);
    }
    if (!bytes.empty()) {
        const MemoryResult<void> written = context.memory.writeBytes(address, bytes);
        if (!written.has_value()) {
            return std::unexpected(std::format("read: {}", written.error().describe()));
        }
    }
    context.registers[kReturnValue] = bytes.size();
    return {};
}

std::expected<void, std::string> DefaultSyscallProvider::doAllocate(SyscallContext& context) {
    const u64 size = context.registers[kArg1];
    const MemoryResult<u64> block = context.memory.allocate(size);
    if (!block.has_value()) {
        // A failed allocation is reported in-band as a null pointer, the way a
        // real allocator does; the program decides what to do about it.
        context.registers[kReturnValue] = 0;
        return {};
    }
    context.registers[kReturnValue] = *block;
    return {};
}

}  // namespace minitool::vm
