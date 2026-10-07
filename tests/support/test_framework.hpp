// SPDX-License-Identifier: MIT
#pragma once

/// A minimal xUnit runner with the subset of the GoogleTest surface this
/// project uses (see docs/adr/ADR-010-test-framework.md). It exists so that the
/// suite needs no network access and no third-party checkout: the toolchain
/// itself has no dependencies, and neither should its tests.
///
/// Supported: TEST, TEST_F, testing::Test, EXPECT_/ASSERT_ EQ NE LT LE GT GE,
/// TRUE, FALSE, STREQ, plus FAIL/SUCCEED/ADD_FAILURE and `<<` message
/// streaming. ASSERT_* aborts the current test; EXPECT_* records and continues.

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace minitest {

/// Thrown by a failed ASSERT_* to unwind out of the test body. Never thrown
/// from a destructor: the throw happens in Check::finish(), which the for-loop
/// in MT_CHECK_ calls as an ordinary iteration expression.
struct TestAborted {};

// ------------------------------------------------------------ value printing

template <typename T>
concept Formattable = std::formattable<T, char>;

template <typename T>
concept HasToString = requires(const T& value) {
    { toString(value) } -> std::convertible_to<std::string>;
};

template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
        return "nullptr";
    } else if constexpr (std::is_convertible_v<T, std::string_view> && !std::is_arithmetic_v<T>) {
        return std::format("\"{}\"", std::string_view{value});
    } else if constexpr (Formattable<T>) {
        return std::format("{}", value);
    } else if constexpr (HasToString<T>) {
        return std::string{toString(value)};
    } else if constexpr (std::is_enum_v<T>) {
        return std::format("enum({})", static_cast<std::int64_t>(value));
    } else if constexpr (std::is_pointer_v<T>) {
        return value == nullptr ? std::string{"nullptr"}
                                : std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(value));
    } else {
        return "<unprintable value>";
    }
}

// --------------------------------------------------------------- comparisons

struct Result {
    bool ok = true;
    std::string detail;
};

inline Result pass() {
    return Result{};
}

inline Result failure(std::string detail) {
    return Result{false, std::move(detail)};
}

#define MT_DEFINE_COMPARISON(suffix, op)                                                     \
    template <typename A, typename B>                                                        \
    Result cmp##suffix(const char* atext, const char* btext, const A& a, const B& b) {       \
        if (a op b) {                                                                        \
            return pass();                                                                   \
        }                                                                                    \
        return failure(std::format("  expected: {} " #op " {}\n    actual: {} vs {}", atext, \
                                   btext, describe(a), describe(b)));                        \
    }

MT_DEFINE_COMPARISON(EQ, ==)
MT_DEFINE_COMPARISON(NE, !=)
MT_DEFINE_COMPARISON(LT, <)
MT_DEFINE_COMPARISON(LE, <=)
MT_DEFINE_COMPARISON(GT, >)
MT_DEFINE_COMPARISON(GE, >=)
#undef MT_DEFINE_COMPARISON

template <typename T>
Result cmpTrue(const char* text, const T& value) {
    if (static_cast<bool>(value)) {
        return pass();
    }
    return failure(std::format("  expected: {} is true\n    actual: false", text));
}

template <typename T>
Result cmpFalse(const char* text, const T& value) {
    if (!static_cast<bool>(value)) {
        return pass();
    }
    return failure(std::format("  expected: {} is false\n    actual: true", text));
}

inline Result cmpStrEq(const char* atext, const char* btext, std::string_view a,
                       std::string_view b) {
    if (a == b) {
        return pass();
    }
    return failure(
        std::format("  expected: {} == {}\n    actual: \"{}\" vs \"{}\"", atext, btext, a, b));
}

// -------------------------------------------------------------- test records

class Test;

struct TestCase {
    std::string suite;
    std::string name;
    std::unique_ptr<Test> (*factory)();
};

