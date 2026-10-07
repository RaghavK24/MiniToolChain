// SPDX-License-Identifier: MIT
#include "minitool/debugger/debugger.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <istream>
#include <ostream>
#include <sstream>

#include "minitool/disassembler/disassembler.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool::debugger {
namespace {

/// Parses "0x1234", "4096" or a symbol name.
[[nodiscard]] std::optional<u64> parseAddress(std::string_view text,
                                              const executable::Executable& executable) {
    if (text.empty()) {
        return std::nullopt;
    }
    int base = 10;
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
        base = 16;
    }
    u64 value = 0;
    const std::from_chars_result result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (result.ec == std::errc{} && result.ptr == digits.data() + digits.size()) {
        return value;
    }
    const executable::SymbolEntry* symbol = executable.findSymbol(text);
    return symbol == nullptr ? std::nullopt : std::optional<u64>{symbol->address};
}

[[nodiscard]] std::vector<std::string> splitWords(std::string_view line) {
    std::vector<std::string> words;
    std::istringstream stream{std::string{line}};
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

}  // namespace

std::string describeStop(const StopEvent& event) {
    switch (event.reason) {
        case StopReason::Breakpoint:
            return std::format("stopped at breakpoint, PC = 0x{:016X}{}", event.pc,
                               event.message.empty() ? "" : " (" + event.message + ")");
        case StopReason::Watchpoint:
            return std::format("watchpoint triggered: {}", event.message);
        case StopReason::StepComplete:
            return std::format("PC = 0x{:016X}", event.pc);
        case StopReason::Halted:
            return std::format("program halted at 0x{:016X}{}", event.pc,
                               event.message.empty() ? "" : ", " + event.message);
        case StopReason::Fault:
            return std::format("runtime error: {} at 0x{:016X}\n  {}", vm::vmErrorName(event.error),
                               event.pc, event.message);
        case StopReason::NotRunning:
            return "the program is not running";
    }
    return "stopped";
}

Debugger::Debugger(vm::VirtualMachine& machine, const executable::Executable& executable)
    : machine_(machine), executable_(executable) {}

u32 Debugger::addBreakpoint(u64 address) {
    Breakpoint breakpoint;
    breakpoint.id = next_id_++;
    breakpoint.address = address;
    const executable::SymbolEntry* symbol = executable_.symbolContaining(address);
    if (symbol != nullptr) {
        breakpoint.symbol = symbol->address == address
                                ? symbol->name
                                : std::format("{}+{}", symbol->name, address - symbol->address);
    }
    breakpoints_.push_back(std::move(breakpoint));
    return breakpoints_.back().id;
}

std::optional<u32> Debugger::addBreakpointBySymbol(std::string_view name) {
    const executable::SymbolEntry* symbol = executable_.findSymbol(name);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    return addBreakpoint(symbol->address);
}

bool Debugger::removeBreakpoint(u32 id) {
    const auto removed = std::ranges::remove_if(
        breakpoints_, [id](const Breakpoint& breakpoint) { return breakpoint.id == id; });
    if (removed.empty()) {
        return false;
    }
    breakpoints_.erase(removed.begin(), removed.end());
    return true;
}

bool Debugger::setBreakpointEnabled(u32 id, bool enabled) {
    for (Breakpoint& breakpoint : breakpoints_) {
        if (breakpoint.id == id) {
            breakpoint.enabled = enabled;
            return true;
        }
    }
    return false;
}

std::optional<u32> Debugger::addWatchpoint(u64 address) {
    const vm::MemoryResult<u64> value = machine_.memory().readU64(address);
    if (!value.has_value()) {
        return std::nullopt;
    }
    Watchpoint watchpoint;
    watchpoint.id = next_id_++;
    watchpoint.address = address;
    watchpoint.previous = *value;
    watchpoints_.push_back(watchpoint);
    return watchpoint.id;
}

bool Debugger::removeWatchpoint(u32 id) {
    const auto removed = std::ranges::remove_if(
        watchpoints_, [id](const Watchpoint& watchpoint) { return watchpoint.id == id; });
    if (removed.empty()) {
        return false;
    }
    watchpoints_.erase(removed.begin(), removed.end());
    return true;
}

const Breakpoint* Debugger::breakpointAt(u64 address) const {
    for (const Breakpoint& breakpoint : breakpoints_) {
        if (breakpoint.enabled && breakpoint.address == address) {
            return &breakpoint;
        }
    }
    return nullptr;
}

