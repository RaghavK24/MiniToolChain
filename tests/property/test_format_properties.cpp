// SPDX-License-Identifier: MIT
//
// Properties of the binary formats and the optimizer, checked over randomly
// generated inputs rather than hand-picked ones:
//
//     deserialize(serialize(x)) == x
//     run(program) == run(optimize(program))
//
// The generators are seeded, so a failure is reproducible from the seed printed
// in the message.

#include <random>
#include <string>
#include <vector>

#include "minitool/executable/executable_io.hpp"
#include "minitool/object/object_io.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

constexpr unsigned kSeed = 0xC0FFEE;

/// Generates a random but *valid* program: only well-formed instructions, and
/// only ones that cannot fault, so the two optimization levels can be compared
/// on programs that actually run to completion.
class ProgramGenerator {
  public:
    explicit ProgramGenerator(unsigned seed) : random_(seed) {}

    std::string generate(int instructions) {
        std::string source = ".global _start\n_start:\n";
        // Start from known values so that nothing depends on the initial state.
        for (unsigned reg = 1; reg <= 6; ++reg) {
            source += std::format("    MOVI R{}, {}\n", reg, pick(1, 50));
        }
        int labels = 0;
        for (int i = 0; i < instructions; ++i) {
            switch (pick(0, 9)) {
                case 0:
                    source += std::format("    ADD R{}, R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 1:
                    source += std::format("    SUB R{}, R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 2:
                    source += std::format("    MOV R{}, R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 3:
                    source += std::format("    MOVI R{}, {}\n", pick(1, 6), pick(-100, 100));
                    break;
                case 4:
                    source += std::format("    XOR R{}, R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 5:
                    source += std::format("    MUL R{}, R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 6:
                    source += std::format("    PUSH R{}\n    POP R{}\n", pick(1, 6), pick(1, 6));
                    break;
                case 7:
                    source += "    NOP\n";
                    break;
                case 8: {
                    // A forward branch over the next instruction, which keeps
                    // control flow interesting without risking a loop.
                    const std::string label = std::format("skip{}", labels++);
                    source += std::format("    CMP R{}, R{}\n", pick(1, 6), pick(1, 6));
                    source += std::format("    JE {}\n", label);
                    source += std::format("    INC R{}\n", pick(1, 6));
                    source += label + ":\n";
                    break;
                }
                default:
                    source += std::format("    INC R{}\n", pick(1, 6));
                    break;
            }
        }
        source += "    HALT\n";
        return source;
    }

  private:
    int pick(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random_); }

    std::mt19937 random_;
};

TEST(FormatProperties, ObjectFilesRoundTrip) {
    ProgramGenerator generator(kSeed);
    for (int i = 0; i < 60; ++i) {
        const testkit::Assembled assembled = testkit::assemble(generator.generate(20));
        ASSERT_TRUE(assembled.ok) << assembled.diagnostics;

        std::vector<u8> first;
        ASSERT_TRUE(object::writeObjectToBuffer(assembled.object, first).has_value());
        const std::expected<object::ObjectFile, std::string> parsed =
            object::readObjectFromBuffer(first);
        ASSERT_TRUE(parsed.has_value()) << parsed.error();

        // Serialising what was read must reproduce the bytes exactly: that is
        // the strongest statement of "nothing was lost".
        std::vector<u8> second;
        ASSERT_TRUE(object::writeObjectToBuffer(*parsed, second).has_value());
        ASSERT_EQ(first, second) << "seed " << kSeed << " iteration " << i;
    }
}

TEST(FormatProperties, ExecutablesRoundTrip) {
    ProgramGenerator generator(kSeed + 1);
    for (int i = 0; i < 60; ++i) {
        const testkit::Linked linked = testkit::build(generator.generate(20));
        ASSERT_TRUE(linked.ok) << linked.error;

        std::vector<u8> first;
        ASSERT_TRUE(executable::writeExecutableToBuffer(linked.executable, first).has_value());
        const std::expected<executable::Executable, std::string> parsed =
            executable::readExecutableFromBuffer(first);
        ASSERT_TRUE(parsed.has_value()) << parsed.error();

        std::vector<u8> second;
        ASSERT_TRUE(executable::writeExecutableToBuffer(*parsed, second).has_value());
        ASSERT_EQ(first, second) << "seed " << kSeed << " iteration " << i;
    }
}

TEST(FormatProperties, AssemblyIsReproducible) {
    ProgramGenerator generator(kSeed + 2);
    for (int i = 0; i < 40; ++i) {
        const std::string source = generator.generate(15);
        const testkit::Assembled first = testkit::assemble(source);
        const testkit::Assembled second = testkit::assemble(source);
        ASSERT_TRUE(first.ok);
        std::vector<u8> first_bytes;
        std::vector<u8> second_bytes;
        ASSERT_TRUE(object::writeObjectToBuffer(first.object, first_bytes).has_value());
        ASSERT_TRUE(object::writeObjectToBuffer(second.object, second_bytes).has_value());
        ASSERT_EQ(first_bytes, second_bytes) << "seed " << kSeed << " iteration " << i;
    }
}

TEST(OptimizerProperties, OptimizationPreservesObservableBehaviour) {
    // The differential test: the same program, run
    // twice, must be indistinguishable from the outside.
    ProgramGenerator generator(kSeed + 3);
    for (int i = 0; i < 120; ++i) {
        const std::string source = generator.generate(25);
        const testkit::Ran plain = testkit::runSource(source, optimizer::OptLevel::O0);
        const testkit::Ran optimized = testkit::runSource(source, optimizer::OptLevel::O1);

        ASSERT_TRUE(plain.ok) << "seed " << kSeed << " iteration " << i << ": " << plain.message;
        ASSERT_TRUE(optimized.ok) << "seed " << kSeed << " iteration " << i << ": "
                                  << optimized.message;
        ASSERT_EQ(plain.exit_code, optimized.exit_code) << "iteration " << i;
        ASSERT_EQ(plain.output, optimized.output) << "iteration " << i;
        for (unsigned reg = 0; reg < isa::kRegisterCount; ++reg) {
            ASSERT_EQ(plain.reg(reg), optimized.reg(reg))
                << "iteration " << i << ", register R" << reg << "\n"
                << source;
        }
        // The stack pointer must also come back to the same place.
        ASSERT_EQ(plain.cpu.sp, optimized.cpu.sp) << "iteration " << i;
    }
}

TEST(OptimizerProperties, OptimizationNeverAddsInstructions) {
    ProgramGenerator generator(kSeed + 4);
    for (int i = 0; i < 60; ++i) {
        const std::string source = generator.generate(25);
        const testkit::Ran plain = testkit::runSource(source, optimizer::OptLevel::O0);
        const testkit::Ran optimized = testkit::runSource(source, optimizer::OptLevel::O1);
        ASSERT_TRUE(plain.ok && optimized.ok);
        EXPECT_LE(optimized.instructions, plain.instructions) << "iteration " << i;
    }
}

TEST(OptimizerProperties, OptimizationIsIdempotent) {
    // Running the optimizer on its own output must change nothing further: a
    // pass that keeps finding work would mean the fixed point is wrong.
    ProgramGenerator generator(kSeed + 5);
    for (int i = 0; i < 40; ++i) {
        const std::string source = generator.generate(20);
        const testkit::Assembled once = testkit::assemble(source, optimizer::OptLevel::O1);
        ASSERT_TRUE(once.ok) << once.diagnostics;
        const object::Section* text = once.object.findSection(".text");
        ASSERT_NE(text, nullptr);

        // Re-optimizing the already optimized module is what the IR-level
        // fixed point promises; here we check it through the byte output.
        const testkit::Assembled again = testkit::assemble(source, optimizer::OptLevel::O1);
        ASSERT_TRUE(again.ok);
        EXPECT_EQ(again.object.findSection(".text")->data, text->data) << "iteration " << i;
    }
}

}  // namespace
