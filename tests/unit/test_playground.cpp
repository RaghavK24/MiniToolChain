// SPDX-License-Identifier: MIT
//
// The playground is the only place where untrusted text arrives over a socket
// and gets compiled and executed, so these tests care less about the happy path
// than about what happens when the input is wrong, enormous, endless, or not
// even text.

#include <string>
#include <string_view>

#include "minitool/playground/http_server.hpp"
#include "minitool/playground/session.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace minitool;
using playground::RunReport;
using playground::RunRequest;
using playground::Stage;

RunReport runText(std::string source, std::string input = {},
                  optimizer::OptLevel level = optimizer::OptLevel::O0) {
    RunRequest request;
    request.source = std::move(source);
    request.input = std::move(input);
    request.opt_level = level;
    return playground::runSource(request);
}

constexpr std::string_view kHello = R"(
.section .data
message:
    .asciz "Hello, World!\n"
.section .text
.global _start
_start:
    MOVI R1, 1
    LEA  R2, message
    MOVI R3, 14
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";

// ------------------------------------------------------------- session --

TEST(Playground, RunsAProgramAndCapturesOutput) {
    const RunReport report = runText(std::string{kHello});
    ASSERT_TRUE(report.ok) << report.error << report.diagnostics;
    EXPECT_EQ(report.stage, Stage::Finished);
    EXPECT_EQ(report.output, "Hello, World!\n");
    EXPECT_EQ(report.exit_code, 0U);
    EXPECT_EQ(report.instructions, 6U);
    EXPECT_FALSE(report.output_truncated);
    EXPECT_FALSE(report.disassembly.empty());
}

TEST(Playground, ReportsAnAssemblyErrorWithACaret) {
    const RunReport report = runText(".section .text\n.global _start\n_start:\n    MOV R1, 7\n");
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.stage, Stage::Assemble);
    // The user needs the line, the column and the caret, not just a refusal.
    EXPECT_NE(report.diagnostics.find("playground.asm:4:13"), std::string::npos)
        << report.diagnostics;
    EXPECT_NE(report.diagnostics.find("^"), std::string::npos) << report.diagnostics;
    EXPECT_TRUE(report.disassembly.empty());
}

TEST(Playground, ReportsALinkErrorWithoutRepeatingItself) {
    const RunReport report = runText(".section .text\nfoo:\n    HALT\n");
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.stage, Stage::Link);
    EXPECT_NE(report.diagnostics.find("entry point '_start'"), std::string::npos);
    // The linker reports through the diagnostic engine and returns the reason;
    // the report must not show the same sentence twice.
    EXPECT_TRUE(report.error.empty()) << report.error;
}

TEST(Playground, ReportsARuntimeTrap) {
    const RunReport report = runText(
        ".section .text\n.global _start\n_start:\n    MOVI R1, 1\n    MOVI R2, 0\n"
        "    DIV R1, R2\n    HALT\n");
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.stage, Stage::Execute);
    EXPECT_NE(report.error.find("zero"), std::string::npos) << report.error;
    // It got far enough to run, so the machine state is worth showing.
    EXPECT_EQ(report.instructions, 3U);
}

TEST(Playground, StopsAnEndlessProgramAtTheBudget) {
    RunRequest request;
    request.source = ".section .text\n.global _start\n_start:\n    JMP _start\n";
    request.budget = 5000;
    const RunReport report = playground::runSource(request);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.stage, Stage::Execute);
    EXPECT_EQ(report.instructions, 5000U);
}

TEST(Playground, ClampsAnOutrageousBudget) {
    RunRequest request;
    request.source = ".section .text\n.global _start\n_start:\n    JMP _start\n";
    request.budget = playground::kMaxPlaygroundBudget * 100;
    const RunReport report = playground::runSource(request);
    EXPECT_EQ(report.instructions, playground::kMaxPlaygroundBudget);
}

TEST(Playground, FeedsStdinToTheReadSyscall) {
    constexpr std::string_view kEcho = R"(
.section .bss
buffer:
    .space 16
.section .text
.global _start
_start:
    MOVI R1, 0
    LEA  R2, buffer
    MOVI R3, 5
    SYSCALL 2
    MOVI R1, 1
    LEA  R2, buffer
    MOV  R3, R14
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";
    const RunReport report = runText(std::string{kEcho}, "abcde");
    ASSERT_TRUE(report.ok) << report.error << report.diagnostics;
    EXPECT_EQ(report.output, "abcde");
}