/// Process-wide registry. Deliberately function-local so that registration from
/// static initialisers in any translation unit is well-ordered.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct RunState {
    std::vector<std::string> failures;
};

inline RunState& runState() {
    static RunState state;
    return state;
}

/// Base for TEST_F fixtures; TEST() bodies use it directly.
class Test {
  public:
    Test() = default;
    Test(const Test&) = delete;
    Test& operator=(const Test&) = delete;
    virtual ~Test() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
    virtual void TestBody() = 0;
};

struct Registrar {
    Registrar(const char* suite, const char* name, std::unique_ptr<Test> (*factory)()) {
        registry().push_back(TestCase{suite, name, factory});
    }
};

/// One assertion in flight. Reports on finish(), which the MT_CHECK_ loop calls
/// exactly once after the optional `<< ...` message has been streamed.
class Check {
  public:
    Check(const char* file, int line, Result result, bool fatal)
        : file_(file), line_(line), result_(std::move(result)), fatal_(fatal) {}

    [[nodiscard]] bool pending() const noexcept { return !result_.ok && !finished_; }

    std::ostringstream& stream() noexcept { return message_; }

    void finish() {
        finished_ = true;
        std::string text = std::format("{}:{}: failure\n{}", file_, line_, result_.detail);
        const std::string extra = message_.str();
        if (!extra.empty()) {
            text += "\n  message: " + extra;
        }
        runState().failures.push_back(text);
        std::fputs(text.c_str(), stderr);
        std::fputc('\n', stderr);
        if (fatal_) {
            throw TestAborted{};
        }
    }

  private:
    const char* file_;
    int line_;
    Result result_;
    bool fatal_;
    bool finished_ = false;
    std::ostringstream message_;
};

}  // namespace minitest

namespace testing {
using Test = ::minitest::Test;
}  // namespace testing

// --------------------------------------------------------------- the macros

#define MT_CHECK_(result_expr, fatal)                                             \
    for (::minitest::Check mt_check_{__FILE__, __LINE__, (result_expr), (fatal)}; \
         mt_check_.pending(); mt_check_.finish())                                 \
    mt_check_.stream()

#define EXPECT_EQ(a, b) MT_CHECK_(::minitest::cmpEQ(#a, #b, (a), (b)), false)
#define EXPECT_NE(a, b) MT_CHECK_(::minitest::cmpNE(#a, #b, (a), (b)), false)
#define EXPECT_LT(a, b) MT_CHECK_(::minitest::cmpLT(#a, #b, (a), (b)), false)
#define EXPECT_LE(a, b) MT_CHECK_(::minitest::cmpLE(#a, #b, (a), (b)), false)
#define EXPECT_GT(a, b) MT_CHECK_(::minitest::cmpGT(#a, #b, (a), (b)), false)
#define EXPECT_GE(a, b) MT_CHECK_(::minitest::cmpGE(#a, #b, (a), (b)), false)
#define EXPECT_TRUE(a) MT_CHECK_(::minitest::cmpTrue(#a, (a)), false)
#define EXPECT_FALSE(a) MT_CHECK_(::minitest::cmpFalse(#a, (a)), false)
#define EXPECT_STREQ(a, b) MT_CHECK_(::minitest::cmpStrEq(#a, #b, (a), (b)), false)

#define ASSERT_EQ(a, b) MT_CHECK_(::minitest::cmpEQ(#a, #b, (a), (b)), true)
#define ASSERT_NE(a, b) MT_CHECK_(::minitest::cmpNE(#a, #b, (a), (b)), true)
#define ASSERT_LT(a, b) MT_CHECK_(::minitest::cmpLT(#a, #b, (a), (b)), true)
#define ASSERT_LE(a, b) MT_CHECK_(::minitest::cmpLE(#a, #b, (a), (b)), true)
#define ASSERT_GT(a, b) MT_CHECK_(::minitest::cmpGT(#a, #b, (a), (b)), true)
#define ASSERT_GE(a, b) MT_CHECK_(::minitest::cmpGE(#a, #b, (a), (b)), true)
#define ASSERT_TRUE(a) MT_CHECK_(::minitest::cmpTrue(#a, (a)), true)
#define ASSERT_FALSE(a) MT_CHECK_(::minitest::cmpFalse(#a, (a)), true)
#define ASSERT_STREQ(a, b) MT_CHECK_(::minitest::cmpStrEq(#a, #b, (a), (b)), true)

