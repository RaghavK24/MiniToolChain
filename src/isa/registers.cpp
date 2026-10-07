// SPDX-License-Identifier: MIT
#include "minitool/isa/registers.hpp"

#include <array>
#include <cctype>

namespace minitool::isa {
namespace {

constexpr std::array<std::string_view, kRegisterCount> kNames{
    "R0", "R1", "R2",  "R3",  "R4",  "R5",  "R6",  "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
};

[[nodiscard]] char upper(char c) noexcept {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

}  // namespace

std::string_view registerName(Reg reg) noexcept {
    if (!isValidRegister(reg)) {
        return "R?";
    }
    return kNames[registerIndex(reg)];
}

std::optional<Reg> parseRegister(std::string_view text) noexcept {
    if (text.size() == 2) {
        const char a = upper(text[0]);
        const char b = upper(text[1]);
        if (a == 'F' && b == 'P') {
            return kFramePointer;
        }
        if (a == 'R' && b == 'V') {
            return kReturnValue;
        }
    }
    if (text.size() < 2 || text.size() > 3 || upper(text[0]) != 'R') {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(1);
    if (digits.size() > 1 && digits[0] == '0') {
        return std::nullopt;  // no leading zeros: "R01" is not a register
    }
    unsigned value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<unsigned>(c - '0');
    }
    if (value >= kRegisterCount) {
        return std::nullopt;
    }
    return static_cast<Reg>(static_cast<u8>(value));
}

}  // namespace minitool::isa
