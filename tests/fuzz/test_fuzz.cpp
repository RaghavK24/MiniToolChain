// SPDX-License-Identifier: MIT
//
// Fuzzing, in a form that runs on every build rather than only under a fuzzing
// engine: deterministic pseudo-random inputs driven through each parser and
// loader. The invariant under test:
//
//     malformed input may produce an error, but must never crash, hang, or
//     invoke undefined behaviour.
//
// Every case is reproducible from its seed, so a failure can be replayed. Run
// the suite under ASan/UBSan to make "no undefined behaviour" more than a hope.

#include <random>
#include <string>
#include <vector>

#include "minitool/common/byte_order.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/executable/executable_io.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/lexer/lexer.hpp"
#include "minitool/object/object_io.hpp"
#include "minitool/parser/parser.hpp"
#include "support/test_framework.hpp"
#include "support/toolchain.hpp"

namespace {

using namespace minitool;

constexpr unsigned kSeed = 0x5EED'1234;
constexpr int kIterations = 2000;

/// Characters chosen to look enough like assembly to reach deep into the
/// parser rather than dying on the first byte.
constexpr std::string_view kAlphabet =
    "MOVIADHLTRJPXY0123456789 \n\t,:[]+-#.\"';_abcdefRSTUVWXYZ\\%$@!*/&|^~()<>=";

std::string randomText(std::mt19937& random, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length(0, max_length);
    std::uniform_int_distribution<std::size_t> character(0, kAlphabet.size() - 1);
    std::string text;
    const std::size_t count = length(random);
    text.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        text.push_back(kAlphabet[character(random)]);
    }
    return text;
}

std::vector<u8> randomBytes(std::mt19937& random, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length(0, max_length);
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::vector<u8> bytes(length(random));
    for (u8& value : bytes) {
        value = static_cast<u8>(byte(random));
    }
    return bytes;
}

TEST(Fuzz, LexerTerminatesOnAnyInput) {
    std::mt19937 random(kSeed);
    for (int i = 0; i < kIterations; ++i) {
        const std::string text = randomText(random, 200);
        lexer::Lexer lexer(text, 0);
        // The guarantee is progress: at most one token per character, plus Eof.
        std::size_t tokens = 0;
        while (lexer.next().type != lexer::TokenType::Eof) {
            ++tokens;
            ASSERT_LE(tokens, text.size() + 1)
                << "lexer failed to make progress, seed " << kSeed << " iteration " << i;
        }
    }
}

TEST(Fuzz, ParserSurvivesAnyInput) {
    std::mt19937 random(kSeed + 1);
    std::size_t accepted = 0;
    for (int i = 0; i < kIterations; ++i) {
        SourceManager sources;
        const FileId file = sources.addFile("fuzz.asm", randomText(random, 200));
        diag::DiagnosticEngine diagnostics(sources);
        lexer::Lexer lexer(sources.text(file), file);
        parser::Parser parser(lexer, diagnostics);
        if (parser.parse().has_value()) {
            ++accepted;
        }
    }
    // Some random input really is valid assembly (an empty file, a bare label);
    // the point is that nothing crashed either way.
    EXPECT_GT(accepted, 0U);
}

TEST(Fuzz, WholeFrontEndSurvivesAnyInput) {
    std::mt19937 random(kSeed + 2);
    for (int i = 0; i < 500; ++i) {
        static_cast<void>(testkit::assemble(randomText(random, 300)));
    }
}

TEST(Fuzz, DecoderAcceptsOrRejectsEveryWordWithoutCrashing) {
    std::mt19937_64 random(kSeed + 3);
    std::size_t decoded = 0;
    for (int i = 0; i < 20000; ++i) {
        const u64 word = random();
        const std::expected<isa::Instruction, isa::DecodeError> instruction = isa::decode(word);
        if (instruction.has_value()) {
            ++decoded;
            // Anything accepted must re-encode to exactly the same word: the
            // codec is a bijection, and a fuzzed word is the best way to notice
            // it is not.
            const std::expected<u64, isa::EncodeError> again = isa::encode(*instruction);
            ASSERT_TRUE(again.has_value());
            ASSERT_EQ(*again, word) << "seed " << kSeed << " word 0x" << std::hex << word;
        }
    }
    // With 36 of 256 opcodes defined and reserved fields checked, a few percent
    // of random words are valid; a count of zero would mean the test is not
    // exercising the decoder at all.
    EXPECT_GT(decoded, 100U);
}

