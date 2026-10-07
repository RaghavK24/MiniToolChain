// SPDX-License-Identifier: MIT
#include "minitool/optimizer/optimizer.hpp"

#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/isa.hpp"
#include "minitool/isa/semantics.hpp"

namespace minitool::optimizer {
namespace {

using ir::Instruction;
using ir::Item;
using ir::Label;

[[nodiscard]] bool isInstruction(const Item& item) noexcept {
    return std::holds_alternative<Instruction>(item);
}

[[nodiscard]] bool isLabel(const Item& item) noexcept {
    return std::holds_alternative<Label>(item);
}

/// A basic block ends after anything that can transfer control elsewhere, and
/// after SYSCALL, which can change registers and memory in ways this optimizer
/// deliberately does not model.
[[nodiscard]] bool endsBlock(isa::Opcode opcode) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(opcode);
    if (info == nullptr) {
        return true;
    }
    return info->is_branch || info->is_terminator || opcode == isa::Opcode::SYSCALL;
}

/// Register read by the instruction in its `src` position, if any.
[[nodiscard]] std::optional<isa::Reg> sourceRegister(const isa::Instruction& machine) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(machine.opcode);
    if (info == nullptr) {
        return std::nullopt;
    }
    if (info->format == isa::Format::Reg2 || info->format == isa::Format::Mem) {
        return machine.src;
    }
    return std::nullopt;
}

[[nodiscard]] bool readsRegister(const isa::Instruction& machine, isa::Reg reg) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(machine.opcode);
    if (info == nullptr) {
        return true;
    }
    if (info->reads_dst && machine.dst == reg) {
        return true;
    }
    const std::optional<isa::Reg> source = sourceRegister(machine);
    return source.has_value() && *source == reg;
}

[[nodiscard]] bool writesRegister(const isa::Instruction& machine, isa::Reg reg) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(machine.opcode);
    return info != nullptr && info->writes_dst && machine.dst == reg;
}

[[nodiscard]] bool writesFlags(isa::Opcode opcode) noexcept {
    const isa::OpcodeInfo* info = isa::findOpcode(opcode);
    return info != nullptr && info->writes_flags;
}

/// The half-open range [begin, end) of one basic block, plus where control can
/// go from it.
struct Block {
    std::size_t begin = 0;
    std::size_t end = 0;
    /// Indices of the blocks control may reach from here.
    std::vector<std::size_t> successors;
    /// True if a successor could not be determined (a call, a computed branch,
    /// or a branch to a symbol this section does not define), in which case
    /// liveness assumes the worst.
    bool unknown_successor = false;
    /// FLAGS may be read by something reachable from the end of this block.
    bool flags_live_out = true;
};

/// The control-flow graph of one section, and the FLAGS liveness derived from
/// it.
///
/// A block starts at the section start or at a label, and ends after a branch,
/// HALT, RET or SYSCALL. Labels are the only entry points, which is exactly
/// why the optimizer may never delete one.
///
/// The only fact computed here is whether FLAGS is live out of each block.
/// That is what decides whether a flag-setting instruction may be rewritten
/// into one that sets no flags: the difference between an optimizer that is
/// correct and one that quietly breaks the conditional branch two blocks later.
class ControlFlow {
  public:
    explicit ControlFlow(const std::vector<Item>& items) : items_(&items) {
        splitBlocks();
        linkSuccessors();
        computeFlagLiveness();
    }

    [[nodiscard]] std::span<const Block> blocks() const noexcept { return blocks_; }

    /// True if FLAGS written by the instruction at `index` cannot be read
    /// before something overwrites it.
    [[nodiscard]] bool flagsDeadAfter(std::size_t block_index, std::size_t index) const {
        const Block& block = blocks_[block_index];
        for (std::size_t i = index + 1; i < block.end; ++i) {
            if (!isInstruction((*items_)[i])) {
                continue;
            }
            const isa::Opcode opcode = std::get<Instruction>((*items_)[i]).machine.opcode;
            if (isa::readsFlags(opcode)) {
                return false;
            }
            if (writesFlags(opcode)) {
                return true;
            }
        }
        return !block.flags_live_out;
    }

  private:
    void splitBlocks() {
        std::size_t begin = 0;
        for (std::size_t i = 0; i < items_->size(); ++i) {
            const Item& item = (*items_)[i];
            if (isLabel(item)) {
                if (i > begin) {
                    blocks_.push_back(Block{begin, i, {}, false, true});
                }
                begin = i + 1;
                label_block_.emplace(std::get<Label>(item).name, blocks_.size());
                continue;
            }
            if (isInstruction(item) && endsBlock(std::get<Instruction>(item).machine.opcode)) {
                blocks_.push_back(Block{begin, i + 1, {}, false, true});
                begin = i + 1;
            }
        }
        if (begin < items_->size() || blocks_.empty()) {
            blocks_.push_back(Block{begin, items_->size(), {}, false, true});
        }
    }

