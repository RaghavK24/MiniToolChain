// SPDX-License-Identifier: MIT
#include "minitool/assembler/pipeline.hpp"

#include "minitool/assembler/assembler.hpp"
#include "minitool/assembler/sema.hpp"
#include "minitool/ir/lower.hpp"
#include "minitool/lexer/lexer.hpp"
#include "minitool/parser/parser.hpp"

namespace minitool {

std::expected<AssembleResult, std::string> assembleSource(const SourceManager& sources, FileId file,
                                                          diag::DiagnosticEngine& diagnostics,
                                                          const AssembleOptions& options) {
    if (!sources.contains(file)) {
        return std::unexpected(std::string{"no such source file"});
    }

    lexer::Lexer lexer(sources.text(file), file);
    parser::Parser parser(lexer, diagnostics);
    const std::expected<ast::Program, std::string> program = parser.parse();
    if (!program.has_value()) {
        return std::unexpected(program.error());
    }

    SemanticAnalyzer analyzer(diagnostics);
    if (!analyzer.analyze(*program)) {
        return std::unexpected(std::string{"semantic analysis failed"});
    }

    std::expected<ir::Module, std::string> module =
        ir::lower(*program, std::string{sources.name(file)}, diagnostics);
    if (!module.has_value()) {
        return std::unexpected(module.error());
    }

    const optimizer::Optimizer optimizer(options.opt_level);
    const optimizer::OptStats stats = optimizer.run(*module);

    Assembler assembler(diagnostics);
    std::expected<object::ObjectFile, std::string> object = assembler.assemble(*module);
    if (!object.has_value()) {
        return std::unexpected(object.error());
    }
    if (!options.emit_debug_info) {
        object->debug_info.clear();
        object->source_files.clear();
    }

    AssembleResult result;
    result.object = std::move(*object);
    result.stats = stats;
    result.module = std::move(*module);
    return result;
}

}  // namespace minitool
