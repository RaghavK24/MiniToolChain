// SPDX-License-Identifier: MIT
//
// End-to-end tests: source text in, program behaviour out, through every stage
// of the real toolchain. If one of these fails, something between the lexer and
// the VM is broken, and the unit tests say which.

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "minitool/disassembler/disassembler.hpp"
#include "minitool/executable/executable_io.hpp"
#include "minitool/object/object_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

/// Reads one of the checked-in example programs. Tests that need an example
/// skip themselves if the working directory is not the repository root, so the
/// suite still runs from anywhere.
std::optional<std::string> readExample(std::string_view name) {
    const std::filesystem::path path = std::filesystem::path("examples") / name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    // Not the istreambuf_iterator range-construction idiom: GCC 13's
    // -Wnull-dereference misfires on it at -O3, entirely inside <streambuf>
    // and <bits/basic_string.tcc> -- nothing this file could fix locally,
    // since none of the flagged code is ours.
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

TEST(Pipeline, TheAcceptanceProgramFromThePlan) {
    // This exact program must go all the way through.
    const testkit::Ran ran = testkit::runSource(R"(
.section .text

.global _start

_start:
    MOVI R1, 40
    MOVI R2, 2
    ADD R1, R2
    CALL print_number
    HALT

print_number:
    MOV R14, R1
    RET
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 42U);
    EXPECT_EQ(ran.reg(14), 42U);
    EXPECT_EQ(ran.instructions, 7U);
}

TEST(Pipeline, HelloWorldPrintsThroughASyscall) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.rodata
message: .asciz "Hello, MiniToolchain!\n"
.text
_start:
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 22
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.output, "Hello, MiniToolchain!\n");
    EXPECT_EQ(ran.exit_code, 0U);
}

TEST(Pipeline, FibonacciWithALoop) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
_start:
    MOVI R1, 0          ; previous
    MOVI R2, 1          ; current
    MOVI R3, 10         ; iterations remaining
    MOVI R4, 0
loop:
    CMP  R3, R4
    JLE  done
    MOV  R5, R2
    ADD  R2, R1
    MOV  R1, R5
    DEC  R3
    JMP  loop
done:
    MOV  R14, R1
    HALT
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(14), 55U);  // fib(10)
}

TEST(Pipeline, ArraysThroughLoadAndStore) {
    const testkit::Ran ran = testkit::runSource(R"(
.global _start
.bss
array: .space 40
.text
_start:
    LEA  R1, array
    MOVI R2, 0          ; index
    MOVI R3, 5          ; count
    MOVI R7, 8
fill:
    CMP  R2, R3
    JGE  sum_setup
    MOV  R4, R2
    INC  R4
    MOV  R5, R2
    MUL  R5, R7
    ADD  R5, R1
    STORE [R5 + 0], R4
    INC  R2
    JMP  fill
sum_setup:
    MOVI R2, 0
    MOVI R6, 0
sum:
    CMP  R2, R3
    JGE  done
    MOV  R5, R2
    MUL  R5, R7
    ADD  R5, R1
    LOAD R4, [R5 + 0]
    ADD  R6, R4
    INC  R2
    JMP  sum
done:
    MOV  R14, R6
    HALT
)");
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(14), 15U);  // 1+2+3+4+5
}

TEST(Pipeline, ObjectAndExecutableSurviveARoundTripToDisk) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "minitool_pipeline_test";
    std::filesystem::create_directories(directory);
    const std::filesystem::path object_path = directory / "program.mobj";
    const std::filesystem::path executable_path = directory / "program.mexe";

    const testkit::Assembled assembled = testkit::assemble(
        ".global _start\n_start:\n    MOVI R1, 7\n    CALL twice\n    HALT\n"
        "twice:\n    ADD R1, R1\n    RET\n");
    ASSERT_TRUE(assembled.ok) << assembled.diagnostics;
    ASSERT_TRUE(object::writeObject(assembled.object, object_path).has_value());

    const std::expected<object::ObjectFile, std::string> reloaded = object::readObject(object_path);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error();

    const std::vector<object::ObjectFile> objects{*reloaded};
    const testkit::Linked linked = testkit::link(objects);
    ASSERT_TRUE(linked.ok) << linked.error;
    ASSERT_TRUE(executable::writeExecutable(linked.executable, executable_path).has_value());

    const std::expected<executable::Executable, std::string> loaded =
        executable::readExecutable(executable_path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const testkit::Ran ran = testkit::run(*loaded);
    ASSERT_TRUE(ran.ok) << ran.message;
    EXPECT_EQ(ran.reg(1), 14U);

    std::filesystem::remove_all(directory);
}