std::optional<StopEvent> Debugger::checkWatchpoints() {
    for (Watchpoint& watchpoint : watchpoints_) {
        if (!watchpoint.enabled) {
            continue;
        }
        const vm::MemoryResult<u64> value = machine_.memory().readU64(watchpoint.address);
        if (!value.has_value() || *value == watchpoint.previous) {
            continue;
        }
        StopEvent event;
        event.reason = StopReason::Watchpoint;
        event.pc = machine_.state().pc;
        event.message = std::format("[{}] 0x{:X}: 0x{:X} -> 0x{:X}", watchpoint.id,
                                    watchpoint.address, watchpoint.previous, *value);
        watchpoint.previous = *value;
        return event;
    }
    return std::nullopt;
}

StopEvent Debugger::singleStep() {
    if (machine_.state().halted) {
        return StopEvent{StopReason::Halted, machine_.state().pc, "already halted",
                         vm::VMError::None};
    }
    const std::expected<void, vm::VMFault> stepped = machine_.step();
    started_ = true;
    if (!stepped.has_value()) {
        return StopEvent{StopReason::Fault, stepped.error().pc, stepped.error().message,
                         stepped.error().error};
    }
    if (const std::optional<StopEvent> triggered = checkWatchpoints(); triggered.has_value()) {
        return *triggered;
    }
    if (machine_.state().halted) {
        return StopEvent{StopReason::Halted, machine_.state().pc,
                         std::format("exit code {}", machine_.state().exit_code),
                         vm::VMError::None};
    }
    return StopEvent{StopReason::StepComplete, machine_.state().pc, {}, vm::VMError::None};
}

StopEvent Debugger::step() {
    if (!machine_.loaded()) {
        return StopEvent{StopReason::NotRunning, 0, "no program is loaded", vm::VMError::None};
    }
    return singleStep();
}

StopEvent Debugger::run(u64 budget) {
    if (!machine_.loaded()) {
        return StopEvent{StopReason::NotRunning, 0, "no program is loaded", vm::VMError::None};
    }
    for (u64 executed = 0; executed < budget; ++executed) {
        // The breakpoint at the current PC is skipped on the first step so that
        // `continue` from a breakpoint makes progress instead of re-reporting
        // the instruction it is parked on.
        if (executed > 0 || !started_) {
            const Breakpoint* hit = breakpointAt(machine_.state().pc);
            if (hit != nullptr) {
                started_ = true;
                for (Breakpoint& breakpoint : breakpoints_) {
                    if (breakpoint.id == hit->id) {
                        ++breakpoint.hits;
                    }
                }
                return StopEvent{StopReason::Breakpoint, machine_.state().pc, hit->symbol,
                                 vm::VMError::None};
            }
        }
        const StopEvent event = singleStep();
        if (event.reason != StopReason::StepComplete) {
            return event;
        }
    }
    return StopEvent{StopReason::Fault, machine_.state().pc,
                     std::format("stopped after {} instructions", budget),
                     vm::VMError::BudgetExhausted};
}

StopEvent Debugger::stepOver(u64 budget) {
    if (!machine_.loaded()) {
        return StopEvent{StopReason::NotRunning, 0, "no program is loaded", vm::VMError::None};
    }
    const u64 pc = machine_.state().pc;
    const vm::MemoryResult<u64> word = machine_.memory().fetchInstruction(pc);
    const std::expected<isa::Instruction, isa::DecodeError> decoded =
        word.has_value() ? isa::decode(*word)
                         : std::expected<isa::Instruction, isa::DecodeError>{
                               std::unexpect, isa::DecodeError::Truncated};
    if (!decoded.has_value() || decoded->opcode != isa::Opcode::CALL) {
        return singleStep();
    }

    // Run until control comes back to the instruction after the CALL with the
    // stack no deeper than it is now. That is the return, even if the callee
    // recursed.
    const u64 resume = pc + isa::kInstructionSize;
    const u64 depth = machine_.state().sp;
    for (u64 executed = 0; executed < budget; ++executed) {
        const StopEvent event = singleStep();
        if (event.reason != StopReason::StepComplete) {
            return event;
        }
        if (machine_.state().pc == resume && machine_.state().sp >= depth) {
            return event;
        }
        const Breakpoint* hit = breakpointAt(machine_.state().pc);
        if (hit != nullptr) {
            return StopEvent{StopReason::Breakpoint, machine_.state().pc, hit->symbol,
                             vm::VMError::None};
        }
    }
    return StopEvent{StopReason::Fault, machine_.state().pc, "step over did not return",
                     vm::VMError::BudgetExhausted};
}

