// The mods beside a world (mods.h) and their commands: list_mods, enable_mod.
#include "../src/editor_commands.h"
#include "../src/mods.h"

#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/world/mods.h"

#include "giant_test.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

// A world's folder (only its mods), emptied before and after.
struct ModWorld {
    std::filesystem::path folder;
    explicit ModWorld(const char* name) : folder(std::filesystem::temp_directory_path() / name) {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder);
    }
    ~ModWorld() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    // A mod's folder with its manifest (as the text given, or the mod's own).
    void put(const std::string& name, const std::string& text) const {
        std::filesystem::create_directories(folder / "mods" / name);
        GIANT_CHECK(write_text_file(folder / "mods" / name / "mod.json", text));
    }
    void put(const world::Mod& mod) const { put(mod.id, world::save_mod(mod)); }
    void enabled(const std::string& text) const {
        std::filesystem::create_directories(folder / "mods");
        GIANT_CHECK(write_text_file(folder / "mods" / "enabled.json", text));
    }
};

world::Mod mod(std::string id, world::ModVersion version = {1, 0, 0}, std::vector<world::ModNeed> needs = {}) {
    world::Mod m;
    m.name = "The " + id;
    m.id = std::move(id);
    m.version = version;
    m.needs = std::move(needs);
    return m;
}

const ModList::Found* found(const ModList& list, const std::string& id) {
    for (const ModList::Found& f : list.mods) {
        if (f.mod.id == id) {
            return &f;
        }
    }
    return nullptr;
}