TEST(Pipeline, TheWholeToolchainIsDeterministic) {
    // Same source, same bytes: twice, through both formats.
    const auto buildBytes = [](std::string_view source) {
        const testkit::Linked linked = testkit::build(source);
        EXPECT_TRUE(linked.ok) << linked.error;
        std::vector<u8> bytes;
        EXPECT_TRUE(executable::writeExecutableToBuffer(linked.executable, bytes).has_value());
        return bytes;
    };
    constexpr std::string_view kSource =
        ".global _start\n.rodata\nm: .asciz \"x\"\n.text\n_start:\n    LEA R1, m\n    HALT\n";
    EXPECT_EQ(buildBytes(kSource), buildBytes(kSource));
}

TEST(Pipeline, DisassemblingALinkedProgramShowsTheOriginalCode) {
    const testkit::Linked linked = testkit::build(
        ".global _start\n_start:\n    MOVI R1, 99\n    CALL helper\n    HALT\n"
        "helper:\n    INC R1\n    RET\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    const std::string listing = disassembler::Disassembler{}.disassemble(linked.executable);
    EXPECT_TRUE(listing.find("MOVI R1, 99") != std::string::npos);
    EXPECT_TRUE(listing.find("<helper>:") != std::string::npos);
    EXPECT_TRUE(listing.find("INC R1") != std::string::npos);
}

TEST(Pipeline, OptimizationDoesNotChangeTheAnswer) {
    constexpr std::string_view kSource = R"(
.global _start
_start:
    MOVI R1, 6
    MOVI R2, 7
    MUL  R1, R2
    MOV  R3, R3
    NOP
    PUSH R1
    POP  R4
    HALT
)";
    const testkit::Ran plain = testkit::runSource(kSource, optimizer::OptLevel::O0);
    const testkit::Ran optimized = testkit::runSource(kSource, optimizer::OptLevel::O1);
    ASSERT_TRUE(plain.ok) << plain.message;
    ASSERT_TRUE(optimized.ok) << optimized.message;
    EXPECT_EQ(plain.reg(1), 42U);
    EXPECT_EQ(optimized.reg(1), 42U);
    EXPECT_EQ(plain.reg(4), optimized.reg(4));
    // ...but it should have taken fewer instructions to get there.
    EXPECT_LT(optimized.instructions, plain.instructions);
}

TEST(Pipeline, EveryCheckedInExampleAssemblesLinksAndRuns) {
    constexpr std::array<std::string_view, 8> kExamples{
        "hello.asm", "arithmetic.asm", "factorial.asm", "fibonacci.asm",
        "loops.asm", "arrays.asm",     "functions.asm", "recursion.asm"};
    std::size_t checked = 0;
    for (const std::string_view name : kExamples) {
        const std::optional<std::string> source = readExample(name);
        if (!source.has_value()) {
            continue;  // not running from the repository root
        }
        ++checked;
        const testkit::Ran plain = testkit::runSource(*source, optimizer::OptLevel::O0);
        EXPECT_TRUE(plain.ok) << name << ": " << plain.message;
        // Every example must also survive optimization unchanged.
        const testkit::Ran optimized = testkit::runSource(*source, optimizer::OptLevel::O1);
        EXPECT_TRUE(optimized.ok) << name << ": " << optimized.message;
        EXPECT_EQ(plain.output, optimized.output) << name;
        EXPECT_EQ(plain.reg(14), optimized.reg(14)) << name;
    }
    EXPECT_EQ(checked, kExamples.size())
        << "run the tests from the repository root to check the examples";
}

TEST(Pipeline, ErrorExamplesAllFailWithDiagnostics) {
    const std::filesystem::path directory("examples/errors");
    if (!std::filesystem::exists(directory)) {
        return;
    }
    std::size_t checked = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".asm") {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::ostringstream contents;
        contents << file.rdbuf();
        const std::string source = contents.str();
        const testkit::Assembled assembled = testkit::assemble(source);
        const bool rejected = !assembled.ok || !testkit::build(source).ok;
        EXPECT_TRUE(rejected) << entry.path().string() << " was expected to fail";
        ++checked;
    }
    EXPECT_GT(checked, 0U);
}

}  // namespace