StopEvent Debugger::finish(u64 budget) {
    if (!machine_.loaded()) {
        return StopEvent{StopReason::NotRunning, 0, "no program is loaded", vm::VMError::None};
    }
    const u64 depth = machine_.state().sp;
    for (u64 executed = 0; executed < budget; ++executed) {
        const StopEvent event = singleStep();
        if (event.reason != StopReason::StepComplete) {
            return event;
        }
        if (machine_.state().sp > depth) {
            return event;  // the frame was popped
        }
    }
    return StopEvent{StopReason::Fault, machine_.state().pc, "function did not return",
                     vm::VMError::BudgetExhausted};
}

std::string Debugger::formatRegisters() const {
    const vm::CPUState& cpu = machine_.state();
    std::string out;
    for (unsigned i = 0; i < isa::kRegisterCount; ++i) {
        out += std::format("{:<4} 0x{:016X} {:>21}", isa::registerName(static_cast<isa::Reg>(i)),
                           cpu.registers[i], static_cast<i64>(cpu.registers[i]));
        out += (i % 2 == 1) ? "\n" : "   ";
    }
    out += std::format("PC   0x{:016X}   SP   0x{:016X}\n", cpu.pc, cpu.sp);
    out += std::format("FLAGS 0x{:X} [{}{}{}{}]  instructions: {}\n", cpu.flags,
                       (cpu.flags & vm::kZeroFlag) != 0 ? "Z" : "-",
                       (cpu.flags & vm::kSignFlag) != 0 ? "S" : "-",
                       (cpu.flags & vm::kCarryFlag) != 0 ? "C" : "-",
                       (cpu.flags & vm::kOverflowFlag) != 0 ? "O" : "-", cpu.instruction_count);
    return out;
}

std::string Debugger::formatMemory(u64 address, u64 length) const {
    std::string out;
    constexpr u64 kPerLine = 16;
    for (u64 offset = 0; offset < length; offset += kPerLine) {
        const u64 line_address = address + offset;
        std::string hex;
        std::string ascii;
        for (u64 i = 0; i < kPerLine && offset + i < length; ++i) {
            const vm::MemoryResult<u8> byte = machine_.memory().readByte(line_address + i);
            if (byte.has_value()) {
                hex += std::format("{:02X} ", *byte);
                ascii += (*byte >= 0x20 && *byte < 0x7F) ? static_cast<char>(*byte) : '.';
            } else {
                hex += "?? ";
                ascii += ' ';
            }
        }
        out += std::format("{:016X}  {:<48} |{}|\n", line_address, hex, ascii);
    }
    return out;
}

std::string Debugger::formatStack(u64 words) const {
    const vm::CPUState& cpu = machine_.state();
    std::string out =
        std::format("stack (SP = 0x{:016X}, top = 0x{:016X})\n", cpu.sp, machine_.stackTop());
    for (u64 i = 0; i < words; ++i) {
        const u64 address = cpu.sp + i * sizeof(u64);
        if (address >= machine_.stackTop()) {
            break;
        }
        const vm::MemoryResult<u64> value = machine_.memory().readU64(address);
        if (!value.has_value()) {
            break;
        }
        const std::optional<std::string> described = describeAddress(*value);
        out += std::format("  [SP+{:<3}] 0x{:016X}  0x{:016X}{}\n", i * sizeof(u64), address,
                           *value, described.has_value() ? "  ; " + *described : "");
    }
    return out;
}

std::string Debugger::formatDisassembly(u64 address, u64 count) const {
    std::vector<u8> code;
    code.reserve(static_cast<std::size_t>(count) * isa::kInstructionSize);
    for (u64 i = 0; i < count; ++i) {
        const vm::MemoryResult<u64> word =
            machine_.memory().readU64(address + i * isa::kInstructionSize);
        if (!word.has_value()) {
            break;
        }
        for (unsigned byte = 0; byte < isa::kInstructionSize; ++byte) {
            code.push_back(static_cast<u8>((*word >> (8U * byte)) & 0xFFU));
        }
    }
    if (code.empty()) {
        return std::format("no readable code at 0x{:016X}\n", address);
    }
    disassembler::Options options;
    options.show_labels = false;
    const disassembler::Disassembler disassembler(options);
    std::string text = disassembler.disassemble(code, address, &executable_);

    // Mark the current instruction, which is what a reader is looking for.
    const std::string marker = std::format("{:016X}:", machine_.state().pc);
    const std::size_t at = text.find(marker);
    if (at != std::string::npos) {
        text.insert(at, "=> ");
    }
    return text;
}

