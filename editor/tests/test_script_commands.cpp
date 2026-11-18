// load_scripts, unload_scripts and get_scripts as far as they need no .NET:
// whose say it is that a script file runs (the loading itself is the main
// loop's, and is tested end to end by tools/mcp_smoke.py).
#include "../src/editor_commands.h"

#include "giant/core/text_files.h"
#include "giant/core/text.h"

#include "giant_test.h"

#include <filesystem>
#include <string>
#include <system_error>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    std::filesystem::path folder;
    ai::CommandRegistry commands;
    EditorState state;
    explicit Fixture(const char* name) : folder(std::filesystem::temp_directory_path() / name) {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder, error);
        state.dotnet_available = true;
        register_script_commands(commands, state);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    std::string file(const char* name) const {
        GIANT_CHECK(write_text_file(folder / name, "not really an assembly"));
        return path_to_utf8(folder / name);
    }
    ai::CommandResult load(const std::string& path) { return commands.execute("load_scripts", {{"path", path}}); }
};

} // namespace

GIANT_TEST("scripts: a file a command names is loaded only once the person allowed it, in the window") {
    Fixture f("giant_script_commands_test_ask");
    f.state.scripts_consent = ScriptsConsent::ask;
    const std::string a = f.file("A.dll");
    const std::string key = script_file_key(path_from_utf8(a));
    GIANT_CHECK(!key.empty());
    // The command asks; nothing is loaded.
    ai::CommandResult r = f.load(a);
    GIANT_CHECK(r.ok && r.value["asked"] == true && r.value["file"] == key && !r.value.contains("loading"));
    GIANT_CHECK(f.state.scripts_to_load.empty() && f.state.scripts_request == key && f.state.scripts_allowed.empty());
    GIANT_CHECK(f.commands.execute("get_scripts").value["asked"] == key);
    // Asked again about the same file: the same question. About another: one at a time.
    GIANT_CHECK(f.load(a).value["asked"] == true && f.state.scripts_request == key);
    const std::string b = f.file("B.dll");
    r = f.load(b);
    GIANT_CHECK(!r.ok && r.error.find("still being asked") != std::string::npos && f.state.scripts_request == key);
    // Not allowed: not loaded, and asked about again the next time.
    GIANT_CHECK(answer_scripts_request(f.state, false).find("not allowed") != std::string::npos);
    GIANT_CHECK(!f.state.scripts_request && f.state.scripts_to_load.empty() && f.state.scripts_allowed.empty());
    GIANT_CHECK(f.commands.execute("get_scripts").value["asked"].is_null());
    GIANT_CHECK(answer_scripts_request(f.state, true) == "nothing was asked" && f.state.scripts_to_load.empty());
    GIANT_CHECK(f.load(a).value["asked"] == true);
    // Allowed: it loads after the frame, and again without asking while the editor runs.
    f.state.scripts_unload_requested = true;
    GIANT_CHECK(answer_scripts_request(f.state, true).find("are allowed") != std::string::npos);
    GIANT_CHECK(f.state.scripts_to_load == key && !f.state.scripts_unload_requested && !f.state.scripts_request &&
                f.state.scripts_allowed.size() == 1);
    f.state.scripts_to_load.clear(); // (the main loop took it)
    r = f.load(a);
    GIANT_CHECK(r.ok && r.value["loading"] == key && !r.value.contains("asked") && f.state.scripts_to_load == key && !f.state.scripts_request);
    // However the path is written, it is that file.
    f.state.scripts_to_load.clear();
    std::error_code error;
    std::filesystem::create_directories(f.folder / "sub", error);
    GIANT_CHECK(f.load(path_to_utf8(f.folder / "sub" / ".." / "A.dll")).value["loading"] == key);
    // Another file is another question.
    GIANT_CHECK(f.load(b).value["asked"] == true && f.state.scripts_request == script_file_key(path_from_utf8(b)));
    // unload_scripts withdraws a question still open; a load that needs none does too (the latest request wins).
    GIANT_CHECK(f.commands.execute("unload_scripts").ok && !f.state.scripts_request && f.state.scripts_to_load.empty());
    GIANT_CHECK(f.load(b).value["asked"] == true && f.load(a).value["loading"] == key && !f.state.scripts_request);
    // A file that is not there, and an editor without .NET.
    GIANT_CHECK(f.load(path_to_utf8(f.folder / "none.dll")).error.find("no such file") != std::string::npos);
    f.state.dotnet_available = false;
    GIANT_CHECK(!f.load(b).ok && !f.state.scripts_request);
}

GIANT_TEST("scripts: no window, no loading by command; automated runs say so when they start the editor") {
    Fixture f("giant_script_commands_test_consent");
    const std::string a = f.file("A.dll");
    const std::string key = script_file_key(path_from_utf8(a));
    // The default: none can be loaded by a command.
    GIANT_CHECK(EditorState().scripts_consent == ScriptsConsent::none);
    ai::CommandResult r = f.load(a);
    GIANT_CHECK(!r.ok && r.error.find("no window to ask") != std::string::npos && f.state.scripts_to_load.empty() && !f.state.scripts_request);
    // What the person chose themselves (--scripts: main puts it on the list) loads again.
    f.state.scripts_allowed.push_back(key);
    GIANT_CHECK(f.load(a).value["loading"] == key && f.state.scripts_to_load == key);
    f.state.scripts_allowed.clear();
    f.state.scripts_to_load.clear();
    // --allow-scripts-by-command: loaded as named, nobody asked.
    f.state.scripts_consent = ScriptsConsent::by_command;
    r = f.load(a);
    GIANT_CHECK(r.ok && r.value["loading"] == a && f.state.scripts_to_load == a && !f.state.scripts_request && f.state.scripts_allowed.empty());
}

