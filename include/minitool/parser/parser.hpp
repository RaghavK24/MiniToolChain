// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "minitool/ast/ast.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/lexer/lexer.hpp"

namespace minitool::parser {

/// Recursive-descent parser for the assembly grammar in docs/assembly.md.
///
///   program     := { line }
///   line        := { label } [ instruction | directive ] NEWLINE
///   label       := IDENT ':' | '.' IDENT ':'
///   instruction := IDENT [ operand { ',' operand } ]
///   directive   := '.' IDENT [ operand { ',' operand } ]
///   operand     := register | '#' expr | expr | memory | STRING
///   memory      := '[' register [ ('+' | '-') expr ] ']'
///   expr        := INTEGER | CHAR | symbol [ ('+' | '-') INTEGER ]
///
/// The parser produces AST nodes only: it never encodes an instruction and
/// never resolves a symbol (architectural rule 2). Errors are reported to the
/// DiagnosticEngine and recovered from by skipping to the next line, so one bad
/// line does not hide the rest of the file.
class Parser {
  public:
    Parser(lexer::Lexer& lexer, diag::DiagnosticEngine& diagnostics) noexcept;

    /// Parses the whole file. Returns the program, or an error summary if any
    /// diagnostic of Error severity was produced (the details are in the
    /// engine, not in the string).
    std::expected<ast::Program, std::string> parse();

  private:
    static constexpr std::size_t kLookahead = 3;

    /// Token `ahead` positions from the cursor, filling the lookahead buffer on
    /// demand. Error tokens are reported once and skipped here so that no other
    /// parser rule has to think about them.
    const lexer::Token& peek(std::size_t ahead = 0);
    lexer::Token advance();
    bool match(lexer::TokenType type);
    [[nodiscard]] bool check(lexer::TokenType type) { return peek().type == type; }
    std::optional<lexer::Token> expect(lexer::TokenType type, std::string_view what);

    void skipToEndOfLine();
    void error(const lexer::Token& token, diag::ErrorCode code, std::string message);

    /// Parses one source line into `out`; returns false if the line was
    /// malformed (a diagnostic has been reported and the line skipped).
    bool parseLine(ast::Program& out);
    std::optional<ast::InstructionNode> parseInstruction();
    std::optional<ast::DirectiveNode> parseDirective();
    std::optional<ast::Operand> parseOperand();
    std::optional<ast::Operand> parseMemoryOperand();
    /// symbol [ ('+' | '-') INTEGER ]. The addend lands in `immediate`.
    std::optional<ast::Operand> parseSymbolExpression(std::string name, SourceLocation location);

    lexer::Lexer& lexer_;
    diag::DiagnosticEngine& diagnostics_;
    std::array<lexer::Token, kLookahead> buffer_{};
    std::size_t buffered_ = 0;
    bool failed_ = false;
};

}  // namespace minitool::parser
