// The editor's MCP messages in and replies out, between frames: what the main
// loop calls. Replies are written on the writer's thread, never here: a pipe
// holds little (4 KiB on Windows, less than one get_engine_info), and a write
// of more waits for the client to read, which held the frame.
#pragma once

#include "giant/ai/mcp_server.h"
#include "giant/core/log.h"
#include "giant/platform/line_reader.h"
#include "giant/platform/line_writer.h"

#include <string>

namespace giant::editor {

// With more replies than this waiting to be written, no more messages are
// run: a client that sends without reading has only so many answers made for
// it. (One reply may be bigger: a screenshot. What it sends meanwhile stays
// with the reader, which keeps reading.)
inline constexpr usize k_mcp_most_waiting_bytes = 4u * 1024 * 1024;

// Leaving, the replies still waiting (the answer to quit among them) are
// waited for while they are being read, and this long after the last was.
inline constexpr int k_mcp_last_replies_ms = 5000;

// Answers every MCP message that arrived since the last frame. Returns false
// once the client can no longer receive replies. Messages after a quit
// request are not run.
inline bool pump_mcp(platform::LineReader& reader, platform::LineWriter& writer, ai::McpServer& server,
                     const bool& quit_requested, usize most_waiting_bytes = k_mcp_most_waiting_bytes) {
    if (writer.waiting_bytes() > most_waiting_bytes) {
        return writer.ok(); // (the lines stay with the reader)
    }
    for (const std::string& line : reader.take_lines()) {
        if (quit_requested) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (auto reply = server.handle(line)) {
            writer.send(std::move(*reply));
        }
    }
    return writer.ok();
}

// Before the program leaves: waits for the replies sent to be written, for as
// long as the client goes on reading them and `quiet_ms` after it last did.
// True: all were written. A client that went is nothing to wait for.
inline bool write_last_replies(const platform::LineWriter& writer, int quiet_ms = k_mcp_last_replies_ms) {
    constexpr int k_step_ms = 50;
    usize waiting = writer.waiting_bytes();
    int quiet = 0;
    while (!writer.finish(k_step_ms)) {
        if (!writer.ok()) {
            return false;
        }
        const usize now = writer.waiting_bytes();
        quiet = now < waiting ? 0 : quiet + k_step_ms;
        waiting = now;
        if (quiet >= quiet_ms) {
            log_warning("editor", "MCP replies were left unwritten: the client stopped reading");
            return false;
        }
    }
    return true;
}

} // namespace giant::editor
