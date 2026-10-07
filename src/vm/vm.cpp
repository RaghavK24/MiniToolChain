// SPDX-License-Identifier: MIT
#include "minitool/vm/vm.hpp"

#include <format>

#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"

namespace minitool::vm {
namespace {

[[nodiscard]] Permission permissionsOf(executable::SegmentFlags flags) noexcept {
    Permission permissions = Permission::None;
    if (executable::hasFlag(flags, executable::SegmentFlags::Read)) {
        permissions |= Permission::Read;
    }
    if (executable::hasFlag(flags, executable::SegmentFlags::Write)) {
        permissions |= Permission::Write;
    }
    if (executable::hasFlag(flags, executable::SegmentFlags::Exec)) {
        // Fetching also needs to be able to read the word; Exec implies Read
        // for the instruction fetch path only, which asks for Exec explicitly.
        permissions |= Permission::Read;
        permissions |= Permission::Exec;
    }
    return permissions;
}

}  // namespace

std::string_view vmErrorName(VMError error) noexcept {
    switch (error) {
        case VMError::None:
            return "none";
        case VMError::InvalidMemoryAccess:
            return "invalid memory access";
        case VMError::PermissionViolation:
            return "permission violation";
        case VMError::IllegalInstruction:
            return "illegal instruction";
        case VMError::DivisionByZero:
            return "division by zero";
        case VMError::StackOverflow:
            return "stack overflow";
        case VMError::StackUnderflow:
            return "stack underflow";
        case VMError::SyscallError:
            return "syscall error";
        case VMError::BudgetExhausted:
            return "instruction budget exhausted";
        case VMError::NotLoaded:
            return "no program loaded";
    }
    return "unknown error";
}

VirtualMachine::VirtualMachine() : syscalls_(std::make_unique<DefaultSyscallProvider>()) {}

VirtualMachine::~VirtualMachine() = default;

void VirtualMachine::setSyscallProvider(std::unique_ptr<SyscallProvider> provider) {
    syscalls_ =
        provider != nullptr ? std::move(provider) : std::make_unique<DefaultSyscallProvider>();
}

void VirtualMachine::setTraceSink(TraceSink sink) {
    trace_ = std::move(sink);
}

std::expected<void, std::string> VirtualMachine::load(const executable::Executable& executable) {
    const std::expected<void, std::string> valid = executable::validate(executable);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }

    memory_.reset();
    for (const executable::Segment& segment : executable.segments) {
        if (!memory_.addRegion(segment.name, segment.virtual_address, segment.virtual_size,
                               permissionsOf(segment.flags), segment.data)) {
            return std::unexpected(std::format("cannot map segment '{}' at 0x{:X}", segment.name,
                                               segment.virtual_address));
        }
    }
    if (!memory_.addRegion("stack", kStackTop - kStackSize, kStackSize,
                           Permission::Read | Permission::Write)) {
        return std::unexpected(std::string{"the stack overlaps a program segment"});
    }
    if (!memory_.addRegion("heap", kHeapBase, kHeapSize, Permission::Read | Permission::Write)) {
        return std::unexpected(std::string{"the heap overlaps a program segment"});
    }
    stack_top_ = kStackTop;
    stack_limit_ = kStackTop - kStackSize;

    cpu_ = CPUState{};
    cpu_.pc = executable.entry_point;
    cpu_.sp = stack_top_;
    loaded_ = true;
    return {};
}

VMFault VirtualMachine::faultFromMemory(const MemoryFault& fault, u64 pc,
                                        std::string_view what) const {
    VMError error = VMError::InvalidMemoryAccess;
    switch (fault.kind) {
        case MemoryErrorKind::ReadDenied:
        case MemoryErrorKind::WriteDenied:
        case MemoryErrorKind::ExecuteDenied:
            error = VMError::PermissionViolation;
            break;
        case MemoryErrorKind::Unmapped:
        case MemoryErrorKind::CrossesRegionEnd:
            // An unmapped access just below the stack is a stack overflow, and
            // saying so is far more useful than "unmapped address".
            if (fault.address < stack_limit_ && stack_limit_ - fault.address <= kStackSize) {
                error = VMError::StackOverflow;
            }
            break;
        case MemoryErrorKind::OutOfMemory:
            error = VMError::InvalidMemoryAccess;
            break;
    }
    return VMFault{error, std::format("{}: {}", what, fault.describe()), pc};
}

