// SPDX-License-Identifier: MIT
#include <string_view>
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;
using testkit::assemble;

const object::Section* section(const object::ObjectFile& object, std::string_view name) {
    return object.findSection(name);
}

/// Decodes the instruction at `index` of .text.
isa::Instruction instructionAt(const object::ObjectFile& object, std::size_t index) {
    const object::Section* text = section(object, ".text");
    const std::span<const u8> bytes{text->data};
    const std::expected<isa::Instruction, isa::DecodeError> decoded =
        isa::decodeFrom(bytes.subspan(index * isa::kInstructionSize, isa::kInstructionSize));
    return decoded.value_or(isa::Instruction{});
}

TEST(Assembler, EncodesInstructionsIntoText) {
    const testkit::Assembled result = assemble("MOVI R1, 42\nADD R1, R2\nHALT\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    const object::Section* text = section(result.object, ".text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->data.size(), 3U * isa::kInstructionSize);
    EXPECT_EQ(instructionAt(result.object, 0),
              isa::Instruction::regImm(isa::Opcode::MOVI, isa::Reg::R1, 42));
    EXPECT_EQ(instructionAt(result.object, 1),
              isa::Instruction::reg2(isa::Opcode::ADD, isa::Reg::R1, isa::Reg::R2));
    EXPECT_EQ(instructionAt(result.object, 2), isa::Instruction::none(isa::Opcode::HALT));
}

