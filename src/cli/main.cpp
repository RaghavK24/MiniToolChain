// SPDX-License-Identifier: MIT
//
// `minitool` is the single driver for the whole toolchain. Every subcommand is
// a thin shell over the libraries: it parses arguments, calls in, and renders
// the result. No compilation, linking or execution logic lives here.

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "minitool/assembler/pipeline.hpp"
#include "minitool/common/print.hpp"
#include "minitool/common/source_manager.hpp"
#include "minitool/debugger/debugger.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/disassembler/disassembler.hpp"
#include "minitool/executable/executable_io.hpp"
#include "minitool/isa/encoding.hpp"
#include "minitool/isa/isa.hpp"
#include "minitool/linker/linker.hpp"
#include "minitool/object/object_io.hpp"
#include "minitool/playground/http_server.hpp"
#include "minitool/vm/vm.hpp"

namespace {

using minitool::u64;
using minitool::io::printLine;

constexpr std::string_view kVersion = "1.0.0";

/// Exit codes the driver itself uses. A program's own exit code is passed
/// through by `run`, so these stay out of the way at the top of the range.
constexpr int kUsageError = 2;
constexpr int kToolError = 1;

int usage() {
    printLine("minitool {} - a complete toolchain for the MiniToolchain 64-bit virtual ISA",
              kVersion);
    printLine();
    printLine("usage: minitool <command> [options]");
    printLine();
    printLine("  build <src...> -o <exe>       assemble and link in one step");
    printLine("  assemble <src> -o <obj>       assemble one source file");
    printLine("  link <obj...> -o <exe>        link object files");
    printLine("  run <exe>                     execute a program");
    printLine("  disassemble <exe>             print the program as assembly");
    printLine("  debug <exe>                   start the interactive debugger");
    printLine("  objdump <obj>                 describe an object file");
    printLine("  verify <exe>                  validate an executable image");
    printLine("  isa                           print the instruction table");
    printLine("  decode <hex-word>             decode one 64-bit instruction word");
    printLine("  bench [iterations]            run the built-in benchmarks");
    printLine("  serve                         open the playground UI in a browser");
    printLine("  version                       print the version");
    printLine();
    printLine("options:");
    printLine("  -o <path>        output file");
    printLine("  -O0 | -O1        optimization level (default -O0)");
    printLine("  -g | -gno        emit or omit debug line information (default -g)");
    printLine("  --entry <name>   entry symbol for link/build (default _start)");
    printLine("  --trace          print every instruction as it executes");
    printLine("  --max-instructions <n>   stop a run after n instructions");
    printLine("  --stats          report what the optimizer did");
    printLine("  -x <command>     run a debugger command before going interactive");
    printLine("  --port <n>       port for `serve` (default 8080)");
    printLine("  --host <addr>    bind address for `serve` (default 127.0.0.1)");
    return kUsageError;
}

/// Options gathered from the command line. Unknown flags are an error rather
/// than being ignored, so a typo never silently changes what a build does.
struct Arguments {
    std::vector<std::string> inputs;
    std::string output;
    std::string entry = "_start";
    std::vector<std::string> commands;
    minitool::optimizer::OptLevel opt_level = minitool::optimizer::OptLevel::O0;
    bool debug_info = true;
    bool trace = false;
    bool stats = false;
    bool show_bytes = true;
    u64 max_instructions = minitool::vm::VirtualMachine::kDefaultBudget;
    u64 port = 8080;
    std::string host = "127.0.0.1";
    std::string error;
};

[[nodiscard]] bool parseU64(std::string_view text, u64& out) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), out, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