std::string Debugger::formatBacktrace(u64 max_frames) const {
    const vm::CPUState& cpu = machine_.state();
    std::string out =
        std::format("#0  0x{:016X}  {}\n", cpu.pc, describeAddress(cpu.pc).value_or("<unknown>"));
    u64 frame = 1;
    // Without frame pointers to walk, scan the stack for values that look like
    // return addresses: word-aligned, inside the text segment, and immediately
    // preceded by a CALL.
    for (u64 address = cpu.sp; address < machine_.stackTop() && frame < max_frames;
         address += sizeof(u64)) {
        const vm::MemoryResult<u64> value = machine_.memory().readU64(address);
        if (!value.has_value()) {
            break;
        }
        const u64 candidate = *value;
        if ((candidate % isa::kInstructionSize) != 0 || candidate < isa::kInstructionSize) {
            continue;
        }
        const vm::MemoryResult<u64> previous =
            machine_.memory().fetchInstruction(candidate - isa::kInstructionSize);
        if (!previous.has_value()) {
            continue;
        }
        const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decode(*previous);
        if (!decoded.has_value() || decoded->opcode != isa::Opcode::CALL) {
            continue;
        }
        out += std::format("#{}  0x{:016X}  {}\n", frame, candidate,
                           describeAddress(candidate).value_or("<unknown>"));
        ++frame;
    }
    return out;
}

std::string Debugger::formatSourceLocation(u64 address) const {
    const executable::DebugEntry* entry = executable_.debugEntryFor(address);
    if (entry == nullptr || entry->file >= executable_.source_files.size()) {
        return std::format("0x{:016X}: no source information\n", address);
    }
    return std::format("{}:{}:{}\n", executable_.source_files[entry->file], entry->line,
                       entry->column);
}

std::string Debugger::formatBreakpoints() const {
    if (breakpoints_.empty() && watchpoints_.empty()) {
        return "no breakpoints\n";
    }
    std::string out;
    for (const Breakpoint& breakpoint : breakpoints_) {
        out += std::format("breakpoint {} at 0x{:016X}{}{} ({} hit{})\n", breakpoint.id,
                           breakpoint.address,
                           breakpoint.symbol.empty() ? "" : " in " + breakpoint.symbol,
                           breakpoint.enabled ? "" : " [disabled]", breakpoint.hits,
                           breakpoint.hits == 1 ? "" : "s");
    }
    for (const Watchpoint& watchpoint : watchpoints_) {
        out += std::format("watchpoint {} on 0x{:016X} (last value 0x{:X})\n", watchpoint.id,
                           watchpoint.address, watchpoint.previous);
    }
    return out;
}

std::optional<u64> Debugger::findSymbol(std::string_view name) const {
    const executable::SymbolEntry* symbol = executable_.findSymbol(name);
    return symbol == nullptr ? std::nullopt : std::optional<u64>{symbol->address};
}

std::optional<std::string> Debugger::describeAddress(u64 address) const {
    const executable::SymbolEntry* symbol = executable_.symbolContaining(address);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    if (symbol->address == address) {
        return symbol->name;
    }
    return std::format("{}+{}", symbol->name, address - symbol->address);
}