TEST(Assembler, PlacesLabelsAtTheirOffsets) {
    const testkit::Assembled result = assemble("first:\n    NOP\nsecond:\n    NOP\nthird:\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    EXPECT_EQ(result.object.symbols.find("first")->value, 0U);
    EXPECT_EQ(result.object.symbols.find("second")->value, 8U);
    EXPECT_EQ(result.object.symbols.find("third")->value, 16U);
    EXPECT_TRUE(result.object.symbols.find("third")->defined);
}

TEST(Assembler, ResolvesForwardAndBackwardReferencesWithRelocations) {
    const testkit::Assembled result =
        assemble("start:\n    JMP end\n    NOP\nend:\n    JMP start\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    ASSERT_EQ(result.object.relocations.size(), 2U);
    for (const object::Relocation& relocation : result.object.relocations) {
        // A branch to a label is always PC-relative into the immediate field.
        EXPECT_EQ(relocation.type, object::RelocationType::PCREL48);
        EXPECT_EQ(relocation.addend, 0);
    }
    EXPECT_EQ(result.object.relocations[0].offset, 0U);
    EXPECT_EQ(result.object.relocations[1].offset, 16U);
    // The placeholder is zero until the linker patches it.
    EXPECT_EQ(instructionAt(result.object, 0).imm, 0);
}

TEST(Assembler, UsesAbsoluteRelocationsForAddresses) {
    const testkit::Assembled result =
        assemble(".data\nvalue:\n.qword 7\n.text\n    LEA R1, value\n    MOVI R2, value + 8\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    ASSERT_EQ(result.object.relocations.size(), 2U);
    EXPECT_EQ(result.object.relocations[0].type, object::RelocationType::IMM48);
    EXPECT_EQ(result.object.relocations[1].type, object::RelocationType::IMM48);
    EXPECT_EQ(result.object.relocations[1].addend, 8);
}

TEST(Assembler, EmitsDataDirectives) {
    const testkit::Assembled result = assemble(
        ".data\n"
        "bytes:  .byte 1, 2, 3\n"
        "word:   .word 0x1234\n"
        "text:   .asciz \"hi\"\n"
        "quad:   .qword 0x1122334455667788\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    const object::Section* data = section(result.object, ".data");
    ASSERT_NE(data, nullptr);
    const std::vector<u8> expected{1,    2,    3,  // .byte
                                   0x34, 0x12,     // .word, little-endian
                                   'h',  'i',  0,  // .asciz
                                   0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    EXPECT_EQ(data->data, expected);
    EXPECT_EQ(result.object.symbols.find("word")->value, 3U);
    EXPECT_EQ(result.object.symbols.find("quad")->value, 8U);
}

TEST(Assembler, HonoursAlignAndSpace) {
    const testkit::Assembled result =
        assemble(".data\n.byte 1\n.align 8\naligned:\n.space 4, 0xAB\n.byte 2\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    const object::Section* data = section(result.object, ".data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(result.object.symbols.find("aligned")->value, 8U);
    const std::vector<u8> expected{1, 0, 0, 0, 0, 0, 0, 0, 0xAB, 0xAB, 0xAB, 0xAB, 2};
    EXPECT_EQ(data->data, expected);
}

TEST(Assembler, GivesBssASizeButNoData) {
    const testkit::Assembled result = assemble(".bss\nbuffer:\n.space 64\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    const object::Section* bss = section(result.object, ".bss");
    ASSERT_NE(bss, nullptr);
    EXPECT_EQ(bss->size, 64U);
    EXPECT_TRUE(bss->data.empty());
}

TEST(Assembler, RejectsDataAndCodeInTheWrongSections) {
    const testkit::Assembled data_in_bss = assemble(".bss\n.byte 1\n");
    EXPECT_FALSE(data_in_bss.ok);
    EXPECT_TRUE(data_in_bss.mentions("cannot appear in .bss"));

    const testkit::Assembled code_in_data = assemble(".data\n    NOP\n");
    EXPECT_FALSE(code_in_data.ok);
    EXPECT_TRUE(code_in_data.mentions("only appear in .text"));
}

TEST(Assembler, AppliesSymbolBindings) {
    const testkit::Assembled result =
        assemble(".global visible\n.extern elsewhere\n.weak maybe\nvisible:\n    CALL elsewhere\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    EXPECT_EQ(result.object.symbols.find("visible")->binding, SymbolBinding::Global);
    EXPECT_TRUE(result.object.symbols.find("visible")->defined);
    EXPECT_EQ(result.object.symbols.find("elsewhere")->binding, SymbolBinding::Extern);
    EXPECT_FALSE(result.object.symbols.find("elsewhere")->defined);
    EXPECT_EQ(result.object.symbols.find("maybe")->binding, SymbolBinding::Weak);
}

TEST(Assembler, TreatsUnknownNamesAsExternal) {
    const testkit::Assembled result = assemble("    CALL somewhere_else\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    const Symbol* symbol = result.object.symbols.find("somewhere_else");
    ASSERT_NE(symbol, nullptr);
    EXPECT_EQ(symbol->binding, SymbolBinding::Extern);
}

TEST(Assembler, RejectsAnUndefinedLocalLabel) {
    // A `.L` label cannot come from another file, so this can never link.
    const testkit::Assembled result = assemble("    JMP .Lmissing\n");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.mentions("local label '.Lmissing' is used but never defined"));
}

TEST(Assembler, RejectsAnExternThatIsAlsoDefined) {
    const testkit::Assembled result = assemble(".extern here\nhere:\n    NOP\n");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.mentions("declared .extern but also defined"));
}

TEST(Assembler, RecordsALineForEveryInstruction) {
    const testkit::Assembled result = assemble("NOP\nHALT\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    ASSERT_EQ(result.object.debug_info.size(), 2U);
    EXPECT_EQ(result.object.debug_info[0].line, 1U);
    EXPECT_EQ(result.object.debug_info[0].offset, 0U);
    EXPECT_EQ(result.object.debug_info[1].line, 2U);
    EXPECT_EQ(result.object.debug_info[1].offset, 8U);
    ASSERT_EQ(result.object.source_files.size(), 1U);
    EXPECT_EQ(result.object.source_files[0], "test.asm");
}

TEST(Assembler, OrdersSectionsCanonicallyWhateverTheSourceDid) {
    const testkit::Assembled result =
        assemble(".bss\n.space 8\n.data\n.byte 1\n.text\nNOP\n.rodata\n.byte 2\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    ASSERT_EQ(result.object.sections.size(), 4U);
    EXPECT_EQ(result.object.sections[0].name, ".text");
    EXPECT_EQ(result.object.sections[1].name, ".rodata");
    EXPECT_EQ(result.object.sections[2].name, ".data");
    EXPECT_EQ(result.object.sections[3].name, ".bss");
}

TEST(Assembler, InterleavesSectionsInSourceOrder) {
    // Switching back to a section appends to it rather than starting over.
    const testkit::Assembled result =
        assemble(".data\n.byte 1\n.text\nNOP\n.data\n.byte 2\n.text\nHALT\n");
    ASSERT_TRUE(result.ok) << result.diagnostics;
    EXPECT_EQ(section(result.object, ".data")->data, (std::vector<u8>{1, 2}));
    EXPECT_EQ(section(result.object, ".text")->data.size(), 2U * isa::kInstructionSize);
}

}  // namespace
