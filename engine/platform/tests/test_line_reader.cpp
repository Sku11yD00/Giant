#include "giant/platform/line_reader.h"

#include "giant_test.h"

#include <sstream>

using namespace giant::platform;

namespace {
std::vector<std::string> read_all(LineReader& reader) {
    std::vector<std::string> all;
    for (int i = 0; i < 200 && !reader.finished(); ++i) {
        reader.wait(50);
        for (auto& line : reader.take_lines()) {
            all.push_back(std::move(line));
        }
    }
    return all;
}
} // namespace

GIANT_TEST("line reader: delivers every line and then finishes") {
    std::istringstream in("first\nsecond\r\n\nlast without newline");
    LineReader reader(in);
    const auto lines = read_all(reader);
    GIANT_CHECK(reader.finished());
    GIANT_CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        GIANT_CHECK(lines[0] == "first");
        GIANT_CHECK(lines[1] == "second"); // \r stripped
        GIANT_CHECK(lines[2].empty());
        GIANT_CHECK(lines[3] == "last without newline");
    }
}

GIANT_TEST("line reader: an empty stream finishes with no lines") {
    std::istringstream in("");
    LineReader reader(in);
    GIANT_CHECK(read_all(reader).empty());
    GIANT_CHECK(reader.finished());
}

GIANT_TEST("line reader: can be destroyed before anything is read") {
    // Static: the stream must outlive a reader thread that may be detached.
    static std::istringstream in("a\nb\n");
    { LineReader reader(in); }
    GIANT_CHECK(true);
}
