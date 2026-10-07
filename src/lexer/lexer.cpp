// SPDX-License-Identifier: MIT
#include "minitool/lexer/lexer.hpp"

#include <charconv>
#include <format>
#include <limits>

#include "minitool/isa/registers.hpp"

namespace minitool::lexer {
namespace {

[[nodiscard]] bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool isIdentifierStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] bool isIdentifierContinue(char c) noexcept {
    return isIdentifierStart(c) || isDigit(c);
}

[[nodiscard]] char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool isDigitOfBase(char c, int base) noexcept {
    const char c_lower = lower(c);
    switch (base) {
        case 2:
            return c_lower == '0' || c_lower == '1';
        case 8:
            return c_lower >= '0' && c_lower <= '7';
        case 16:
            return isDigit(c_lower) || (c_lower >= 'a' && c_lower <= 'f');
        default:
            return isDigit(c_lower);
    }
}

}  // namespace

Lexer::Lexer(std::string_view source, FileId file) noexcept
    : source_(source),
      file_(file),
      ptr_(source.data()),
      end_(source.data() + source.size()),
      line_start_(source.data()) {}

Token Lexer::next() {
    if (peeked_) {
        peeked_ = false;
        return std::move(peeked_token_);
    }
    return lexNext();
}

Token Lexer::peek() {
    if (!peeked_) {
        peeked_token_ = lexNext();
        peeked_ = true;
    }
    return peeked_token_;
}

Token Lexer::makeToken(TokenType type, const char* start) const {
    Token token;
    token.type = type;
    token.text = std::string_view(start, static_cast<std::size_t>(ptr_ - start));
    token.location = SourceLocation{file_, line_, static_cast<u32>(start - line_start_) + 1U,
                                    static_cast<u32>(ptr_ - start)};
    return token;
}

Token Lexer::makeError(const char* start, std::string message) const {
    // `text` keeps the offending source spelling so diagnostics can underline
    // it; the message travels in strValue, which the token owns.
    Token token = makeToken(TokenType::Error, start);
    token.strValue = std::move(message);
    return token;
}

void Lexer::skipHorizontalWhitespaceAndComments() {
    while (!atEnd()) {
        const char c = *ptr_;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++ptr_;
        } else if (c == ';') {
            // ';' starts a comment that runs to (but does not include) the
            // newline, so line structure survives for the parser.
            while (!atEnd() && *ptr_ != '\n') {
                ++ptr_;
            }
        } else {
            break;
        }
    }
}

Token Lexer::lexNext() {
    skipHorizontalWhitespaceAndComments();
    if (atEnd()) {
        return makeToken(TokenType::Eof, ptr_);
    }

    const char* start = ptr_;
    const char c = advance();
    switch (c) {
        case ':':
            return makeToken(TokenType::Colon, start);
        case ',':
            return makeToken(TokenType::Comma, start);
        case '+':
            return makeToken(TokenType::Plus, start);
        case '.':
            return makeToken(TokenType::Dot, start);
        case '#':
            return makeToken(TokenType::Hash, start);
        case '[':
            return makeToken(TokenType::LBracket, start);
        case ']':
            return makeToken(TokenType::RBracket, start);
        case '\n': {
            const Token token = makeToken(TokenType::Newline, start);
            ++line_;
            line_start_ = ptr_;
            return token;
        }
        case '-':
            // "-5" is one integer token; a '-' followed by anything else is the
            // binary operator used in `[R1 - 8]` and `symbol - 4`.
            if (isDigit(peekChar())) {
                return lexInteger(start, true);
            }
            return makeToken(TokenType::Minus, start);
        case '"':
            return lexString(start);
        case '\'':
            return lexCharacter(start);
        default:
            break;
    }
    if (isIdentifierStart(c)) {
        return lexIdentifierOrRegister(start);
    }
    if (isDigit(c)) {
        return lexInteger(start, false);
    }
    return makeError(start, std::format("unexpected character '{}'", c));
}

Token Lexer::lexIdentifierOrRegister(const char* start) {
    while (!atEnd() && isIdentifierContinue(*ptr_)) {
        ++ptr_;
    }
    Token token = makeToken(TokenType::Identifier, start);
    if (isa::parseRegister(token.text).has_value()) {
        token.type = TokenType::Register;
    }
    return token;
}