#define ADD_FAILURE() MT_CHECK_(::minitest::failure("  ADD_FAILURE()"), false)
#define FAIL() MT_CHECK_(::minitest::failure("  FAIL()"), true)
#define SUCCEED() static_cast<void>(0)

#define MT_TEST_(suite, name, base)                                                      \
    namespace {                                                                          \
    class suite##_##name##_Test : public base {                                          \
      public:                                                                            \
        void TestBody() override;                                                        \
        static std::unique_ptr<::minitest::Test> create() {                              \
            return std::make_unique<suite##_##name##_Test>();                            \
        }                                                                                \
    };                                                                                   \
    const ::minitest::Registrar mt_reg_##suite##_##name{#suite, #name,                   \
                                                        &suite##_##name##_Test::create}; \
    }                                                                                    \
    void suite##_##name##_Test::TestBody()

#define TEST(suite, name) MT_TEST_(suite, name, ::minitest::Test)
#define TEST_F(fixture, name) MT_TEST_(fixture, name, fixture)

// ------------------------------------------------------------------ the main

namespace minitest {

inline int runAll(std::string_view filter) {
    std::size_t passed = 0;
    std::vector<std::string> failed_names;
    for (const TestCase& test : registry()) {
        const std::string full = test.suite + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string_view::npos) {
            continue;
        }
        runState().failures.clear();
        std::fprintf(stderr, "[ RUN      ] %s\n", full.c_str());
        std::unique_ptr<Test> instance = test.factory();
        try {
            instance->SetUp();
            instance->TestBody();
        } catch (const TestAborted&) {
            // Already reported by Check::finish().
        } catch (const std::exception& error) {
            runState().failures.emplace_back(std::string{"uncaught exception: "} + error.what());
            std::fprintf(stderr, "uncaught exception: %s\n", error.what());
        } catch (...) {
            runState().failures.emplace_back("uncaught non-standard exception");
            std::fprintf(stderr, "uncaught non-standard exception\n");
        }
        try {
            instance->TearDown();
        } catch (...) {
            runState().failures.emplace_back("exception thrown from TearDown");
        }
        if (runState().failures.empty()) {
            ++passed;
            std::fprintf(stderr, "[       OK ] %s\n", full.c_str());
        } else {
            failed_names.push_back(full);
            std::fprintf(stderr, "[  FAILED  ] %s\n", full.c_str());
        }
    }
    std::fprintf(stderr, "\n[==========] %zu passed, %zu failed\n", passed, failed_names.size());
    for (const std::string& name : failed_names) {
        std::fprintf(stderr, "[  FAILED  ] %s\n", name.c_str());
    }
    return failed_names.empty() ? 0 : 1;
}

}  // namespace minitest

#ifdef MINITEST_DEFINE_MAIN
int main(int argc, char** argv) {
    std::string_view filter;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        constexpr std::string_view kFilter = "--filter=";
        constexpr std::string_view kGtestFilter = "--gtest_filter=";
        if (arg.starts_with(kFilter)) {
            filter = arg.substr(kFilter.size());
        } else if (arg.starts_with(kGtestFilter)) {
            filter = arg.substr(kGtestFilter.size());
        } else if (arg == "--list") {
            for (const ::minitest::TestCase& test : ::minitest::registry()) {
                std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
            }
            return 0;
        }
    }
    return ::minitest::runAll(filter);
}
#endif