Arguments parseArguments(std::span<const std::string_view> args) {
    Arguments parsed;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= args.size()) {
                parsed.error = std::format("{} needs a value", name);
                return {};
            }
            return args[++i];
        };
        if (arg == "-o") {
            parsed.output = std::string{next("-o")};
        } else if (arg == "--entry") {
            parsed.entry = std::string{next("--entry")};
        } else if (arg == "-x") {
            parsed.commands.emplace_back(next("-x"));
        } else if (arg == "-O0") {
            parsed.opt_level = minitool::optimizer::OptLevel::O0;
        } else if (arg == "-O1" || arg == "-O") {
            parsed.opt_level = minitool::optimizer::OptLevel::O1;
        } else if (arg == "-g") {
            parsed.debug_info = true;
        } else if (arg == "-gno" || arg == "--no-debug") {
            parsed.debug_info = false;
        } else if (arg == "--trace") {
            parsed.trace = true;
        } else if (arg == "--stats") {
            parsed.stats = true;
        } else if (arg == "--no-bytes") {
            parsed.show_bytes = false;
        } else if (arg == "--port") {
            if (!parseU64(next("--port"), parsed.port) || parsed.port == 0 || parsed.port > 65535) {
                parsed.error = "--port needs a number between 1 and 65535";
            }
        } else if (arg == "--host") {
            parsed.host = std::string{next("--host")};
        } else if (arg == "--max-instructions") {
            if (!parseU64(next("--max-instructions"), parsed.max_instructions)) {
                parsed.error = "--max-instructions needs a number";
            }
        } else if (arg.starts_with('-') && arg.size() > 1) {
            parsed.error = std::format("unknown option '{}'", arg);
        } else {
            parsed.inputs.emplace_back(arg);
        }
    }
    return parsed;
}

/// Assembles one file, printing any diagnostics. Returns nullopt on failure.
std::optional<minitool::AssembleResult> assembleFile(const std::filesystem::path& path,
                                                     const Arguments& arguments) {
    const std::expected<std::vector<minitool::u8>, std::string> bytes =
        minitool::object::readFileBytes(path);
    if (!bytes.has_value()) {
        printLine(stderr, "error: {}", bytes.error());
        return std::nullopt;
    }
    minitool::SourceManager sources;
    const minitool::FileId file =
        sources.addFile(path.generic_string(),
                        std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));

    minitool::diag::DiagnosticEngine diagnostics(sources);
    minitool::AssembleOptions options;
    options.opt_level = arguments.opt_level;
    options.emit_debug_info = arguments.debug_info;

    std::expected<minitool::AssembleResult, std::string> result =
        minitool::assembleSource(sources, file, diagnostics, options);
    const std::string rendered = diagnostics.renderAll();
    if (!rendered.empty()) {
        std::fputs(rendered.c_str(), stderr);
    }
    if (!result.has_value()) {
        printLine(stderr, "error: {}: {}", path.string(), result.error());
        return std::nullopt;
    }
    if (arguments.stats) {
        printLine(stderr, "{}: {} {}", path.string(),
                  minitool::optimizer::optLevelName(arguments.opt_level), result->stats.summary());
    }
    return std::move(*result);
}

int cmdIsa() {
    printLine("{:<6} {:<9} {:<8} {}", "op", "mnemonic", "format", "notes");
    for (const minitool::isa::OpcodeInfo& info : minitool::isa::allOpcodes()) {
        std::string notes;
        if (info.writes_flags) {
            notes += "flags ";
        }
        if (info.is_branch) {
            notes += "branch ";
        }
        if (info.is_terminator) {
            notes += "terminator ";
        }
        printLine("0x{:02X}   {:<9} {:<8} {}", static_cast<unsigned>(info.opcode), info.mnemonic,
                  minitool::isa::formatName(info.format), notes);
    }
    printLine();
    printLine("{} instructions, {} bytes each, little-endian", minitool::isa::allOpcodes().size(),
              minitool::isa::kInstructionSize);
    return 0;
}

int cmdDecode(std::string_view text) {
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
    }
    u64 word = 0;
    const std::from_chars_result result =
        std::from_chars(digits.data(), digits.data() + digits.size(), word, 16);
    if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        printLine(stderr, "error: '{}' is not a 64-bit hexadecimal word", text);
        return kToolError;
    }
    const std::expected<minitool::isa::Instruction, minitool::isa::DecodeError> decoded =
        minitool::isa::decode(word);
    if (!decoded.has_value()) {
        printLine(stderr, "error: {}", minitool::isa::decodeErrorName(decoded.error()));
        return kToolError;
    }
    printLine("{:016X}  {}", word, minitool::isa::toString(*decoded));
    return 0;
}

int cmdAssemble(const Arguments& arguments) {
    if (arguments.inputs.size() != 1 || arguments.output.empty()) {
        printLine(stderr, "usage: minitool assemble <source.asm> -o <output.mobj>");
        return kUsageError;
    }
    const std::optional<minitool::AssembleResult> result =
        assembleFile(arguments.inputs.front(), arguments);
    if (!result.has_value()) {
        return kToolError;
    }
    const std::expected<void, std::string> written =
        minitool::object::writeObject(result->object, arguments.output);
    if (!written.has_value()) {
        printLine(stderr, "error: {}", written.error());
        return kToolError;
    }
    printLine("assembled {} -> {}", arguments.inputs.front(), arguments.output);
    return 0;
}

