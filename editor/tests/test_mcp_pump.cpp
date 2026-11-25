#include "giant_test.h"

#include "../src/mcp_pump.h"

#include "giant/core/log.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

using namespace giant;
using namespace giant::editor;

namespace {

// A client: what it sends arrives when the test sends it, and what it is sent
// is taken only while it reads.
struct Client {
    std::mutex mutex; // protects all below
    std::condition_variable changed;
    std::deque<std::string> sent;
    std::string got;
    bool reading = true;
    bool closed = false;

    long read(char* data, usize size) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return !sent.empty() || closed; });
        if (sent.empty()) {
            return 0;
        }
        std::string& front = sent.front();
        const usize n = std::min(size, front.size());
        std::copy_n(front.data(), n, data);
        front.erase(0, n);
        if (front.empty()) {
            sent.pop_front();
        }
        return static_cast<long>(n);
    }
    bool write(std::string_view text) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return reading || closed; });
        if (closed) {
            return false;
        }
        got.append(text);
        return true;
    }
    void send(std::string text) {
        std::lock_guard lock(mutex);
        sent.push_back(std::move(text));
        changed.notify_all();
    }
    void read_replies(bool on) {
        std::lock_guard lock(mutex);
        reading = on;
        changed.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }
    usize replies() {
        std::lock_guard lock(mutex);
        return static_cast<usize>(std::count(got.begin(), got.end(), '\n'));
    }
};

std::string ping(int id) { return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"ping"})" + "\n"; }

struct Bench {
    std::shared_ptr<Client> client = std::make_shared<Client>();
    ai::CommandRegistry registry;
    ai::McpServer server{registry, ai::McpServerInfo{}};
    platform::LineReader reader{[c = client](char* data, usize size) { return c->read(data, size); }};
    platform::LineWriter writer{[c = client](std::string_view text) { return c->write(text); }};
    bool quit = false;
};

} // namespace

GIANT_TEST("mcp pump: replies go out though the client reads none, and no more are made past the most that may wait") {
    Bench bench;
    bench.client->read_replies(false);
    // The first messages are answered (one or all, as they arrive) and the answers wait: nothing held the pump.
    bench.client->send(ping(1) + ping(2) + ping(3));
    usize waiting = 0;
    for (int i = 0; i < 4000 && waiting == 0; ++i) {
        bench.reader.wait(5);
        GIANT_CHECK(pump_mcp(bench.reader, bench.writer, bench.server, bench.quit, 16));
        waiting = bench.writer.waiting_bytes();
    }
    GIANT_CHECK(waiting > 16);
    GIANT_CHECK(bench.client->replies() == 0);
    // With that much waiting, what comes next is not run: it stays with the reader.
    bench.client->send(ping(4) + ping(5));
    bench.reader.wait(2000);
    const usize before = bench.writer.waiting_bytes();
    for (int i = 0; i < 20; ++i) {
        GIANT_CHECK(pump_mcp(bench.reader, bench.writer, bench.server, bench.quit, 16));
    }
    GIANT_CHECK(bench.writer.waiting_bytes() == before);
    // The client reads: the answers arrive, and the rest is run.
    bench.client->read_replies(true);
    GIANT_CHECK(bench.writer.finish(20000));
    for (int i = 0; i < 4000 && bench.client->replies() < 5; ++i) {
        bench.reader.wait(5);
        GIANT_CHECK(pump_mcp(bench.reader, bench.writer, bench.server, bench.quit, 16));
        bench.writer.finish(5);
    }
    GIANT_CHECK(bench.client->replies() == 5);
    GIANT_CHECK(write_last_replies(bench.writer, 1000));
    bench.client->close();
}

GIANT_TEST("mcp pump: leaving waits for a client that reads, and not for one that stopped or went") {
    {
        Bench bench;
        bench.client->read_replies(false);
        bench.client->send(ping(1));
        for (int i = 0; i < 4000 && bench.writer.waiting_bytes() == 0; ++i) {
            bench.reader.wait(5);
            pump_mcp(bench.reader, bench.writer, bench.server, bench.quit);
        }
        set_console_logging(false);
        GIANT_CHECK(!write_last_replies(bench.writer, 100)); // stopped reading: given up on
        set_console_logging(true);
        bench.client->read_replies(true);
        GIANT_CHECK(write_last_replies(bench.writer, 20000)); // reads again: written
        GIANT_CHECK(bench.client->replies() == 1);
        bench.client->close();
    }
    {
        Bench bench;
        bench.client->read_replies(false);
        bench.client->send(ping(1));
        for (int i = 0; i < 4000 && bench.writer.waiting_bytes() == 0; ++i) {
            bench.reader.wait(5);
            pump_mcp(bench.reader, bench.writer, bench.server, bench.quit);
        }
        bench.client->close();
        GIANT_CHECK(!write_last_replies(bench.writer, 20000)); // went: told at once, not after the wait
        GIANT_CHECK(!pump_mcp(bench.reader, bench.writer, bench.server, bench.quit));
    }
}