    /// The last instruction of a block, or nullptr if it holds no instructions.
    [[nodiscard]] const Instruction* terminator(const Block& block) const {
        for (std::size_t i = block.end; i > block.begin; --i) {
            const Item& item = (*items_)[i - 1];
            if (isInstruction(item)) {
                return &std::get<Instruction>(item);
            }
        }
        return nullptr;
    }

    void linkSuccessors() {
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            Block& block = blocks_[i];
            const Instruction* last = terminator(block);
            const isa::OpcodeInfo* info =
                last == nullptr ? nullptr : isa::findOpcode(last->machine.opcode);
            const bool ends_with_branch = info != nullptr && info->is_branch;
            const bool falls_through = info == nullptr || !info->is_terminator;

            if (falls_through && i + 1 < blocks_.size()) {
                block.successors.push_back(i + 1);
            }
            if (!ends_with_branch) {
                continue;
            }
            if (last->machine.opcode == isa::Opcode::CALL) {
                // What a callee does to FLAGS is not modelled, so a call site
                // is treated as reaching somewhere unknown.
                block.unknown_successor = true;
                continue;
            }
            if (!last->isSymbolic()) {
                block.unknown_successor = true;  // a literal displacement
                continue;
            }
            const auto target = label_block_.find(last->symbol->name);
            if (target == label_block_.end()) {
                block.unknown_successor = true;  // defined in another file
            } else {
                block.successors.push_back(target->second);
            }
        }
    }

    /// Backward dataflow over the one bit of state that matters here, iterated
    /// to a fixed point so that loops are handled correctly.
    void computeFlagLiveness() {
        std::vector<bool> live_in(blocks_.size(), false);
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            live_in[i] = blockReadsFlagsBeforeWriting(blocks_[i]);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = blocks_.size(); i > 0; --i) {
                Block& block = blocks_[i - 1];
                bool live_out = block.unknown_successor;
                for (const std::size_t successor : block.successors) {
                    live_out = live_out || live_in[successor];
                }
                if (live_out != block.flags_live_out) {
                    block.flags_live_out = live_out;
                    changed = true;
                }
                // FLAGS is live into a block if the block reads it before
                // writing it, or if it passes straight through.
                const bool live =
                    blockReadsFlagsBeforeWriting(block) || (live_out && !blockWritesFlags(block));
                if (live != live_in[i - 1]) {
                    live_in[i - 1] = live;
                    changed = true;
                }
            }
        }
    }

    /// True if the block needs the FLAGS value its predecessor left behind.
    [[nodiscard]] bool blockReadsFlagsBeforeWriting(const Block& block) const {
        for (std::size_t i = block.begin; i < block.end; ++i) {
            if (!isInstruction((*items_)[i])) {
                continue;
            }
            const isa::Opcode opcode = std::get<Instruction>((*items_)[i]).machine.opcode;
            if (isa::readsFlags(opcode)) {
                return true;
            }
            if (writesFlags(opcode)) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool blockWritesFlags(const Block& block) const {
        for (std::size_t i = block.begin; i < block.end; ++i) {
            if (isInstruction((*items_)[i]) &&
                writesFlags(std::get<Instruction>((*items_)[i]).machine.opcode)) {
                return true;
            }
        }
        return false;
    }

    const std::vector<Item>* items_;
    std::vector<Block> blocks_;
    std::unordered_map<std::string, std::size_t> label_block_;
};

/// Constant values known to be in registers at a point inside a block.
class ConstantMap {
  public:
    void set(isa::Reg reg, u64 value) { values_[isa::registerIndex(reg)] = value; }
    void clear(isa::Reg reg) { values_[isa::registerIndex(reg)].reset(); }
    void clearAll() { values_ = {}; }

    [[nodiscard]] std::optional<u64> get(isa::Reg reg) const {
        return values_[isa::registerIndex(reg)];
    }

  private:
    std::array<std::optional<u64>, isa::kRegisterCount> values_{};
};

}  // namespace

std::string_view optLevelName(OptLevel level) noexcept {
    return level == OptLevel::O0 ? "O0" : "O1";
}

std::string OptStats::summary() const {
    return std::format(
        "{} -> {} instructions ({} folded, {} identities, {} dead stores, {} unreachable, "
        "{} peepholes)",
        instructions_before, instructions_after, constants_folded, identities_eliminated,
        dead_stores_removed, unreachable_removed, peepholes_applied);
}

Optimizer::Optimizer(OptLevel level) noexcept : level_(level) {}

OptStats Optimizer::run(ir::Module& module) const {
    OptStats stats;
    stats.instructions_before = module.instructionCount();
    if (level_ != OptLevel::O0) {
        for (ir::Section& section : module.sections) {
            if (section.kind == ir::SectionKind::Text) {
                optimizeSection(section, stats);
            }
        }
    }
    stats.instructions_after = module.instructionCount();
    return stats;
}