int linkObjects(std::span<const minitool::object::ObjectFile> objects, const Arguments& arguments) {
    minitool::SourceManager sources;
    minitool::diag::DiagnosticEngine diagnostics(sources);
    minitool::linker::Linker linker(diagnostics);
    minitool::linker::LinkOptions options;
    options.entry = arguments.entry;

    const std::expected<minitool::executable::Executable, std::string> executable =
        linker.link(objects, options);
    if (!executable.has_value()) {
        printLine(stderr, "error: link failed: {}", executable.error());
        return kToolError;
    }
    const std::expected<void, std::string> written =
        minitool::executable::writeExecutable(*executable, arguments.output);
    if (!written.has_value()) {
        printLine(stderr, "error: {}", written.error());
        return kToolError;
    }
    u64 image = 0;
    for (const minitool::executable::Segment& segment : executable->segments) {
        image += segment.virtual_size;
    }
    printLine("linked {} object(s) -> {} (entry 0x{:X}, {} bytes of image)", objects.size(),
              arguments.output, executable->entry_point, image);
    return 0;
}

int cmdLink(const Arguments& arguments) {
    if (arguments.inputs.empty() || arguments.output.empty()) {
        printLine(stderr, "usage: minitool link <file.mobj...> -o <program.mexe>");
        return kUsageError;
    }
    std::vector<minitool::object::ObjectFile> objects;
    for (const std::string& input : arguments.inputs) {
        std::expected<minitool::object::ObjectFile, std::string> object =
            minitool::object::readObject(input);
        if (!object.has_value()) {
            printLine(stderr, "error: {}", object.error());
            return kToolError;
        }
        objects.push_back(std::move(*object));
    }
    return linkObjects(objects, arguments);
}

int cmdBuild(const Arguments& arguments) {
    if (arguments.inputs.empty() || arguments.output.empty()) {
        printLine(stderr, "usage: minitool build <source.asm...> -o <program.mexe>");
        return kUsageError;
    }
    std::vector<minitool::object::ObjectFile> objects;
    for (const std::string& input : arguments.inputs) {
        std::optional<minitool::AssembleResult> result = assembleFile(input, arguments);
        if (!result.has_value()) {
            return kToolError;
        }
        objects.push_back(std::move(result->object));
    }
    return linkObjects(objects, arguments);
}

int cmdRun(const Arguments& arguments) {
    if (arguments.inputs.size() != 1) {
        printLine(stderr, "usage: minitool run <program.mexe> [--trace]");
        return kUsageError;
    }
    const std::expected<minitool::executable::Executable, std::string> executable =
        minitool::executable::readExecutable(arguments.inputs.front());
    if (!executable.has_value()) {
        printLine(stderr, "error: {}", executable.error());
        return kToolError;
    }

    minitool::vm::VirtualMachine machine;
    if (arguments.trace) {
        machine.setTraceSink([](u64 pc, const minitool::isa::Instruction& instruction) {
            printLine(stderr, "PC=0x{:016X}  {}", pc, minitool::isa::toString(instruction));
        });
    }
    const std::expected<void, std::string> loaded = machine.load(*executable);
    if (!loaded.has_value()) {
        printLine(stderr, "error: cannot load: {}", loaded.error());
        return kToolError;
    }

    const minitool::vm::RunResult result = machine.run(arguments.max_instructions);
    if (!result.ok()) {
        printLine(stderr, "runtime error: {}", minitool::vm::vmErrorName(result.error));
        printLine(stderr, "  {}", result.message);
        printLine(stderr, "  PC = 0x{:016X}, after {} instructions", result.pc,
                  result.instructions);
        return kToolError;
    }
    if (arguments.stats) {
        printLine(stderr, "{} instructions executed", result.instructions);
    }
    // A program's own exit code is what the driver returns, truncated to the
    // byte a shell can actually observe.
    return static_cast<int>(result.exit_code & 0xFFU);
}

