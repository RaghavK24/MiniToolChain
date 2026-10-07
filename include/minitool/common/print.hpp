// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>

/// Thin wrapper over std::format for line-oriented output. <print> is C++23
/// but is not available in every supported standard library (GCC 13 lacks
/// it), so the toolchain routes all console output through here instead.
///
/// Deliberately not named `println`: a standard library that *does* provide
/// `std::println` (libc++ does, well before GCC's did) makes an unqualified
/// call ambiguous the moment this header's `println` is pulled in with a
/// `using` declaration -- both overloads take a `std::format_string`, so ADL
/// pulls `std::println` into the same candidate set, and neither is a better
/// match than the other. `printLine` cannot collide with a future std name.
namespace minitool::io {

inline void writeLine(std::FILE* out, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), out);
    std::fputc('\n', out);
}

template <typename... Args>
void printLine(std::FILE* out, std::format_string<Args...> fmt, Args&&... args) {
    writeLine(out, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void printLine(std::format_string<Args...> fmt, Args&&... args) {
    printLine(stdout, fmt, std::forward<Args>(args)...);
}

inline void printLine() {
    std::fputc('\n', stdout);
}

}  // namespace minitool::io
