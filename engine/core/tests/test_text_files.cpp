// Whole text files (core/text_files.h).
#include "giant/core/text_files.h"

#include "giant_test.h"

#include <filesystem>
#include <string>
#include <system_error>

using namespace giant;

GIANT_TEST("text files: written whole and read back, with a limit on the size read") {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "giant_text_files_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    const std::filesystem::path file = folder / "a.txt";
    GIANT_CHECK(!read_text_file(file));
    const std::string text = std::string("line\n\0second\r\n", 14) + std::string(100000, 'x');
    GIANT_CHECK(write_text_file(file, text) && read_text_file(file) == text);
    GIANT_CHECK(!std::filesystem::exists(folder / "a.txt.partial"));
    GIANT_CHECK(read_text_file(file, text.size()) == text && !read_text_file(file, text.size() - 1));
    GIANT_CHECK(write_text_file(file, "") && read_text_file(file) == std::string()); // over the old one
    GIANT_CHECK(!write_text_file(folder / "none" / "a.txt", "x") && !read_text_file(folder)); // no such folder; a folder
    // A write that cannot take its name (a folder has it) leaves nothing half written beside it.
    std::filesystem::create_directories(folder / "taken" / "inside");
    GIANT_CHECK(!write_text_file(folder / "taken", "x") && !std::filesystem::exists(folder / "taken.partial"));
    std::filesystem::remove_all(folder, error);
}

GIANT_TEST("text files: an environment variable is read as a path, empty when unset") {
    GIANT_CHECK(environment_path("GIANT_NO_SUCH_VARIABLE_SET").empty() && !environment_path("PATH").empty());
}