std::expected<void, VMFault> VirtualMachine::push(u64 value, u64 pc) {
    if (cpu_.sp < stack_limit_ + sizeof(u64)) {
        return std::unexpected(VMFault{
            VMError::StackOverflow,
            std::format("stack overflow: SP = 0x{:X}, limit = 0x{:X}", cpu_.sp, stack_limit_), pc});
    }
    const u64 target = cpu_.sp - sizeof(u64);
    const MemoryResult<void> written = memory_.writeU64(target, value);
    if (!written.has_value()) {
        return std::unexpected(faultFromMemory(written.error(), pc, "push"));
    }
    cpu_.sp = target;
    return {};
}

std::expected<u64, VMFault> VirtualMachine::pop(u64 pc) {
    if (cpu_.sp >= stack_top_) {
        return std::unexpected(
            VMFault{VMError::StackUnderflow,
                    std::format("stack underflow: SP = 0x{:X} is at or above the stack top 0x{:X}",
                                cpu_.sp, stack_top_),
                    pc});
    }
    const MemoryResult<u64> value = memory_.readU64(cpu_.sp);
    if (!value.has_value()) {
        return std::unexpected(faultFromMemory(value.error(), pc, "pop"));
    }
    cpu_.sp += sizeof(u64);
    return *value;
}

std::expected<void, VMFault> VirtualMachine::step() {
    if (!loaded_) {
        return std::unexpected(VMFault{VMError::NotLoaded, "no program is loaded", cpu_.pc});
    }
    if (cpu_.halted) {
        return {};
    }

    const u64 pc = cpu_.pc;
    if ((pc % isa::kInstructionSize) != 0) {
        return std::unexpected(VMFault{
            VMError::IllegalInstruction,
            std::format("PC 0x{:X} is not {}-byte aligned", pc, isa::kInstructionSize), pc});
    }

    // Fetch requires execute permission, which is what stops a program jumping
    // into its own data.
    const MemoryResult<u64> word = memory_.fetchInstruction(pc);
    if (!word.has_value()) {
        return std::unexpected(faultFromMemory(word.error(), pc, "instruction fetch"));
    }
    const std::expected<isa::Instruction, isa::DecodeError> decoded = isa::decode(*word);
    if (!decoded.has_value()) {
        return std::unexpected(VMFault{VMError::IllegalInstruction,
                                       std::format("cannot decode 0x{:016X}: {}", *word,
                                                   isa::decodeErrorName(decoded.error())),
                                       pc});
    }

    if (trace_) {
        trace_(pc, *decoded);
    }

    // The PC advances before execution so that a branch simply overwrites it
    // and CALL can push the already-correct return address.
    cpu_.pc = pc + isa::kInstructionSize;
    ++cpu_.instruction_count;
    return execute(*decoded, pc);
}