const Json* listed(const Json& mods, const std::string& id) {
    for (const Json& m : mods) {
        if (m.value("id", "") == id) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

GIANT_TEST("mods: a world without mods lists none, and its mods are found by folder") {
    const ModWorld w("giant_mods_test_read");
    ModList list = read_mods(w.folder);
    GIANT_CHECK(list.mods.empty() && list.broken.empty() && list.missing.empty() && list.order.empty() && list.list_error.empty() && !list.too_many);
    w.put(mod("winter", {1, 2, 0}, {{"base", {2, 0, 0}}}));
    w.put(mod("base", {2, 1, 0}));
    w.put(mod("alone"));
    GIANT_CHECK(write_text_file(w.folder / "mods" / "notes.txt", "a file is no mod, and no trouble")); // (not a folder)
    list = read_mods(w.folder);
    GIANT_CHECK(list.mods.size() == 3 && list.mods[0].mod.id == "alone" && list.mods[1].mod.id == "base" && list.mods[2].mod.id == "winter");
    GIANT_CHECK(list.broken.empty() && list.order.empty() && !list.mods[0].on && list.mods[0].place == 0 && list.mods[0].problem.empty());
    GIANT_CHECK(list.mods[2].mod == mod("winter", {1, 2, 0}, {{"base", {2, 0, 0}}}));
    // On: in order, the needed one first; one whose need is off says so.
    GIANT_CHECK(!set_mod_on(w.folder, "winter", true));
    list = read_mods(w.folder);
    GIANT_CHECK(found(list, "winter")->on && found(list, "winter")->place == 0 && found(list, "winter")->problem.find("not on") != std::string::npos);
    GIANT_CHECK(!set_mod_on(w.folder, "base", true) && !set_mod_on(w.folder, "alone", true));
    list = read_mods(w.folder);
    GIANT_CHECK(list.order == std::vector<std::string>({"alone", "base", "winter"}) && found(list, "winter")->place == 3 &&
                found(list, "winter")->problem.empty());
    // The list is the world's file, sorted, and survives being read again.
    const std::string text = read_text_file(w.folder / "mods" / "enabled.json").value_or("");
    GIANT_CHECK(text == world::save_enabled_mods(std::vector<std::string>{"alone", "base", "winter"}));
    GIANT_CHECK(!set_mod_on(w.folder, "base", true) && read_text_file(w.folder / "mods" / "enabled.json") == text); // on already
    GIANT_CHECK(!set_mod_on(w.folder, "base", false));
    list = read_mods(w.folder);
    GIANT_CHECK(list.order == std::vector<std::string>{"alone"} && !found(list, "base")->on && !found(list, "winter")->problem.empty());
    // A mod whose folder went: still on in the list, shown missing, and can be turned off.
    GIANT_CHECK(!set_mod_on(w.folder, "base", true));
    std::filesystem::remove_all(w.folder / "mods" / "alone");
    list = read_mods(w.folder);
    GIANT_CHECK(list.missing == std::vector<std::string>{"alone"} && list.order == std::vector<std::string>({"base", "winter"}));
    GIANT_CHECK(!set_mod_on(w.folder, "alone", false) && read_mods(w.folder).missing.empty());
}

GIANT_TEST("mods: folders that are no mod are listed broken with why, and cannot be turned on") {
    const ModWorld w("giant_mods_test_broken");
    w.put(mod("good"));
    w.put("Bad-Name", world::save_mod(mod("x")));
    w.put("other", world::save_mod(mod("another"))); // the id is not the folder's name
    w.put("garbled", "{ not json");
    w.put("typo", R"({"format":"giant-mod","format_version":1,"id":"typo","name":"T","version":"1.0.0","neds":[]})");
    w.put("huge", std::string(world::k_max_mod_file_bytes + 1, ' '));
    std::filesystem::create_directories(w.folder / "mods" / "empty");
    const ModList list = read_mods(w.folder);
    GIANT_CHECK(list.mods.size() == 1 && list.mods[0].mod.id == "good" && list.broken.size() == 6);
    const auto why = [&](const std::string& folder) {
        for (const ModList::Broken& b : list.broken) {
            if (b.folder == folder) {
                return b.error;
            }
        }
        return std::string();
    };
    GIANT_CHECK(why("Bad-Name").find("named by its id") != std::string::npos);
    GIANT_CHECK(why("other").find("says its id is \"another\"") != std::string::npos);
    GIANT_CHECK(why("garbled").find("not JSON") != std::string::npos);
    GIANT_CHECK(why("typo").find("neds: unknown key") != std::string::npos);
    GIANT_CHECK(why("huge").find("could not be read") != std::string::npos);
    GIANT_CHECK(why("empty").find("no mod.json") != std::string::npos);
    for (usize i = 1; i < list.broken.size(); ++i) {
        GIANT_CHECK(list.broken[i - 1].folder < list.broken[i].folder);
    }
    GIANT_CHECK(set_mod_on(w.folder, "typo", true).value_or("").find("neds: unknown key") != std::string::npos);
    GIANT_CHECK(set_mod_on(w.folder, "nowhere", true).value_or("").find("there is no mod \"nowhere\"") != std::string::npos);
    GIANT_CHECK(set_mod_on(w.folder, "../good", true).value_or("").find("no mod's id") != std::string::npos);
    GIANT_CHECK(!std::filesystem::exists(w.folder / "mods" / "enabled.json")); // nothing was written
}

GIANT_TEST("mods: a list of those on that cannot be read turns none on and is never overwritten") {
    const ModWorld w("giant_mods_test_list");
    w.put(mod("good"));
    w.enabled("{ broken");
    const ModList list = read_mods(w.folder);
    GIANT_CHECK(list.list_error.find("enabled.json") != std::string::npos && list.mods.size() == 1 && !list.mods[0].on && list.order.empty());
    GIANT_CHECK(set_mod_on(w.folder, "good", true).value_or("").find("mend it") != std::string::npos);
    GIANT_CHECK(set_mod_on(w.folder, "good", false).value_or("").find("mend it") != std::string::npos);
    GIANT_CHECK(read_text_file(w.folder / "mods" / "enabled.json") == std::string("{ broken"));
    // Written by hand, unsorted: read, and written sorted at the next change.
    w.put(mod("also"));
    w.enabled(R"({"format":"giant-mods","format_version":1,"enabled":["good","also"]})");
    GIANT_CHECK(read_mods(w.folder).order == std::vector<std::string>({"also", "good"}));
    GIANT_CHECK(!set_mod_on(w.folder, "also", false) &&
                read_text_file(w.folder / "mods" / "enabled.json") == world::save_enabled_mods(std::vector<std::string>{"good"}));
}

GIANT_TEST("mods: more folders than there may be mods are not all looked at, and it is said") {
    const ModWorld w("giant_mods_test_many");
    for (usize i = 0; i < world::k_max_mods + 3; ++i) {
        std::filesystem::create_directories(w.folder / "mods" / ("m" + std::to_string(1000 + i)));
    }
    const ModList list = read_mods(w.folder);
    GIANT_CHECK(list.too_many && list.mods.empty() && list.broken.size() == world::k_max_mods && list.broken.front().folder == "m1000");
}

GIANT_TEST("mods: list_mods and enable_mod") {
    const ModWorld w("giant_mods_test_commands");
    ai::CommandRegistry commands;
    EditorState state;
    register_mod_commands(commands, state);
    // No world: said plainly.
    GIANT_CHECK(commands.execute("list_mods").error.find("open one first") != std::string::npos);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "a"}}).error.find("open one first") != std::string::npos);
    state.mods_world = path_to_utf8(w.folder);
    Json list = commands.execute("list_mods").value;
    GIANT_CHECK(list["mods"].empty() && list["broken"].empty() && list["missing"].empty() && list["load_order"].empty() &&
                !list.contains("error") && list["folder"].get<std::string>().ends_with("mods"));
    world::Mod winter = mod("winter", {1, 2, 0}, {{"base", {2, 0, 0}}});
    winter.author = "Someone";
    winter.description = "Snow.";
    winter.after = {"roads"};
    w.put(winter);
    w.put(mod("base", {1, 9, 9}));
    w.put("garbled", "{");
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["mods"].size() == 2 && list["broken"].size() == 1 && list["broken"][0]["folder"] == "garbled");
    const Json* m = listed(list["mods"], "winter");
    GIANT_CHECK(m != nullptr && (*m)["name"] == "The winter" && (*m)["version"] == "1.2.0" && (*m)["author"] == "Someone" &&
                (*m)["description"] == "Snow." && (*m)["needs"][0]["id"] == "base" && (*m)["needs"][0]["at_least"] == "2.0.0" &&
                (*m)["after"][0] == "roads" && (*m)["enabled"] == false && (*m)["loaded"] == false && !m->contains("place") &&
                !m->contains("problem"));
    // On, and not loaded for its need: the answer says why; with the need there and new enough, loaded.
    Json on = commands.execute("enable_mod", {{"id", "winter"}}).value;
    GIANT_CHECK(on["enabled"] == true && on["loaded"] == false && on["problem"].get<std::string>().find("not on") != std::string::npos);
    on = commands.execute("enable_mod", {{"id", "base"}}).value;
    GIANT_CHECK(on["loaded"] == true && on["load_order"] == Json::array({"base"}));
    list = commands.execute("list_mods").value;
    GIANT_CHECK((*listed(list["mods"], "winter"))["problem"].get<std::string>().find("at 2.0.0 or newer, and 1.9.9 is there") != std::string::npos);
    w.put(mod("base", {2, 0, 0}));
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["load_order"] == Json::array({"base", "winter"}) && (*listed(list["mods"], "winter"))["place"] == 2 &&
                (*listed(list["mods"], "base"))["place"] == 1 && (*listed(list["mods"], "winter"))["loaded"] == true);
    // Off again; what is not there cannot be turned on; a broken one says why.
    const Json off = commands.execute("enable_mod", {{"id", "base"}, {"enabled", false}}).value;
    GIANT_CHECK(off["enabled"] == false && off["loaded"] == false && off["load_order"].empty());
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "nowhere"}}).error.find("there is no mod") != std::string::npos);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "garbled"}}).error.find("not JSON") != std::string::npos);
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", ""}}).ok && !commands.execute("enable_mod").ok &&
                !commands.execute("enable_mod", {{"id", "base"}, {"on", true}}).ok);
    // The list that cannot be read is said by list_mods, and enable_mod leaves it be.
    w.enabled("[]");
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["error"].get<std::string>().find("enabled.json") != std::string::npos && list["load_order"].empty());
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", "base"}}).ok && read_text_file(w.folder / "mods" / "enabled.json") == std::string("[]"));
    // list_mods changes nothing; enable_mod is not for the undo history.
    GIANT_CHECK(commands.find("list_mods")->read_only && !commands.find("enable_mod")->read_only);
}
