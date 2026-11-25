// Writes text lines to a blocking sink (normally stdout) on its own thread, so
// the editor loop can send them without ever waiting. The MCP server's replies
// go through it: a pipe holds only so much (4 KiB on Windows), and a write of
// more waits for the client to read, which cost the frame that wrote it.
//
// Like LineReader this owns a std::thread: a blocking OS write would tie up a
// job-system worker for as long as the client takes.
#pragma once

#include "giant/core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace giant::platform {

class LineWriter {
public:
    // Writes all of `text` (a line, or a piece of a long one: the pieces come
    // in order, nothing between them), blocking until it is taken. False: the sink is
    // gone (nothing more is written); so is one that throws. Called only on
    // the writer thread.
    using WriteAll = std::function<bool(std::string_view text)>;

    // Writes the process's standard output with raw OS writes, not through
    // std::cout or stdio (a thread blocked in stdio holds stdout's lock, and
    // exit() waits for that lock). Whoever makes one writes stdout no other
    // way while it lives. Only one per process. Elsewhere than Windows the
    // caller ignores SIGPIPE first (a client that closed its end is then a
    // failed write, not the end of the process).
    static std::unique_ptr<LineWriter> to_stdout();

    // `write` must stay callable until it returns. A writer destroyed with
    // anything still waiting lets its thread go: the line under way, blocked
    // or not, ends on its own, and what waited behind it is never written.
    // Whoever wants it all written calls finish() first.
    explicit LineWriter(WriteAll write);
    ~LineWriter();

    LineWriter(const LineWriter&) = delete;
    LineWriter& operator=(const LineWriter&) = delete;

    // Queues `line` and a \n behind what waits; never blocks on the sink.
    // Lines are written whole, in the order sent. False: the sink is gone and
    // the line was dropped.
    bool send(std::string line);

    // False once a write failed (the client closed its end).
    bool ok() const;

    // Bytes sent and not written yet, the write under way among them (a long
    // line's fall 64 KiB at a time as it is written).
    usize waiting_bytes() const;

    // Blocks until everything sent is written, the sink fails, or the timeout
    // passes. True: nothing waits. For leaving and for tests.
    bool finish(int timeout_ms) const;

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace giant::platform