void Optimizer::optimizeSection(ir::Section& section, OptStats& stats) const {
    // Passes feed each other (folding creates dead stores, deleting a jump
    // exposes unreachable code), so iterate to a fixed point. The bound is a
    // safety net; the passes only ever delete or simplify, so they converge.
    constexpr int kMaxIterations = 8;
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        if (!runPasses(section, stats)) {
            return;
        }
    }
}

bool Optimizer::runPasses(ir::Section& section, OptStats& stats) const {
    std::vector<Item>& items = section.items;
    bool changed = false;
    std::vector<bool> remove(items.size(), false);

    // ---------------------------------------------------------- unreachable --
    // Everything between an unconditional terminator and the next label can
    // never execute: labels are the only way in.
    bool reachable = true;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (isLabel(items[i])) {
            reachable = true;
            continue;
        }
        if (!isInstruction(items[i])) {
            continue;
        }
        const Instruction& instruction = std::get<Instruction>(items[i]);
        if (!reachable) {
            remove[i] = true;
            ++stats.unreachable_removed;
            changed = true;
            continue;
        }
        const isa::OpcodeInfo* info = isa::findOpcode(instruction.machine.opcode);
        if (info != nullptr && info->is_terminator) {
            reachable = false;
        }
    }

    // ------------------------------------------------------------- peephole --
    for (std::size_t i = 0; i + 1 < items.size(); ++i) {
        if (remove[i] || !isInstruction(items[i]) || !isInstruction(items[i + 1]) ||
            remove[i + 1]) {
            continue;
        }
        Instruction& first = std::get<Instruction>(items[i]);
        const Instruction& second = std::get<Instruction>(items[i + 1]);
        if (first.machine.opcode == isa::Opcode::PUSH &&
            second.machine.opcode == isa::Opcode::POP) {
            if (first.machine.dst == second.machine.dst) {
                // PUSH rX / POP rX restores both the register and SP.
                remove[i] = true;
                remove[i + 1] = true;
                stats.peepholes_applied += 2;
            } else {
                // PUSH rX / POP rY is a register move through the stack. Read
                // the pushed register before overwriting the dst field.
                const isa::Reg pushed = first.machine.dst;
                first.machine.opcode = isa::Opcode::MOV;
                first.machine.dst = second.machine.dst;
                first.machine.src = pushed;
                remove[i + 1] = true;
                ++stats.peepholes_applied;
            }
            changed = true;
            ++i;  // do not re-examine the instruction just consumed
        }
    }

    // A jump to the label that immediately follows it is a no-op.
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (remove[i] || !isInstruction(items[i])) {
            continue;
        }
        const Instruction& instruction = std::get<Instruction>(items[i]);
        if (instruction.machine.opcode != isa::Opcode::JMP || !instruction.isSymbolic() ||
            instruction.symbol->addend != 0) {
            continue;
        }
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            if (remove[j]) {
                continue;
            }
            if (!isLabel(items[j])) {
                break;
            }
            if (std::get<Label>(items[j]).name == instruction.symbol->name) {
                remove[i] = true;
                ++stats.peepholes_applied;
                changed = true;
                break;
            }
        }
    }

    // ------------------------------------ constant folding, per basic block --
    const ControlFlow flow(items);
    for (std::size_t block_index = 0; block_index < flow.blocks().size(); ++block_index) {
        const Block& block = flow.blocks()[block_index];
        ConstantMap constants;
        for (std::size_t i = block.begin; i < block.end; ++i) {
            if (remove[i] || !isInstruction(items[i])) {
                continue;
            }
            Instruction& instruction = std::get<Instruction>(items[i]);
            isa::Instruction& machine = instruction.machine;

            // A symbolic operand has no value until the linker runs, so it is
            // opaque to folding, but it still clobbers its destination.
            if (instruction.isSymbolic()) {
                constants.clear(machine.dst);
                continue;
            }

            if (machine.opcode == isa::Opcode::MOVI || machine.opcode == isa::Opcode::LEA) {
                constants.set(machine.dst, static_cast<u64>(machine.imm));
                continue;
            }
            if (machine.opcode == isa::Opcode::MOV) {
                const std::optional<u64> value = constants.get(machine.src);
                if (value.has_value() &&
                    byteorder::fitsSigned(static_cast<i64>(*value), isa::kImmediateBits)) {
                    machine.opcode = isa::Opcode::MOVI;
                    machine.imm = static_cast<i64>(*value);
                    machine.src = isa::Reg::R0;
                    constants.set(machine.dst, *value);
                    ++stats.constants_folded;
                    changed = true;
                } else if (value.has_value()) {
                    constants.set(machine.dst, *value);
                } else {
                    constants.clear(machine.dst);
                }
                continue;
            }

            const bool binary = isa::isBinaryAlu(machine.opcode);
            const bool unary = isa::isUnaryAlu(machine.opcode);
            if (!binary && !unary) {
                // Anything else (LOAD, POP, ...) makes its destination unknown.
                if (writesRegister(machine, machine.dst)) {
                    constants.clear(machine.dst);
                }
                continue;
            }

            const std::optional<u64> lhs = constants.get(machine.dst);
            const std::optional<u64> rhs =
                binary ? constants.get(machine.src) : std::optional<u64>{0};
            if (!lhs.has_value() || !rhs.has_value()) {
                if (writesRegister(machine, machine.dst)) {
                    constants.clear(machine.dst);
                }
                continue;
            }

            const std::expected<isa::AluResult, isa::AluError> evaluated =
                binary ? isa::evaluateBinary(machine.opcode, *lhs, *rhs)
                       : isa::evaluateUnary(machine.opcode, *lhs);
            if (!evaluated.has_value()) {
                // A division by zero must be left in place so the VM traps at
                // exactly the point the program asked for it.
                constants.clear(machine.dst);
                continue;
            }
            if (!evaluated->writes_value) {
                // CMP / TEST: no value to fold, and the flags are the point.
                continue;
            }
            // Replacing the instruction with MOVI also drops its FLAGS write,
            // so only do it where the flags cannot be observed.
            const bool flags_ok = !evaluated->writes_flags || flow.flagsDeadAfter(block_index, i);
            const bool representable =
                byteorder::fitsSigned(static_cast<i64>(evaluated->value), isa::kImmediateBits);
            if (flags_ok && representable) {
                machine.opcode = isa::Opcode::MOVI;
                machine.imm = static_cast<i64>(evaluated->value);
                machine.src = isa::Reg::R0;
                ++stats.constants_folded;
                changed = true;
            }
            constants.set(machine.dst, evaluated->value);
        }
    }

    // ------------------------------------------ identities, per basic block --
    for (const Block& block : flow.blocks()) {
        for (std::size_t i = block.begin; i < block.end; ++i) {
            if (remove[i] || !isInstruction(items[i])) {
                continue;
            }
            const Instruction& instruction = std::get<Instruction>(items[i]);
            const isa::Instruction& machine = instruction.machine;
            if (instruction.isSymbolic()) {
                continue;
            }
            // `MOV rX, rX` writes nothing and sets no flags, and NOP is by
            // definition removable. Arithmetic identities such as `ADD rX, rY`
            // where rY is known to be zero are left to constant folding, which
            // already knows whether the flags matter.
            const bool is_self_move =
                machine.opcode == isa::Opcode::MOV && machine.dst == machine.src;
            const bool is_nop = machine.opcode == isa::Opcode::NOP;
            if (is_self_move || is_nop) {
                remove[i] = true;
                ++stats.identities_eliminated;
                changed = true;
            }
        }
    }

    // ------------------------------------- dead stores, per basic block ------
    // A write is dead when the same register is written again later in the
    // block with no intervening read. Nothing is assumed about liveness across
    // the block boundary, so this is safe without a global analysis.
    for (const Block& block : flow.blocks()) {
        for (std::size_t i = block.begin; i < block.end; ++i) {
            if (remove[i] || !isInstruction(items[i])) {
                continue;
            }
            const Instruction& instruction = std::get<Instruction>(items[i]);
            const isa::Instruction& machine = instruction.machine;
            const isa::OpcodeInfo* info = isa::findOpcode(machine.opcode);
            // Only pure register-defining moves are candidates: an instruction
            // that also sets flags, touches memory or reads its destination has
            // effects beyond the register.
            const bool pure_define =
                info != nullptr && info->writes_dst && !info->reads_dst && !info->writes_flags &&
                (machine.opcode == isa::Opcode::MOVI || machine.opcode == isa::Opcode::MOV ||
                 machine.opcode == isa::Opcode::LEA);
            if (!pure_define) {
                continue;
            }
            for (std::size_t j = i + 1; j < block.end; ++j) {
                if (remove[j] || !isInstruction(items[j])) {
                    continue;
                }
                const isa::Instruction& later = std::get<Instruction>(items[j]).machine;
                if (readsRegister(later, machine.dst)) {
                    break;
                }
                if (writesRegister(later, machine.dst)) {
                    remove[i] = true;
                    ++stats.dead_stores_removed;
                    changed = true;
                    break;
                }
            }
        }
    }

    if (changed) {
        std::vector<Item> kept;
        kept.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!remove[i]) {
                kept.push_back(std::move(items[i]));
            }
        }
        items = std::move(kept);
    }
    return changed;
}

}  // namespace minitool::optimizer
