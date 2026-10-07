// SPDX-License-Identifier: MIT
#include <string>
#include <string_view>

#include "minitool/isa/encoding.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

/// Runs a fragment wrapped in the usual entry-point boilerplate.
testkit::Ran runBody(std::string_view body) {
    return testkit::runSource(std::string{".global _start\n_start:\n"} + std::string{body} +
                              "\n    HALT\n");
}

TEST(Vm, MovesAndImmediates) {
    const testkit::Ran ran = runBody("    MOVI R1, 42\n    MOV R2, R1\n    MOVI R3, -7");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 42U);
    EXPECT_EQ(ran.reg(2), 42U);
    EXPECT_EQ(static_cast<i64>(ran.reg(3)), -7);
}

TEST(Vm, Arithmetic) {
    const testkit::Ran ran = runBody(
        "    MOVI R1, 40\n    MOVI R2, 2\n    ADD R1, R2\n"
        "    MOVI R3, 10\n    MOVI R4, 3\n    SUB R3, R4\n"
        "    MOVI R5, 6\n    MOVI R6, 7\n    MUL R5, R6\n"
        "    MOVI R7, 43\n    MOVI R8, 5\n    MOD R7, R8\n"
        "    MOVI R9, 5\n    INC R9\n    DEC R9\n    DEC R9\n"
        "    MOVI R10, 3\n    NEG R10");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 42U);
    EXPECT_EQ(ran.reg(3), 7U);
    EXPECT_EQ(ran.reg(5), 42U);
    EXPECT_EQ(ran.reg(7), 3U);
    EXPECT_EQ(ran.reg(9), 4U);
    EXPECT_EQ(static_cast<i64>(ran.reg(10)), -3);
}

TEST(Vm, LogicalAndShifts) {
    const testkit::Ran ran = runBody(
        "    MOVI R1, 12\n    MOVI R2, 10\n    AND R1, R2\n"
        "    MOVI R3, 12\n    OR R3, R2\n"
        "    MOVI R4, 12\n    XOR R4, R2\n"
        "    MOVI R5, 1\n    MOVI R6, 4\n    SHL R5, R6\n"
        "    MOVI R7, 256\n    SHR R7, R6\n"
        "    MOVI R8, 0\n    NOT R8");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 8U);
    EXPECT_EQ(ran.reg(3), 14U);
    EXPECT_EQ(ran.reg(4), 6U);
    EXPECT_EQ(ran.reg(5), 16U);
    EXPECT_EQ(ran.reg(7), 16U);
    EXPECT_EQ(ran.reg(8), ~u64{0});
}

TEST(Vm, ConditionalBranchesFollowComparisons) {
    // Counts down from 5, summing as it goes: 5+4+3+2+1 = 15.
    const testkit::Ran ran = runBody(
        "    MOVI R1, 5\n"
        "    MOVI R2, 0\n"
        "    MOVI R3, 0\n"
        "loop:\n"
        "    CMP R1, R3\n"
        "    JLE done\n"
        "    ADD R2, R1\n"
        "    DEC R1\n"
        "    JMP loop\n"
        "done:");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(2), 15U);
    EXPECT_EQ(ran.reg(1), 0U);
}

TEST(Vm, SignedComparisonsUseSignAndOverflow) {
    const testkit::Ran ran = runBody(
        "    MOVI R1, -5\n"
        "    MOVI R2, 3\n"
        "    CMP R1, R2\n"
        "    MOVI R3, 0\n"
        "    JL negative_is_less\n"
        "    JMP end\n"
        "negative_is_less:\n"
        "    MOVI R3, 1\n"
        "end:");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(3), 1U);
}

TEST(Vm, CallAndReturnUseTheStack) {
    const testkit::Ran ran = testkit::runSource(
        ".global _start\n"
        "_start:\n"
        "    MOVI R1, 20\n"
        "    CALL double_it\n"
        "    CALL double_it\n"
        "    HALT\n"
        "double_it:\n"
        "    MOVI R2, 2\n"
        "    MUL R1, R2\n"
        "    RET\n");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 80U);
    // The stack is balanced again once both calls have returned.
    EXPECT_EQ(ran.cpu.sp, vm::VirtualMachine::kStackTop);
}

TEST(Vm, PushAndPopRoundTrip) {
    const testkit::Ran ran = runBody(
        "    MOVI R1, 11\n    MOVI R2, 22\n"
        "    PUSH R1\n    PUSH R2\n"
        "    POP R3\n    POP R4");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(3), 22U);
    EXPECT_EQ(ran.reg(4), 11U);
    EXPECT_EQ(ran.cpu.sp, vm::VirtualMachine::kStackTop);
}

TEST(Vm, LoadAndStoreReachMemory) {
    const testkit::Ran ran = testkit::runSource(
        ".global _start\n"
        ".data\n"
        "slot:   .qword 0\n"
        ".text\n"
        "_start:\n"
        "    LEA R1, slot\n"
        "    MOVI R2, 1234\n"
        "    STORE [R1 + 0], R2\n"
        "    LOAD R3, [R1 + 0]\n"
        "    HALT\n");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(3), 1234U);
}

