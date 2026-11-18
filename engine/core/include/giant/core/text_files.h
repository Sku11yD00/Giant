// Whole text files, read and written in one go (scenes, cells, settings:
// rule 8, data is text). They wait on the disk: never on the main thread
// while a game runs, except in a command the person asked for.
#pragma once

#include "giant/core/types.h"

#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace giant {

// The file's bytes, or nothing when it cannot be read or is over `most` bytes.
std::optional<std::string> read_text_file(const std::filesystem::path& path, usize most = std::numeric_limits<usize>::max());

// Written beside (<path>.partial), then renamed over: a crash mid-write
// never leaves half a file under its name. False when it could not be.
bool write_text_file(const std::filesystem::path& path, const std::string& text);

} // namespace giant
