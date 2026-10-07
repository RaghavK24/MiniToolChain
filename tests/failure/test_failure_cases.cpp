// SPDX-License-Identifier: MIT
//
// The failure catalogue. Every one of these must produce a
// clear, structured error, and none of them may crash the process, hang, or
// silently produce a wrong program.

#include <string>
#include <string_view>
#include <vector>

#include "minitool/executable/executable_io.hpp"
#include "minitool/object/object_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

/// Asserts that assembling `source` fails and says something recognisable.
void expectAssemblyError(std::string_view source, std::string_view expected) {
    const testkit::Assembled result = testkit::assemble(source);
    EXPECT_FALSE(result.ok) << source;
    EXPECT_TRUE(result.mentions(expected)) << result.diagnostics << result.error;
}

void expectLinkError(std::string_view source, std::string_view expected) {
    const testkit::Linked linked = testkit::build(source);
    EXPECT_FALSE(linked.ok) << source;
    EXPECT_TRUE(linked.mentions(expected)) << linked.error;
}

void expectRuntimeError(std::string_view source, vm::VMError expected) {
    const testkit::Ran ran = testkit::runSource(source);
    EXPECT_FALSE(ran.ok) << source;
    EXPECT_EQ(ran.error, expected) << ran.message;
}

// --- front end --------------------------------------------------------------

TEST(Failure, UnknownOpcode) {
    expectAssemblyError("NOTANOPCODE R1\n", "unknown instruction");
}

TEST(Failure, InvalidRegister) {
    // R19 does not exist, so it lexes as an identifier and fails as an operand.
    expectAssemblyError("MOVI R19, 1\n", "expects a register");
}

TEST(Failure, MalformedInstruction) {
    expectAssemblyError("MOVI R1 5\n", "after end of statement");
    expectAssemblyError("LOAD R1, [R2\n", "']'");
}

TEST(Failure, MissingOperand) {
    expectAssemblyError("ADD R1\n", "takes 2 operands");
}

TEST(Failure, ExtraOperand) {
    expectAssemblyError("HALT R1\n", "takes 0 operands");
}

TEST(Failure, DuplicateLabel) {
    expectAssemblyError("a:\nNOP\na:\nHALT\n", "duplicate label");
}

TEST(Failure, IntegerOverflowInAnImmediate) {
    expectAssemblyError("MOVI R1, 99999999999999999999\n", "64 bits");
    expectAssemblyError("MOVI R1, 281474976710656\n", "48 signed bits");
}

TEST(Failure, InvalidDirective) {
    expectAssemblyError(".notadirective\n", "unknown directive");
    expectAssemblyError(".align 5\n", "power of two");
}

// --- symbols and linking ----------------------------------------------------

TEST(Failure, UndefinedSymbol) {
    expectLinkError(".global _start\n_start:\n    CALL missing\n    HALT\n",
                    "undefined symbol 'missing'");
}

TEST(Failure, DuplicateGlobalSymbol) {
    constexpr std::string_view kFirst = ".global dup\n.global _start\n_start:\ndup:\n    HALT\n";
    constexpr std::string_view kSecond = ".global dup\ndup:\n    RET\n";
    const std::array<std::string_view, 2> sources{kFirst, kSecond};
    const testkit::Linked linked = testkit::buildAll(sources);
    EXPECT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("more than one object"));
}

TEST(Failure, MissingEntryPoint) {
    const testkit::Assembled assembled = testkit::assemble("other:\n    HALT\n");
    ASSERT_TRUE(assembled.ok);
    const std::vector<object::ObjectFile> objects{assembled.object};
    const testkit::Linked linked = testkit::link(objects);
    EXPECT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("entry point"));
}

TEST(Failure, RelocationOverflow) {
    // Force a branch further than the 48-bit displacement can express by
    // moving the target section far away from the code.
    const testkit::Assembled assembled = testkit::assemble(
        ".global _start\n.data\nfar:\n    .qword 0\n.text\n_start:\n    LEA R1, far\n    HALT\n");
    ASSERT_TRUE(assembled.ok) << assembled.diagnostics;
    const std::vector<object::ObjectFile> objects{assembled.object};
    linker::LinkOptions options;
    options.data_base = u64{1} << 60U;  // beyond a 48-bit absolute field
    const testkit::Linked linked = testkit::link(objects, options);
    EXPECT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("does not fit")) << linked.error;
}

TEST(Failure, SectionThatOutgrowsItsRegion) {
    const testkit::Assembled assembled =
        testkit::assemble(".global _start\n.text\n_start:\n    HALT\n.bss\n.space 2000000\n");
    ASSERT_TRUE(assembled.ok) << assembled.diagnostics;
    const std::vector<object::ObjectFile> objects{assembled.object};
    const testkit::Linked linked = testkit::link(objects);
    EXPECT_FALSE(linked.ok);
    EXPECT_TRUE(linked.mentions("region")) << linked.error;
}

// --- binary formats ---------------------------------------------------------

