// SPDX-License-Identifier: MIT
#pragma once

/// Helpers that drive the toolchain the way a user does (source in, program
/// out), so that tests state what they are checking instead of re-wiring the
/// pipeline each time.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/assembler/pipeline.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/linker/linker.hpp"
#include "minitool/object/object.hpp"
#include "minitool/vm/vm.hpp"

namespace testkit {

using namespace minitool;  // NOLINT(google-build-using-namespace): test-only

/// One assembly job: the object it produced (if any) and everything the
/// toolchain said about it.
struct Assembled {
    bool ok = false;
    object::ObjectFile object;
    optimizer::OptStats stats;
    std::string diagnostics;
    std::string error;

    /// True if any reported diagnostic mentions `text`; used to assert on the
    /// message a user actually sees.
    [[nodiscard]] bool mentions(std::string_view text) const {
        return diagnostics.find(text) != std::string::npos || error.find(text) != std::string::npos;
    }
};

inline Assembled assemble(std::string_view source,
                          optimizer::OptLevel level = optimizer::OptLevel::O0,
                          std::string name = "test.asm") {
    SourceManager sources;
    const FileId file = sources.addFile(std::move(name), std::string{source});
    diag::DiagnosticEngine diagnostics(sources);
    AssembleOptions options;
    options.opt_level = level;

    Assembled result;
    std::expected<AssembleResult, std::string> assembled =
        assembleSource(sources, file, diagnostics, options);
    result.diagnostics = diagnostics.renderAll();
    if (assembled.has_value()) {
        result.ok = true;
        result.object = std::move(assembled->object);
        result.stats = assembled->stats;
    } else {
        result.error = assembled.error();
    }
    return result;
}

struct Linked {
    bool ok = false;
    executable::Executable executable;
    std::string error;

    [[nodiscard]] bool mentions(std::string_view text) const {
        return error.find(text) != std::string::npos;
    }
};

inline Linked link(std::span<const object::ObjectFile> objects,
                   const linker::LinkOptions& options = {}) {
    SourceManager sources;
    diag::DiagnosticEngine diagnostics(sources);
    linker::Linker linker(diagnostics);
    Linked result;
    std::expected<executable::Executable, std::string> executable = linker.link(objects, options);
    if (executable.has_value()) {
        result.ok = true;
        result.executable = std::move(*executable);
    } else {
        result.error = executable.error();
    }
    return result;
}

/// Assembles and links one source file.
inline Linked build(std::string_view source, optimizer::OptLevel level = optimizer::OptLevel::O0) {
    const Assembled assembled = assemble(source, level);
    if (!assembled.ok) {
        Linked failed;
        failed.error = assembled.error + assembled.diagnostics;
        return failed;
    }
    const std::vector<object::ObjectFile> objects{assembled.object};
    return link(objects);
}

/// Assembles and links several sources, as separate objects.
inline Linked buildAll(std::span<const std::string_view> sources,
                       optimizer::OptLevel level = optimizer::OptLevel::O0) {
    std::vector<object::ObjectFile> objects;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const Assembled assembled = assemble(sources[i], level, std::format("unit{}.asm", i));
        if (!assembled.ok) {
            Linked failed;
            failed.error = assembled.error + assembled.diagnostics;
            return failed;
        }
        objects.push_back(assembled.object);
    }
    return link(objects);
}

/// The observable result of running a program: how it ended, what it printed,
/// and the final machine state.
struct Ran {
    bool ok = false;
    vm::VMError error = vm::VMError::None;
    std::string message;
    std::string output;
    u64 exit_code = 0;
    u64 instructions = 0;
    vm::CPUState cpu;

    [[nodiscard]] u64 reg(unsigned index) const { return cpu.registers[index]; }
};

inline Ran run(const executable::Executable& executable, std::string_view input = {},
               u64 budget = 100'000) {
    vm::VirtualMachine machine;
    auto provider = std::make_unique<vm::DefaultSyscallProvider>();
    provider->capture = true;
    provider->input = std::string{input};
    vm::DefaultSyscallProvider* captured = provider.get();
    machine.setSyscallProvider(std::move(provider));

    Ran result;
    const std::expected<void, std::string> loaded = machine.load(executable);
    if (!loaded.has_value()) {
        result.message = loaded.error();
        return result;
    }
    const vm::RunResult run_result = machine.run(budget);
    result.ok = run_result.ok();
    result.error = run_result.error;
    result.message = run_result.message;
    result.exit_code = run_result.exit_code;
    result.instructions = run_result.instructions;
    result.output = captured->output;
    result.cpu = machine.state();
    return result;
}

/// Source in, execution out: the whole toolchain in one call.
inline Ran runSource(std::string_view source, optimizer::OptLevel level = optimizer::OptLevel::O0,
                     std::string_view input = {}) {
    const Linked linked = build(source, level);
    if (!linked.ok) {
        Ran failed;
        failed.message = linked.error;
        return failed;
    }
    return run(linked.executable, input);
}

}  // namespace testkit