TEST(Playground, ReportsWhatTheOptimizerDidAtO1) {
    constexpr std::string_view kFoldable = R"(
.section .text
.global _start
_start:
    MOVI R1, 2
    MOVI R2, 3
    ADD  R1, R2
    HALT
)";
    const RunReport o0 = runText(std::string{kFoldable}, {}, optimizer::OptLevel::O0);
    const RunReport o1 = runText(std::string{kFoldable}, {}, optimizer::OptLevel::O1);
    ASSERT_TRUE(o0.ok);
    ASSERT_TRUE(o1.ok);
    EXPECT_EQ(o0.stats.total(), 0U);
    EXPECT_GT(o1.stats.total(), 0U);
    // Whatever it did, the program still has to compute the same answer.
    EXPECT_EQ(o0.registers[1], o1.registers[1]);
}

TEST(Playground, RejectsSourceLargerThanTheLimit) {
    RunRequest request;
    request.source = std::string(playground::kMaxSourceBytes + 1, ' ');
    const RunReport report = playground::runSource(request);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.stage, Stage::Assemble);
    EXPECT_NE(report.error.find("limit"), std::string::npos) << report.error;
}

TEST(Playground, TruncatesRunawayOutputInsteadOfGrowingForever) {
    // Writes 32 bytes at a time, far past the output cap.
    constexpr std::string_view kFlood = R"(
.section .data
chunk:
    .asciz "0123456789abcdef0123456789abcde\n"
.section .text
.global _start
_start:
    MOVI R4, 20000
loop:
    MOVI R1, 1
    LEA  R2, chunk
    MOVI R3, 32
    SYSCALL 1
    DEC  R4
    CMP  R4, R0
    JG   loop
    MOVI R1, 0
    SYSCALL 0
)";
    const RunReport report = runText(std::string{kFlood});
    EXPECT_TRUE(report.output_truncated);
    EXPECT_LE(report.output.size(), playground::kMaxOutputBytes);
}

// ---------------------------------------------------------------- HTTP --

using playground::handleRequest;
using playground::HttpResponse;

TEST(PlaygroundHttp, ServesThePage) {
    const HttpResponse response = handleRequest("GET", "/", "");
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "text/html; charset=utf-8");
    EXPECT_NE(response.body.find("<!doctype html>"), std::string::npos);
    EXPECT_NE(response.body.find("MiniToolchain"), std::string::npos);
}

TEST(PlaygroundHttp, ThePageIsWholeAndCarriesEveryExample) {
    // The page is assembled from several string literals, because MSVC caps a
    // single one at 16380 bytes. A dropped or mis-ordered chunk would truncate
    // the page silently -- it would still be served, still be valid-looking
    // HTML at the top, and simply stop working. These assertions are what
    // notice that.
    const std::string page = handleRequest("GET", "/", "").body;
    EXPECT_TRUE(page.starts_with("<!doctype html>"));
    EXPECT_NE(page.find("</html>"), std::string::npos) << "page is truncated";
    EXPECT_NE(page.find("</script>"), std::string::npos);

    // Every entry the dropdown offers must actually be present.
    for (const std::string_view name :
         {"Hello, World!", "Arithmetic", "Bitwise and shifts", "Conditionals and jumps",
          "The stack", "Factorial (loops)", "Fibonacci (recursion)", "Arrays in memory",
          "Print a number", "Echo (reads stdin)", "Heap allocation", "Optimizer (try -O0 vs -O1)",
          "Error: bad operand", "Error: divide by zero", "Error: read-only memory"}) {
        EXPECT_NE(page.find(name), std::string::npos) << "missing example: " << name;
    }
}

TEST(PlaygroundHttp, RejectsTheWrongMethod) {
    EXPECT_EQ(handleRequest("POST", "/", "").status, 405);
    EXPECT_EQ(handleRequest("GET", "/api/run", "").status, 405);
}

TEST(PlaygroundHttp, AnUnknownPathIsNotFound) {
    EXPECT_EQ(handleRequest("GET", "/../secrets", "").status, 404);
    EXPECT_EQ(handleRequest("GET", "/api/other", "").status, 404);
}

TEST(PlaygroundHttp, RunsTheSourceInTheRequestBody) {
    const HttpResponse response = handleRequest("POST", "/api/run", kHello);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
    EXPECT_NE(response.body.find("\"ok\":true"), std::string::npos) << response.body;
    EXPECT_NE(response.body.find("Hello, World!"), std::string::npos);
    EXPECT_NE(response.body.find("\"stage\":\"finished\""), std::string::npos);
}

TEST(PlaygroundHttp, RefusesASourceOverTheLimit) {
    const std::string huge(playground::kMaxSourceBytes + 1, ' ');
    EXPECT_EQ(handleRequest("POST", "/api/run", huge).status, 413);
}