GIANT_TEST("scripts: the list of files allowed in a run is bounded, the oldest asked about again") {
    Fixture f("giant_script_commands_test_many");
    f.state.scripts_consent = ScriptsConsent::ask;
    std::string first;
    for (usize i = 0; i < k_most_scripts_allowed + 1; ++i) {
        const std::string path = f.file(("S" + std::to_string(i) + ".dll").c_str());
        if (i == 0) {
            first = path;
        }
        GIANT_CHECK(f.load(path).value["asked"] == true);
        GIANT_CHECK(answer_scripts_request(f.state, true).find("are allowed") != std::string::npos);
    }
    GIANT_CHECK(f.state.scripts_allowed.size() == k_most_scripts_allowed);
    GIANT_CHECK(f.load(first).value["asked"] == true);
    // Allowing a file twice keeps one entry.
    const std::string last = path_to_utf8(f.folder / ("S" + std::to_string(k_most_scripts_allowed) + ".dll"));
    GIANT_CHECK(f.commands.execute("unload_scripts").ok && f.load(last).value.contains("loading"));
    f.state.scripts_request = script_file_key(path_from_utf8(last));
    GIANT_CHECK(answer_scripts_request(f.state, true).find("are allowed") != std::string::npos &&
                f.state.scripts_allowed.size() == k_most_scripts_allowed);
}

GIANT_TEST("scripts: Claude's load_scripts is asked about once: by the command where it only asks, else by the chat's gate") {
    Fixture f("giant_script_commands_test_gate");
    ChatGate& gate = f.state.chat_gate;
    gate.asks = true;
    const ai::CommandDesc& load = *f.commands.find("load_scripts");
    const ai::CommandDesc& unload = *f.commands.find("unload_scripts");
    const std::string a = f.file("A.dll");
    const Json args{{"path", a}};
    // An editor that asks, a file not yet allowed: the command's own window asks; the gate does not ask first.
    f.state.scripts_consent = ScriptsConsent::ask;
    GIANT_CHECK(!gate.holds(load, args) && gate.holds(unload, Json::object()));
    // Arguments the command would refuse, or a file that is not there: nothing would be asked, so the gate holds.
    GIANT_CHECK(gate.holds(load, Json::object()) && gate.holds(load, {{"path", 3}}) && gate.holds(load, Json::array()) &&
                gate.holds(load, {{"path", path_to_utf8(f.folder / "none.dll")}}));
    // Allowed before: the command would load at once, replacing what runs. The gate asks.
    GIANT_CHECK(f.load(a).value["asked"] == true && answer_scripts_request(f.state, true).find("are allowed") != std::string::npos);
    GIANT_CHECK(gate.holds(load, args));
    // An editor that loads by command, or has no window, or no .NET: the command asks no one. The gate asks.
    f.state.scripts_allowed.clear();
    GIANT_CHECK(!gate.holds(load, args));
    f.state.scripts_consent = ScriptsConsent::by_command;
    GIANT_CHECK(gate.holds(load, args));
    f.state.scripts_consent = ScriptsConsent::none;
    GIANT_CHECK(gate.holds(load, args));
    f.state.scripts_consent = ScriptsConsent::ask;
    f.state.dotnet_available = false;
    GIANT_CHECK(gate.holds(load, args));
}

GIANT_TEST("scripts: a link is the file it points to; pointed elsewhere, that file is asked about") {
    Fixture f("giant_script_commands_test_link");
    f.state.scripts_consent = ScriptsConsent::ask;
    const std::string a = f.file("A.dll");
    const std::string b = f.file("B.dll");
    const std::filesystem::path link = f.folder / "Link.dll";
    std::error_code error;
    std::filesystem::create_symlink(f.folder / "A.dll", link, error);
    if (error) {
        return; // (links cannot be made here: Windows without the right to)
    }
    const std::string key_a = script_file_key(path_from_utf8(a));
    const std::string key_b = script_file_key(path_from_utf8(b));
    GIANT_CHECK(script_file_key(link) == key_a && key_a != key_b);
    // Allowed through the link, it is A that is allowed, and A that is loaded.
    GIANT_CHECK(f.load(path_to_utf8(link)).value["file"] == key_a && answer_scripts_request(f.state, true).find("are allowed") != std::string::npos);
    GIANT_CHECK(f.state.scripts_to_load == key_a && f.load(a).value["loading"] == key_a);
    // The link pointed at B: B was never allowed.
    std::filesystem::remove(link, error);
    std::filesystem::create_symlink(f.folder / "B.dll", link, error);
    GIANT_CHECK(!error);
    f.state.scripts_to_load.clear();
    const ai::CommandResult r = f.load(path_to_utf8(link));
    GIANT_CHECK(r.ok && r.value["asked"] == true && r.value["file"] == key_b && f.state.scripts_to_load.empty());
}
