// SPDX-License-Identifier: MIT
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "minitool/assembler/sema.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/ir/lower.hpp"
#include "minitool/lexer/lexer.hpp"
#include "minitool/optimizer/optimizer.hpp"
#include "minitool/parser/parser.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

/// Lowers `source` to IR and optimizes it, keeping both the result and the
/// statistics.
struct Optimized {
    ir::Module module;
    optimizer::OptStats stats;

    [[nodiscard]] std::vector<isa::Instruction> instructions() const {
        std::vector<isa::Instruction> result;
        const ir::Section* text = module.findSection(ir::SectionKind::Text);
        if (text == nullptr) {
            return result;
        }
        for (const ir::Item& item : text->items) {
            if (std::holds_alternative<ir::Instruction>(item)) {
                result.push_back(std::get<ir::Instruction>(item).machine);
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<std::string> labels() const {
        std::vector<std::string> names;
        const ir::Section* text = module.findSection(ir::SectionKind::Text);
        if (text == nullptr) {
            return names;
        }
        for (const ir::Item& item : text->items) {
            if (std::holds_alternative<ir::Label>(item)) {
                names.push_back(std::get<ir::Label>(item).name);
            }
        }
        return names;
    }
};

Optimized optimize(std::string_view source, optimizer::OptLevel level = optimizer::OptLevel::O1) {
    SourceManager sources;
    const FileId file = sources.addFile("test.asm", std::string{source});
    diag::DiagnosticEngine diagnostics(sources);
    lexer::Lexer lexer(sources.text(file), file);
    parser::Parser parser(lexer, diagnostics);
    const std::expected<ast::Program, std::string> program = parser.parse();
    EXPECT_TRUE(program.has_value()) << diagnostics.renderAll();
    if (!program.has_value()) {
        return {};
    }
    SemanticAnalyzer analyzer(diagnostics);
    EXPECT_TRUE(analyzer.analyze(*program)) << diagnostics.renderAll();

    std::expected<ir::Module, std::string> module = ir::lower(*program, "test.asm", diagnostics);
    EXPECT_TRUE(module.has_value());
    Optimized result;
    if (!module.has_value()) {
        return result;
    }
    const optimizer::Optimizer optimizer(level);
    result.stats = optimizer.run(*module);
    result.module = std::move(*module);
    return result;
}

TEST(Optimizer, O0ChangesNothing) {
    const Optimized result =
        optimize("MOVI R1, 2\nMOVI R2, 3\nADD R1, R2\nHALT\n", optimizer::OptLevel::O0);
    EXPECT_EQ(result.instructions().size(), 4U);
    EXPECT_EQ(result.stats.total(), 0U);
}

TEST(Optimizer, FoldsConstantArithmetic) {
    const Optimized result = optimize("MOVI R1, 40\nMOVI R2, 2\nADD R1, R2\nHALT\n");
    const std::vector<isa::Instruction> instructions = result.instructions();
    // ADD becomes MOVI 42, and the two feeding MOVIs become dead.
    ASSERT_FALSE(instructions.empty());
    EXPECT_GT(result.stats.constants_folded, 0U);
    bool found = false;
    for (const isa::Instruction& instruction : instructions) {
        if (instruction.opcode == isa::Opcode::MOVI && instruction.dst == isa::Reg::R1 &&
            instruction.imm == 42) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Optimizer, DoesNotFoldWhenTheFlagsAreLive) {
    // The ADD's flags are read by the JE, so rewriting it to MOVI, which sets
    // no flags, would send the program down the wrong branch.
    const Optimized result =
        optimize("MOVI R1, 1\nMOVI R2, -1\nADD R1, R2\nJE zero\nHALT\nzero:\n    HALT\n");
    bool has_add = false;
    for (const isa::Instruction& instruction : result.instructions()) {
        has_add = has_add || instruction.opcode == isa::Opcode::ADD;
    }
    EXPECT_TRUE(has_add) << "folded an ADD whose flags are used";
    EXPECT_EQ(result.stats.constants_folded, 0U);
}

TEST(Optimizer, FoldsWhenTheFlagsAreDeadAcrossBlocks) {
    // Here the ADD's flags reach a block that overwrites them with a CMP before
    // anything reads them, so folding is safe, and the liveness analysis has
    // to look past the end of the block to see that.
    const Optimized result = optimize(
        "MOVI R1, 40\nMOVI R2, 2\nADD R1, R2\nJMP next\nnext:\n    CMP R1, R2\n    JE done\n"
        "done:\n    HALT\n");
    EXPECT_GT(result.stats.constants_folded, 0U);
}

TEST(Optimizer, DoesNotFoldDivisionByZero) {
    // The trap has to happen where the program asked for it.
    const Optimized result = optimize("MOVI R1, 1\nMOVI R2, 0\nDIV R1, R2\nHALT\n");
    bool has_div = false;
    for (const isa::Instruction& instruction : result.instructions()) {
        has_div = has_div || instruction.opcode == isa::Opcode::DIV;
    }
    EXPECT_TRUE(has_div);
}

TEST(Optimizer, RemovesSelfMovesAndNops) {
    const Optimized result = optimize("MOV R1, R1\nNOP\nMOVI R2, 1\nHALT\n");
    for (const isa::Instruction& instruction : result.instructions()) {
        EXPECT_NE(instruction.opcode, isa::Opcode::NOP);
    }
    EXPECT_GT(result.stats.identities_eliminated, 0U);
}

TEST(Optimizer, RemovesDeadStores) {
    const Optimized result = optimize("MOVI R1, 1\nMOVI R1, 2\nHALT\n");
    const std::vector<isa::Instruction> instructions = result.instructions();
    ASSERT_EQ(instructions.size(), 2U);
    EXPECT_EQ(instructions[0].imm, 2);
    EXPECT_EQ(result.stats.dead_stores_removed, 1U);
}

TEST(Optimizer, KeepsAStoreThatIsRead) {
    // PUSH reads R1 and cannot be folded away, so the first MOVI is live.
    const Optimized result = optimize("MOVI R1, 1\nPUSH R1\nMOVI R1, 2\nHALT\n");
    EXPECT_EQ(result.stats.dead_stores_removed, 0U);
    bool has_first = false;
    for (const isa::Instruction& instruction : result.instructions()) {
        has_first = has_first || (instruction.opcode == isa::Opcode::MOVI &&
                                  instruction.dst == isa::Reg::R1 && instruction.imm == 1);
    }
    EXPECT_TRUE(has_first);
}

TEST(Optimizer, RemovesUnreachableCode) {
    const Optimized result = optimize("HALT\nMOVI R1, 1\nMOVI R2, 2\nreachable:\nNOP\nHALT\n");
    EXPECT_EQ(result.stats.unreachable_removed, 2U);
    // The label is still there: it is a branch target, and deleting one would
    // break every branch that names it.
    EXPECT_EQ(result.labels(), (std::vector<std::string>{"reachable"}));
}

TEST(Optimizer, CollapsesPushPopPairs) {
    const Optimized same = optimize("MOVI R1, 5\nPUSH R1\nPOP R1\nHALT\n");
    for (const isa::Instruction& instruction : same.instructions()) {
        EXPECT_NE(instruction.opcode, isa::Opcode::PUSH);
        EXPECT_NE(instruction.opcode, isa::Opcode::POP);
    }

    const Optimized moved = optimize("MOVI R1, 5\nPUSH R1\nPOP R2\nHALT\n");
    bool has_move = false;
    for (const isa::Instruction& instruction : moved.instructions()) {
        if (instruction.opcode == isa::Opcode::MOV || instruction.opcode == isa::Opcode::MOVI) {
            has_move = has_move || instruction.dst == isa::Reg::R2;
        }
        EXPECT_NE(instruction.opcode, isa::Opcode::PUSH);
    }
    EXPECT_TRUE(has_move);
}

TEST(Optimizer, RemovesAJumpToTheNextInstruction) {
    const Optimized result = optimize("JMP next\nnext:\n    HALT\n");
    for (const isa::Instruction& instruction : result.instructions()) {
        EXPECT_NE(instruction.opcode, isa::Opcode::JMP);
    }
    EXPECT_EQ(result.labels(), (std::vector<std::string>{"next"}));
}

TEST(Optimizer, NeverFoldsAcrossALabel) {
    // R1 is only known to be 1 on the fall-through path; the branch could
    // arrive with anything in it.
    const Optimized result = optimize("MOVI R1, 1\ntarget:\nMOVI R2, 2\nADD R1, R2\nJMP target\n");
    bool has_add = false;
    for (const isa::Instruction& instruction : result.instructions()) {
        has_add = has_add || instruction.opcode == isa::Opcode::ADD;
    }
    EXPECT_TRUE(has_add);
}

TEST(Optimizer, TreatsSymbolicOperandsAsUnknown) {
    const Optimized result =
        optimize(".data\nvalue:\n.qword 1\n.text\nLEA R1, value\nMOVI R2, 1\nADD R1, R2\nHALT\n");
    bool has_add = false;
    for (const isa::Instruction& instruction : result.instructions()) {
        has_add = has_add || instruction.opcode == isa::Opcode::ADD;
    }
    EXPECT_TRUE(has_add) << "folded an address that is not known until link time";
}

TEST(Optimizer, ReportsWhatItDid) {
    const Optimized result = optimize("MOVI R1, 40\nMOVI R2, 2\nADD R1, R2\nHALT\nNOP\n");
    EXPECT_EQ(result.stats.instructions_before, 5U);
    EXPECT_LT(result.stats.instructions_after, result.stats.instructions_before);
    EXPECT_TRUE(result.stats.summary().find("instructions") != std::string::npos);
    EXPECT_EQ(optimizer::optLevelName(optimizer::OptLevel::O1), "O1");
}

// --- semantic equivalence ---------------------------------------------------
//
// Every optimization has to preserve what the program does. These run the same
// source at both levels and compare the observable result, which is the only
// standard the optimizer is held to.

void expectSameBehaviour(std::string_view source) {
    const testkit::Ran unoptimized = testkit::runSource(source, optimizer::OptLevel::O0);
    const testkit::Ran optimized = testkit::runSource(source, optimizer::OptLevel::O1);
    ASSERT_TRUE(unoptimized.ok) << unoptimized.message;
    ASSERT_TRUE(optimized.ok) << optimized.message;
    EXPECT_EQ(unoptimized.output, optimized.output);
    EXPECT_EQ(unoptimized.exit_code, optimized.exit_code);
    for (unsigned i = 0; i < isa::kRegisterCount; ++i) {
        EXPECT_EQ(unoptimized.reg(i), optimized.reg(i)) << "register R" << i;
    }
}

TEST(OptimizerEquivalence, Arithmetic) {
    expectSameBehaviour(
        ".global _start\n_start:\n"
        "    MOVI R1, 40\n    MOVI R2, 2\n    ADD R1, R2\n"
        "    MOVI R3, 6\n    MOVI R4, 7\n    MUL R3, R4\n"
        "    MOV R5, R5\n    NOP\n    HALT\n");
}

TEST(OptimizerEquivalence, Loops) {
    expectSameBehaviour(R"(
.global _start
_start:
    MOVI R1, 10
    MOVI R2, 0
    MOVI R3, 0
loop:
    CMP R1, R3
    JLE done
    ADD R2, R1
    DEC R1
    JMP loop
done:
    HALT
)");
}

TEST(OptimizerEquivalence, CallsAndTheStack) {
    expectSameBehaviour(R"(
.global _start
_start:
    MOVI R1, 3
    PUSH R1
    POP R2
    CALL helper
    HALT
helper:
    MOVI R3, 9
    PUSH R3
    POP R4
    RET
)");
}

TEST(OptimizerEquivalence, MemoryAndOutput) {
    expectSameBehaviour(R"(
.global _start
.rodata
message: .asciz "ok\n"
.data
slot:    .qword 0
.text
_start:
    LEA  R5, slot
    MOVI R6, 123
    STORE [R5 + 0], R6
    LOAD R7, [R5 + 0]
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 3
    SYSCALL 1
    HALT
)");
}

TEST(OptimizerEquivalence, ConditionalsThatDependOnFlags) {
    expectSameBehaviour(R"(
.global _start
_start:
    MOVI R1, 1
    MOVI R2, -1
    ADD R1, R2
    JE was_zero
    MOVI R3, 100
    JMP end
was_zero:
    MOVI R3, 200
end:
    HALT
)");
}

}  // namespace