TEST(PlaygroundHttp, ReadsTheOptimizationLevelFromTheQuery) {
    constexpr std::string_view kFoldable =
        ".section .text\n.global _start\n_start:\n    MOVI R1, 2\n    MOVI R2, 3\n"
        "    ADD R1, R2\n    HALT\n";
    const HttpResponse o0 = handleRequest("POST", "/api/run?opt=0", kFoldable);
    const HttpResponse o1 = handleRequest("POST", "/api/run?opt=1", kFoldable);
    EXPECT_NE(o0.body.find("\"total\":0"), std::string::npos) << o0.body;
    EXPECT_EQ(o1.body.find("\"total\":0"), std::string::npos) << o1.body;
}

TEST(PlaygroundHttp, DecodesStdinFromTheQueryString) {
    constexpr std::string_view kEcho = R"(
.section .bss
buffer:
    .space 16
.section .text
.global _start
_start:
    MOVI R1, 0
    LEA  R2, buffer
    MOVI R3, 3
    SYSCALL 2
    MOVI R1, 1
    LEA  R2, buffer
    MOV  R3, R14
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";
    // "a b" percent-encoded, exercising both %XX and the literal '+'.
    const HttpResponse response = handleRequest("POST", "/api/run?stdin=a%20b", kEcho);
    EXPECT_NE(response.body.find("\"output\":\"a b\""), std::string::npos) << response.body;

    const HttpResponse plus = handleRequest("POST", "/api/run?stdin=a+b", kEcho);
    EXPECT_NE(plus.body.find("\"output\":\"a b\""), std::string::npos) << plus.body;
}

TEST(PlaygroundHttp, EscapesWhatJsonRequires) {
    // A program printing a quote, a backslash and a tab must not break the
    // response the browser has to parse.
    constexpr std::string_view kQuotes = R"(
.section .data
text:
    .asciz "\"\\\t"
.section .text
.global _start
_start:
    MOVI R1, 1
    LEA  R2, text
    MOVI R3, 3
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";
    const HttpResponse response = handleRequest("POST", "/api/run", kQuotes);
    EXPECT_NE(response.body.find("\\\""), std::string::npos) << response.body;
    EXPECT_NE(response.body.find("\\\\"), std::string::npos) << response.body;
    EXPECT_NE(response.body.find("\\t"), std::string::npos) << response.body;
}

TEST(PlaygroundHttp, ReplacesBytesThatAreNotValidUtf8) {
    // 0x80 is a continuation byte with nothing in front of it. Passing it
    // through verbatim would make JSON.parse throw in the browser, which would
    // look like a broken playground rather than a program printing a raw byte.
    constexpr std::string_view kRawByte = R"(
.section .data
raw:
    .byte 0x80, 0x00
.section .text
.global _start
_start:
    MOVI R1, 1
    LEA  R2, raw
    MOVI R3, 1
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";
    const HttpResponse response = handleRequest("POST", "/api/run", kRawByte);
    EXPECT_EQ(response.body.find('\x80'), std::string::npos) << "raw byte leaked into the JSON";
    EXPECT_NE(response.body.find("\xEF\xBF\xBD"), std::string::npos) << response.body;
}

TEST(PlaygroundHttp, KeepsValidUtf8Intact) {
    constexpr std::string_view kUnicode = R"(
.section .data
text:
    .asciz "\xE2\x9C\x93"
.section .text
.global _start
_start:
    MOVI R1, 1
    LEA  R2, text
    MOVI R3, 3
    SYSCALL 1
    MOVI R1, 0
    SYSCALL 0
)";
    const HttpResponse response = handleRequest("POST", "/api/run", kUnicode);
    // U+2713 CHECK MARK survives; it is well-formed, so nothing replaces it.
    EXPECT_NE(response.body.find("\xE2\x9C\x93"), std::string::npos) << response.body;
    EXPECT_EQ(response.body.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(PlaygroundHttp, SendsRegistersAsStringsSoTheyKeepEveryBit) {
    // 2^63 cannot survive a JSON number: the browser would parse it as a
    // double and lose the low bits. It goes out as a string for BigInt.
    constexpr std::string_view kBigValue = R"(
.section .text
.global _start
_start:
    MOVI R1, 1
    MOVI R2, 63
    SHL  R1, R2
    HALT
)";
    const HttpResponse response = handleRequest("POST", "/api/run", kBigValue);
    EXPECT_NE(response.body.find("\"9223372036854775808\""), std::string::npos) << response.body;
}

TEST(PlaygroundHttp, StillAnswersWhenTheProgramIsBroken) {
    // Whatever the input, the endpoint answers 200 with a report; a failing
    // program is data, not a server error.
    const HttpResponse response = handleRequest("POST", "/api/run", "this is not assembly");
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(response.body.find("\"stage\":\"assemble\""), std::string::npos);
}

TEST(PlaygroundHttp, HandlesAnEmptyBody) {
    const HttpResponse response = handleRequest("POST", "/api/run", "");
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("\"ok\":false"), std::string::npos) << response.body;
}

}  // namespace
