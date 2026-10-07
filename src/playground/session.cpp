// SPDX-License-Identifier: MIT

#include "minitool/playground/session.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "minitool/assembler/pipeline.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/disassembler/disassembler.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/linker/linker.hpp"
#include "minitool/object/object.hpp"
#include "minitool/vm/vm.hpp"

namespace minitool::playground {
namespace {

/// The playground's name for the buffer being edited. It appears in every
/// diagnostic, so it is deliberately something a user recognises rather than a
/// temporary path.
constexpr const char* kSourceName = "playground.asm";

/// Captures output like the default provider but stops accumulating at
/// `kMaxOutputBytes`. The program keeps running (truncating is a display
/// decision, not a trap), but memory stays bounded however hard it writes.
class CappedSyscallProvider final : public vm::SyscallProvider {
  public:
    [[nodiscard]] std::expected<void, std::string> invoke(u64 number,
                                                          vm::SyscallContext& context) override {
        const std::expected<void, std::string> result = inner_.invoke(number, context);
        if (inner_.output.size() > kMaxOutputBytes) {
            inner_.output.resize(kMaxOutputBytes);
            truncated = true;
        }
        return result;
    }

    void setInput(std::string input) {
        inner_.capture = true;
        inner_.input = std::move(input);
    }

    [[nodiscard]] const std::string& output() const noexcept { return inner_.output; }

    bool truncated = false;

  private:
    vm::DefaultSyscallProvider inner_;
};

/// Records `message` as the report's headline error, unless the diagnostics
/// already say it.
///
/// The linker reports through the diagnostic engine *and* returns the reason,
/// so without this the UI would show the same sentence twice under two
/// different headings.
void setError(RunReport& report, std::string message) {
    if (!message.empty() && report.diagnostics.find(message) != std::string::npos) {
        return;
    }
    report.error = std::move(message);
}

/// Fills in the machine state that the UI displays after a run. Done in one
/// place so a report is never half-populated.
void recordState(RunReport& report, const vm::VirtualMachine& machine) {
    const vm::CPUState& state = machine.state();
    report.registers = state.registers;
    report.pc = state.pc;
    report.sp = state.sp;
    report.flags = state.flags;
}

}  // namespace

std::string_view stageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::Assemble:
            return "assemble";
        case Stage::Link:
            return "link";
        case Stage::Load:
            return "load";
        case Stage::Execute:
            return "execute";
        case Stage::Finished:
            return "finished";
    }
    return "unknown";
}

RunReport runSource(const RunRequest& request) {
    RunReport report;

    if (request.source.size() > kMaxSourceBytes) {
        report.stage = Stage::Assemble;
        report.error = std::format("source is {} bytes; the limit is {}", request.source.size(),
                                   kMaxSourceBytes);
        return report;
    }

    // ------------------------------------------------------------ assemble --
    SourceManager sources;
    const FileId file = sources.addFile(kSourceName, request.source);
    diag::DiagnosticEngine diagnostics(sources);

    AssembleOptions assemble_options;
    assemble_options.opt_level = request.opt_level;

    const std::expected<AssembleResult, std::string> assembled =
        assembleSource(sources, file, diagnostics, assemble_options);
    // Diagnostics are rendered even on success: a program can assemble cleanly
    // and still have produced warnings worth showing.
    report.diagnostics = diagnostics.renderAll();
    if (!assembled.has_value()) {
        report.stage = Stage::Assemble;
        setError(report, assembled.error());
        return report;
    }
    report.stats = assembled->stats;

    // ---------------------------------------------------------------- link --
    const std::vector<object::ObjectFile> objects{assembled->object};
    linker::Linker linker(diagnostics);
    const std::expected<executable::Executable, std::string> linked =
        linker.link(objects, linker::LinkOptions{});
    report.diagnostics = diagnostics.renderAll();
    if (!linked.has_value()) {
        report.stage = Stage::Link;
        setError(report, linked.error());
        return report;
    }

    if (request.want_disassembly) {
        const disassembler::Disassembler disassembler;
        report.disassembly = disassembler.disassemble(*linked);
    }

    // ----------------------------------------------------------------- run --
    vm::VirtualMachine machine;
    auto provider = std::make_unique<CappedSyscallProvider>();
    provider->setInput(request.input);
    CappedSyscallProvider* captured = provider.get();
    machine.setSyscallProvider(std::move(provider));

    const std::expected<void, std::string> loaded = machine.load(*linked);
    if (!loaded.has_value()) {
        report.stage = Stage::Load;
        setError(report, loaded.error());
        return report;
    }

    const u64 budget = std::clamp<u64>(request.budget, 1, kMaxPlaygroundBudget);
    const vm::RunResult run_result = machine.run(budget);

    report.output = captured->output();
    report.output_truncated = captured->truncated;
    report.exit_code = run_result.exit_code;
    report.instructions = run_result.instructions;
    recordState(report, machine);

    if (!run_result.ok()) {
        report.stage = Stage::Execute;
        setError(report, run_result.message);
        return report;
    }

    report.ok = true;
    report.stage = Stage::Finished;
    return report;
}

}  // namespace minitool::playground
