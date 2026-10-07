// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "minitool/ast/ast.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"

namespace minitool {

/// Validates a parsed program against the ISA and the directive rules before
/// anything is lowered or encoded.
///
/// Everything checked here is a property of one file in isolation: instruction
/// shapes, operand kinds, literal ranges, duplicate labels, directive
/// arguments. Cross-file questions (is this external symbol defined anywhere?
/// does this relocation fit?) belong to the linker, and are not second-guessed
/// here.
class SemanticAnalyzer {
  public:
    explicit SemanticAnalyzer(diag::DiagnosticEngine& diagnostics) noexcept;

    /// Returns true if the program is free of errors. Every problem found is
    /// reported to the engine; analysis continues after each one so that a
    /// single run reports as much as it can.
    bool analyze(const ast::Program& program);

  private:
    void checkInstruction(const ast::InstructionNode& instruction);
    void checkDirective(const ast::DirectiveNode& directive);
    void checkLabel(const ast::LabelNode& label);

    /// Reports an error and records that the program is invalid.
    void error(diag::ErrorCode code, SourceLocation location, std::string message);
    void errorWithNote(diag::ErrorCode code, SourceLocation location, std::string message,
                       SourceLocation note_location, std::string note);

    /// True if `operand` is an integer literal or a symbol reference: the two
    /// things that can produce a value for an immediate field.
    static bool isValueOperand(const ast::Operand& operand) noexcept;
    /// Checks a literal against a field width, reporting IntegerOverflow.
    bool checkSignedRange(const ast::Operand& operand, unsigned bits, std::string_view what);

    diag::DiagnosticEngine& diagnostics_;
    /// Label name -> where it was defined, for duplicate detection.
    std::unordered_map<std::string, SourceLocation> labels_;
    bool valid_ = true;
};

}  // namespace minitool
