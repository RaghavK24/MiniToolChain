// SPDX-License-Identifier: MIT
#include <string>
#include <string_view>
#include <vector>

#include "minitool/lexer/lexer.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using namespace minitool::lexer;

Token lexOne(std::string_view text) {
    Lexer lexer(text, 1);
    return lexer.next();
}

std::vector<Token> lexAll(std::string_view text) {
    Lexer lexer(text, 1);
    std::vector<Token> tokens;
    while (true) {
        Token token = lexer.next();
        const bool done = token.type == TokenType::Eof;
        tokens.push_back(std::move(token));
        if (done) {
            return tokens;
        }
    }
}

TEST(Lexer, ClassifiesASimpleInstruction) {
    const std::vector<Token> tokens = lexAll("MOVI R1, 10");
    ASSERT_EQ(tokens.size(), 5U);
    EXPECT_EQ(tokens[0].type, TokenType::Identifier);
    EXPECT_EQ(tokens[0].text, "MOVI");
    EXPECT_EQ(tokens[1].type, TokenType::Register);
    EXPECT_EQ(tokens[1].text, "R1");
    EXPECT_EQ(tokens[2].type, TokenType::Comma);
    EXPECT_EQ(tokens[3].type, TokenType::Integer);
    EXPECT_EQ(tokens[3].intValue, 10);
    EXPECT_EQ(tokens[4].type, TokenType::Eof);
}

TEST(Lexer, ParsesEveryIntegerBase) {
    EXPECT_EQ(lexOne("255").intValue, 255);
    EXPECT_EQ(lexOne("0xFF").intValue, 255);
    EXPECT_EQ(lexOne("0xff").intValue, 255);
    EXPECT_EQ(lexOne("0b1010").intValue, 10);
    EXPECT_EQ(lexOne("0o17").intValue, 15);
    EXPECT_EQ(lexOne("-5").intValue, -5);
}

TEST(Lexer, ParsesNegativeNonDecimalLiterals) {
    // The prefix sits one character further along after a sign; getting this
    // wrong silently lexed "-0x10" as 0 followed by an identifier.
    EXPECT_EQ(lexOne("-0x10").type, TokenType::Integer);
    EXPECT_EQ(lexOne("-0x10").intValue, -16);
    EXPECT_EQ(lexOne("-0b101").intValue, -5);
}

TEST(Lexer, RejectsIntegersThatDoNotFit) {
    EXPECT_EQ(lexOne("99999999999999999999").type, TokenType::Error);
    EXPECT_EQ(lexOne("0xFFFFFFFFFFFFFFFFF").type, TokenType::Error);
    // The most negative i64 is representable and must not be rejected.
    EXPECT_EQ(lexOne("-9223372036854775808").type, TokenType::Integer);
}

TEST(Lexer, RejectsMalformedNumbers) {
    EXPECT_EQ(lexOne("0x").type, TokenType::Error);
    EXPECT_EQ(lexOne("0b2").type, TokenType::Error);
    EXPECT_EQ(lexOne("12abc").type, TokenType::Error);
}

TEST(Lexer, RecognisesRegistersIncludingAbiAliases) {
    for (const char* name : {"R0", "R1", "R9", "R10", "R15", "FP", "RV", "r7", "fp"}) {
        EXPECT_EQ(lexOne(name).type, TokenType::Register) << name;
    }
    // Not registers: out of range, leading zero, or just an identifier.
    for (const char* name : {"R16", "R99", "R01", "RX", "Rr"}) {
        EXPECT_EQ(lexOne(name).type, TokenType::Identifier) << name;
    }
}

TEST(Lexer, DecodesStringEscapes) {
    const Token token = lexOne("\"a\\nb\\t\\\\\\x41\\0\"");
    ASSERT_EQ(token.type, TokenType::String);
    const std::string expected = std::string("a\nb\t\\A") + '\0';
    EXPECT_EQ(token.strValue, expected);
}

TEST(Lexer, DecodesCharacterLiterals) {
    EXPECT_EQ(lexOne("'c'").intValue, 'c');
    EXPECT_EQ(lexOne("'\\n'").intValue, '\n');
    EXPECT_EQ(lexOne("'\\x41'").intValue, 65);
    EXPECT_EQ(lexOne("'\\q'").type, TokenType::Error);
    EXPECT_EQ(lexOne("'ab'").type, TokenType::Error);
    EXPECT_EQ(lexOne("''").type, TokenType::Error);
}

TEST(Lexer, ReportsUnterminatedLiterals) {
    EXPECT_EQ(lexOne("\"hello").type, TokenType::Error);
    EXPECT_EQ(lexOne("\"hello\nworld\"").type, TokenType::Error);
    EXPECT_EQ(lexOne("'c").type, TokenType::Error);
}

TEST(Lexer, SkipsCommentsButKeepsLineStructure) {
    const std::vector<Token> tokens = lexAll("NOP ; a comment\nHALT");
    ASSERT_EQ(tokens.size(), 4U);
    EXPECT_EQ(tokens[0].text, "NOP");
    EXPECT_EQ(tokens[1].type, TokenType::Newline);
    EXPECT_EQ(tokens[2].text, "HALT");
    EXPECT_EQ(tokens[3].type, TokenType::Eof);
}

TEST(Lexer, TracksSourceLocations) {
    Lexer lexer("A\n  BB", 7);
    const Token first = lexer.next();
    EXPECT_EQ(first.location.file, 7U);
    EXPECT_EQ(first.location.line, 1U);
    EXPECT_EQ(first.location.column, 1U);
    EXPECT_EQ(first.location.length, 1U);

    static_cast<void>(lexer.next());  // newline
    const Token second = lexer.next();
    EXPECT_EQ(second.location.line, 2U);
    EXPECT_EQ(second.location.column, 3U);
    EXPECT_EQ(second.location.length, 2U);
}

TEST(Lexer, PeekDoesNotConsume) {
    Lexer lexer("NOP HALT", 1);
    EXPECT_EQ(lexer.peek().text, "NOP");
    EXPECT_EQ(lexer.peek().text, "NOP");
    EXPECT_EQ(lexer.next().text, "NOP");
    EXPECT_EQ(lexer.next().text, "HALT");
}

TEST(Lexer, HandlesPunctuationAndEmptyInput) {
    const std::vector<Token> tokens = lexAll(":,+-.#[]");
    ASSERT_EQ(tokens.size(), 9U);
    EXPECT_EQ(tokens[0].type, TokenType::Colon);
    EXPECT_EQ(tokens[1].type, TokenType::Comma);
    EXPECT_EQ(tokens[2].type, TokenType::Plus);
    EXPECT_EQ(tokens[3].type, TokenType::Minus);
    EXPECT_EQ(tokens[4].type, TokenType::Dot);
    EXPECT_EQ(tokens[5].type, TokenType::Hash);
    EXPECT_EQ(tokens[6].type, TokenType::LBracket);
    EXPECT_EQ(tokens[7].type, TokenType::RBracket);
    EXPECT_EQ(lexOne("").type, TokenType::Eof);
}

TEST(Lexer, AlwaysMakesProgressOnGarbage) {
    // The guarantee the fuzzer relies on: any input reaches Eof.
    const std::vector<Token> tokens = lexAll("@@ $$ \x01 ?? %%");
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
    EXPECT_GT(tokens.size(), 1U);
}

}  // namespace
