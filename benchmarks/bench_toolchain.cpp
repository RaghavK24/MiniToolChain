// SPDX-License-Identifier: MIT
//
// The benchmark suite. It measures each stage of the toolchain separately, so a
// regression can be attributed to a stage rather than to "the compiler got
// slower", and reports p50/p95/p99 alongside the throughput.
//
// Numbers mean nothing without their context, so the report starts with the
// machine, compiler and build configuration. Do not quote a
// figure from here without the header that came with it.
//
//   build/msvc-release/bench_toolchain.exe [iterations]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/assembler/pipeline.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/disassembler/disassembler.hpp"
#include "minitool/executable/executable_io.hpp"
#include "minitool/lexer/lexer.hpp"
#include "minitool/linker/linker.hpp"
#include "minitool/object/object_io.hpp"
#include "minitool/parser/parser.hpp"
#include "minitool/vm/vm.hpp"

namespace {

using namespace minitool;
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

/// The timings of one benchmark: mean, p50, p95 and p99.
struct Measurement {
    std::string name;
    /// What one iteration processed, for the throughput column.
    double units = 0;
    std::string unit_name;
    std::vector<double> samples;  // seconds per iteration

    [[nodiscard]] double percentile(double fraction) const {
        std::vector<double> sorted = samples;
        std::ranges::sort(sorted);
        const std::size_t index =
            std::min(sorted.size() - 1,
                     static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())));
        return sorted[index];
    }

    [[nodiscard]] double mean() const {
        return std::accumulate(samples.begin(), samples.end(), 0.0) /
               static_cast<double>(samples.size());
    }
};

/// Runs `work` `iterations` times, timing each run.
template <typename Work>
Measurement measure(std::string name, double units, std::string unit_name, int iterations,
                    Work&& work) {
    Measurement measurement{std::move(name), units, std::move(unit_name), {}};
    measurement.samples.reserve(static_cast<std::size_t>(iterations));
    // One untimed run so that lazy allocation and cold caches do not land in
    // the first sample.
    work();
    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        work();
        measurement.samples.push_back(Seconds(Clock::now() - start).count());
    }
    return measurement;
}

void report(const Measurement& measurement) {
    const double p50 = measurement.percentile(0.50);
    const double throughput = p50 > 0 ? measurement.units / p50 : 0;
    std::printf("%-26s %9.3f %9.3f %9.3f %9.3f  %12.0f %s/s\n", measurement.name.c_str(),
                measurement.mean() * 1e6, p50 * 1e6, measurement.percentile(0.95) * 1e6,
                measurement.percentile(0.99) * 1e6, throughput, measurement.unit_name.c_str());
}

/// A synthetic program of roughly `instructions` instructions, with loops,
/// calls and data so that no stage is measured on an unrealistically flat
/// input.
std::string makeProgram(int instructions) {
    std::string source =
        ".global _start\n"
        ".rodata\nmessage: .asciz \"benchmark\\n\"\n"
        ".data\nslot: .qword 0\n"
        ".text\n_start:\n";
    for (int i = 0; i < instructions / 8; ++i) {
        source += std::format(
            "    MOVI R1, {}\n"
            "    MOVI R2, {}\n"
            "    ADD  R1, R2\n"
            "    CMP  R1, R2\n"
            "    JLE  skip{}\n"
            "    SUB  R1, R2\n"
            "skip{}:\n"
            "    LEA  R3, slot\n"
            "    STORE [R3 + 0], R1\n",
            i % 100, (i % 7) + 1, i, i);
    }
    source += "    HALT\n";
    return source;
}

/// A program that runs for a long time in few instructions, for the VM.
std::string makeLoop(int iterations) {
    return std::format(R"(
.global _start
_start:
    MOVI R1, {}
    MOVI R2, 0
    MOVI R3, 1
loop:
    CMP  R1, R2
    JLE  done
    ADD  R4, R1
    SUB  R1, R3
    JMP  loop
done:
    HALT
)",
                       iterations);
}

