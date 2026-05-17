// src/tools/bash/AnsiStripInternal.hpp
//
// Internal header for AnsiStrip — used only within the bash sub-library.

#pragma once

#include <string>
#include <string_view>

namespace batbox::tools::bash {

/// Removes ANSI/VT escape sequences from @p input and returns the stripped text.
/// UTF-8 multi-byte sequences are passed through unmodified.
[[nodiscard]] std::string ansi_strip(std::string_view input);

} // namespace batbox::tools::bash
