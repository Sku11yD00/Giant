#include "giant/platform/line_writer.h"

#include "giant/core/log.h"

#include "giant_test.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace giant;
using namespace giant::platform;

namespace {

// A sink that takes what is written only while the test lets it, like a pipe
// whose reader is slow; it can also be closed, like a client that went.
struct ScriptedOutput {
    std::mutex mutex; // protects all below
    std::condition_variable changed;
    std::string written;
    bool open = true;    // false: writes block
    bool closed = false; // true: writes fail
    int blocked = 0;     // writes waiting now

    bool write(std::string_view text) {
        std::unique_lock lock(mutex);
        ++blocked;
        changed.notify_all();
        changed.wait(lock, [&] { return open || closed; });
        --blocked;
        if (closed) {
            return false;
        }
        written.append(text);
        changed.notify_all();
        return true;
    }
    void hold() {
        std::lock_guard lock(mutex);
        open = false;
    }
    void let_go() {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }
    // Waits until a write is blocked in the sink.
    bool wait_blocked() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(20), [&] { return blocked > 0; });
    }
    std::string text() {
        std::lock_guard lock(mutex);
        return written;
    }
};

std::unique_ptr<LineWriter> writer_for(const std::shared_ptr<ScriptedOutput>& output) {
    return std::make_unique<LineWriter>([output](std::string_view text) { return output->write(text); });
}

} // namespace

GIANT_TEST("line writer: lines arrive whole, each ended, in the order sent") {
    auto output = std::make_shared<ScriptedOutput>();
    auto writer = writer_for(output);
    std::string expected;
    for (int i = 0; i < 200; ++i) {
        const std::string line = "line " + std::to_string(i) + std::string(static_cast<usize>(i) * 7, 'x');
        expected += line + "\n";
        GIANT_CHECK(writer->send(line));
    }
    GIANT_CHECK(writer->finish(20000));
    GIANT_CHECK(writer->ok());
    GIANT_CHECK(writer->waiting_bytes() == 0);
    GIANT_CHECK(output->text() == expected);
}

GIANT_TEST("line writer: sending never waits for a sink that takes nothing") {
    auto output = std::make_shared<ScriptedOutput>();
    output->hold();
    auto writer = writer_for(output);
    const std::string big(256 * 1024, 'r'); // far more than a pipe holds
    // (Each of these would block for good if sending wrote: the test would hang, not fail.)
    GIANT_CHECK(writer->send(big));
    GIANT_CHECK(writer->send("after"));
    GIANT_CHECK(output->wait_blocked());
    GIANT_CHECK(writer->waiting_bytes() == big.size() + 1 + 6);
    GIANT_CHECK(!writer->finish(20)); // still waiting: says so, and comes back
    GIANT_CHECK(writer->ok());
    GIANT_CHECK(output->text().empty());
    output->let_go();
    GIANT_CHECK(writer->finish(20000));
    GIANT_CHECK(writer->waiting_bytes() == 0);
    GIANT_CHECK(output->text() == big + "\nafter\n");
}

GIANT_TEST("line writer: a sink that went is told, and what waited is dropped") {
    auto output = std::make_shared<ScriptedOutput>();
    output->hold();
    auto writer = writer_for(output);
    GIANT_CHECK(writer->send("one"));
    GIANT_CHECK(writer->send("two"));
    GIANT_CHECK(output->wait_blocked());
    output->close();
    GIANT_CHECK(!writer->finish(20000)); // comes back at the failure, not at the timeout
    GIANT_CHECK(!writer->ok());
    GIANT_CHECK(writer->waiting_bytes() == 0);
    GIANT_CHECK(!writer->send("three"));
    GIANT_CHECK(writer->waiting_bytes() == 0);
    GIANT_CHECK(output->text().empty());
}

GIANT_TEST("line writer: destroyed while blocked, the thread finishes on its own") {
    auto output = std::make_shared<ScriptedOutput>();
    std::weak_ptr<ScriptedOutput> watch = output;
    output->hold();
    {
        auto writer = writer_for(output);
        GIANT_CHECK(writer->send("stuck"));
        GIANT_CHECK(writer->send("never written"));
        GIANT_CHECK(output->wait_blocked());
        // The thread is now blocked in the write: destroying the writer lets it go.
    }
    output->let_go();
    // The thread ends once that write returns, and releases its copy of the
    // output (it holds the last reference but this one).
    for (int i = 0; i < 4000 && watch.use_count() > 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    GIANT_CHECK(watch.use_count() == 1);
    GIANT_CHECK(output->text() == "stuck\n"); // what waited behind it was dropped
}

GIANT_TEST("line writer: destroyed with nothing waiting, it ends at once") {
    auto output = std::make_shared<ScriptedOutput>();
    {
        auto writer = writer_for(output);
        GIANT_CHECK(writer->send("done"));
        GIANT_CHECK(writer->finish(20000));
    }
    GIANT_CHECK(output.use_count() == 1); // joined: the thread's copy is gone
    GIANT_CHECK(output->text() == "done\n");
}

GIANT_TEST("line writer: a sink that throws is a sink that went") {
    auto writer = std::make_unique<LineWriter>([](std::string_view) -> bool { throw std::runtime_error("no sink"); });
    set_console_logging(false);
    writer->send("one");
    GIANT_CHECK(!writer->finish(20000));
    set_console_logging(true);
    GIANT_CHECK(!writer->ok());
    GIANT_CHECK(!writer->send("two"));
}

GIANT_TEST("line writer: what waits falls while a long line is being read") {
    // A sink that takes one piece, then holds: the line is part written.
    struct Slow {
        std::mutex mutex; // protects all below
        std::condition_variable changed;
        usize taken = 0;
        bool open = false;
    };
    auto slow = std::make_shared<Slow>();
    auto writer = std::make_unique<LineWriter>([slow](std::string_view text) {
        std::unique_lock lock(slow->mutex);
        slow->changed.wait(lock, [&] { return slow->taken == 0 || slow->open; });
        slow->taken += text.size();
        slow->changed.notify_all();
        return true;
    });
    const usize size = 1024 * 1024;
    GIANT_CHECK(writer->send(std::string(size, 'p')));
    {
        std::unique_lock lock(slow->mutex);
        GIANT_CHECK(slow->changed.wait_for(lock, std::chrono::seconds(20), [&] { return slow->taken > 0; }));
        GIANT_CHECK(slow->taken < size);
    }
    usize waiting = writer->waiting_bytes();
    for (int i = 0; i < 4000 && waiting == size + 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        waiting = writer->waiting_bytes();
    }
    GIANT_CHECK(waiting > 0 && waiting < size + 1);
    {
        std::lock_guard lock(slow->mutex);
        slow->open = true;
        slow->changed.notify_all();
    }
    GIANT_CHECK(writer->finish(20000));
    std::lock_guard lock(slow->mutex);
    GIANT_CHECK(slow->taken == size + 1);
}