int cmdDisassemble(const Arguments& arguments) {
    if (arguments.inputs.size() != 1) {
        printLine(stderr, "usage: minitool disassemble <program.mexe>");
        return kUsageError;
    }
    const std::expected<minitool::executable::Executable, std::string> executable =
        minitool::executable::readExecutable(arguments.inputs.front());
    if (!executable.has_value()) {
        printLine(stderr, "error: {}", executable.error());
        return kToolError;
    }
    minitool::disassembler::Options options;
    options.show_bytes = arguments.show_bytes;
    const minitool::disassembler::Disassembler disassembler(options);
    std::fputs(disassembler.disassemble(*executable).c_str(), stdout);
    return 0;
}

int cmdDebug(const Arguments& arguments) {
    if (arguments.inputs.size() != 1) {
        printLine(stderr, "usage: minitool debug <program.mexe>");
        return kUsageError;
    }
    const std::expected<minitool::executable::Executable, std::string> executable =
        minitool::executable::readExecutable(arguments.inputs.front());
    if (!executable.has_value()) {
        printLine(stderr, "error: {}", executable.error());
        return kToolError;
    }
    minitool::vm::VirtualMachine machine;
    const std::expected<void, std::string> loaded = machine.load(*executable);
    if (!loaded.has_value()) {
        printLine(stderr, "error: cannot load: {}", loaded.error());
        return kToolError;
    }
    minitool::debugger::Debugger debugger(machine, *executable);
    for (const std::string& command : arguments.commands) {
        if (!debugger.executeCommand(command, std::cout)) {
            return 0;
        }
    }
    debugger.interactiveLoop(std::cin, std::cout);
    return 0;
}

int cmdObjdump(const Arguments& arguments) {
    if (arguments.inputs.size() != 1) {
        printLine(stderr, "usage: minitool objdump <file.mobj>");
        return kUsageError;
    }
    const std::expected<minitool::object::ObjectFile, std::string> object =
        minitool::object::readObject(arguments.inputs.front());
    if (!object.has_value()) {
        printLine(stderr, "error: {}", object.error());
        return kToolError;
    }

    printLine("{}: MiniToolchain object, version {}", arguments.inputs.front(), object->version);
    printLine();
    printLine("sections:");
    for (const minitool::object::Section& section : object->sections) {
        printLine("  [{}] {:<10} {:<7} size {:<8} align {:<4} {} bytes of data", section.index,
                  section.name, minitool::object::sectionTypeName(section.type), section.size,
                  section.alignment, section.data.size());
    }
    printLine();
    printLine("symbols:");
    for (minitool::u32 i = 0; i < object->symbols.size(); ++i) {
        const minitool::Symbol& symbol = object->symbols.at(i);
        const char* binding = "local";
        switch (symbol.binding) {
            case minitool::SymbolBinding::Global:
                binding = "global";
                break;
            case minitool::SymbolBinding::Weak:
                binding = "weak";
                break;
            case minitool::SymbolBinding::Extern:
                binding = "extern";
                break;
            case minitool::SymbolBinding::Local:
                break;
        }
        if (symbol.defined) {
            printLine("  [{}] {:<20} {:<7} section {} + 0x{:X}", i, symbol.name, binding,
                      symbol.section, symbol.value);
        } else {
            printLine("  [{}] {:<20} {:<7} undefined", i, symbol.name, binding);
        }
    }
    printLine();
    printLine("relocations:");
    for (const minitool::object::Relocation& relocation : object->relocations) {
        printLine("  {:<8} section {} + 0x{:<6X} -> {} {:+}",
                  minitool::object::relocationTypeName(relocation.type), relocation.section,
                  relocation.offset, object->symbols.at(relocation.symbol).name, relocation.addend);
    }
    if (!object->debug_info.empty()) {
        printLine();
        printLine("debug line table: {} entries over {} file(s)", object->debug_info.size(),
                  object->source_files.size());
    }

    // Disassemble the text section: an object dump that cannot show you the
    // code is only half a tool.
    for (const minitool::object::Section& section : object->sections) {
        if (section.type != minitool::object::SectionType::Text || section.data.empty()) {
            continue;
        }
        printLine();
        printLine("disassembly of {} (offsets, not addresses -- this file is not linked yet):",
                  section.name);
        const minitool::disassembler::Disassembler disassembler;
        std::fputs(disassembler.disassemble(section.data, 0).c_str(), stdout);
    }
    return 0;
}