Token Lexer::lexInteger(const char* start, bool negative) {
    // On entry the first character (the sign, or the first digit) is consumed.
    const char* digits_begin = negative ? start + 1 : start;
    int base = 10;
    if (*digits_begin == '0') {
        // For "-0x10" the cursor still sits on '0'; for "0x10" it sits on 'x'.
        const char prefix = negative ? peekCharAt(1) : peekChar();
        const std::size_t prefix_skip = negative ? 2U : 1U;
        switch (lower(prefix)) {
            case 'x':
                base = 16;
                break;
            case 'b':
                base = 2;
                break;
            case 'o':
                base = 8;
                break;
            default:
                break;
        }
        if (base != 10) {
            ptr_ += prefix_skip;
            digits_begin = ptr_;
        }
    }

    while (!atEnd() && isDigitOfBase(*ptr_, base)) {
        ++ptr_;
    }
    if (digits_begin == ptr_) {
        // Consume the rest of the malformed literal so the caller makes progress.
        while (!atEnd() && isIdentifierContinue(*ptr_)) {
            ++ptr_;
        }
        return makeError(start, "malformed integer literal");
    }
    if (!atEnd() && isIdentifierContinue(*ptr_)) {
        while (!atEnd() && isIdentifierContinue(*ptr_)) {
            ++ptr_;
        }
        return makeError(start, std::format("invalid digit in base-{} integer literal", base));
    }

    u64 magnitude = 0;
    const std::from_chars_result result = std::from_chars(digits_begin, ptr_, magnitude, base);
    if (result.ec != std::errc{} || result.ptr != ptr_) {
        return makeError(start, "integer literal does not fit in 64 bits");
    }
    // The magnitude of the most negative i64 is 2^63, which is not itself a
    // valid i64; handle it without ever forming a signed overflow.
    constexpr u64 kMaxNegativeMagnitude = u64{1} << 63U;
    if (negative ? magnitude > kMaxNegativeMagnitude
                 : magnitude > static_cast<u64>(std::numeric_limits<i64>::max())) {
        return makeError(start, "integer literal does not fit in 64 bits");
    }

    Token token = makeToken(TokenType::Integer, start);
    token.intValue = negative ? static_cast<i64>(~magnitude + 1U) : static_cast<i64>(magnitude);
    return token;
}

bool Lexer::decodeEscape(i64& value, std::string& error) {
    if (atEnd()) {
        error = "unterminated escape sequence";
        return false;
    }
    const char c = advance();
    switch (c) {
        case 'n':
            value = '\n';
            return true;
        case 't':
            value = '\t';
            return true;
        case 'r':
            value = '\r';
            return true;
        case '0':
            value = 0;
            return true;
        case '\\':
            value = '\\';
            return true;
        case '\'':
            value = '\'';
            return true;
        case '"':
            value = '"';
            return true;
        case 'x': {
            u32 digits = 0;
            u32 number = 0;
            while (digits < 2 && !atEnd() && isDigitOfBase(*ptr_, 16)) {
                const char digit = lower(advance());
                number = number * 16U +
                         static_cast<u32>(isDigit(digit) ? digit - '0' : digit - 'a' + 10);
                ++digits;
            }
            if (digits == 0) {
                error = "\\x needs at least one hexadecimal digit";
                return false;
            }
            value = static_cast<i64>(number);
            return true;
        }
        default:
            error = std::format("unknown escape sequence '\\{}'", c);
            return false;
    }
}

Token Lexer::lexString(const char* start) {
    std::string decoded;
    while (true) {
        if (atEnd() || peekChar() == '\n') {
            return makeError(start, "unterminated string literal");
        }
        const char c = advance();
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            i64 value = 0;
            std::string error;
            if (!decodeEscape(value, error)) {
                return makeError(start, std::move(error));
            }
            decoded.push_back(static_cast<char>(static_cast<u8>(value)));
        } else {
            decoded.push_back(c);
        }
    }
    Token token = makeToken(TokenType::String, start);
    token.strValue = std::move(decoded);
    return token;
}

Token Lexer::lexCharacter(const char* start) {
    if (atEnd() || peekChar() == '\n') {
        return makeError(start, "unterminated character literal");
    }
    i64 value = 0;
    const char c = advance();
    if (c == '\\') {
        std::string error;
        if (!decodeEscape(value, error)) {
            return makeError(start, std::move(error));
        }
    } else if (c == '\'') {
        return makeError(start, "empty character literal");
    } else {
        value = static_cast<i64>(static_cast<u8>(c));
    }
    if (atEnd() || peekChar() != '\'') {
        return makeError(start, "unterminated character literal");
    }
    ++ptr_;  // closing quote
    Token token = makeToken(TokenType::Character, start);
    token.intValue = value;
    return token;
}

}  // namespace minitool::lexer
