// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

#include "minitool/lexer/token.hpp"

namespace minitool::lexer {

/// Hand-written scanner over one source file.
///
/// The lexer performs no semantic analysis (architectural rule 1): it does not
/// know which identifiers are opcodes, whether a label is defined twice, or
/// whether an immediate fits an instruction field. It only classifies
/// characters, decodes literals and attaches source locations. Malformed input
/// yields a TokenType::Error token; the lexer never throws and always makes
/// progress, so a caller looping to Eof is guaranteed to terminate.
class Lexer {
  public:
    /// `source` must outlive the lexer (it is normally owned by a SourceManager).
    Lexer(std::string_view source, FileId file) noexcept;

    /// Consumes and returns the next token.
    Token next();

    /// Returns the next token without consuming it.
    Token peek();

  private:
    Token lexNext();
    Token lexIdentifierOrRegister(const char* start);
    Token lexInteger(const char* start, bool negative);
    Token lexString(const char* start);
    Token lexCharacter(const char* start);

    void skipHorizontalWhitespaceAndComments();
    [[nodiscard]] bool atEnd() const noexcept { return ptr_ >= end_; }
    [[nodiscard]] char peekChar() const noexcept { return atEnd() ? '\0' : *ptr_; }
    [[nodiscard]] char peekCharAt(std::size_t ahead) const noexcept {
        return (ptr_ + ahead) < end_ ? *(ptr_ + ahead) : '\0';
    }
    char advance() noexcept { return atEnd() ? '\0' : *ptr_++; }

    Token makeToken(TokenType type, const char* start) const;
    Token makeError(const char* start, std::string message) const;
    /// Decodes one escape sequence after the backslash. Returns false and sets
    /// `error` if the sequence is not one the language defines.
    bool decodeEscape(i64& value, std::string& error);

    std::string_view source_;
    FileId file_;
    const char* ptr_;
    const char* end_;
    u32 line_ = 1;
    const char* line_start_;

    bool peeked_ = false;
    Token peeked_token_;
};

}  // namespace minitool::lexer
