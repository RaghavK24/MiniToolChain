// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "minitool/common/types.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/isa/instruction.hpp"
#include "minitool/isa/semantics.hpp"
#include "minitool/vm/memory.hpp"
#include "minitool/vm/syscall.hpp"

namespace minitool::vm {

/// FLAGS bits, defined once in the ISA layer and re-exported here so that VM
/// code reads naturally.
inline constexpr u64 kZeroFlag = isa::kZeroFlag;
inline constexpr u64 kSignFlag = isa::kSignFlag;
inline constexpr u64 kCarryFlag = isa::kCarryFlag;
inline constexpr u64 kOverflowFlag = isa::kOverflowFlag;

struct CPUState {
    std::array<u64, isa::kRegisterCount> registers{};
    u64 pc = 0;
    u64 sp = 0;
    u64 flags = 0;
    bool halted = false;
    u64 instruction_count = 0;
    u64 exit_code = 0;
};

/// Every way a program can fail at run time. `None` means it ran to HALT.
enum class VMError : u8 {
    None,
    InvalidMemoryAccess,
    PermissionViolation,
    IllegalInstruction,
    DivisionByZero,
    StackOverflow,
    StackUnderflow,
    SyscallError,
    /// The instruction budget ran out. This is the guard against a program that never
    /// halts, used by the fuzzers and by `minitool run --max-instructions`.
    BudgetExhausted,
    NotLoaded,
};

[[nodiscard]] std::string_view vmErrorName(VMError error) noexcept;

struct VMFault {
    VMError error = VMError::None;
    std::string message;
    /// The address of the instruction that faulted.
    u64 pc = 0;
};

struct RunResult {
    VMError error = VMError::None;
    std::string message;
    u64 exit_code = 0;
    u64 instructions = 0;
    u64 pc = 0;

    [[nodiscard]] bool ok() const noexcept { return error == VMError::None; }
};

/// The virtual CPU.
///
/// The execution loop is deliberately explicit (fetch, decode, execute,
/// update), and every step that can fail returns a structured error rather than
/// trapping the host. No input, however malformed, may crash the process, hang
/// it (the instruction budget bounds every run) or invoke undefined behaviour.
///
/// The VM understands executables and nothing else: no source, no objects, no
/// relocations (architectural rule 6).
class VirtualMachine {
  public:
    /// Default layout of the regions the loader adds on top of the image.
    static constexpr u64 kStackTop = 0x7FFF'0000;
    static constexpr u64 kStackSize = 1U << 20U;
    static constexpr u64 kHeapBase = 0x1000'0000;
    static constexpr u64 kHeapSize = 1U << 20U;
    /// Default cap on instructions executed by one run() call.
    static constexpr u64 kDefaultBudget = 100'000'000;

    VirtualMachine();
    ~VirtualMachine();
    VirtualMachine(const VirtualMachine&) = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;

    /// Maps the executable's segments plus a stack and a heap, and resets the
    /// CPU to the entry point. The image is validated first.
    [[nodiscard]] std::expected<void, std::string> load(const executable::Executable& executable);

    /// Executes one instruction.
    [[nodiscard]] std::expected<void, VMFault> step();

    /// Runs until the program halts, faults, or `budget` instructions have been
    /// executed.
    RunResult run(u64 budget = kDefaultBudget);

    [[nodiscard]] const CPUState& state() const noexcept { return cpu_; }
    [[nodiscard]] CPUState& state() noexcept { return cpu_; }
    [[nodiscard]] VirtualMemory& memory() noexcept { return memory_; }
    [[nodiscard]] const VirtualMemory& memory() const noexcept { return memory_; }
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    /// Replaces the syscall implementation. Never null: passing nullptr
    /// restores the default provider.
    void setSyscallProvider(std::unique_ptr<SyscallProvider> provider);
    [[nodiscard]] SyscallProvider& syscalls() noexcept { return *syscalls_; }

    /// Called with the address and decoded form of each instruction before it
    /// executes. Used by `--trace` and by the debugger.
    using TraceSink = std::function<void(u64 pc, const isa::Instruction&)>;
    void setTraceSink(TraceSink sink);

    /// Bounds of the stack region, for stack-overflow classification and for
    /// the debugger's stack display.
    [[nodiscard]] u64 stackTop() const noexcept { return stack_top_; }
    [[nodiscard]] u64 stackLimit() const noexcept { return stack_limit_; }

  private:
    [[nodiscard]] std::expected<void, VMFault> execute(const isa::Instruction& instruction, u64 pc);
    [[nodiscard]] std::expected<void, VMFault> push(u64 value, u64 pc);
    [[nodiscard]] std::expected<u64, VMFault> pop(u64 pc);
    /// Turns a memory fault into a VM fault, classifying stack accesses.
    [[nodiscard]] VMFault faultFromMemory(const MemoryFault& fault, u64 pc,
                                          std::string_view what) const;

    CPUState cpu_;
    VirtualMemory memory_;
    std::unique_ptr<SyscallProvider> syscalls_;
    TraceSink trace_;
    bool loaded_ = false;
    u64 stack_top_ = kStackTop;
    u64 stack_limit_ = kStackTop - kStackSize;
};

}  // namespace minitool::vm
