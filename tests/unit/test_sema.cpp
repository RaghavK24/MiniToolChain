// SPDX-License-Identifier: MIT
#include <string_view>

#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using testkit::assemble;

/// Semantic analysis runs inside the pipeline, so these tests assert on what a
/// user would see: the assembly fails, with a message that names the problem.
void expectRejected(std::string_view source, std::string_view message) {
    const testkit::Assembled result = assemble(source);
    EXPECT_FALSE(result.ok) << source;
    EXPECT_TRUE(result.mentions(message)) << result.diagnostics;
}

void expectAccepted(std::string_view source) {
    const testkit::Assembled result = assemble(source);
    EXPECT_TRUE(result.ok) << result.diagnostics << result.error;
}

TEST(Sema, RejectsUnknownInstructions) {
    expectRejected("FROBNICATE R1\n", "unknown instruction 'FROBNICATE'");
}

TEST(Sema, ChecksOperandCounts) {
    expectRejected("HALT R1\n", "takes 0 operands");
    expectRejected("MOVI R1\n", "takes 2 operands");
    expectRejected("ADD R1, R2, R3\n", "takes 2 operands");
    expectRejected("PUSH\n", "takes 1 operand");
}

TEST(Sema, ChecksOperandKinds) {
    expectRejected("MOV R1, 5\n", "expects a register");
    expectRejected("ADD 5, R2\n", "expects a register");
    expectRejected("MOVI R1, [R2]\n", "expects an immediate or a symbol");
    expectRejected("LOAD R1, R2\n", "memory operand");
    expectRejected("STORE R1, R2\n", "memory operand");
    expectRejected("JMP R1\n", "expects a label or a displacement");
    expectRejected("SYSCALL R1\n", "literal service number");
}

TEST(Sema, AcceptsBothMemoryOperandOrders) {
    expectAccepted("LOAD R1, [R2 + 8]\nSTORE [R3 - 16], R4\n");
}

TEST(Sema, ChecksImmediateRanges) {
    // The immediate field is 48 bits signed.
    expectAccepted("MOVI R1, 140737488355327\n");
    expectRejected("MOVI R1, 140737488355328\n", "does not fit in 48 signed bits");
    expectAccepted("MOVI R1, -140737488355328\n");
    expectRejected("MOVI R1, -140737488355329\n", "does not fit in 48 signed bits");
    expectRejected("SYSCALL -1\n", "out of range");
}

TEST(Sema, RejectsUnalignedBranchDisplacements) {
    expectRejected("JMP 7\n", "not a multiple of 8");
    expectAccepted("JMP 16\n");
}

TEST(Sema, DetectsDuplicateLabels) {
    const testkit::Assembled result = assemble("a:\n    NOP\na:\n    HALT\n");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.mentions("duplicate label 'a'"));
    // The diagnostic points at both definitions, which is what makes it useful.
    EXPECT_TRUE(result.mentions("previously defined here"));
}

TEST(Sema, ChecksDataDirectiveRanges) {
    expectAccepted(".data\n.byte 255\n.byte -128\n.word 65535\n.dword 1\n.qword 1\n");
    expectRejected(".data\n.byte 256\n", "does not fit in the 1 byte(s)");
    expectRejected(".data\n.word 70000\n", "does not fit in the 2 byte(s)");
}

TEST(Sema, ChecksDirectiveArguments) {
    expectRejected(".section\n", "takes exactly 1 operand");
    expectRejected(".section .nowhere\n", "unknown section '.nowhere'");
    expectRejected(".align 3\n", "power of two");
    expectRejected(".align -4\n", "positive integer");
    expectRejected(".space -1\n", "non-negative size");
    expectRejected(".asciz 5\n", "expects a string literal");
    expectRejected(".global\n", "at least one symbol name");
    expectRejected(".byte\n", "at least one value");
}

TEST(Sema, RejectsASymbolInATooNarrowDataSlot) {
    expectRejected(".data\n.byte message\n", "needs .dword or .qword");
    expectAccepted(".data\nmessage:\n.qword message\n");
}

TEST(Sema, ReportsEveryProblemInOnePass) {
    // One run should describe the whole file, not just the first mistake.
    const testkit::Assembled result = assemble("BOGUS\nMOV R1, 5\nHALT R2\n");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.mentions("BOGUS"));
    EXPECT_TRUE(result.mentions("expects a register"));
    EXPECT_TRUE(result.mentions("takes 0 operands"));
}

}  // namespace