std::expected<void, VMFault> VirtualMachine::execute(const isa::Instruction& instruction, u64 pc) {
    const unsigned dst = isa::registerIndex(instruction.dst);
    const unsigned src = isa::registerIndex(instruction.src);
    // decode() cannot produce an out-of-range register, but an Instruction can
    // also be built in code; check rather than index the register file blindly.
    if (dst >= isa::kRegisterCount || src >= isa::kRegisterCount) {
        return std::unexpected(
            VMFault{VMError::IllegalInstruction,
                    std::format("register index {}/{} is out of range", dst, src), pc});
    }

    switch (instruction.opcode) {
        case isa::Opcode::NOP:
            return {};
        case isa::Opcode::HALT:
            cpu_.halted = true;
            return {};
        case isa::Opcode::SYSCALL: {
            SyscallContext context{std::span<u64, isa::kRegisterCount>{cpu_.registers}, memory_};
            const std::expected<void, std::string> result =
                syscalls_->invoke(static_cast<u64>(instruction.imm), context);
            if (!result.has_value()) {
                return std::unexpected(VMFault{VMError::SyscallError, result.error(), pc});
            }
            if (context.halt) {
                cpu_.halted = true;
                cpu_.exit_code = context.exit_code;
            }
            return {};
        }
        case isa::Opcode::MOV:
            cpu_.registers[dst] = cpu_.registers[src];
            return {};
        case isa::Opcode::MOVI:
        case isa::Opcode::LEA:
            cpu_.registers[dst] = static_cast<u64>(instruction.imm);
            return {};
        case isa::Opcode::LOAD: {
            const u64 address = cpu_.registers[src] + static_cast<u64>(instruction.imm);
            const MemoryResult<u64> value = memory_.readU64(address);
            if (!value.has_value()) {
                return std::unexpected(faultFromMemory(value.error(), pc, "LOAD"));
            }
            cpu_.registers[dst] = *value;
            return {};
        }
        case isa::Opcode::STORE: {
            const u64 address = cpu_.registers[dst] + static_cast<u64>(instruction.imm);
            const MemoryResult<void> written = memory_.writeU64(address, cpu_.registers[src]);
            if (!written.has_value()) {
                return std::unexpected(faultFromMemory(written.error(), pc, "STORE"));
            }
            return {};
        }
        case isa::Opcode::PUSH:
            return push(cpu_.registers[dst], pc);
        case isa::Opcode::POP: {
            const std::expected<u64, VMFault> value = pop(pc);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            cpu_.registers[dst] = *value;
            return {};
        }
        case isa::Opcode::CALL: {
            // cpu_.pc already points at the instruction after the CALL.
            const std::expected<void, VMFault> pushed = push(cpu_.pc, pc);
            if (!pushed.has_value()) {
                return pushed;
            }
            cpu_.pc = isa::branchTarget(pc, instruction.imm);
            return {};
        }
        case isa::Opcode::RET: {
            const std::expected<u64, VMFault> address = pop(pc);
            if (!address.has_value()) {
                return std::unexpected(address.error());
            }
            cpu_.pc = *address;
            return {};
        }
        default:
            break;
    }

    const isa::OpcodeInfo* info = isa::findOpcode(instruction.opcode);
    if (info != nullptr && info->is_branch) {
        if (isa::branchTaken(instruction.opcode, cpu_.flags)) {
            cpu_.pc = isa::branchTarget(pc, instruction.imm);
        }
        return {};
    }

    // Everything left is an ALU operation, evaluated by the shared semantics
    // layer so that the VM and the optimizer cannot disagree.
    const bool binary = isa::isBinaryAlu(instruction.opcode);
    const std::expected<isa::AluResult, isa::AluError> result =
        binary ? isa::evaluateBinary(instruction.opcode, cpu_.registers[dst], cpu_.registers[src])
               : isa::evaluateUnary(instruction.opcode, cpu_.registers[dst]);
    if (!result.has_value()) {
        if (result.error() == isa::AluError::DivisionByZero) {
            return std::unexpected(
                VMFault{VMError::DivisionByZero,
                        std::format("{} by zero", isa::opcodeName(instruction.opcode)), pc});
        }
        return std::unexpected(
            VMFault{VMError::IllegalInstruction,
                    std::format("{} is not executable", isa::opcodeName(instruction.opcode)), pc});
    }
    if (result->writes_value) {
        cpu_.registers[dst] = result->value;
    }
    if (result->writes_flags) {
        cpu_.flags = result->flags & isa::kFlagsMask;
    }
    return {};
}

RunResult VirtualMachine::run(u64 budget) {
    RunResult result;
    if (!loaded_) {
        result.error = VMError::NotLoaded;
        result.message = "no program is loaded";
        return result;
    }
    u64 executed = 0;
    while (!cpu_.halted) {
        if (executed >= budget) {
            result.error = VMError::BudgetExhausted;
            result.message = std::format(
                "stopped after {} instructions; the program may not terminate", executed);
            result.pc = cpu_.pc;
            result.instructions = cpu_.instruction_count;
            return result;
        }
        const std::expected<void, VMFault> stepped = step();
        ++executed;
        if (!stepped.has_value()) {
            result.error = stepped.error().error;
            result.message = stepped.error().message;
            result.pc = stepped.error().pc;
            result.instructions = cpu_.instruction_count;
            return result;
        }
    }
    result.exit_code = cpu_.exit_code;
    result.instructions = cpu_.instruction_count;
    result.pc = cpu_.pc;
    return result;
}

}  // namespace minitool::vm
