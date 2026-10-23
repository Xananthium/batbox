#pragma once

#include <batbox/core/Result.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace batbox::conversation {

// A single pathological tool result, not a conversation/context limit.
inline constexpr std::size_t kToolOutputArchiveThreshold = 512 * 1024;
inline constexpr std::size_t kToolOutputArchivePageBytes = 64 * 1024;

// Publish exact original bytes, bounded readable pages and a page manifest
// before returning a head/tail context view. No value means below threshold.
// On any failure callers retain their original message. Never rewrites logs.
[[nodiscard]] Result<std::optional<std::string>> archive_tool_output(
    std::string_view content, const std::filesystem::path& directory);

} // namespace batbox::conversation
