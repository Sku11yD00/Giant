// Small text helpers shared across modules.
#pragma once

#include <filesystem>
#include <string>

namespace giant {

// A path as UTF-8 text (for logs, JSON, the C# side). Use it instead of
// path::string(), which throws on Windows for names outside the ANSI code
// page, such as a user folder in Chinese.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace giant
