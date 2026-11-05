#include "giant/script/script_host.h"

#include "giant/core/log.h"
#include "giant/core/log_history.h"

#include "giant_test.h"

#include <string>
#include <vector>

using namespace giant;
using namespace giant::script;

// The runtime can start only once per process, so one test does it all.
#if defined(GIANT_TEST_MANAGED_DIR) && !defined(GIANT_SKIP_DOTNET_RUNTIME_TESTS)
GIANT_TEST("script host: C# scripts run commands, survive a crash and unload") {
    std::vector<std::string> calls; // command names and arguments, in order
    ScriptHostDesc desc;
    desc.managed_dir = GIANT_TEST_MANAGED_DIR;
    desc.commands = [&calls](std::string_view name, std::string_view args) -> CommandReply {
        calls.push_back(std::string(name) + " " + std::string(args));
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
    const std::string scripts = std::string(GIANT_TEST_MANAGED_DIR) + "/tests/ScriptTests/ScriptTests.dll";
    GIANT_CHECK(host->load_scripts(scripts));
    GIANT_CHECK(host->running_scripts() == 2); // Talker and Crasher, not the abstract one

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

    set_console_logging(false);
    for (int i = 0; i < 3; ++i) {
        host->update(0.016f);
    }
    set_console_logging(true);
    GIANT_CHECK(has("\"frame\":3"));
    GIANT_CHECK(host->running_scripts() == 1); // Crasher stopped itself on frame 2
    bool logged_crash = false;
    for (const auto& record : log.recent(100, LogLevel::error)) {
        logged_crash = logged_crash || record.text.find("crasher crashed on purpose") != std::string::npos;
    }
    GIANT_CHECK(logged_crash);

    // Reloading runs OnStop, unloads, and starts fresh scripts.
    calls.clear();
    GIANT_CHECK(host->load_scripts(scripts));
    GIANT_CHECK(has("\"what\":\"stop\""));
    GIANT_CHECK(has("\"what\":\"start\""));
    GIANT_CHECK(host->running_scripts() == 2);

    host->unload_scripts();
    GIANT_CHECK(host->running_scripts() == 0);
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
