// SPDX-License-Identifier: MIT
#include <memory>
#include <sstream>
#include <string>

#include "minitool/debugger/debugger.hpp"
#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

constexpr std::string_view kProgram = R"(
.global _start
.data
counter: .qword 0
.text
_start:
    MOVI R1, 40
    MOVI R2, 2
    CALL add_them
    LEA  R3, counter
    STORE [R3 + 0], R1
    HALT

add_them:
    ADD R1, R2
    RET
)";

/// A loaded program plus the debugger attached to it. The executable has to
/// outlive the debugger, which is why this is one object.
struct Session {
    testkit::Linked linked;
    vm::VirtualMachine machine;
    std::unique_ptr<debugger::Debugger> debugger;

    [[nodiscard]] u64 symbol(std::string_view name) const {
        const executable::SymbolEntry* entry = linked.executable.findSymbol(name);
        return entry == nullptr ? 0 : entry->address;
    }
    [[nodiscard]] std::string command(std::string_view line) {
        std::ostringstream out;
        static_cast<void>(debugger->executeCommand(line, out));
        return out.str();
    }
};

std::unique_ptr<Session> makeSession(std::string_view source = kProgram) {
    auto session = std::make_unique<Session>();
    session->linked = testkit::build(source);
    EXPECT_TRUE(session->linked.ok) << session->linked.error;
    EXPECT_TRUE(session->machine.load(session->linked.executable).has_value());
    session->debugger =
        std::make_unique<debugger::Debugger>(session->machine, session->linked.executable);
    return session;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(Debugger, StopsAtABreakpointBeforeExecutingIt) {
    const auto session = makeSession();
    const u64 address = session->symbol("add_them");
    ASSERT_NE(address, 0U);
    static_cast<void>(session->debugger->addBreakpoint(address));

    const debugger::StopEvent event = session->debugger->run();
    EXPECT_EQ(event.reason, debugger::StopReason::Breakpoint);
    EXPECT_EQ(event.pc, address);
    // The ADD inside add_them has not run yet.
    EXPECT_EQ(session->machine.state().registers[1], 40U);
}

TEST(Debugger, SetsBreakpointsBySymbol) {
    const auto session = makeSession();
    const std::optional<u32> id = session->debugger->addBreakpointBySymbol("add_them");
    ASSERT_TRUE(id.has_value());
    EXPECT_FALSE(session->debugger->addBreakpointBySymbol("no_such_thing").has_value());
    EXPECT_EQ(session->debugger->breakpoints().size(), 1U);
    EXPECT_EQ(session->debugger->breakpoints()[0].symbol, "add_them");
}

TEST(Debugger, ContinuesPastABreakpointItIsParkedOn) {
    const auto session = makeSession();
    static_cast<void>(session->debugger->addBreakpointBySymbol("add_them"));
    EXPECT_EQ(session->debugger->run().reason, debugger::StopReason::Breakpoint);
    // Continuing must make progress rather than reporting the same stop again.
    const debugger::StopEvent event = session->debugger->run();
    EXPECT_EQ(event.reason, debugger::StopReason::Halted);
    EXPECT_EQ(session->machine.state().registers[1], 42U);
}

TEST(Debugger, CountsHits) {
    const auto session = makeSession(R"(
.global _start
_start:
    MOVI R1, 3
loop:
    DEC R1
    CMP R1, R0
    JG loop
    HALT
)");
    static_cast<void>(session->debugger->addBreakpointBySymbol("loop"));
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(session->debugger->run().reason, debugger::StopReason::Breakpoint);
    }
    EXPECT_EQ(session->debugger->breakpoints()[0].hits, 3U);
}

TEST(Debugger, RemovesAndDisablesBreakpoints) {
    const auto session = makeSession();
    const u32 id = session->debugger->addBreakpoint(session->symbol("add_them"));
    EXPECT_TRUE(session->debugger->setBreakpointEnabled(id, false));
    EXPECT_EQ(session->debugger->run().reason, debugger::StopReason::Halted);
    EXPECT_TRUE(session->debugger->removeBreakpoint(id));
    EXPECT_TRUE(session->debugger->breakpoints().empty());
    EXPECT_FALSE(session->debugger->removeBreakpoint(id));
    EXPECT_FALSE(session->debugger->setBreakpointEnabled(999, true));
}

