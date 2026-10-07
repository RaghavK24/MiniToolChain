// SPDX-License-Identifier: MIT
#include <string>
#include <string_view>
#include <variant>

#include "minitool/common/source_manager.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/parser/parser.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using namespace minitool::ast;

/// Parses `code` and keeps the diagnostics around so a test can assert on the
/// message, not just on failure.
struct Parsed {
    SourceManager sources;
    std::unique_ptr<diag::DiagnosticEngine> engine;
    std::expected<Program, std::string> result = std::unexpected(std::string{});
    std::string rendered;

    [[nodiscard]] bool ok() const { return result.has_value(); }
    [[nodiscard]] const Program& program() const { return *result; }
    [[nodiscard]] bool mentions(std::string_view text) const {
        return rendered.find(text) != std::string::npos;
    }
};

std::unique_ptr<Parsed> parse(std::string_view code) {
    auto parsed = std::make_unique<Parsed>();
    const FileId file = parsed->sources.addFile("test.asm", std::string{code});
    parsed->engine = std::make_unique<diag::DiagnosticEngine>(parsed->sources);
    lexer::Lexer lexer(parsed->sources.text(file), file);
    parser::Parser parser(lexer, *parsed->engine);
    parsed->result = parser.parse();
    parsed->rendered = parsed->engine->renderAll();
    return parsed;
}

TEST(Parser, ParsesLabelsInstructionsAndOperands) {
    const auto parsed = parse("_start:\n    MOVI R1, 10\n    HALT\n");
    ASSERT_TRUE(parsed->ok());
    const Program& program = parsed->program();
    ASSERT_EQ(program.statements.size(), 3U);

    ASSERT_TRUE(std::holds_alternative<LabelNode>(program.statements[0]));
    EXPECT_EQ(std::get<LabelNode>(program.statements[0]).name, "_start");
    EXPECT_FALSE(std::get<LabelNode>(program.statements[0]).local);

    const auto& instruction = std::get<InstructionNode>(program.statements[1]);
    EXPECT_EQ(instruction.mnemonic, "MOVI");
    ASSERT_EQ(instruction.operands.size(), 2U);
    EXPECT_EQ(instruction.operands[0].type, OperandType::Register);
    EXPECT_EQ(instruction.operands[0].reg, isa::Reg::R1);
    EXPECT_EQ(instruction.operands[1].type, OperandType::Immediate);
    EXPECT_EQ(instruction.operands[1].immediate, 10);
}

TEST(Parser, AcceptsALabelAndAnInstructionOnOneLine) {
    const auto parsed = parse("loop: done: ADD R1, R2\n");
    ASSERT_TRUE(parsed->ok());
    ASSERT_EQ(parsed->program().statements.size(), 3U);
    EXPECT_EQ(std::get<LabelNode>(parsed->program().statements[0]).name, "loop");
    EXPECT_EQ(std::get<LabelNode>(parsed->program().statements[1]).name, "done");
    EXPECT_EQ(std::get<InstructionNode>(parsed->program().statements[2]).mnemonic, "ADD");
}

TEST(Parser, ParsesLocalLabelsAndReferences) {
    const auto parsed = parse(".L1:\n    JMP .L1\n");
    ASSERT_TRUE(parsed->ok());
    ASSERT_EQ(parsed->program().statements.size(), 2U);
    const auto& label = std::get<LabelNode>(parsed->program().statements[0]);
    EXPECT_EQ(label.name, ".L1");
    EXPECT_TRUE(label.local);
    const auto& jump = std::get<InstructionNode>(parsed->program().statements[1]);
    EXPECT_EQ(jump.operands.at(0).type, OperandType::Symbol);
    EXPECT_EQ(jump.operands.at(0).symbol, ".L1");
}

TEST(Parser, ParsesMemoryOperandsInBothDirections) {
    const auto parsed = parse("LOAD R1, [R2 + 8]\nSTORE [R1 - 8], R2\nLOAD R3, [R4]\n");
    ASSERT_TRUE(parsed->ok());
    const Program& program = parsed->program();
    ASSERT_EQ(program.statements.size(), 3U);

    const auto& load = std::get<InstructionNode>(program.statements[0]);
    EXPECT_EQ(load.operands[1].type, OperandType::Memory);
    EXPECT_EQ(load.operands[1].reg, isa::Reg::R2);
    EXPECT_EQ(load.operands[1].immediate, 8);

    const auto& store = std::get<InstructionNode>(program.statements[1]);
    EXPECT_EQ(store.operands[0].type, OperandType::Memory);
    EXPECT_EQ(store.operands[0].immediate, -8);

    const auto& bare = std::get<InstructionNode>(program.statements[2]);
    EXPECT_EQ(bare.operands[1].immediate, 0);
}

TEST(Parser, ParsesSymbolPlusConstantExpressions) {
    const auto parsed = parse(".qword message + 8, message - 4, message\n");
    ASSERT_TRUE(parsed->ok());
    const auto& directive = std::get<DirectiveNode>(parsed->program().statements.at(0));
    ASSERT_EQ(directive.operands.size(), 3U);
    EXPECT_EQ(directive.operands[0].symbol, "message");
    EXPECT_EQ(directive.operands[0].immediate, 8);
    EXPECT_EQ(directive.operands[1].immediate, -4);
    EXPECT_EQ(directive.operands[2].immediate, 0);
}

