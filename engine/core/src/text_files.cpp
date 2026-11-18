#include "giant/core/text_files.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace giant {

std::optional<std::string> read_text_file(const std::filesystem::path& path, usize most) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > most) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return text.str();
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path partial = path;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    return !error;
}

} // namespace giant