TEST(Debugger, StepsOneInstructionAtATime) {
    const auto session = makeSession();
    const u64 entry = session->linked.executable.entry_point;
    EXPECT_EQ(session->machine.state().pc, entry);
    EXPECT_EQ(session->debugger->step().reason, debugger::StopReason::StepComplete);
    EXPECT_EQ(session->machine.state().pc, entry + isa::kInstructionSize);
    EXPECT_EQ(session->machine.state().registers[1], 40U);
}

TEST(Debugger, StepsOverACall) {
    const auto session = makeSession();
    static_cast<void>(session->debugger->step());  // MOVI R1, 40
    static_cast<void>(session->debugger->step());  // MOVI R2, 2
    const u64 call_site = session->machine.state().pc;
    const debugger::StopEvent event = session->debugger->stepOver();
    EXPECT_EQ(event.reason, debugger::StopReason::StepComplete);
    // One "next" over the CALL lands on the following instruction, with the
    // callee's work already done.
    EXPECT_EQ(session->machine.state().pc, call_site + isa::kInstructionSize);
    EXPECT_EQ(session->machine.state().registers[1], 42U);
}

TEST(Debugger, FinishesTheCurrentFunction) {
    const auto session = makeSession();
    static_cast<void>(session->debugger->addBreakpointBySymbol("add_them"));
    static_cast<void>(session->debugger->run());
    const debugger::StopEvent event = session->debugger->finish();
    EXPECT_EQ(event.reason, debugger::StopReason::StepComplete);
    EXPECT_EQ(session->machine.state().registers[1], 42U);
}

TEST(Debugger, ReportsAFaultInsteadOfCrashing) {
    const auto session = makeSession(
        ".global _start\n_start:\n    MOVI R1, 1\n    MOVI R2, 0\n    DIV R1, R2\n    HALT\n");
    const debugger::StopEvent event = session->debugger->run();
    EXPECT_EQ(event.reason, debugger::StopReason::Fault);
    EXPECT_EQ(event.error, vm::VMError::DivisionByZero);
    EXPECT_TRUE(contains(debugger::describeStop(event), "division by zero"));
}

