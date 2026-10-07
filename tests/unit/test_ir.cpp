// SPDX-License-Identifier: MIT
#include <string_view>
#include <variant>

#include "minitool/assembler/sema.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/ir/lower.hpp"
#include "minitool/lexer/lexer.hpp"
#include "minitool/parser/parser.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;

struct Lowered {
    SourceManager sources;
    std::unique_ptr<diag::DiagnosticEngine> engine;
    std::expected<ir::Module, std::string> module = std::unexpected(std::string{});

    [[nodiscard]] bool ok() const { return module.has_value(); }
    [[nodiscard]] const ir::Section& text() const {
        return *module->findSection(ir::SectionKind::Text);
    }
};

std::unique_ptr<Lowered> lower(std::string_view source) {
    auto lowered = std::make_unique<Lowered>();
    const FileId file = lowered->sources.addFile("test.asm", std::string{source});
    lowered->engine = std::make_unique<diag::DiagnosticEngine>(lowered->sources);
    lexer::Lexer lexer(lowered->sources.text(file), file);
    parser::Parser parser(lexer, *lowered->engine);
    const std::expected<ast::Program, std::string> program = parser.parse();
    if (!program.has_value()) {
        return lowered;
    }
    SemanticAnalyzer analyzer(*lowered->engine);
    if (!analyzer.analyze(*program)) {
        return lowered;
    }
    lowered->module = ir::lower(*program, "test.asm", *lowered->engine);
    return lowered;
}

TEST(Ir, LowersInstructionsWithTheirOpcodes) {
    const auto lowered = lower("MOVI R1, 5\nADD R1, R2\nHALT\n");
    ASSERT_TRUE(lowered->ok());
    const ir::Section& text = lowered->text();
    ASSERT_EQ(text.items.size(), 3U);

    const auto& first = std::get<ir::Instruction>(text.items[0]);
    EXPECT_EQ(first.machine.opcode, isa::Opcode::MOVI);
    EXPECT_EQ(first.machine.dst, isa::Reg::R1);
    EXPECT_EQ(first.machine.imm, 5);
    EXPECT_FALSE(first.isSymbolic());
    EXPECT_EQ(first.location.line, 1U);

    const auto& second = std::get<ir::Instruction>(text.items[1]);
    EXPECT_EQ(second.machine.opcode, isa::Opcode::ADD);
    EXPECT_EQ(second.machine.src, isa::Reg::R2);
}

TEST(Ir, KeepsSymbolicOperandsSymbolic) {
    const auto lowered = lower("JMP target + 16\ntarget:\n    HALT\n");
    ASSERT_TRUE(lowered->ok());
    const auto& jump = std::get<ir::Instruction>(lowered->text().items.at(0));
    ASSERT_TRUE(jump.isSymbolic());
    EXPECT_EQ(jump.symbol->name, "target");
    EXPECT_EQ(jump.symbol->addend, 16);
    // The immediate stays zero: the value is not known until the link.
    EXPECT_EQ(jump.machine.imm, 0);
}

TEST(Ir, LowersMemoryOperandsInBothDirections) {
    const auto lowered = lower("LOAD R1, [R2 + 8]\nSTORE [R3 - 8], R4\n");
    ASSERT_TRUE(lowered->ok());
    const auto& load = std::get<ir::Instruction>(lowered->text().items.at(0));
    EXPECT_EQ(load.machine.dst, isa::Reg::R1);
    EXPECT_EQ(load.machine.src, isa::Reg::R2);
    EXPECT_EQ(load.machine.imm, 8);

    const auto& store = std::get<ir::Instruction>(lowered->text().items.at(1));
    // STORE encodes the base register in dst and the stored value in src.
    EXPECT_EQ(store.machine.dst, isa::Reg::R3);
    EXPECT_EQ(store.machine.src, isa::Reg::R4);
    EXPECT_EQ(store.machine.imm, -8);
}

TEST(Ir, SplitsStatementsIntoSections) {
    const auto lowered = lower(".text\nNOP\n.data\n.byte 1\n.bss\n.space 8\n");
    ASSERT_TRUE(lowered->ok());
    EXPECT_NE(lowered->module->findSection(ir::SectionKind::Text), nullptr);
    EXPECT_NE(lowered->module->findSection(ir::SectionKind::Data), nullptr);
    EXPECT_NE(lowered->module->findSection(ir::SectionKind::Bss), nullptr);
    EXPECT_EQ(lowered->module->findSection(ir::SectionKind::Rodata), nullptr);
    EXPECT_EQ(lowered->module->instructionCount(), 1U);
}

TEST(Ir, MergesLiteralDataIntoRunsButNotAcrossSymbols) {
    const auto lowered = lower(".data\n.byte 1, 2\n.qword thing\n.byte 3\nthing:\n");
    ASSERT_TRUE(lowered->ok());
    const ir::Section& data = *lowered->module->findSection(ir::SectionKind::Data);
    ASSERT_EQ(data.items.size(), 4U);
    EXPECT_TRUE(std::holds_alternative<ir::Bytes>(data.items[0]));
    EXPECT_EQ(std::get<ir::Bytes>(data.items[0]).data.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<ir::SymbolValue>(data.items[1]));
    EXPECT_EQ(std::get<ir::SymbolValue>(data.items[1]).width, 8U);
    EXPECT_EQ(std::get<ir::SymbolValue>(data.items[1]).symbol.name, "thing");
    EXPECT_TRUE(std::holds_alternative<ir::Bytes>(data.items[2]));
    EXPECT_TRUE(std::holds_alternative<ir::Label>(data.items[3]));
}

TEST(Ir, RecordsSymbolDeclarations) {
    const auto lowered = lower(".global a\n.extern b\n.weak c\na:\n");
    ASSERT_TRUE(lowered->ok());
    EXPECT_EQ(lowered->module->globals, (std::vector<std::string>{"a"}));
    EXPECT_EQ(lowered->module->externs, (std::vector<std::string>{"b"}));
    EXPECT_EQ(lowered->module->weaks, (std::vector<std::string>{"c"}));
}

TEST(Ir, StartsInTextWithoutASectionDirective) {
    const auto lowered = lower("NOP\n");
    ASSERT_TRUE(lowered->ok());
    EXPECT_EQ(lowered->module->instructionCount(), 1U);
    EXPECT_NE(lowered->module->findSection(ir::SectionKind::Text), nullptr);
}

TEST(Ir, SectionNamesRoundTrip) {
    for (const ir::SectionKind kind : {ir::SectionKind::Text, ir::SectionKind::Rodata,
                                       ir::SectionKind::Data, ir::SectionKind::Bss}) {
        EXPECT_EQ(ir::sectionKindFromName(ir::sectionName(kind)), kind);
    }
    EXPECT_FALSE(ir::sectionKindFromName(".nope").has_value());
    EXPECT_FALSE(ir::sectionHasData(ir::SectionKind::Bss));
    EXPECT_TRUE(ir::sectionHasData(ir::SectionKind::Text));
}

}  // namespace
