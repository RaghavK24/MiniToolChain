// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/common/types.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/vm/vm.hpp"

namespace minitool::debugger {

struct Breakpoint {
    u32 id = 0;
    u64 address = 0;
    bool enabled = true;
    /// The symbol the breakpoint was set on, for display.
    std::string symbol;
    u32 hits = 0;
};

struct Watchpoint {
    u32 id = 0;
    u64 address = 0;
    u64 previous = 0;
    bool enabled = true;
};

/// Why the debugger gave control back.
enum class StopReason : u8 {
    Breakpoint,
    Watchpoint,
    StepComplete,
    Halted,
    Fault,
    NotRunning,
};

struct StopEvent {
    StopReason reason = StopReason::StepComplete;
    u64 pc = 0;
    std::string message;
    /// Only meaningful when `reason` is Fault.
    vm::VMError error = vm::VMError::None;
};

/// An interactive debugger driven entirely through the VM's public interface
/// (architectural rule 7): it single-steps the machine and inspects its state,
/// and never reaches into the CPU's internals or patches the program image.
/// That is why breakpoints here are a set of addresses checked before each
/// instruction rather than trap instructions written into the code.
class Debugger {
  public:
    Debugger(vm::VirtualMachine& machine, const executable::Executable& executable);

    // --- breakpoints -------------------------------------------------------
    u32 addBreakpoint(u64 address);
    /// Sets a breakpoint on a symbol. Returns nullopt if the name is unknown.
    std::optional<u32> addBreakpointBySymbol(std::string_view name);
    bool removeBreakpoint(u32 id);
    bool setBreakpointEnabled(u32 id, bool enabled);
    [[nodiscard]] std::span<const Breakpoint> breakpoints() const noexcept { return breakpoints_; }

    /// Watchpoints report when the 8 bytes at an address change.
    std::optional<u32> addWatchpoint(u64 address);
    bool removeWatchpoint(u32 id);
    [[nodiscard]] std::span<const Watchpoint> watchpoints() const noexcept { return watchpoints_; }

    // --- execution ---------------------------------------------------------
    /// Runs until a breakpoint, a watchpoint, a fault, or the program halts.
    StopEvent run(u64 budget = vm::VirtualMachine::kDefaultBudget);
    /// Executes exactly one instruction.
    StopEvent step();
    /// Steps one instruction, but runs a CALL to completion.
    StopEvent stepOver(u64 budget = vm::VirtualMachine::kDefaultBudget);
    /// Runs until the current function returns.
    StopEvent finish(u64 budget = vm::VirtualMachine::kDefaultBudget);

    // --- inspection --------------------------------------------------------
    [[nodiscard]] std::string formatRegisters() const;
    [[nodiscard]] std::string formatMemory(u64 address, u64 length) const;
    [[nodiscard]] std::string formatStack(u64 words = 8) const;
    [[nodiscard]] std::string formatDisassembly(u64 address, u64 count = 8) const;
    /// Walks saved return addresses down the stack.
    [[nodiscard]] std::string formatBacktrace(u64 max_frames = 32) const;
    [[nodiscard]] std::string formatSourceLocation(u64 address) const;
    [[nodiscard]] std::string formatBreakpoints() const;

    [[nodiscard]] std::optional<u64> findSymbol(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> describeAddress(u64 address) const;

    // --- interactive -------------------------------------------------------
    /// Runs the command loop. Returns when the user quits or input ends.
    void interactiveLoop(std::istream& in, std::ostream& out);
    /// Executes one command line and writes its output. Returns false for
    /// "quit", which is how the loop terminates.
    bool executeCommand(std::string_view line, std::ostream& out);

  private:
    [[nodiscard]] const Breakpoint* breakpointAt(u64 address) const;
    /// Steps once and classifies the outcome.
    StopEvent singleStep();
    [[nodiscard]] std::optional<StopEvent> checkWatchpoints();

    vm::VirtualMachine& machine_;
    const executable::Executable& executable_;
    std::vector<Breakpoint> breakpoints_;
    std::vector<Watchpoint> watchpoints_;
    u32 next_id_ = 1;
    bool started_ = false;
};

/// Renders a stop event as the line the debugger prints.
[[nodiscard]] std::string describeStop(const StopEvent& event);

}  // namespace minitool::debugger