int cmdVerify(const Arguments& arguments) {
    if (arguments.inputs.size() != 1) {
        printLine(stderr, "usage: minitool verify <program.mexe>");
        return kUsageError;
    }
    // readExecutable already validates structure, checksum and image; a
    // successful read *is* a successful verification.
    const std::expected<minitool::executable::Executable, std::string> executable =
        minitool::executable::readExecutable(arguments.inputs.front());
    if (!executable.has_value()) {
        printLine(stderr, "invalid: {}", executable.error());
        return kToolError;
    }
    printLine("{}: valid MiniToolchain executable", arguments.inputs.front());
    printLine("  entry point : 0x{:016X}", executable->entry_point);
    printLine("  segments    : {}", executable->segments.size());
    for (const minitool::executable::Segment& segment : executable->segments) {
        printLine("    {:<8} 0x{:016X} .. 0x{:016X}  {}  {} bytes on disk", segment.name,
                  segment.virtual_address, segment.virtual_address + segment.virtual_size,
                  minitool::executable::flagsToString(segment.flags), segment.data.size());
    }
    printLine("  symbols     : {}", executable->symbols.size());
    printLine("  debug lines : {}", executable->debug_info.size());
    return 0;
}

/// A quick end-to-end timing of the pipeline. The full benchmark suite lives in
/// benchmarks/; this is the version that is always available.
int cmdBench(const Arguments& arguments) {
    u64 iterations = 200;
    if (!arguments.inputs.empty() && !parseU64(arguments.inputs.front(), iterations)) {
        printLine(stderr, "usage: minitool bench [iterations]");
        return kUsageError;
    }

    std::string source = ".section .text\n.global _start\n_start:\n";
    for (int i = 0; i < 200; ++i) {
        source += "    MOVI R1, 7\n    MOVI R2, 5\n    ADD R1, R2\n    CMP R1, R2\n";
    }
    source += "    HALT\n";

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::size_t instructions = 0;
    for (u64 i = 0; i < iterations; ++i) {
        minitool::SourceManager sources;
        const minitool::FileId file = sources.addFile("bench.asm", source);
        minitool::diag::DiagnosticEngine diagnostics(sources);
        const std::expected<minitool::AssembleResult, std::string> result =
            minitool::assembleSource(sources, file, diagnostics);
        if (!result.has_value()) {
            printLine(stderr, "error: benchmark source failed to assemble");
            return kToolError;
        }
        instructions += result->stats.instructions_before;
    }
    const auto elapsed = Clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    printLine("assembled {} instructions in {:.3f} s ({:.0f} instructions/s)", instructions,
              seconds, seconds > 0 ? static_cast<double>(instructions) / seconds : 0.0);
    printLine("(see benchmarks/ for the full suite)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    const std::string_view command = args[0];
    const Arguments arguments = parseArguments(std::span{args}.subspan(1));
    if (!arguments.error.empty()) {
        printLine(stderr, "error: {}", arguments.error);
        return kUsageError;
    }

    if (command == "version") {
        printLine("{}", kVersion);
        return 0;
    }
    if (command == "isa") {
        return cmdIsa();
    }
    if (command == "decode") {
        if (arguments.inputs.size() != 1) {
            return usage();
        }
        return cmdDecode(arguments.inputs.front());
    }
    if (command == "assemble") {
        return cmdAssemble(arguments);
    }
    if (command == "link") {
        return cmdLink(arguments);
    }
    if (command == "build") {
        return cmdBuild(arguments);
    }
    if (command == "run") {
        return cmdRun(arguments);
    }
    if (command == "disassemble" || command == "dis") {
        return cmdDisassemble(arguments);
    }
    if (command == "debug") {
        return cmdDebug(arguments);
    }
    if (command == "objdump") {
        return cmdObjdump(arguments);
    }
    if (command == "verify") {
        return cmdVerify(arguments);
    }
    if (command == "bench") {
        return cmdBench(arguments);
    }
    if (command == "serve") {
        minitool::playground::ServeOptions options;
        options.port = static_cast<minitool::u16>(arguments.port);
        options.host = arguments.host;
        return minitool::playground::serve(options);
    }
    if (command == "help" || command == "--help" || command == "-h") {
        static_cast<void>(usage());
        return 0;
    }
    printLine(stderr, "error: unknown command '{}'", command);
    return usage();
}