TEST(Failure, TruncatedObjectFile) {
    const testkit::Assembled assembled = testkit::assemble("NOP\n");
    std::vector<u8> bytes;
    ASSERT_TRUE(object::writeObjectToBuffer(assembled.object, bytes).has_value());
    bytes.resize(bytes.size() / 2);
    const std::expected<object::ObjectFile, std::string> parsed =
        object::readObjectFromBuffer(bytes);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Failure, CorruptedObjectFile) {
    const testkit::Assembled assembled = testkit::assemble("NOP\nHALT\n");
    std::vector<u8> bytes;
    ASSERT_TRUE(object::writeObjectToBuffer(assembled.object, bytes).has_value());
    bytes[bytes.size() - 3] ^= 0xFF;
    const std::expected<object::ObjectFile, std::string> parsed =
        object::readObjectFromBuffer(bytes);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_TRUE(parsed.error().find("checksum") != std::string::npos);
}

TEST(Failure, ObjectFileThatIsNotAnObjectFile) {
    const std::string text = "this is a text file, not an object";
    const std::vector<u8> bytes(text.begin(), text.end());
    EXPECT_FALSE(object::readObjectFromBuffer(bytes).has_value());
    EXPECT_FALSE(executable::readExecutableFromBuffer(bytes).has_value());
}

TEST(Failure, InvalidExecutableIsRejectedByTheLoader) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    std::vector<u8> bytes;
    ASSERT_TRUE(executable::writeExecutableToBuffer(linked.executable, bytes).has_value());
    bytes[20] ^= 0xFF;  // somewhere in the segment table
    EXPECT_FALSE(executable::readExecutableFromBuffer(bytes).has_value());
}

TEST(Failure, InvalidEntryPointIsRefusedBeforeExecution) {
    executable::Executable executable;
    executable.entry_point = 0x999;
    executable::Segment text;
    text.name = ".text";
    text.type = executable::SegmentType::Text;
    text.flags = executable::SegmentFlags::Read | executable::SegmentFlags::Exec;
    text.virtual_address = 0x10000;
    text.virtual_size = 8;
    text.data = {0, 0, 0, 0, 0, 0, 0, 0x01};
    executable.segments.push_back(std::move(text));

    vm::VirtualMachine machine;
    const std::expected<void, std::string> loaded = machine.load(executable);
    EXPECT_FALSE(loaded.has_value());
}

// --- runtime ----------------------------------------------------------------

TEST(Failure, ExecutingNonExecutableMemory) {
    expectRuntimeError(R"(
.global _start
.data
slot: .qword 0
.text
_start:
    LEA  R1, slot
    PUSH R1
    RET
)",
                       vm::VMError::PermissionViolation);
}

TEST(Failure, WritingReadOnlyMemory) {
    expectRuntimeError(R"(
.global _start
.rodata
value: .qword 1
.text
_start:
    LEA  R1, value
    MOVI R2, 2
    STORE [R1 + 0], R2
    HALT
)",
                       vm::VMError::PermissionViolation);
}

TEST(Failure, OutOfRangeMemoryAccess) {
    expectRuntimeError(
        ".global _start\n_start:\n    MOVI R1, 123456789\n    LOAD R2, [R1 + 0]\n    HALT\n",
        vm::VMError::InvalidMemoryAccess);
}

TEST(Failure, DivisionByZero) {
    expectRuntimeError(
        ".global _start\n_start:\n    MOVI R1, 1\n    MOVI R2, 0\n    DIV R1, R2\n    HALT\n",
        vm::VMError::DivisionByZero);
    expectRuntimeError(
        ".global _start\n_start:\n    MOVI R1, 1\n    MOVI R2, 0\n    MOD R1, R2\n    HALT\n",
        vm::VMError::DivisionByZero);
}

TEST(Failure, StackUnderflow) {
    expectRuntimeError(".global _start\n_start:\n    POP R1\n    HALT\n",
                       vm::VMError::StackUnderflow);
    expectRuntimeError(".global _start\n_start:\n    RET\n", vm::VMError::StackUnderflow);
}

TEST(Failure, InvalidSyscall) {
    expectRuntimeError(".global _start\n_start:\n    SYSCALL 999\n    HALT\n",
                       vm::VMError::SyscallError);
}

TEST(Failure, SyscallWithABadBuffer) {
    expectRuntimeError(R"(
.global _start
_start:
    MOVI R1, 1
    MOVI R2, 123456789
    MOVI R3, 16
    SYSCALL 1
    HALT
)",
                       vm::VMError::SyscallError);
}

TEST(Failure, IllegalInstructionInMemory) {
    // Hand-build an image whose only instruction is an undefined opcode.
    executable::Executable executable;
    executable.entry_point = 0x10000;
    executable::Segment text;
    text.name = ".text";
    text.type = executable::SegmentType::Text;
    text.flags = executable::SegmentFlags::Read | executable::SegmentFlags::Exec;
    text.virtual_address = 0x10000;
    text.virtual_size = 8;
    text.data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    executable.segments.push_back(std::move(text));

    const testkit::Ran ran = testkit::run(executable);
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::IllegalInstruction);
}

TEST(Failure, RunningOffTheEndOfTheCode) {
    // No HALT: execution walks past the last instruction into unmapped memory.
    const testkit::Ran ran = testkit::runSource(".global _start\n_start:\n    NOP\n");
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::InvalidMemoryAccess);
}

TEST(Failure, AProgramThatNeverTerminatesIsStopped) {
    const testkit::Linked linked = testkit::build(".global _start\n_start:\nspin:\n    JMP spin\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const testkit::Ran ran = testkit::run(linked.executable, {}, 1000);
    EXPECT_FALSE(ran.ok);
    EXPECT_EQ(ran.error, vm::VMError::BudgetExhausted);
}

}  // namespace