TEST(Debugger, WatchesAMemoryLocation) {
    const auto session = makeSession();
    const std::optional<u32> id = session->debugger->addWatchpoint(session->symbol("counter"));
    ASSERT_TRUE(id.has_value());
    const debugger::StopEvent event = session->debugger->run();
    EXPECT_EQ(event.reason, debugger::StopReason::Watchpoint);
    EXPECT_TRUE(contains(event.message, "0x2A"));  // 42, freshly stored
    EXPECT_FALSE(session->debugger->addWatchpoint(0xDEAD'0000).has_value());
    EXPECT_TRUE(session->debugger->removeWatchpoint(*id));
}

TEST(Debugger, ShowsRegisters) {
    const auto session = makeSession();
    static_cast<void>(session->debugger->step());
    const std::string text = session->debugger->formatRegisters();
    EXPECT_TRUE(contains(text, "R1   0x0000000000000028"));  // 40
    EXPECT_TRUE(contains(text, "PC"));
    EXPECT_TRUE(contains(text, "FLAGS"));
}

TEST(Debugger, DumpsMemoryAsHexAndText) {
    const auto session = makeSession(R"(
.global _start
.rodata
message: .asciz "Hi!"
.text
_start:
    HALT
)");
    const std::string text = session->debugger->formatMemory(session->symbol("message"), 4);
    EXPECT_TRUE(contains(text, "48 69 21 00"));
    EXPECT_TRUE(contains(text, "|Hi!"));
    // Unmapped bytes are shown as ?? rather than being invented.
    EXPECT_TRUE(contains(session->debugger->formatMemory(0xDEAD'0000, 4), "??"));
}

TEST(Debugger, ShowsTheStackAndBacktrace) {
    const auto session = makeSession();
    static_cast<void>(session->debugger->addBreakpointBySymbol("add_them"));
    static_cast<void>(session->debugger->run());

    const std::string stack = session->debugger->formatStack(4);
    EXPECT_TRUE(contains(stack, "SP"));

    const std::string backtrace = session->debugger->formatBacktrace();
    EXPECT_TRUE(contains(backtrace, "#0"));
    EXPECT_TRUE(contains(backtrace, "add_them"));
    // The caller's frame is found by looking for a return address that follows
    // a CALL.
    EXPECT_TRUE(contains(backtrace, "#1"));
    EXPECT_TRUE(contains(backtrace, "_start"));
}

TEST(Debugger, DisassemblesAroundTheProgramCounter) {
    const auto session = makeSession();
    const std::string text =
        session->debugger->formatDisassembly(session->linked.executable.entry_point, 3);
    EXPECT_TRUE(contains(text, "MOVI R1, 40"));
    EXPECT_TRUE(contains(text, "=>"));  // the current instruction is marked
}

TEST(Debugger, MapsAddressesBackToSource) {
    const auto session = makeSession();
    const std::string text =
        session->debugger->formatSourceLocation(session->linked.executable.entry_point);
    EXPECT_TRUE(contains(text, "test.asm:"));
    EXPECT_TRUE(
        contains(session->debugger->formatSourceLocation(0xDEAD'0000), "no source information"));
}

TEST(Debugger, LooksUpSymbolsBothWays) {
    const auto session = makeSession();
    EXPECT_TRUE(session->debugger->findSymbol("add_them").has_value());
    EXPECT_FALSE(session->debugger->findSymbol("nope").has_value());
    EXPECT_EQ(session->debugger->describeAddress(session->symbol("add_them")).value_or(""),
              "add_them");
    EXPECT_EQ(
        session->debugger->describeAddress(session->symbol("add_them") + isa::kInstructionSize)
            .value_or(""),
        "add_them+8");
}

TEST(Debugger, RunsCommands) {
    const auto session = makeSession();
    EXPECT_TRUE(contains(session->command("help"), "breakpoint"));
    EXPECT_TRUE(contains(session->command("break add_them"), "breakpoint 1"));
    EXPECT_TRUE(contains(session->command("info"), "add_them"));
    EXPECT_TRUE(contains(session->command("run"), "breakpoint"));
    EXPECT_TRUE(contains(session->command("registers"), "R1"));
    EXPECT_TRUE(contains(session->command("print R1"), "40"));
    EXPECT_TRUE(contains(session->command("backtrace"), "#0"));
    EXPECT_TRUE(contains(session->command("disassemble"), "ADD"));
    EXPECT_TRUE(contains(session->command("x 0x10000 16"), "0000000000010000"));
    EXPECT_TRUE(contains(session->command("stack"), "SP"));
    EXPECT_TRUE(contains(session->command("list"), "test.asm"));
    EXPECT_TRUE(contains(session->command("step"), "PC ="));
    EXPECT_TRUE(contains(session->command("delete 1"), "deleted"));
    EXPECT_TRUE(contains(session->command("continue"), "halted"));
}

TEST(Debugger, RejectsBadCommandsWithoutGivingUp) {
    const auto session = makeSession();
    EXPECT_TRUE(contains(session->command("frobnicate"), "unknown command"));
    EXPECT_TRUE(contains(session->command("break not_a_symbol"), "cannot resolve"));
    EXPECT_TRUE(contains(session->command("print R99"), "is not a register"));
    EXPECT_TRUE(contains(session->command("delete abc"), "expected a breakpoint id"));
    EXPECT_TRUE(session->command("").empty());
}

TEST(Debugger, QuitLeavesTheLoop) {
    const auto session = makeSession();
    std::ostringstream out;
    EXPECT_FALSE(session->debugger->executeCommand("quit", out));
    EXPECT_TRUE(session->debugger->executeCommand("info", out));
}

TEST(Debugger, DrivesTheInteractiveLoop) {
    const auto session = makeSession();
    std::istringstream in("break add_them\nrun\nregisters\nquit\n");
    std::ostringstream out;
    session->debugger->interactiveLoop(in, out);
    const std::string text = out.str();
    EXPECT_TRUE(contains(text, "minidbg"));
    EXPECT_TRUE(contains(text, "(minidbg)"));
    EXPECT_TRUE(contains(text, "breakpoint"));
}

}  // namespace
