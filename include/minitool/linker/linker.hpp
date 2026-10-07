// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <span>
#include <string>

#include "minitool/common/types.hpp"
#include "minitool/diagnostics/diagnostic_engine.hpp"
#include "minitool/executable/executable.hpp"
#include "minitool/object/object.hpp"

namespace minitool::linker {

/// Default virtual layout. Each region is 1 MiB apart, which is far more than
/// any program this toolchain produces needs, and keeps addresses readable in a
/// hex dump. docs/linker.md documents the whole address space, including the
/// stack and heap the VM adds at load time.
inline constexpr u64 kTextBase = 0x0001'0000;
inline constexpr u64 kRodataBase = 0x0010'0000;
inline constexpr u64 kDataBase = 0x0020'0000;
inline constexpr u64 kBssBase = 0x0030'0000;
/// The largest a single merged section may become before it would collide with
/// the next region.
inline constexpr u64 kRegionSize = 0x000F'0000;

struct LinkOptions {
    /// The symbol the entry point is taken from.
    std::string entry = "_start";
    u64 text_base = kTextBase;
    u64 rodata_base = kRodataBase;
    u64 data_base = kDataBase;
    u64 bss_base = kBssBase;
    /// Copy local symbols into the executable's symbol table. Useful for
    /// debugging, off for the smallest possible image.
    bool keep_local_symbols = true;
};

/// Combines relocatable objects into a loadable image.
///
/// Stages, in order: merge sections, resolve symbols, assign addresses, apply
/// relocations, build the executable. The linker knows nothing about the VM's
/// implementation (architectural rule 5). It only produces an image the loader
/// is prepared to validate.
class Linker {
  public:
    explicit Linker(diag::DiagnosticEngine& diagnostics) noexcept;

    [[nodiscard]] std::expected<executable::Executable, std::string> link(
        std::span<const object::ObjectFile> objects, const LinkOptions& options = {});

  private:
    diag::DiagnosticEngine& diagnostics_;
};

}  // namespace minitool::linker
