// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include "minitool/disassembler/disassembler.hpp"
#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;
using namespace minitool::disassembler;

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::vector<u8> encodeAll(std::span<const isa::Instruction> instructions) {
    std::vector<u8> bytes(instructions.size() * isa::kInstructionSize);
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const std::expected<void, isa::EncodeError> encoded = isa::encodeInto(
            std::span<u8>{bytes}.subspan(i * isa::kInstructionSize, isa::kInstructionSize),
            instructions[i]);
        EXPECT_TRUE(encoded.has_value());
    }
    return bytes;
}

TEST(Disassembler, RendersEveryOperandForm) {
    const std::vector<isa::Instruction> instructions{
        isa::Instruction::regImm(isa::Opcode::MOVI, isa::Reg::R1, 42),
        isa::Instruction::reg2(isa::Opcode::ADD, isa::Reg::R1, isa::Reg::R2),
        isa::Instruction::reg1(isa::Opcode::PUSH, isa::Reg::R3),
        isa::Instruction::mem(isa::Opcode::LOAD, isa::Reg::R1, isa::Reg::R2, 8),
        isa::Instruction::mem(isa::Opcode::STORE, isa::Reg::R1, isa::Reg::R2, -8),
        isa::Instruction::syscall(1),
        isa::Instruction::none(isa::Opcode::HALT),
    };
    const std::string text = Disassembler{}.disassemble(encodeAll(instructions), 0x1000);
    EXPECT_TRUE(contains(text, "MOVI R1, 42"));
    EXPECT_TRUE(contains(text, "ADD R1, R2"));
    EXPECT_TRUE(contains(text, "PUSH R3"));
    EXPECT_TRUE(contains(text, "LOAD R1, [R2 + 8]"));
    EXPECT_TRUE(contains(text, "STORE [R1 - 8], R2"));
    EXPECT_TRUE(contains(text, "SYSCALL 1"));
    EXPECT_TRUE(contains(text, "HALT"));
}

TEST(Disassembler, ShowsAddressesAndRawWords) {
    const std::vector<isa::Instruction> instructions{isa::Instruction::none(isa::Opcode::NOP),
                                                     isa::Instruction::none(isa::Opcode::HALT)};
    const std::string text = Disassembler{}.disassemble(encodeAll(instructions), 0x10000);
    EXPECT_TRUE(contains(text, "0000000000010000:"));
    EXPECT_TRUE(contains(text, "0000000000010008:"));
    EXPECT_TRUE(contains(text, "0100000000000000"));  // the HALT word
}

TEST(Disassembler, ResolvesBranchTargetsToAddresses) {
    // A raw displacement is not what a reader wants; the target is.
    const std::vector<isa::Instruction> instructions{
        isa::Instruction::jump(isa::Opcode::JMP, 8),
        isa::Instruction::none(isa::Opcode::NOP),
        isa::Instruction::none(isa::Opcode::HALT),
    };
    const std::string text = Disassembler{}.disassemble(encodeAll(instructions), 0x1000);
    EXPECT_TRUE(contains(text, "JMP"));
    EXPECT_TRUE(contains(text, "0x1010"));
}

TEST(Disassembler, NamesSymbolsWhenItCan) {
    const testkit::Linked linked =
        testkit::build(".global _start\n_start:\n    CALL helper\n    HALT\nhelper:\n    RET\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const std::string text = Disassembler{}.disassemble(linked.executable);
    EXPECT_TRUE(contains(text, "<_start>:"));
    EXPECT_TRUE(contains(text, "<helper>:"));
    EXPECT_TRUE(contains(text, "CALL"));
    EXPECT_TRUE(contains(text, "helper (0x"));
}

TEST(Disassembler, ShowsAnOffsetIntoASymbol) {
    const testkit::Linked linked =
        testkit::build(".global _start\n_start:\n    NOP\n    JMP _start\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const std::string text = Disassembler{}.disassemble(linked.executable);
    EXPECT_TRUE(contains(text, "_start (0x"));
}

TEST(Disassembler, PrintsUndecodableWordsAsData) {
    // All-ones is not a valid instruction: the disassembler must say so rather
    // than invent a mnemonic.
    const std::vector<u8> bytes(8, 0xFF);
    const std::string text = Disassembler{}.disassemble(bytes, 0);
    EXPECT_TRUE(contains(text, ".qword"));
    EXPECT_TRUE(contains(text, "unknown opcode"));
}

TEST(Disassembler, ReportsTrailingBytes) {
    const std::vector<u8> bytes(12, 0);  // one instruction plus four spare bytes
    const std::string text = Disassembler{}.disassemble(bytes, 0);
    EXPECT_TRUE(contains(text, "4 trailing byte(s)"));
}

TEST(Disassembler, HonoursItsOptions) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;

    Options options;
    options.show_bytes = false;
    options.show_labels = false;
    const std::string plain = Disassembler{options}.disassemble(linked.executable);
    EXPECT_FALSE(contains(plain, "<_start>:"));
    EXPECT_FALSE(contains(plain, "0100000000000000"));
    EXPECT_TRUE(contains(plain, "HALT"));
}

TEST(Disassembler, RoundTripsWithTheAssembler) {
    // What the disassembler prints must re-assemble to the same bytes: the two
    // share a decoder, and this is the check that they also share a syntax.
    constexpr std::string_view kSource =
        ".global _start\n"
        "_start:\n"
        "    MOVI R1, 42\n"
        "    MOV R2, R1\n"
        "    ADD R1, R2\n"
        "    SUB R1, R2\n"
        "    PUSH R1\n"
        "    POP R3\n"
        "    LOAD R4, [R2 + 16]\n"
        "    STORE [R2 - 16], R4\n"
        "    CMP R1, R2\n"
        "    NEG R1\n"
        "    NOT R2\n"
        "    SYSCALL 0\n"
        "    HALT\n";
    const testkit::Assembled original = testkit::assemble(kSource);
    ASSERT_TRUE(original.ok) << original.diagnostics;
    const object::Section* text = original.object.findSection(".text");
    ASSERT_NE(text, nullptr);

    Options options;
    options.show_bytes = false;
    options.show_symbols = false;
    options.show_labels = false;
    const std::string listing = Disassembler{options}.disassemble(text->data, 0);

    // Strip the address column to recover plain assembly.
    std::string reassembled = ".global _start\n_start:\n";
    std::size_t start = 0;
    while (start < listing.size()) {
        const std::size_t end = listing.find('\n', start);
        const std::string line = listing.substr(start, end - start);
        const std::size_t colon = line.find(":  ");
        if (colon != std::string::npos) {
            reassembled += "    " + line.substr(colon + 3) + "\n";
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    const testkit::Assembled again = testkit::assemble(reassembled);
    ASSERT_TRUE(again.ok) << again.diagnostics << "\n" << reassembled;
    EXPECT_EQ(again.object.findSection(".text")->data, text->data);
}

TEST(Disassembler, HandlesAnExecutableWithNoCode) {
    executable::Executable executable;
    executable.segments.push_back(executable::Segment{executable::SegmentType::Data,
                                                      executable::SegmentFlags::Read,
                                                      ".data",
                                                      0x1000,
                                                      8,
                                                      {1, 2, 3, 4, 5, 6, 7, 8}});
    const std::string text = Disassembler{}.disassemble(executable);
    EXPECT_TRUE(contains(text, "no executable segments"));
}

}  // namespace