TEST(Parser, ParsesDirectives) {
    const auto parsed = parse(
        ".section .text\n.global _start\n.extern helper\n.byte 1, 2\n"
        ".asciz \"hi\"\n.align 4\n.space 16, 0xFF\n");
    ASSERT_TRUE(parsed->ok());
    const Program& program = parsed->program();
    ASSERT_EQ(program.statements.size(), 7U);

    const auto& section = std::get<DirectiveNode>(program.statements[0]);
    EXPECT_EQ(section.type, DirectiveType::Section);
    EXPECT_EQ(section.operands.at(0).symbol, ".text");

    EXPECT_EQ(std::get<DirectiveNode>(program.statements[1]).type, DirectiveType::Global);
    EXPECT_EQ(std::get<DirectiveNode>(program.statements[2]).type, DirectiveType::Extern);

    const auto& bytes = std::get<DirectiveNode>(program.statements[3]);
    ASSERT_EQ(bytes.operands.size(), 2U);
    EXPECT_EQ(bytes.operands[0].immediate, 1);
    EXPECT_EQ(bytes.operands[1].immediate, 2);

    const auto& text = std::get<DirectiveNode>(program.statements[4]);
    EXPECT_EQ(text.type, DirectiveType::Asciz);
    // The string arrives decoded, without its quotes: syntax is the lexer's job.
    EXPECT_EQ(text.operands.at(0).text, "hi");

    EXPECT_EQ(std::get<DirectiveNode>(program.statements[5]).operands.at(0).immediate, 4);
    EXPECT_EQ(std::get<DirectiveNode>(program.statements[6]).operands.at(1).immediate, 255);
}

TEST(Parser, AcceptsSectionShorthands) {
    const auto parsed = parse(".text\n.data\n.rodata\n.bss\n");
    ASSERT_TRUE(parsed->ok());
    ASSERT_EQ(parsed->program().statements.size(), 4U);
    EXPECT_EQ(std::get<DirectiveNode>(parsed->program().statements[0]).operands.at(0).symbol,
              ".text");
    EXPECT_EQ(std::get<DirectiveNode>(parsed->program().statements[3]).operands.at(0).symbol,
              ".bss");
}

TEST(Parser, AcceptsTheOptionalHashOnImmediates) {
    const auto parsed = parse("MOVI R1, #10\n");
    ASSERT_TRUE(parsed->ok());
    const auto& instruction = std::get<InstructionNode>(parsed->program().statements.at(0));
    EXPECT_EQ(instruction.operands.at(1).immediate, 10);
}

TEST(Parser, KeepsForwardReferencesAsSymbols) {
    const auto parsed = parse("JMP end\nend:\n");
    ASSERT_TRUE(parsed->ok());
    const auto& jump = std::get<InstructionNode>(parsed->program().statements.at(0));
    EXPECT_EQ(jump.operands.at(0).type, OperandType::Symbol);
    EXPECT_EQ(jump.operands.at(0).symbol, "end");
}

TEST(Parser, ReportsMissingOperand) {
    const auto parsed = parse("MOVI R1,\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_TRUE(parsed->mentions("expected an operand"));
}

TEST(Parser, ReportsUnknownDirective) {
    const auto parsed = parse(".nonsense 1\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_TRUE(parsed->mentions("unknown directive '.nonsense'"));
}

TEST(Parser, ReportsUnclosedMemoryOperand) {
    const auto parsed = parse("LOAD R1, [R2 + 8\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_TRUE(parsed->mentions("']'"));
}

TEST(Parser, ReportsTrailingJunkAfterAStatement) {
    // Note that "HALT R1" is *not* a parse error: how many operands HALT takes
    // is a semantic question, and the parser does not answer those (rule 1).
    const auto parsed = parse("MOVI R1, 5 R2\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_TRUE(parsed->mentions("after end of statement"));
    EXPECT_TRUE(parse("HALT R1\n")->ok());
}

TEST(Parser, RecoversAndReportsEveryBadLine) {
    const auto parsed = parse(".nonsense\nMOVI R1,\n.alsobad\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_EQ(parsed->engine->errorCount(), 3U);
}

TEST(Parser, ForwardsLexicalErrors) {
    const auto parsed = parse("MOVI R1, 0xZZ\n");
    EXPECT_FALSE(parsed->ok());
    EXPECT_TRUE(parsed->mentions("integer"));
}

TEST(Parser, AcceptsAnEmptyProgram) {
    const auto parsed = parse("");
    ASSERT_TRUE(parsed->ok());
    EXPECT_TRUE(parsed->program().statements.empty());

    const auto blank = parse("\n\n   ; just a comment\n\n");
    ASSERT_TRUE(blank->ok());
    EXPECT_TRUE(blank->program().statements.empty());
}

}  // namespace