TEST(Fuzz, ObjectReaderSurvivesRandomBytes) {
    std::mt19937 random(kSeed + 4);
    for (int i = 0; i < kIterations; ++i) {
        static_cast<void>(object::readObjectFromBuffer(randomBytes(random, 512)));
    }
}

TEST(Fuzz, ExecutableReaderSurvivesRandomBytes) {
    std::mt19937 random(kSeed + 5);
    for (int i = 0; i < kIterations; ++i) {
        static_cast<void>(executable::readExecutableFromBuffer(randomBytes(random, 512)));
    }
}

TEST(Fuzz, ReadersSurviveMutationsOfValidFiles) {
    // Random bytes rarely get past the magic; mutating a real file is what
    // reaches the table-parsing code.
    const testkit::Assembled assembled = testkit::assemble(
        ".global _start\n.data\nv: .qword 1\n.text\n_start:\n    LEA R1, v\n    HALT\n");
    ASSERT_TRUE(assembled.ok) << assembled.diagnostics;
    std::vector<u8> object_bytes;
    ASSERT_TRUE(object::writeObjectToBuffer(assembled.object, object_bytes).has_value());

    const testkit::Linked linked = testkit::build(".global _start\n_start:\n    HALT\n");
    ASSERT_TRUE(linked.ok) << linked.error;
    std::vector<u8> exe_bytes;
    ASSERT_TRUE(executable::writeExecutableToBuffer(linked.executable, exe_bytes).has_value());

    std::mt19937 random(kSeed + 6);
    std::uniform_int_distribution<unsigned> byte(0, 255);
    for (int i = 0; i < kIterations; ++i) {
        std::vector<u8> mutated = (i % 2 == 0) ? object_bytes : exe_bytes;
        std::uniform_int_distribution<std::size_t> position(0, mutated.size() - 1);
        const int mutations = 1 + (i % 4);
        for (int m = 0; m < mutations; ++m) {
            mutated[position(random)] = static_cast<u8>(byte(random));
        }
        if (i % 2 == 0) {
            static_cast<void>(object::readObjectFromBuffer(mutated));
        } else {
            static_cast<void>(executable::readExecutableFromBuffer(mutated));
        }
    }
}

TEST(Fuzz, VmSurvivesRandomCodeWithoutHangingOrCrashing) {
    // Fill an executable segment with random words and run it under a budget.
    // Most will fault immediately; none may escape the sandbox or spin forever.
    std::mt19937_64 random(kSeed + 7);
    for (int i = 0; i < 300; ++i) {
        executable::Executable executable;
        executable.entry_point = 0x10000;
        executable::Segment text;
        text.name = ".text";
        text.type = executable::SegmentType::Text;
        text.flags = executable::SegmentFlags::Read | executable::SegmentFlags::Exec;
        text.virtual_address = 0x10000;
        text.virtual_size = 64 * isa::kInstructionSize;
        text.data.resize(static_cast<std::size_t>(text.virtual_size));
        for (std::size_t offset = 0; offset < text.data.size(); offset += isa::kInstructionSize) {
            byteorder::store<u64>(std::span<u8>{text.data}.subspan(offset, isa::kInstructionSize),
                                  random());
        }
        executable.segments.push_back(std::move(text));

        const testkit::Ran ran = testkit::run(executable, {}, 5000);
        // Either it halted or it faulted; either way it came back.
        EXPECT_TRUE(ran.ok || ran.error != vm::VMError::None);
    }
}

TEST(Fuzz, LinkerSurvivesObjectsBuiltFromFuzzedSource) {
    std::mt19937 random(kSeed + 8);
    for (int i = 0; i < 300; ++i) {
        // Random fragments spliced into a valid skeleton reach the linker far
        // more often than pure noise does.
        const std::string source =
            ".global _start\n_start:\n    " + randomText(random, 40) + "\n    HALT\n";
        const testkit::Assembled assembled = testkit::assemble(source);
        if (!assembled.ok) {
            continue;
        }
        const std::vector<object::ObjectFile> objects{assembled.object};
        static_cast<void>(testkit::link(objects));
    }
}

}  // namespace