void printEnvironment() {
    std::printf("MiniToolchain benchmark suite\n");
    std::printf("=============================\n\n");
#if defined(_MSC_VER)
    std::printf("  compiler   : MSVC %d\n", _MSC_VER);
#elif defined(__clang__)
    std::printf("  compiler   : clang %d.%d.%d\n", __clang_major__, __clang_minor__,
                __clang_patchlevel__);
#elif defined(__GNUC__)
    std::printf("  compiler   : gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    std::printf("  compiler   : unknown\n");
#endif
#if defined(NDEBUG)
    std::printf("  build      : release (NDEBUG)\n");
#else
    std::printf("  build      : debug (these numbers are not comparable to a release build)\n");
#endif
#if defined(_WIN32)
    std::printf("  platform   : windows-x64\n");
#elif defined(__linux__)
    std::printf("  platform   : linux\n");
#elif defined(__APPLE__)
    std::printf("  platform   : macos\n");
#endif
    const char* processor = std::getenv("PROCESSOR_IDENTIFIER");
    std::printf("  cpu        : %s\n", processor != nullptr ? processor : "(unknown)");
    std::printf("\n  Record these alongside any number you quote.\n\n");
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = 50;
    if (argc > 1) {
        iterations = std::max(3, std::atoi(argv[1]));
    }

    printEnvironment();

    const std::string source = makeProgram(4000);
    const auto source_bytes = static_cast<double>(source.size());

    // Prepare the inputs each stage needs, outside the timed regions.
    SourceManager sources;
    const FileId file = sources.addFile("bench.asm", source);
    diag::DiagnosticEngine diagnostics(sources);
    const std::expected<AssembleResult, std::string> assembled =
        assembleSource(sources, file, diagnostics);
    if (!assembled.has_value()) {
        std::fprintf(stderr, "benchmark program failed to assemble: %s\n",
                     assembled.error().c_str());
        std::fputs(diagnostics.renderAll().c_str(), stderr);
        return 1;
    }
    const auto instruction_count = static_cast<double>(assembled->stats.instructions_before);

    std::vector<u8> object_bytes;
    static_cast<void>(object::writeObjectToBuffer(assembled->object, object_bytes));
    const std::vector<object::ObjectFile> objects{assembled->object};

    SourceManager empty_sources;
    diag::DiagnosticEngine link_diagnostics(empty_sources);
    linker::Linker linker(link_diagnostics);
    const std::expected<executable::Executable, std::string> executable = linker.link(objects);
    if (!executable.has_value()) {
        std::fprintf(stderr, "benchmark program failed to link: %s\n", executable.error().c_str());
        return 1;
    }
    std::vector<u8> exe_bytes;
    static_cast<void>(executable::writeExecutableToBuffer(*executable, exe_bytes));

    std::printf("%-26s %9s %9s %9s %9s  %12s\n", "benchmark", "mean us", "p50 us", "p95 us",
                "p99 us", "throughput");
    std::printf("%s\n", std::string(88, '-').c_str());

    // --- front end ----------------------------------------------------------
    report(measure("lexer", source_bytes, "bytes", iterations, [&] {
        lexer::Lexer lexer(source, 0);
        while (lexer.next().type != lexer::TokenType::Eof) {
        }
    }));

    report(measure("parser", source_bytes, "bytes", iterations, [&] {
        diag::DiagnosticEngine engine(sources);
        lexer::Lexer lexer(sources.text(file), file);
        parser::Parser parser(lexer, engine);
        static_cast<void>(parser.parse());
    }));

    report(measure("assemble (-O0)", instruction_count, "instructions", iterations, [&] {
        diag::DiagnosticEngine engine(sources);
        static_cast<void>(assembleSource(sources, file, engine));
    }));

    report(measure("assemble (-O1)", instruction_count, "instructions", iterations, [&] {
        diag::DiagnosticEngine engine(sources);
        AssembleOptions options;
        options.opt_level = optimizer::OptLevel::O1;
        static_cast<void>(assembleSource(sources, file, engine, options));
    }));

    // --- binary formats -----------------------------------------------------
    report(
        measure("object write", static_cast<double>(object_bytes.size()), "bytes", iterations, [&] {
            std::vector<u8> bytes;
            static_cast<void>(object::writeObjectToBuffer(assembled->object, bytes));
        }));

    report(measure("object read", static_cast<double>(object_bytes.size()), "bytes", iterations,
                   [&] { static_cast<void>(object::readObjectFromBuffer(object_bytes)); }));

    report(measure("executable read", static_cast<double>(exe_bytes.size()), "bytes", iterations,
                   [&] { static_cast<void>(executable::readExecutableFromBuffer(exe_bytes)); }));

    // --- linker -------------------------------------------------------------
    report(measure("link", instruction_count, "instructions", iterations, [&] {
        SourceManager local_sources;
        diag::DiagnosticEngine engine(local_sources);
        linker::Linker local_linker(engine);
        static_cast<void>(local_linker.link(objects));
    }));

    // --- runtime ------------------------------------------------------------
    const std::string loop_source = makeLoop(20000);
    SourceManager loop_sources;
    const FileId loop_file = loop_sources.addFile("loop.asm", loop_source);
    diag::DiagnosticEngine loop_diagnostics(loop_sources);
    const std::expected<AssembleResult, std::string> loop_assembled =
        assembleSource(loop_sources, loop_file, loop_diagnostics);
    const std::vector<object::ObjectFile> loop_objects{loop_assembled->object};
    SourceManager loop_link_sources;
    diag::DiagnosticEngine loop_link_diagnostics(loop_link_sources);
    linker::Linker loop_linker(loop_link_diagnostics);
    const std::expected<executable::Executable, std::string> loop_executable =
        loop_linker.link(loop_objects);

    // 20000 iterations of a five-instruction loop, plus the prologue.
    constexpr double kLoopInstructions = 20000.0 * 5 + 5;
    report(measure("vm execution", kLoopInstructions, "instructions", iterations, [&] {
        vm::VirtualMachine machine;
        static_cast<void>(machine.load(*loop_executable));
        static_cast<void>(machine.run());
    }));

    report(measure(
        "vm with tracing", kLoopInstructions, "instructions", std::max(3, iterations / 5), [&] {
            vm::VirtualMachine machine;
            u64 counter = 0;
            machine.setTraceSink([&counter](u64, const isa::Instruction&) { ++counter; });
            static_cast<void>(machine.load(*loop_executable));
            static_cast<void>(machine.run());
        }));

    // --- disassembler -------------------------------------------------------
    report(measure("disassemble", instruction_count, "instructions", iterations, [&] {
        static_cast<void>(disassembler::Disassembler{}.disassemble(*executable));
    }));

    // --- the optimizer's effect --------------------------------------------
    std::printf("\noptimizer effect on the benchmark program\n");
    const auto runInstructions = [&](optimizer::OptLevel level) -> u64 {
        SourceManager local_sources;
        const FileId local_file = local_sources.addFile("bench.asm", source);
        diag::DiagnosticEngine engine(local_sources);
        AssembleOptions options;
        options.opt_level = level;
        const std::expected<AssembleResult, std::string> result =
            assembleSource(local_sources, local_file, engine, options);
        const std::vector<object::ObjectFile> local_objects{result->object};
        SourceManager link_sources;
        diag::DiagnosticEngine link_engine(link_sources);
        linker::Linker local_linker(link_engine);
        const std::expected<executable::Executable, std::string> local_executable =
            local_linker.link(local_objects);
        vm::VirtualMachine machine;
        static_cast<void>(machine.load(*local_executable));
        return machine.run().instructions;
    };
    const u64 unoptimized = runInstructions(optimizer::OptLevel::O0);
    const u64 optimized = runInstructions(optimizer::OptLevel::O1);
    std::printf(
        "  instructions executed  -O0 %llu, -O1 %llu (%.1f%% fewer)\n",
        static_cast<unsigned long long>(unoptimized), static_cast<unsigned long long>(optimized),
        unoptimized == 0
            ? 0.0
            : 100.0 * (1.0 - static_cast<double>(optimized) / static_cast<double>(unoptimized)));
    std::printf("  image size             -O0 %zu bytes\n", exe_bytes.size());
    return 0;
}
