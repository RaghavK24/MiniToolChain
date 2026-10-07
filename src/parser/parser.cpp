// SPDX-License-Identifier: MIT
#include "minitool/parser/parser.hpp"

#include <format>
#include <utility>

#include "minitool/isa/registers.hpp"

namespace minitool::parser {
namespace {

/// Directive spelling -> kind. `.text`, `.data`, `.rodata` and `.bss` are
/// accepted as shorthands for `.section <name>`.
struct DirectiveSpelling {
    std::string_view name;
    ast::DirectiveType type;
    /// Non-empty for the section shorthands.
    std::string_view implicit_section;
};

constexpr std::array<DirectiveSpelling, 16> kDirectives{{
    {"section", ast::DirectiveType::Section, ""},
    {"text", ast::DirectiveType::Section, ".text"},
    {"data", ast::DirectiveType::Section, ".data"},
    {"rodata", ast::DirectiveType::Section, ".rodata"},
    {"bss", ast::DirectiveType::Section, ".bss"},
    {"global", ast::DirectiveType::Global, ""},
    {"globl", ast::DirectiveType::Global, ""},
    {"extern", ast::DirectiveType::Extern, ""},
    {"weak", ast::DirectiveType::Weak, ""},
    {"byte", ast::DirectiveType::Byte, ""},
    {"word", ast::DirectiveType::Word, ""},
    {"dword", ast::DirectiveType::Dword, ""},
    {"qword", ast::DirectiveType::Qword, ""},
    {"asciz", ast::DirectiveType::Asciz, ""},
    {"align", ast::DirectiveType::Align, ""},
    {"space", ast::DirectiveType::Space, ""},
}};

const DirectiveSpelling* findDirective(std::string_view name) noexcept {
    for (const DirectiveSpelling& entry : kDirectives) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

Parser::Parser(lexer::Lexer& lexer, diag::DiagnosticEngine& diagnostics) noexcept
    : lexer_(lexer), diagnostics_(diagnostics) {}

const lexer::Token& Parser::peek(std::size_t ahead) {
    while (buffered_ <= ahead) {
        lexer::Token token = lexer_.next();
        while (token.type == lexer::TokenType::Error) {
            // Report the lexical error once, then keep scanning: the parser
            // rules below never see an Error token.
            diagnostics_.error(diag::ErrorCode::LexicalError, token.location, token.strValue);
            failed_ = true;
            token = lexer_.next();
        }
        buffer_[buffered_] = std::move(token);
        ++buffered_;
    }
    return buffer_[ahead];
}

lexer::Token Parser::advance() {
    peek();
    lexer::Token token = std::move(buffer_[0]);
    for (std::size_t i = 1; i < buffered_; ++i) {
        buffer_[i - 1] = std::move(buffer_[i]);
    }
    --buffered_;
    return token;
}

bool Parser::match(lexer::TokenType type) {
    if (peek().type == type) {
        static_cast<void>(advance());
        return true;
    }
    return false;
}

std::optional<lexer::Token> Parser::expect(lexer::TokenType type, std::string_view what) {
    if (peek().type == type) {
        return advance();
    }
    const lexer::Token& token = peek();
    error(token, diag::ErrorCode::ParseError,
          std::format("expected {}, found {}", what, tokenTypeName(token.type)));
    return std::nullopt;
}

void Parser::error(const lexer::Token& token, diag::ErrorCode code, std::string message) {
    diagnostics_.error(code, token.location, std::move(message));
    failed_ = true;
}

void Parser::skipToEndOfLine() {
    while (peek().type != lexer::TokenType::Newline && peek().type != lexer::TokenType::Eof) {
        static_cast<void>(advance());
    }
    static_cast<void>(match(lexer::TokenType::Newline));
}

std::expected<ast::Program, std::string> Parser::parse() {
    ast::Program program;
    while (peek().type != lexer::TokenType::Eof) {
        if (match(lexer::TokenType::Newline)) {
            continue;
        }
        if (!parseLine(program)) {
            skipToEndOfLine();
        }
    }
    if (failed_ || diagnostics_.hasErrors()) {
        return std::unexpected(std::format("{} parse error(s)", diagnostics_.errorCount()));
    }
    return program;
}

bool Parser::parseLine(ast::Program& out) {
    // Any number of labels may precede the statement, on the same line or not:
    //     loop: done: ADD R1, R2
    while (true) {
        const bool plain_label =
            peek().type == lexer::TokenType::Identifier && peek(1).type == lexer::TokenType::Colon;
        const bool local_label = peek().type == lexer::TokenType::Dot &&
                                 peek(1).type == lexer::TokenType::Identifier &&
                                 peek(2).type == lexer::TokenType::Colon;
        if (!plain_label && !local_label) {
            break;
        }
        ast::LabelNode label;
        label.local = local_label;
        if (local_label) {
            const lexer::Token dot = advance();
            const lexer::Token name = advance();
            label.name = "." + std::string{name.text};
            label.location = dot.location;
            label.location.length = dot.location.length + name.location.length;
        } else {
            const lexer::Token name = advance();
            label.name = std::string{name.text};
            label.location = name.location;
        }
        static_cast<void>(advance());  // ':'
        out.statements.emplace_back(std::move(label));
    }

    if (peek().type == lexer::TokenType::Newline || peek().type == lexer::TokenType::Eof) {
        static_cast<void>(match(lexer::TokenType::Newline));
        return true;
    }

    if (peek().type == lexer::TokenType::Dot) {
        std::optional<ast::DirectiveNode> directive = parseDirective();
        if (!directive.has_value()) {
            return false;
        }
        out.statements.emplace_back(std::move(*directive));
    } else if (peek().type == lexer::TokenType::Identifier) {
        std::optional<ast::InstructionNode> instruction = parseInstruction();
        if (!instruction.has_value()) {
            return false;
        }
        out.statements.emplace_back(std::move(*instruction));
    } else {
        error(peek(), diag::ErrorCode::ParseError,
              std::format("expected a label, instruction or directive, found {}",
                          tokenTypeName(peek().type)));
        return false;
    }

    if (peek().type != lexer::TokenType::Newline && peek().type != lexer::TokenType::Eof) {
        error(peek(), diag::ErrorCode::ParseError,
              std::format("unexpected {} after end of statement", tokenTypeName(peek().type)));
        return false;
    }
    static_cast<void>(match(lexer::TokenType::Newline));
    return true;
}

std::optional<ast::InstructionNode> Parser::parseInstruction() {
    const lexer::Token mnemonic = advance();
    ast::InstructionNode instruction;
    instruction.mnemonic = std::string{mnemonic.text};
    instruction.location = mnemonic.location;

    while (peek().type != lexer::TokenType::Newline && peek().type != lexer::TokenType::Eof) {
        std::optional<ast::Operand> operand = parseOperand();
        if (!operand.has_value()) {
            return std::nullopt;
        }
        instruction.operands.push_back(std::move(*operand));
        if (!match(lexer::TokenType::Comma)) {
            break;
        }
        // A comma promises another operand; a trailing one is a mistake, and
        // saying so here is clearer than letting sema count operands later.
        if (peek().type == lexer::TokenType::Newline || peek().type == lexer::TokenType::Eof) {
            error(peek(), diag::ErrorCode::InvalidOperand, "expected an operand after ','");
            return std::nullopt;
        }
    }
    return instruction;
}

std::optional<ast::DirectiveNode> Parser::parseDirective() {
    const lexer::Token dot = advance();
    const std::optional<lexer::Token> name =
        expect(lexer::TokenType::Identifier, "a directive name");
    if (!name.has_value()) {
        return std::nullopt;
    }
    const DirectiveSpelling* spelling = findDirective(name->text);
    if (spelling == nullptr) {
        error(*name, diag::ErrorCode::InvalidDirective,
              std::format("unknown directive '.{}'", name->text));
        return std::nullopt;
    }

    ast::DirectiveNode directive;
    directive.type = spelling->type;
    directive.location = dot.location;
    directive.location.length = dot.location.length + name->location.length;

    if (!spelling->implicit_section.empty()) {
        ast::Operand section;
        section.type = ast::OperandType::Symbol;
        section.symbol = std::string{spelling->implicit_section};
        section.location = directive.location;
        directive.operands.push_back(std::move(section));
        return directive;
    }

    // `.space` takes a size and an optional fill byte; every other directive
    // takes a plain comma-separated operand list.
    while (peek().type != lexer::TokenType::Newline && peek().type != lexer::TokenType::Eof) {
        std::optional<ast::Operand> operand = parseOperand();
        if (!operand.has_value()) {
            return std::nullopt;
        }
        directive.operands.push_back(std::move(*operand));
        if (!match(lexer::TokenType::Comma)) {
            break;
        }
        if (peek().type == lexer::TokenType::Newline || peek().type == lexer::TokenType::Eof) {
            error(peek(), diag::ErrorCode::InvalidOperand, "expected an operand after ','");
            return std::nullopt;
        }
    }
    return directive;
}

std::optional<ast::Operand> Parser::parseOperand() {
    const lexer::Token& token = peek();
    ast::Operand operand;
    operand.location = token.location;

    switch (token.type) {
        case lexer::TokenType::Register: {
            const lexer::Token reg = advance();
            operand.type = ast::OperandType::Register;
            // The lexer only classifies a token as Register when parseRegister
            // succeeds, so this cannot fail.
            operand.reg = isa::parseRegister(reg.text).value_or(isa::Reg::R0);
            return operand;
        }
        case lexer::TokenType::Integer:
        case lexer::TokenType::Character: {
            const lexer::Token value = advance();
            operand.type = ast::OperandType::Immediate;
            operand.immediate = value.intValue;
            return operand;
        }
        case lexer::TokenType::String: {
            const lexer::Token value = advance();
            operand.type = ast::OperandType::String;
            operand.text = value.strValue;
            return operand;
        }
        case lexer::TokenType::Hash: {
            // `#10`: the optional immediate marker.
            static_cast<void>(advance());
            const std::optional<lexer::Token> value =
                expect(lexer::TokenType::Integer, "an integer after '#'");
            if (!value.has_value()) {
                return std::nullopt;
            }
            operand.type = ast::OperandType::Immediate;
            operand.immediate = value->intValue;
            return operand;
        }
        case lexer::TokenType::Identifier: {
            const lexer::Token name = advance();
            return parseSymbolExpression(std::string{name.text}, name.location);
        }
        case lexer::TokenType::Dot: {
            // A section name (`.text`) or a local label reference (`.L1`).
            const lexer::Token dot = advance();
            const std::optional<lexer::Token> name =
                expect(lexer::TokenType::Identifier, "a name after '.'");
            if (!name.has_value()) {
                return std::nullopt;
            }
            SourceLocation location = dot.location;
            location.length = dot.location.length + name->location.length;
            return parseSymbolExpression("." + std::string{name->text}, location);
        }
        case lexer::TokenType::LBracket:
            return parseMemoryOperand();
        default:
            break;
    }
    error(token, diag::ErrorCode::InvalidOperand,
          std::format("expected an operand, found {}", tokenTypeName(token.type)));
    return std::nullopt;
}

std::optional<ast::Operand> Parser::parseSymbolExpression(std::string name,
                                                          SourceLocation location) {
    ast::Operand operand;
    operand.type = ast::OperandType::Symbol;
    operand.symbol = std::move(name);
    operand.location = location;

    // Only `symbol + constant` and `symbol - constant` are supported; general
    // expression evaluation is deliberately out of scope.
    if (peek().type == lexer::TokenType::Plus || peek().type == lexer::TokenType::Minus) {
        const bool negate = peek().type == lexer::TokenType::Minus;
        static_cast<void>(advance());
        const std::optional<lexer::Token> addend =
            expect(lexer::TokenType::Integer, "an integer addend");
        if (!addend.has_value()) {
            return std::nullopt;
        }
        // Negate through u64 so that -INT64_MIN cannot overflow.
        operand.immediate =
            negate ? static_cast<i64>(~static_cast<u64>(addend->intValue) + 1U) : addend->intValue;
        operand.location.length =
            (addend->location.column + addend->location.length) - operand.location.column;
    }
    return operand;
}

std::optional<ast::Operand> Parser::parseMemoryOperand() {
    const lexer::Token open = advance();  // '['
    ast::Operand operand;
    operand.type = ast::OperandType::Memory;
    operand.location = open.location;

    const std::optional<lexer::Token> base =
        expect(lexer::TokenType::Register, "a base register inside '[...]'");
    if (!base.has_value()) {
        return std::nullopt;
    }
    operand.reg = isa::parseRegister(base->text).value_or(isa::Reg::R0);

    if (peek().type == lexer::TokenType::Plus || peek().type == lexer::TokenType::Minus) {
        const bool negate = peek().type == lexer::TokenType::Minus;
        static_cast<void>(advance());
        const std::optional<lexer::Token> displacement =
            expect(lexer::TokenType::Integer, "an integer displacement");
        if (!displacement.has_value()) {
            return std::nullopt;
        }
        operand.immediate = negate
                                ? static_cast<i64>(~static_cast<u64>(displacement->intValue) + 1U)
                                : displacement->intValue;
    } else if (peek().type == lexer::TokenType::Integer) {
        // `[R1 8]` is a common slip; name it rather than reporting "expected ]".
        error(peek(), diag::ErrorCode::InvalidOperand,
              "expected '+' or '-' before the displacement");
        return std::nullopt;
    }

    const std::optional<lexer::Token> close =
        expect(lexer::TokenType::RBracket, "']' to close the memory operand");
    if (!close.has_value()) {
        return std::nullopt;
    }
    operand.location.length =
        (close->location.column + close->location.length) - operand.location.column;
    return operand;
}

}  // namespace minitool::parser
