// Small text helpers shared across modules.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace giant {

// A path as UTF-8 text (for logs, JSON, the C# side). Use it instead of
// path::string(), which throws on Windows for names outside the ANSI code
// page, such as a user folder in Chinese.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// And back: a path from UTF-8 text (a command's argument).
inline std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// A to Z as a to z, every other byte as it is (names of files and joints compared whatever their case).
inline std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace giant
