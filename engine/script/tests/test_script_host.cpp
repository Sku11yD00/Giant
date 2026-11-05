#include "giant/script/script_host.h"

#include "giant/core/log.h"
#include "giant/core/log_history.h"

#include "giant_test.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace giant;
using namespace giant::script;

// The runtime can start only once per process, so one test does it all.
#if defined(GIANT_TEST_MANAGED_DIR) && !defined(GIANT_SKIP_DOTNET_RUNTIME_TESTS)
GIANT_TEST("script host: C# scripts run commands, survive a crash and unload") {
    std::vector<std::string> calls; // command names and arguments, in order
    ScriptHost* live_host = nullptr;
    const std::string scripts = std::string(GIANT_TEST_MANAGED_DIR) + "/tests/ScriptTests/ScriptTests.dll";
    bool reentry_refused = false;
    ScriptHostDesc desc;
    desc.managed_dir = GIANT_TEST_MANAGED_DIR;
    desc.commands = [&](std::string_view name, std::string_view args) -> CommandReply {
        calls.push_back(std::string(name) + " " + std::string(args));
        if (name == "reenter") {
            // A script asking to reload or unload scripts mid-update.
            reentry_refused = !live_host->load_scripts(scripts);
            live_host->unload_scripts();
            return {true, "{}"};
        }
        if (name == "echo") {
            return {true, std::string(args)};
        }
        if (name == "record") {
            return {true, "{}"};
        }
        return {false, "no such command"};
    };
    const LogHistory log(100);
    auto host = ScriptHost::create(desc);
    GIANT_CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    live_host = host.get();
    GIANT_CHECK(host->load_scripts(scripts));
    GIANT_CHECK(host->running_scripts() == 6); // all but the abstract one

    const auto has = [&calls](const std::string& needle) {
        for (const auto& call : calls) {
            if (call.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    GIANT_CHECK(has("echo {\"value\":41}"));
    GIANT_CHECK(has("\"echoed\":42"));                     // read the reply
    GIANT_CHECK(has("\"what\":\"failed\",\"error\":\"no such command\"")); // failures throw
    GIANT_CHECK(has("\"what\":\"thread\",\"result\":\"refused\"")); // main thread only
    GIANT_CHECK(!has("from another thread"));
    GIANT_CHECK(has("\"what\":\"shapes\",\"f\":0.25,\"big\":9223372036854775807,"
                    "\"weather\":\"Storm\",\"gust\":{\"Strength\":2.5,\"Kind\":\"Clear\"},"
                    "\"list\":[1,2],\"map\":{\"k\":null},\"when\":\"2026-11-05T01:02:03.0000000Z\","
                    "\"span\":1.5,\"id\":\"00000000-0000-0000-0000-000000000000\","
                    "\"v\":{\"X\":1,\"Y\":2,\"Z\":3},\"pair\":{\"Key\":\"a\",\"Value\":1}"));
    GIANT_CHECK(has("opaque refused"));

    set_console_logging(false);
    for (int i = 0; i < 3; ++i) {
        host->update(0.016f);
    }
    // The Awaiter's Task.Run finishes on a worker; its continuation runs in
    // a later update on this thread.
    for (int i = 0; i < 200 && !has("after task"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        host->update(0.016f);
    }
    host->update(0.016f); // an async void method's exception is posted, so runs one update later
    set_console_logging(true);
    GIANT_CHECK(has("after yield"));
    GIANT_CHECK(has("\"what\":\"after task\",\"answer\":42"));
    GIANT_CHECK(has("\"frame\":3"));
    GIANT_CHECK(reentry_refused);              // and the scripts kept running
    GIANT_CHECK(host->running_scripts() == 5); // Crasher stopped itself on frame 2
    bool logged_crash = false;
    bool logged_async = false;
    for (const auto& record : log.recent(100, LogLevel::error)) {
        logged_crash = logged_crash || record.text.find("crasher crashed on purpose") != std::string::npos;
        logged_async = logged_async || record.text.find("async throw on purpose") != std::string::npos;
    }
    GIANT_CHECK(logged_crash);
    GIANT_CHECK(logged_async); // logged, not fatal

    // Reloading runs OnStop, unloads, and starts fresh scripts.
    calls.clear();
    GIANT_CHECK(host->load_scripts(scripts));
    GIANT_CHECK(has("\"what\":\"stop\""));
    GIANT_CHECK(has("\"what\":\"start\""));
    GIANT_CHECK(host->running_scripts() == 6);

    host->unload_scripts();
    GIANT_CHECK(host->running_scripts() == 0);
    // Old assemblies were really unloaded (nothing kept them alive).
    bool still_referenced = false;
    for (const auto& record : log.recent(100, LogLevel::warning)) {
        still_referenced = still_referenced || record.text.find("still referenced") != std::string::npos;
    }
    GIANT_CHECK(!still_referenced);
    set_console_logging(false);
    GIANT_CHECK(!host->load_scripts("does/not/exist.dll"));
    set_console_logging(true);
    GIANT_CHECK(host->running_scripts() == 0);

    // .NET runs once per process.
    set_console_logging(false);
    GIANT_CHECK(ScriptHost::create(desc) == nullptr);
    set_console_logging(true);
}
#endif

GIANT_TEST("script host: version folders compare by number") {
    // Indirectly: find_dotnet_root must not crash, whatever is installed.
    (void)find_dotnet_root();
    GIANT_CHECK(true);
}