TEST(Vm, RecursionWorks) {
    // factorial(5) = 120, computed recursively so the stack really is exercised.
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
_start:
    MOVI R1, 5
    CALL factorial
    HALT

factorial:
    MOVI R2, 1
    CMP R1, R2
    JG recurse
    MOVI R14, 1
    RET
recurse:
    PUSH R1
    DEC R1
    CALL factorial
    POP R1
    MOV R3, R14
    MUL R3, R1
    MOV R14, R3
    RET
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(14), 120U);
}

TEST(Vm, WriteSyscallProducesOutput) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.rodata
message: .asciz "hello\n"
.text
_start:
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 6
    SYSCALL 1
    HALT
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.output, "hello\n");
    EXPECT_EQ(ran.reg(14), 6U);
}

TEST(Vm, ExitSyscallStopsWithACode) {
    const testkit::Ran ran = testkit::runSource(
        ".global _start\n_start:\n    MOVI R1, 7\n    SYSCALL 0\n    MOVI R2, 99\n    HALT\n");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.exit_code, 7U);
    // Execution really stopped: the instruction after SYSCALL never ran.
    EXPECT_EQ(ran.reg(2), 0U);
}

TEST(Vm, AllocateSyscallReturnsUsableMemory) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
_start:
    MOVI R1, 16
    SYSCALL 3
    MOV  R5, R14
    MOVI R6, 99
    STORE [R5 + 0], R6
    LOAD R7, [R5 + 0]
    HALT
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(5), vm::VirtualMachine::kHeapBase);
    EXPECT_EQ(ran.reg(7), 99U);
}

TEST(Vm, ReadSyscallConsumesInput) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.bss
buffer: .space 16
.text
_start:
    MOVI R1, 0
    LEA  R2, buffer
    MOVI R3, 4
    SYSCALL 2
    LOAD R5, [R2 + 0]
    HALT
)",
                                                optimizer::OptLevel::O0, "abcd");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(14), 4U);
    EXPECT_EQ(ran.reg(5) & 0xFFFF'FFFFU, 0x6463'6261U);  // "abcd", little-endian
}

TEST(Vm, TrapsOnDivisionByZero) {
    const testkit::Ran ran = runBody("    MOVI R1, 1\n    MOVI R2, 0\n    DIV R1, R2");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::DivisionByZero);
}

TEST(Vm, TrapsOnWritingToReadOnlyMemory) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.rodata
constant: .qword 5
.text
_start:
    LEA  R1, constant
    MOVI R2, 9
    STORE [R1 + 0], R2
    HALT
)");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::PermissionViolation);
}

TEST(Vm, TrapsOnExecutingData) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.data
target: .qword 0
.text
_start:
    LEA  R1, target
    PUSH R1
    RET
)");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::PermissionViolation);
}

TEST(Vm, TrapsOnAnUnmappedAccess) {
    const testkit::Ran ran = runBody("    MOVI R1, 999999\n    LOAD R2, [R1 + 0]");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::InvalidMemoryAccess);
}

TEST(Vm, TrapsOnStackOverflow) {
    // The stack is 1 MiB, so this needs room to run past 131072 pushes before
    // the guard fires, which is more than the default budget allows.
    const testkit::Linked linked =
        testkit::build(".global _start\n_start:\nloop:\n    PUSH R1\n    JMP loop\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable, {}, 1'000'000);
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::StackOverflow);
}

TEST(Vm, TrapsOnStackUnderflow) {
    const testkit::Ran ran = runBody("    POP R1");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::StackUnderflow);
}

TEST(Vm, TrapsOnAnUnknownSyscall) {
    const testkit::Ran ran = runBody("    SYSCALL 77");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::SyscallError);
}

TEST(Vm, StopsAnEndlessProgramAtTheBudget) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\nloop:\n    JMP loop\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable, {}, 500);
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::BudgetExhausted);
    EXPECT_EQ(ran.instructions, 500U);
}

TEST(Vm, RefusesToRunWithoutAProgram) {
    vm::VirtualMachine machine;
    const vm::RunResult result = machine.run();
    EXPECT_EQ(result.error, vm::VMError::NotLoaded);
    EXPECT_FALSE(machine.step().has_value());
}

TEST(Vm, CountsInstructionsAndReportsThePc) {
    const testkit::Ran ran = runBody("    NOP\n    NOP");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.instructions, 3U);  // two NOPs and the HALT
}

TEST(Vm, TracesEveryInstruction) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\n    NOP\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    vm::VirtualMachine machine;
    std::vector<u64> seen;
    machine.setTraceSink([&seen](u64 pc, const isa::Instruction&) { seen.push_back(pc); });
    ASSERT_TRUE(machine.load(linked.executable).has_value());
    static_cast<void>(machine.run());
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0], linker::kTextBase);
    EXPECT_EQ(seen[1], linker::kTextBase + isa::kInstructionSize);
}

}  // namespace
