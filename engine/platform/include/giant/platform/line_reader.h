// Reads text lines from a blocking stream (normally stdin) on its own thread,
// so the editor loop can poll for them without ever blocking. The MCP server
// uses it to receive messages from Claude Code.
//
// This is the one place module code owns a std::thread: a blocking OS read
// would tie up a job-system worker forever.
#pragma once

#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace giant::platform {

class LineReader {
public:
    // Reads the process's standard input with raw OS reads. Not through
    // std::cin or stdio: a thread blocked in stdio holds stdin's lock, and
    // exit() waits for that lock, so the process could never exit.
    static std::unique_ptr<LineReader> from_stdin();

    // Reads `in` (tests use string streams). `in` must outlive the reader
    // thread, which may outlive the reader if it is still blocked in a read.
    explicit LineReader(std::istream& in);
    ~LineReader();

    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    // Every complete line read since the last call, without the line ending.
    std::vector<std::string> take_lines();

    // True once the stream ended (or failed) and every line was taken.
    bool finished() const;

    // Blocks until a line arrives, the stream ends, or the timeout passes.
    // For tests and headless tools; the editor polls instead.
    void wait(int timeout_ms) const;

    struct Shared;
    struct ConstructKey {
    private:
        friend class LineReader;
        ConstructKey() = default;
    };
    explicit LineReader(ConstructKey);

private:
    void start(std::function<bool(std::string&)> read_line);

    std::shared_ptr<Shared> m_shared;
};

} // namespace giant::platform
