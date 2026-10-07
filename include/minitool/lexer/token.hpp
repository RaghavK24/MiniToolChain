// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

#include "minitool/common/source_manager.hpp"
#include "minitool/common/types.hpp"

namespace minitool::lexer {

enum class TokenType : u8 {
    Identifier,
    Register,
    Integer,
    String,
    Character,
    Colon,
    Comma,
    Plus,
    Minus,
    Dot,
    Hash,
    LBracket,
    RBracket,
    Newline,
    Eof,
    /// A malformed token. `text` holds the diagnostic message rather than the
    /// source spelling; `location` still covers the offending characters.
    Error,
};

[[nodiscard]] constexpr std::string_view tokenTypeName(TokenType type) noexcept {
    switch (type) {
        case TokenType::Identifier:
            return "identifier";
        case TokenType::Register:
            return "register";
        case TokenType::Integer:
            return "integer";
        case TokenType::String:
            return "string";
        case TokenType::Character:
            return "character";
        case TokenType::Colon:
            return "':'";
        case TokenType::Comma:
            return "','";
        case TokenType::Plus:
            return "'+'";
        case TokenType::Minus:
            return "'-'";
        case TokenType::Dot:
            return "'.'";
        case TokenType::Hash:
            return "'#'";
        case TokenType::LBracket:
            return "'['";
        case TokenType::RBracket:
            return "']'";
        case TokenType::Newline:
            return "end of line";
        case TokenType::Eof:
            return "end of file";
        case TokenType::Error:
            return "invalid token";
    }
    return "unknown";
}

/// One lexical token.
///
/// `text` borrows the source buffer owned by the SourceManager, which outlives
/// every token. `intValue` carries the parsed value of an Integer or Character
/// token and `strValue` the *decoded* bytes of a String token (escape sequences
/// already resolved), so no consumer ever has to re-interpret source syntax.
/// That is the lexer's job alone (architectural rule 1).
struct Token {
    TokenType type = TokenType::Eof;
    std::string_view text;
    SourceLocation location;
    i64 intValue = 0;
    std::string strValue;
};

}  // namespace minitool::lexer
