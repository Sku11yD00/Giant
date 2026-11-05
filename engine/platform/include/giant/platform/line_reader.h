// Reads text lines from a blocking source (normally stdin) on its own thread,
// so the editor loop can poll for them without ever blocking. The MCP server
// uses it to receive messages from Claude Code.
//
// This is the one place module code owns a std::thread: a blocking OS read
// would tie up a job-system worker forever.
#pragma once

#include "giant/core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace giant::platform {

class LineReader {
public:
    // Reads up to `size` bytes into `data`, blocking until at least one byte
    // is there. Returns the count, 0 at the end of input, or < 0 on an error
    // (also the end). Called only on the reader thread.
    using ReadChunk = std::function<long(char* data, usize size)>;

    // A line longer than this is dropped; `k_too_long_line` is delivered in
    // its place (it is not valid JSON, so the MCP server answers with a parse
    // error instead of the client waiting forever).
    static constexpr usize k_max_line_bytes = 64u * 1024 * 1024;
    static constexpr const char* k_too_long_line = "<line too long>";

    // Reads the process's standard input with raw OS reads. Not through
    // std::cin or stdio: a thread blocked in stdio holds stdin's lock, and
    // exit() waits for that lock, so the process could never exit.
    // Only one per process: a reader destroyed while blocked leaves its
    // thread in the read, and a second reader would race it for input.
    static std::unique_ptr<LineReader> from_stdin();

    // `read` must stay callable until it returns 0 or < 0: if the reader is
    // destroyed while a read is blocked, the thread is detached and finishes
    // on its own. Lines end in \n; a trailing \r is removed.
    explicit LineReader(ReadChunk read);
    ~LineReader();

    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    // Every complete line read since the last call, without the line ending.
    std::vector<std::string> take_lines();

    // True once the input ended (or failed) and every line was taken.
    bool finished() const;

    // Blocks until a line arrives, the input ends, or the timeout passes.
    // For tests and headless tools; the editor polls instead.
    void wait(int timeout_ms) const;

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace giant::platform