bool Debugger::executeCommand(std::string_view line, std::ostream& out) {
    const std::vector<std::string> words = splitWords(line);
    if (words.empty()) {
        return true;
    }
    const std::string& command = words[0];
    const auto argument = [&](std::size_t index) -> std::string_view {
        return index < words.size() ? std::string_view{words[index]} : std::string_view{};
    };

    if (command == "quit" || command == "q") {
        return false;
    }
    if (command == "help" || command == "h") {
        out << "commands:\n"
               "  run, continue (r, c)      run until a breakpoint or exit\n"
               "  step (s)                  execute one instruction\n"
               "  next (n)                  step, but run over a CALL\n"
               "  finish                    run until the current function returns\n"
               "  break <addr|symbol> (b)   set a breakpoint\n"
               "  delete <id> (d)           remove a breakpoint or watchpoint\n"
               "  disable <id>              disable a breakpoint\n"
               "  enable <id>               enable a breakpoint\n"
               "  watch <addr>              stop when the 8 bytes there change\n"
               "  info                      list breakpoints and watchpoints\n"
               "  registers (regs)          print the register file\n"
               "  print <register>          print one register\n"
               "  memory <addr> [len] (x)   hex dump memory\n"
               "  stack                     dump the top of the stack\n"
               "  disassemble [addr] [n]    disassemble around the PC\n"
               "  backtrace (bt)            show the call stack\n"
               "  list (l)                  show the current source line\n"
               "  quit (q)                  leave the debugger\n";
        return true;
    }
    if (command == "run" || command == "r" || command == "continue" || command == "c") {
        out << describeStop(run()) << '\n';
        return true;
    }
    if (command == "step" || command == "s") {
        const StopEvent event = step();
        out << describeStop(event) << '\n';
        if (event.reason == StopReason::StepComplete) {
            out << formatDisassembly(machine_.state().pc, 1);
        }
        return true;
    }
    if (command == "next" || command == "n") {
        out << describeStop(stepOver()) << '\n';
        return true;
    }
    if (command == "finish") {
        out << describeStop(finish()) << '\n';
        return true;
    }
    if (command == "break" || command == "b") {
        const std::optional<u64> address = parseAddress(argument(1), executable_);
        if (!address.has_value()) {
            out << std::format("cannot resolve '{}'\n", argument(1));
            return true;
        }
        const u32 id = addBreakpoint(*address);
        out << std::format("breakpoint {} at 0x{:016X}\n", id, *address);
        return true;
    }
    if (command == "watch") {
        const std::optional<u64> address = parseAddress(argument(1), executable_);
        if (!address.has_value()) {
            out << std::format("cannot resolve '{}'\n", argument(1));
            return true;
        }
        const std::optional<u32> id = addWatchpoint(*address);
        if (!id.has_value()) {
            out << std::format("0x{:X} is not readable\n", *address);
            return true;
        }
        out << std::format("watchpoint {} on 0x{:016X}\n", *id, *address);
        return true;
    }
    if (command == "delete" || command == "d" || command == "disable" || command == "enable") {
        u32 id = 0;
        const std::string_view text = argument(1);
        if (std::from_chars(text.data(), text.data() + text.size(), id).ec != std::errc{}) {
            out << "expected a breakpoint id\n";
            return true;
        }
        if (command == "delete" || command == "d") {
            out << ((removeBreakpoint(id) || removeWatchpoint(id)) ? "deleted\n"
                                                                   : "no such breakpoint\n");
        } else {
            const bool enable = command == "enable";
            out << (setBreakpointEnabled(id, enable) ? "done\n" : "no such breakpoint\n");
        }
        return true;
    }
    if (command == "info") {
        out << formatBreakpoints();
        return true;
    }
    if (command == "registers" || command == "regs") {
        out << formatRegisters();
        return true;
    }
    if (command == "print" || command == "p") {
        const std::optional<isa::Reg> reg = isa::parseRegister(argument(1));
        if (!reg.has_value()) {
            out << std::format("'{}' is not a register\n", argument(1));
            return true;
        }
        const u64 value = machine_.state().registers[isa::registerIndex(*reg)];
        out << std::format("{} = 0x{:016X} ({})\n", isa::registerName(*reg), value,
                           static_cast<i64>(value));
        return true;
    }
    if (command == "memory" || command == "x") {
        const std::optional<u64> address = parseAddress(argument(1), executable_);
        if (!address.has_value()) {
            out << "expected an address\n";
            return true;
        }
        u64 length = 64;
        if (words.size() > 2) {
            const std::string_view text = argument(2);
            static_cast<void>(std::from_chars(text.data(), text.data() + text.size(), length));
        }
        out << formatMemory(*address, std::min<u64>(length, 4096));
        return true;
    }
    if (command == "stack") {
        out << formatStack();
        return true;
    }
    if (command == "disassemble" || command == "dis") {
        const std::optional<u64> address =
            words.size() > 1 ? parseAddress(argument(1), executable_) : machine_.state().pc;
        u64 count = 8;
        if (words.size() > 2) {
            const std::string_view text = argument(2);
            static_cast<void>(std::from_chars(text.data(), text.data() + text.size(), count));
        }
        out << formatDisassembly(address.value_or(machine_.state().pc), std::min<u64>(count, 256));
        return true;
    }
    if (command == "backtrace" || command == "bt") {
        out << formatBacktrace();
        return true;
    }
    if (command == "list" || command == "l") {
        out << formatSourceLocation(machine_.state().pc);
        return true;
    }
    out << std::format("unknown command '{}'; try 'help'\n", command);
    return true;
}

void Debugger::interactiveLoop(std::istream& in, std::ostream& out) {
    out << "minidbg - type 'help' for commands\n";
    out << formatSourceLocation(machine_.state().pc);
    out << formatDisassembly(machine_.state().pc, 1);
    std::string line;
    while (true) {
        out << "(minidbg) " << std::flush;
        if (!std::getline(in, line)) {
            out << '\n';
            return;
        }
        if (!executeCommand(line, out)) {
            return;
        }
    }
}

}  // namespace minitool::debugger
