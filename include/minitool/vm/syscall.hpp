// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "minitool/common/types.hpp"
#include "minitool/vm/memory.hpp"

namespace minitool::vm {

/// The virtual system call numbers. Frozen: they are baked into every program
/// the assembler produces. See docs/vm.md §Syscalls for the full ABI.
enum class SyscallNumber : u64 {
    /// exit(code = R1): stops the machine.
    Exit = 0,
    /// write(fd = R1, buffer = R2, length = R3) -> bytes written in R14.
    Write = 1,
    /// read(fd = R1, buffer = R2, length = R3) -> bytes read in R14.
    Read = 2,
    /// allocate(size = R1) -> address in R14.
    Allocate = 3,
};

[[nodiscard]] std::string_view syscallName(u64 number) noexcept;

/// Everything a syscall may touch. Passing this instead of the whole VM keeps
/// host I/O out of the CPU's execution logic and makes the
/// provider trivially testable.
struct SyscallContext {
    std::span<u64, 16> registers;
    VirtualMemory& memory;
    /// Set by exit() to stop the machine.
    bool halt = false;
    u64 exit_code = 0;
};

/// The seam between the VM and the host. The default implementation talks to
/// stdio; tests substitute one that captures output and supplies canned input.
class SyscallProvider {
  public:
    SyscallProvider() = default;
    SyscallProvider(const SyscallProvider&) = delete;
    SyscallProvider& operator=(const SyscallProvider&) = delete;
    virtual ~SyscallProvider() = default;

    /// Performs the call. Returning an error traps the program with a
    /// SYSCALL_ERROR; it is not a host-level failure.
    [[nodiscard]] virtual std::expected<void, std::string> invoke(u64 number,
                                                                  SyscallContext& context) = 0;
};

/// Implements the four calls above against stdin/stdout/stderr, or against
/// in-memory buffers when `capture` is set.
class DefaultSyscallProvider final : public SyscallProvider {
  public:
    [[nodiscard]] std::expected<void, std::string> invoke(u64 number,
                                                          SyscallContext& context) override;

    /// When true, `write` appends to `output` instead of touching the host, and
    /// `read` consumes `input`.
    bool capture = false;
    std::string output;
    std::string error_output;
    std::string input;

  private:
    [[nodiscard]] std::expected<void, std::string> doWrite(SyscallContext& context);
    [[nodiscard]] std::expected<void, std::string> doRead(SyscallContext& context);
    [[nodiscard]] std::expected<void, std::string> doAllocate(SyscallContext& context);
};

}  // namespace minitool::vm
