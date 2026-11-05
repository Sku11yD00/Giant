#include "giant/platform/line_reader.h"

#include "giant/core/log.h"

#include "giant_test.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

using namespace giant;
using namespace giant::platform;

namespace {

// Input that arrives when the test sends it, in the chunks the test chooses;
// reads block until there is something, like a pipe.
struct ScriptedInput {
    std::mutex mutex; // protects chunks and closed
    std::condition_variable changed;
    std::deque<std::string> chunks;
    bool closed = false;

    long read(char* data, usize size) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return !chunks.empty() || closed; });
        if (chunks.empty()) {
            return 0;
        }
        std::string& front = chunks.front();
        const usize n = std::min(size, front.size());
        std::copy_n(front.data(), n, data);
        front.erase(0, n);
        if (front.empty()) {
            chunks.pop_front();
        }
        return static_cast<long>(n);
    }
    void send(std::string chunk) {
        std::lock_guard lock(mutex);
        chunks.push_back(std::move(chunk));
        changed.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }
};

std::unique_ptr<LineReader> reader_for(const std::shared_ptr<ScriptedInput>& input) {
    return std::make_unique<LineReader>(
        [input](char* data, usize size) { return input->read(data, size); });
}

std::vector<std::string> read_all(LineReader& reader) {
    std::vector<std::string> all;
    for (int i = 0; i < 400 && !reader.finished(); ++i) {
        reader.wait(50);
        for (auto& line : reader.take_lines()) {
            all.push_back(std::move(line));
        }
    }
    return all;
}

} // namespace

GIANT_TEST("line reader: splits chunks into lines, wherever the chunks break") {
    auto input = std::make_shared<ScriptedInput>();
    auto reader = reader_for(input);
    input->send("fir");
    input->send("st\nsec");
    input->send("ond\r\n\nlast without newline");
    input->close();
    const auto lines = read_all(*reader);
    GIANT_CHECK(reader->finished());
    GIANT_CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        GIANT_CHECK(lines[0] == "first");
        GIANT_CHECK(lines[1] == "second"); // \r stripped
        GIANT_CHECK(lines[2].empty());
        GIANT_CHECK(lines[3] == "last without newline");
    }
}

GIANT_TEST("line reader: an empty input finishes with no lines") {
    auto input = std::make_shared<ScriptedInput>();
    auto reader = reader_for(input);
    input->close();
    GIANT_CHECK(read_all(*reader).empty());
    GIANT_CHECK(reader->finished());
}

GIANT_TEST("line reader: a long line arrives whole, an overlong one is replaced") {
    auto input = std::make_shared<ScriptedInput>();
    auto reader = reader_for(input);
    const std::string big(3 * 1024 * 1024, 'x'); // many 64 KiB reads
    input->send(big + "\n");
    // Overlong: sent in pieces so no single string is that large twice.
    const std::string piece(8 * 1024 * 1024, 'y');
    for (usize sent = 0; sent <= LineReader::k_max_line_bytes; sent += piece.size()) {
        input->send(piece);
    }
    input->send("\nafter\n");
    input->close();
    set_console_logging(false);
    const auto lines = read_all(*reader);
    set_console_logging(true);
    GIANT_CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        GIANT_CHECK(lines[0] == big);
        GIANT_CHECK(lines[1] == LineReader::k_too_long_line);
        GIANT_CHECK(lines[2] == "after");
    }
}

GIANT_TEST("line reader: destroyed while blocked, the thread finishes on its own") {
    auto input = std::make_shared<ScriptedInput>();
    std::weak_ptr<ScriptedInput> watch = input;
    {
        auto reader = reader_for(input);
        input->send("one\n");
        reader->wait(1000);
        GIANT_CHECK(reader->take_lines().size() == 1);
        // The thread is now blocked in read: destroying the reader detaches it.
    }
    // Let the detached thread see the end of input, then wait until it has
    // released its copy of the input (it holds the last reference).
    input->close();
    input.reset();
    for (int i = 0; i < 400 && !watch.expired(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    GIANT_CHECK(watch.expired());
}
