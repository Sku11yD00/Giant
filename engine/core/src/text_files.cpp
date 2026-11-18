#include "giant/core/text_files.h"

#include <fstream>
#include <ios>
#include <string>
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
    // As many bytes as it had when it was asked: all of them, or it is not read.
    std::string text(static_cast<usize>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (static_cast<usize>(file.gcount()) != text.size()) {
        return std::nullopt;
    }
    return text;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path partial = path;
    partial += ".partial";
    std::error_code error;
    bool ok = false;
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        ok = static_cast<bool>(file);
    }
    if (ok) {
        std::filesystem::rename(partial, path, error);
        ok = !error;
    }
    if (!ok) {
        std::filesystem::remove(partial, error); // (nothing half written is left lying)
    }
    return ok;
}

} // namespace giant
