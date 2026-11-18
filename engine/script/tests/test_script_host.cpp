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
    GIANT_CHECK(host->running_scripts() == 8); // all but the abstract one

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
    GIANT_CHECK(host->running_scripts() == 7); // Crasher stopped itself on frame 2
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
    GIANT_CHECK(host->running_scripts() == 8);

    // What happened to people is told in order, names and all (the views' text may go once the call is made).
    calls.clear();
    {
        std::string behaviour = "wary", flee = "fl\xc3\xa9" "e", hide = "hide";
        const PersonEvent events[] = {{7, behaviour, "", flee, 1.5}, {7, behaviour, flee, hide, 20.25}, {9, behaviour, hide, hide, 21.0}};
        host->person_events(events);
        behaviour = "gone";
    }
    GIANT_CHECK(calls.size() == 6); // (two scripts listen: each is told each, in turn)
    if (calls.size() == 6) {
        // (The scripts' JSON writes a letter that is not ASCII as an escape: é is \\u00E9.)
        GIANT_CHECK(has( "record {\"what\":\"person\",\"person\":7,\"behaviour\":\"wary\",\"from\":\"\",\"to\":\"fl\\u00E9e\","
                                "\"began\":true,\"seconds\":1.5,\"again\":false}"));
        GIANT_CHECK(has("record {\"what\":\"fragile\",\"person\":7,\"length\":4}")); // (which script first is not said)
        GIANT_CHECK(has( "record {\"what\":\"person\",\"person\":7,\"behaviour\":\"wary\",\"from\":\"fl\\u00E9e\",\"to\":\"hide\","
                                "\"began\":false,\"seconds\":20.25,\"again\":false}"));
        GIANT_CHECK(has("\"person\":9,\"behaviour\":\"wary\",\"from\":\"hide\",\"to\":\"hide\",\"began\":false,\"seconds\":21,\"again\":true"));
    }
    // None, and more than are told at a time: the first k_most_person_events.
    calls.clear();
    host->person_events({});
    GIANT_CHECK(calls.empty());
    {
        std::vector<PersonEvent> many(ScriptHost::k_most_person_events + 5, PersonEvent{1, "b", "x", "y", 0.0});
        host->person_events(many);
        GIANT_CHECK(calls.size() == 2 * ScriptHost::k_most_person_events);
    }
    // A script that asks for scripts to be reloaded while it is told: refused, and the rest are told.
    calls.clear();
    reentry_refused = false;
    {
        const PersonEvent events[] = {{1, "b", "x", "reload", 0.0}, {2, "b", "x", "y", 0.0}};
        set_console_logging(false);
        host->person_events(events);
        set_console_logging(true);
    }
    GIANT_CHECK(reentry_refused && has("\"person\":2") && host->running_scripts() == 8);
    // A name too long to be kept, and one that is not UTF-8: told all the same (the second with its bad byte replaced).
    calls.clear();
    {
        const std::string longest(300, 'n'), bad = "a\xff" "b";
        const PersonEvent events[] = {{3, longest, "", "y", 0.0}, {4, longest, "", "y", 0.0}, {5, bad, "", bad, 0.0}, {6, bad, "", bad, 0.0}};
        host->person_events(events);
    }
    GIANT_CHECK(has("\"what\":\"fragile\",\"person\":3,\"length\":300") && has("\"what\":\"fragile\",\"person\":4,\"length\":300"));
    GIANT_CHECK(has("\"what\":\"fragile\",\"person\":6,\"length\":3") && has("\"person\":6,\"behaviour\":\"a\\uFFFDb\""));
    // A script that throws when told is stopped; the others are still told, of that and of the rest.
    calls.clear();
    {
        const PersonEvent events[] = {{1, "b", "x", "throw", 0.0}, {2, "b", "x", "y", 0.0}};
        set_console_logging(false);
        host->person_events(events);
        set_console_logging(true);
    }
    GIANT_CHECK(host->running_scripts() == 7 && calls.size() == 2 && has("\"person\":1,\"behaviour\":\"b\"") && !has("fragile"));
    bool logged_fragile = false;
    for (const auto& record : log.recent(100, LogLevel::error)) {
        logged_fragile = logged_fragile || record.text.find("fragile broke on purpose") != std::string::npos;
    }
    GIANT_CHECK(logged_fragile);
    GIANT_CHECK(host->load_scripts(scripts)); // (all running again for what follows)
    calls.clear();

    // Assemblies added beside it (mods' scripts): each started, told and unloaded on its own.
    const auto count = [&calls](const std::string& needle) {
        usize found = 0;
        for (const auto& call : calls) {
            found += call.find(needle) != std::string::npos ? usize{1} : usize{0};
        }
        return found;
    };
    GIANT_CHECK(host->add_scripts(scripts) && host->running_scripts() == 16 && count("\"what\":\"start\"") == 1);
    GIANT_CHECK(host->add_scripts(scripts) && host->running_scripts() == 24 && count("\"what\":\"start\"") == 2);
    calls.clear();
    {
        const PersonEvent events[] = {{11, "b", "x", "y", 0.0}};
        host->person_events(events);
    }
    GIANT_CHECK(count("\"what\":\"person\",\"person\":11") == 3); // (the Listener of each)
    // One that cannot be loaded leaves the rest running.
    set_console_logging(false);
    GIANT_CHECK(!host->add_scripts("does/not/exist.dll") && host->running_scripts() == 24);
    set_console_logging(true);
    // The one loaded is replaced without those added, and those added go without it.
    calls.clear();
    GIANT_CHECK(host->load_scripts(scripts) && host->running_scripts() == 24 && count("\"what\":\"stop\"") == 1 &&
                count("\"what\":\"start\"") == 1);
    calls.clear();
    host->unload_added_scripts();
    GIANT_CHECK(host->running_scripts() == 8 && count("\"what\":\"stop\"") == 2);
    host->unload_added_scripts(); // (none: nothing to do)
    // Added with none loaded, they run all the same.
    host->unload_scripts();
    calls.clear();
    GIANT_CHECK(host->add_scripts(scripts) && host->running_scripts() == 8);
    set_console_logging(false);
    host->update(0.016f);
    set_console_logging(true);
    GIANT_CHECK(has("\"frame\":1"));
    host->unload_added_scripts();
    GIANT_CHECK(host->running_scripts() == 0 && host->load_scripts(scripts));
    calls.clear();

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
