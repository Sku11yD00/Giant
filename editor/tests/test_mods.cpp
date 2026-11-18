// The mods beside a world (mods.h) and their commands: list_mods, enable_mod.
#include "../src/editor_commands.h"
#include "../src/game_saves.h"
#include "../src/mesh_library.h"
#include "../src/mods.h"
#include "../src/world_streaming.h"

#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/world/mods.h"
#include "giant/world/people.h"
#include "giant/world/scene.h"

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
    world::ModsOn on;
    GIANT_CHECK(!world::load_enabled_mods(text, on) && on.ids == std::vector<std::string>({"alone", "base", "winter"}) &&
                text == world::save_enabled_mods(on));
    // Each was given its slot as it was turned on, and keeps it when turned off.
    GIANT_CHECK(on.slots.size() == 3 && found(list, "alone")->slot && found(list, "base")->slot && found(list, "winter")->slot);
    const std::optional<u32> base_slot = found(list, "base")->slot;
    GIANT_CHECK(!set_mod_on(w.folder, "base", true) && read_text_file(w.folder / "mods" / "enabled.json") == text); // on already
    GIANT_CHECK(!set_mod_on(w.folder, "base", false) && found(read_mods(w.folder), "base")->slot == base_slot);
    list = read_mods(w.folder);
    GIANT_CHECK(list.order == std::vector<std::string>{"alone"} && !found(list, "base")->on && !found(list, "winter")->problem.empty());
    // A mod whose folder went: still on in the list, shown missing, and can be turned off.
    GIANT_CHECK(!set_mod_on(w.folder, "base", true));
    std::filesystem::remove_all(w.folder / "mods" / "alone");
    list = read_mods(w.folder);
    GIANT_CHECK(list.missing == std::vector<std::string>{"alone"} && list.order == std::vector<std::string>({"base", "winter"}));
    GIANT_CHECK(!set_mod_on(w.folder, "alone", false) && read_mods(w.folder).missing.empty());
    // Off, and off already (nothing there by the name at all): nothing to do, and no trouble.
    GIANT_CHECK(!set_mod_on(w.folder, "never_there", false));
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
    // Folders that are not mods' and not listed: tools' and ones set aside.
    std::filesystem::create_directories(w.folder / "mods" / ".git");
    w.put("_off", world::save_mod(mod("x")));
    w.put("__MACOSX", "junk");
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
    // Meant in other letters: the folder that is wrongly named is pointed at.
    w.put("Winter", world::save_mod(mod("winter")));
    GIANT_CHECK(set_mod_on(w.folder, "winter", true).value_or("").find("the folder \"Winter\" cannot be used") != std::string::npos);
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
    // A folder where the list should be, and a list too long to be one: the same.
    std::filesystem::remove(w.folder / "mods" / "enabled.json");
    std::filesystem::create_directories(w.folder / "mods" / "enabled.json" / "inside");
    GIANT_CHECK(!read_mods(w.folder).list_error.empty() && set_mod_on(w.folder, "good", true) &&
                std::filesystem::is_directory(w.folder / "mods" / "enabled.json" / "inside"));
    std::filesystem::remove_all(w.folder / "mods" / "enabled.json");
    w.enabled(std::string(world::k_max_mod_file_bytes + 1, ' '));
    GIANT_CHECK(!read_mods(w.folder).list_error.empty() && set_mod_on(w.folder, "good", true) &&
                std::filesystem::file_size(w.folder / "mods" / "enabled.json") == world::k_max_mod_file_bytes + 1);
    // Written by hand, unsorted: read, and written sorted at the next change.
    w.put(mod("also"));
    w.enabled(R"({"format":"giant-mods","format_version":1,"enabled":["good","also"]})");
    GIANT_CHECK(read_mods(w.folder).order == std::vector<std::string>({"also", "good"}));
    GIANT_CHECK(!set_mod_on(w.folder, "also", false) &&
                read_text_file(w.folder / "mods" / "enabled.json") == world::save_enabled_mods({{"good"}, {}}));
}

GIANT_TEST("mods: more folders than there may be mods are not all looked at, and it is said") {
    const ModWorld w("giant_mods_test_many");
    for (usize i = 0; i < world::k_max_mods + 3; ++i) {
        std::filesystem::create_directories(w.folder / "mods" / ("m" + std::to_string(1000 + i)));
    }
    const ModList list = read_mods(w.folder);
    GIANT_CHECK(list.too_many && list.mods.empty() && list.broken.size() == world::k_max_mods && list.broken.front().folder == "m1000");
    // One past those looked at cannot be turned on, and why is said.
    w.put(mod("zz_last"));
    GIANT_CHECK(set_mod_on(w.folder, "zz_last", true).value_or("").find("more folders there than the 256") != std::string::npos);
    ai::CommandRegistry commands;
    EditorState state;
    register_mod_commands(commands, state);
    state.mods_world = path_to_utf8(w.folder);
    GIANT_CHECK(commands.execute("list_mods").value["too_many"] == true);
}

GIANT_TEST("mods: a mods folder that cannot be read through is said, not shown as empty") {
    const ModWorld w("giant_mods_test_unread");
    GIANT_CHECK(write_text_file(w.folder / "mods", "a file where the folder should be"));
    const ModList list = read_mods(w.folder);
    GIANT_CHECK(list.mods.empty() && list.folder_error.find("could not be read through") != std::string::npos);
    GIANT_CHECK(set_mod_on(w.folder, "good", true).value_or("").find("could not be read through") != std::string::npos);
    ai::CommandRegistry commands;
    EditorState state;
    register_mod_commands(commands, state);
    state.mods_world = path_to_utf8(w.folder);
    GIANT_CHECK(commands.execute("list_mods").value.contains("folder_error"));
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
    // On, and its folder gone since: turning it on again says there is none, and list_mods shows it missing.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "winter"}}).ok);
    std::filesystem::remove_all(w.folder / "mods" / "winter");
    const Json gone = commands.execute("enable_mod", {{"id", "winter"}}).value;
    GIANT_CHECK(gone["enabled"] == true && gone["loaded"] == false && gone["problem"].get<std::string>().find("no such mod") != std::string::npos);
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["missing"] == Json::array({"winter"}) && !list.contains("too_many") && !list.contains("folder_error"));
    // The list that cannot be read is said by list_mods, and enable_mod leaves it be.
    w.enabled("[]");
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["error"].get<std::string>().find("enabled.json") != std::string::npos && list["load_order"].empty());
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", "base"}}).ok && read_text_file(w.folder / "mods" / "enabled.json") == std::string("[]"));
    // list_mods changes nothing; enable_mod is not for the undo history.
    GIANT_CHECK(commands.find("list_mods")->read_only && !commands.find("enable_mod")->read_only);
}

namespace {

world::Place place(const char* name, double x, const char* tag = "home") {
    world::Place p;
    p.name = name;
    p.position = {x, 0.0, -x};
    p.tags = {tag};
    return p;
}

world::PersonLook look(const char* name, float height = 1.0f) {
    world::PersonLook l;
    l.name = name;
    l.mesh = "people/villager.glb";
    l.height = height;
    return l;
}

world::Routine routine(const char* name, const char* place_name) {
    world::Routine r;
    r.name = name;
    r.entries = {{6.0f, "work", world::RoutineAt::place, place_name, 1.5f}};
    return r;
}

// A saved world with two places, a look, a routine and a person using them; closed again.
struct SavedWorld : ModWorld {
    WorldStreaming streaming;
    world::Scene scene;
    explicit SavedWorld(const char* name) : ModWorld(name) {
        scene.places = {place("cottage", 10), place("smithy", 50, "work")};
        scene.person_looks = {look("villager")};
        scene.person_looks[0].mesh = "people/villager.glb";
        scene.routines = {routine("smith's day", "smithy")};
        world::Person someone;
        someone.id = 1;
        someone.look = "villager";
        someone.home = "cottage";
        someone.routine = "smith's day";
        scene.people = {someone};
        scene.next_person_id = 2;
        GIANT_CHECK(streaming.create(folder, scene, 1, {}).error.empty());
        streaming.close();
    }
    void put_world(const std::string& id, const world::ModWorld& changes) const {
        put(mod(id));
        GIANT_CHECK(write_text_file(folder / "mods" / id / "world.json", world::save_mod_world(changes)));
        GIANT_CHECK(!set_mod_on(folder, id, true));
    }
    bool open() { return !streaming.open(folder, scene, 1); }
    // What the world's file holds now.
    world::WorldIndex file() const {
        world::WorldIndex index;
        GIANT_CHECK(!world::load_world_index(read_text_file(folder / "world.json").value_or(""), index));
        return index;
    }
};

} // namespace

GIANT_TEST("mods: a world opens with its mods laid over it, and its own file stays its own when saved") {
    SavedWorld w("giant_mods_test_laid");
    const world::WorldIndex authored = w.file();
    world::ModWorld winter;
    winter.places = {place("tavern", 30, "rest"), place("smithy", 55, "work")};
    winter.person_looks = {look("guard", 1.2f)};
    winter.person_looks[0].mesh = "mods_art/guard.glb";
    winter.routines = {routine("smith's day", "tavern")};
    w.put_world("winter", winter);
    GIANT_CHECK(w.open());
    // The scene is the world with the mod over it; the index is still the file's.
    GIANT_CHECK(w.scene.places.size() == 3 && world::find_place(w.scene.places, "smithy")->position.x == 55.0 &&
                world::find_place(w.scene.places, "tavern") != nullptr);
    GIANT_CHECK(w.scene.person_looks.size() == 2 && w.scene.person_looks[1].mesh == "mods_art/guard.glb");
    GIANT_CHECK(w.scene.routines[0].entries[0].name == "tavern" && w.streaming.index().places == authored.places);
    const ModsLaid& laid = w.streaming.mods_laid();
    GIANT_CHECK(laid.any && laid.mods == std::vector<std::string>{"winter"} && laid.applied.changes.size() == 4 && laid.applied.problems.empty());
    // What the mod brought, each as it left it (for the editor to import its looks' meshes, and to tell the world's own).
    GIANT_CHECK(laid.places.size() == 2 && laid.person_looks.size() == 1 && laid.person_looks[0].mesh == "mods_art/guard.glb" &&
                laid.routines.size() == 1 && laid.behaviours.empty() && laid.files.size() == 1 && laid.files[0].there);
    // Nothing is unsaved, and saving writes the world's own back as it was: no thing, and no mesh, of the mod's.
    GIANT_CHECK(!w.streaming.unsaved_settings(w.scene));
    WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && w.file().places == authored.places &&
                w.file().person_looks == authored.person_looks && w.file().routines == authored.routines);
    GIANT_CHECK(w.file().meshes == std::vector<std::string>{"people/villager.glb"});
    // What is the world's own is saved as it is changed, made and removed; what is the mod's is not the
    // world's to save, changed or not: the world's own of the name stays as its file had it, and it is said.
    w.scene.places[0].radius = 3.0f;             // the world's own cottage
    w.scene.places.push_back(place("well", -5)); // new
    w.scene.places[1].yaw = 45.0f;               // the mod's smithy (the world has its own of the name)
    w.scene.places[2].radius = 9.0f;             // the mod's tavern (the world has none)
    GIANT_CHECK(w.streaming.unsaved_settings(w.scene));
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept == std::vector<std::string>({"smithy", "tavern"}) && !w.streaming.unsaved_settings(w.scene));
    world::WorldIndex file = w.file();
    GIANT_CHECK(file.places.size() == 3 && file.places[0].radius == 3.0f && file.places[1] == authored.places[1] && file.places[2].name == "well");
    GIANT_CHECK(file.person_looks == authored.person_looks && file.routines == authored.routines);
    // Whatever the way there, the same scene saves the same file: the mod's things removed, saved, put back, saved.
    const world::Scene with_all = w.scene;
    std::erase_if(w.scene.places, [](const world::Place& p) { return p.name == "tavern" || p.name == "smithy"; });
    w.scene.routines[0].entries[0].name = "cottage"; // (the mod's routine, changed)
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.size() == 3 && w.file().places == file.places && w.file().routines == authored.routines);
    w.scene.places = with_all.places;
    w.scene.routines = with_all.routines;
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty() && w.file().places == file.places && w.file().routines == authored.routines);
    // One of the world's own removed while the mod is on is gone from the file.
    std::erase_if(w.scene.places, [](const world::Place& p) { return p.name == "well"; });
    GIANT_CHECK(w.streaming.unsaved_settings(w.scene) && w.streaming.save(w.scene, {}).error.empty() && w.file().places.size() == 2);
    // Opened again: the world as saved with the mod over it, the mod's things as the mod has them.
    w.streaming.close();
    GIANT_CHECK(w.open() && w.scene.places.size() == 3 && world::find_place(w.scene.places, "smithy")->yaw == 0.0f &&
                world::find_place(w.scene.places, "smithy")->position.x == 55.0 && w.scene.places[0].radius == 3.0f &&
                world::find_place(w.scene.places, "tavern")->radius == 1.0f);
    // With the mod off, the world is its own, its smithy where it always was.
    w.streaming.close();
    GIANT_CHECK(!set_mod_on(w.folder, "winter", false) && w.open());
    GIANT_CHECK(!w.streaming.mods_laid().any && w.streaming.mods_laid().mods.empty() && w.scene.places.size() == 2 &&
                w.scene.places[1] == authored.places[1] && w.scene.person_looks.size() == 1 && w.scene.routines[0].entries[0].name == "smithy");
}

GIANT_TEST("mods: a world is not saved while its people need what only a mod brings") {
    SavedWorld w("giant_mods_test_needs");
    world::ModWorld guards;
    guards.person_looks = {look("guard", 1.2f)};
    guards.places = {place("barracks", 70)};
    world::Behaviour brave;
    brave.name = "brave";
    brave.states.resize(1);
    brave.states[0].name = "stand";
    guards.behaviours = {brave};
    guards.routines = {routine("watch", "barracks")};
    w.put_world("guards", guards);
    GIANT_CHECK(w.open() && w.scene.behaviours.size() == 1);
    const std::string before = read_text_file(w.folder / "world.json").value_or("");
    const world::Person as_was = w.scene.people[0];
    for (int what = 0; what < 4; ++what) {
        w.scene.people[0] = as_was;
        (what == 0 ? w.scene.people[0].look : what == 1 ? w.scene.people[0].home : what == 2 ? w.scene.people[0].behaviour : w.scene.people[0].routine) =
            what == 0 ? "guard" : what == 1 ? "barracks" : what == 2 ? "brave" : "watch";
        const std::string error = w.streaming.save(w.scene, {}).error;
        GIANT_CHECK(error.find("only a mod brings") != std::string::npos && read_text_file(w.folder / "world.json") == before);
    }
    // Changing the mod's look does not make it the world's; a look of the world's own under another name does.
    w.scene.people[0] = as_was;
    w.scene.people[0].look = "guard";
    w.scene.person_looks[1].height = 1.25f;
    GIANT_CHECK(!w.streaming.save(w.scene, {}).error.empty());
    w.scene.person_looks.push_back(look("town guard", 1.25f));
    w.scene.people[0].look = "town guard";
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty() && w.file().person_looks.size() == 2 && w.file().people[0].look == "town guard");
    w.streaming.close();
    GIANT_CHECK(!set_mod_on(w.folder, "guards", false) && w.open() && w.scene.person_looks.size() == 2 && w.scene.places.size() == 2);
}

GIANT_TEST("mods: mods clash and fail in the open, and list_mods tells what they did to the world") {
    SavedWorld w("giant_mods_test_told");
    world::ModWorld a, b;
    a.places = {place("tavern", 30, "rest")};
    b.places = {place("tavern", 31, "rest"), place("mill", 90, "work")};
    w.put_world("a_first", a);
    w.put_world("b_second", b);
    w.put(mod("c_broken"));
    GIANT_CHECK(write_text_file(w.folder / "mods" / "c_broken" / "world.json", "{ not json") && !set_mod_on(w.folder, "c_broken", true));
    w.put(mod("d_plain")); // (no world.json: changes nothing)
    GIANT_CHECK(!set_mod_on(w.folder, "d_plain", true));
    GIANT_CHECK(w.open() && world::find_place(w.scene.places, "tavern")->position.x == 31.0 && w.scene.places.size() == 4);
    ai::CommandRegistry commands;
    EditorState state;
    state.world = &w.streaming;
    register_mod_commands(commands, state);
    Json told = commands.execute("list_mods").value["world"];
    GIANT_CHECK(told["mods"] == Json::array({"a_first", "b_second", "c_broken", "d_plain"}) && told["added"] == 2 && told["replaced_world"] == 0 &&
                told["clashes"] == 1 && told["reopen"] == false);
    GIANT_CHECK(told["changes"].size() == 3 && told["changes"][0]["did"] == "replaced_mod" && told["changes"][0]["mod"] == "b_second" &&
                told["changes"][0]["over"] == "a_first" && told["changes"][0]["thing"] == "place" && told["changes"][0]["name"] == "tavern");
    GIANT_CHECK(told["changes"][1]["did"] == "added" && !told["changes"][1].contains("over"));
    GIANT_CHECK(told["problems"].size() == 1 && told["problems"][0]["mod"] == "c_broken" &&
                told["problems"][0]["problem"].get<std::string>().find("world.json") != std::string::npos);
    const auto reopen = [&commands] { return commands.execute("list_mods").value["world"]["reopen"] == true; };
    // A mod turned off since: the world is still as it was opened, and that is said (until it is on again).
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "b_second"}, {"enabled", false}}).ok && w.scene.places.size() == 4);
    told = commands.execute("list_mods").value["world"];
    GIANT_CHECK(told["reopen"] == true && told["mods"].size() == 4);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "b_second"}, {"enabled", true}}).ok && !reopen());
    // A world.json where a mod had none, and gone again.
    std::error_code error;
    GIANT_CHECK(write_text_file(w.folder / "mods" / "d_plain" / "world.json", world::save_mod_world(a)) && reopen());
    GIANT_CHECK(std::filesystem::remove(w.folder / "mods" / "d_plain" / "world.json", error) && !reopen());
    // A mod's world.json written since: the world is not what the mods now give.
    world::ModWorld more = a;
    more.places.push_back(place("an inn much further off along the road", 500, "rest"));
    GIANT_CHECK(write_text_file(w.folder / "mods" / "a_first" / "world.json", world::save_mod_world(more)) && reopen());
    // No world open: nothing of this, and no failure for another world's mods.
    w.streaming.close();
    GIANT_CHECK(!commands.execute("list_mods").ok);
    state.mods_world = path_to_utf8(w.folder);
    GIANT_CHECK(!commands.execute("list_mods").value.contains("world"));
}

GIANT_TEST("mods: save_world never lists a mod's mesh as the world's, and names what it did not save") {
    SavedWorld w("giant_mods_test_commands");
    world::ModWorld winter;
    winter.person_looks = {look("villager", 1.1f), look("guard", 1.2f)};
    winter.person_looks[0].mesh = "mods_art/winter_villager.glb";
    winter.person_looks[1].mesh = "mods_art/guard.glb";
    winter.places = {place("tavern", 30, "rest")};
    w.put_world("winter", winter);
    ai::CommandRegistry commands;
    EditorState state;
    state.world = &w.streaming;
    register_scene_commands(commands, state);
    GIANT_CHECK(commands.execute("open_world", {{"folder", path_to_utf8(w.folder)}}).ok && state.scene.person_looks.size() == 2);
    // The world's person, near the camera, stands in the scene wearing the mod's look.
    world::Entity body;
    body.shape = world::Shape::mesh;
    body.mesh = "mods_art/winter_villager.glb";
    body.person = 1;
    state.scene.add(body);
    auto saved = commands.execute("save_world");
    GIANT_CHECK(saved.ok && !saved.value.contains("mods_things_not_saved"));
    GIANT_CHECK(w.file().meshes == std::vector<std::string>{"people/villager.glb"} && w.file().person_looks.size() == 1 &&
                w.file().person_looks[0].mesh == "people/villager.glb");
    // What was done to the mod's things is not saved, and they are named.
    state.scene.person_looks[1].height = 1.3f;
    std::erase_if(state.scene.places, [](const world::Place& p) { return p.name == "tavern"; });
    saved = commands.execute("save_world");
    GIANT_CHECK(saved.ok && saved.value["mods_things_not_saved"] == Json::array({"tavern", "guard"}) && saved.value["mods_things_not_saved_count"] == 2);
    GIANT_CHECK(w.file().meshes == std::vector<std::string>{"people/villager.glb"} && w.file().places.size() == 2);
}

GIANT_TEST("mods: a mod's people come under ids of its own, stay the same whatever else is on, and are never saved as the world's") {
    SavedWorld w("giant_mods_test_people");
    world::ModWorld guards;
    guards.person_looks = {look("guard", 1.2f)};
    guards.places = {place("barracks", 70)};
    world::Person captain;
    captain.id = 1;
    captain.name = "Captain";
    captain.look = "guard";
    captain.home = "barracks";
    world::Person helper = captain;
    helper.id = 4;
    helper.name = "Helper";
    helper.look = "villager"; // (the world's)
    helper.home = "cottage";
    helper.routine = "smith's day";
    guards.people = {captain, helper};
    w.put_world("guards", guards);
    const std::optional<u32> slot = found(read_mods(w.folder), "guards")->slot;
    GIANT_CHECK(slot.has_value());
    const world::PersonId captain_id = world::mod_person_id(*slot, 1), helper_id = world::mod_person_id(*slot, 4);
    GIANT_CHECK(w.open() && w.scene.people.size() == 3 && w.scene.people[0].id == 1 && w.scene.people[1].id == captain_id &&
                w.scene.people[1].name == "Captain" && w.scene.people[2].id == helper_id && w.scene.next_person_id == 2);
    const ModsLaid& laid = w.streaming.mods_laid();
    GIANT_CHECK(laid.any && laid.people.size() == 2 && laid.people[0].id == captain_id && laid.slots == std::vector<u32>{*slot} &&
                laid.applied.people == std::vector<world::ModPeople>({{0, 2}}));
    // Nothing is unsaved; the world's file keeps its own person alone.
    const world::WorldIndex authored = w.file();
    GIANT_CHECK(!w.streaming.unsaved_settings(w.scene));
    WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && w.file().people == authored.people && w.file().next_person_id == 2);
    // A mod's person changed is not unsaved, not saved, and named; one of the world's own made is saved.
    w.scene.people[1].name = "Major";
    GIANT_CHECK(!w.streaming.unsaved_settings(w.scene));
    world::Person made = w.scene.people[0];
    made.id = 2;
    made.name = "Newcomer";
    w.scene.people.insert(w.scene.people.begin() + 1, made);
    w.scene.next_person_id = 3;
    GIANT_CHECK(w.streaming.unsaved_settings(w.scene));
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept == std::vector<std::string>{"person " + std::to_string(captain_id)} &&
                !w.streaming.unsaved_settings(w.scene));
    GIANT_CHECK(w.file().people.size() == 2 && w.file().people[1] == made && w.file().next_person_id == 3);
    // list_mods tells whose people they are.
    ai::CommandRegistry commands;
    EditorState state;
    state.world = &w.streaming;
    register_mod_commands(commands, state);
    Json told = commands.execute("list_mods").value;
    GIANT_CHECK(told["world"]["people_added"] == 2 && told["world"]["people"] == Json::array({{{"mod", "guards"}, {"count", 2}, {"ids_from", captain_id}}}) &&
                (*listed(told["mods"], "guards"))["slot"] == *slot);
    // Another mod on, loading before it: the same people under the same ids, the mod's as the mod has them.
    w.streaming.close();
    world::ModWorld early;
    early.people = {captain};
    early.people[0].look = "villager";
    early.people[0].home = "";
    w.put_world("a_early", early);
    const std::optional<u32> early_slot = found(read_mods(w.folder), "a_early")->slot;
    GIANT_CHECK(early_slot.has_value() && early_slot != slot && w.open() && w.scene.people.size() == 5);
    const world::Person* again = world::find_person(w.scene.people, captain_id);
    GIANT_CHECK(again != nullptr && again->name == "Captain" && world::find_person(w.scene.people, helper_id) != nullptr &&
                world::find_person(w.scene.people, world::mod_person_id(*early_slot, 1)) != nullptr && world::find_person(w.scene.people, 2) != nullptr);
    // Off and on again: its slot, and so its people's ids, are kept.
    w.streaming.close();
    GIANT_CHECK(!set_mod_on(w.folder, "guards", false) && !set_mod_on(w.folder, "guards", true) && found(read_mods(w.folder), "guards")->slot == slot);
    // With the mods off the world has its own two.
    GIANT_CHECK(!set_mod_on(w.folder, "guards", false) && !set_mod_on(w.folder, "a_early", false) && w.open() && w.scene.people.size() == 2 &&
                !w.streaming.mods_laid().any);
    // A list written by hand, without slots: a mod with people is left out and says how to mend it.
    w.streaming.close();
    w.enabled(R"({"format":"giant-mods","format_version":1,"enabled":["guards"]})");
    GIANT_CHECK(w.open() && w.scene.people.size() == 2 && w.scene.places.size() == 2 && w.streaming.mods_laid().applied.problems.size() == 1 &&
                w.streaming.mods_laid().applied.problems[0].message.find("enable_mod") != std::string::npos);
    // Enabled again (on as it is), it is given a slot, and list_mods says to open the world again for it.
    GIANT_CHECK(commands.execute("list_mods").value["world"]["reopen"] == false);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}}).ok && found(read_mods(w.folder), "guards")->slot.has_value());
    GIANT_CHECK(commands.execute("list_mods").value["world"]["reopen"] == true);
    w.streaming.close();
    GIANT_CHECK(w.open() && w.scene.people.size() == 4 && w.streaming.mods_laid().applied.problems.empty());
    // One of the mod's people removed and the world saved: named, and back when the world is opened again.
    w.scene.people.pop_back();
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.size() == 1 && w.file().people.size() == 2);
    w.streaming.close();
    GIANT_CHECK(w.open() && w.scene.people.size() == 4);
}

GIANT_TEST("mods: a mod's own files are found in its folder, and stay out of the world's file") {
    SavedWorld w("giant_mods_test_files");
    world::ModWorld guards;
    guards.person_looks = {look("guard", 1.2f), look("villager", 1.1f)};
    guards.person_looks[0].mesh = "mod:art/guard.glb";
    guards.person_looks[0].material.maps.base_color = "mod:art/guard.png";
    guards.person_looks[0].hair.file = "mod:art/guard_hair.glb";
    guards.person_looks[1].mesh = "shared/winter_villager.glb"; // (not the mod's own: from the folder the game runs in)
    w.put_world("guards", guards);
    const std::string in_mod = MeshLibrary::normalize(path_to_utf8(w.folder / "mods" / "guards")) + "/";
    GIANT_CHECK(w.open() && w.streaming.mods_laid().applied.problems.empty());
    const world::PersonLook* guard = world::find_person_look(w.scene.person_looks, "guard");
    GIANT_CHECK(guard != nullptr && guard->mesh == in_mod + "art/guard.glb" && guard->material.maps.base_color == in_mod + "art/guard.png" &&
                guard->hair.file == in_mod + "art/guard_hair.glb" &&
                world::find_person_look(w.scene.person_looks, "villager")->mesh == "shared/winter_villager.glb");
    // What the editor imports for the mods is those paths; nothing of them is the world's to save.
    GIANT_CHECK(w.streaming.mods_laid().person_looks.size() == 2 && !w.streaming.unsaved_settings(w.scene));
    const WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && w.file().meshes == std::vector<std::string>{"people/villager.glb"} &&
                w.file().person_looks.size() == 1 && w.file().person_looks[0].mesh == "people/villager.glb");
    // A path that would leave the mod's folder, or the folder the game runs in: the mod is left out, whole, and it is said.
    ai::CommandRegistry commands;
    EditorState state;
    state.world = &w.streaming;
    register_mod_commands(commands, state);
    const world::ModWorld sound = guards;
    for (const char* bad : {"mod:../../other/guard.glb", "//server/share/guard.glb", "C:/Users/someone/guard.glb", "../guard.glb"}) {
        w.streaming.close();
        guards.person_looks[0].mesh = bad;
        GIANT_CHECK(write_text_file(w.folder / "mods" / "guards" / "world.json", world::save_mod_world(guards)));
        GIANT_CHECK(w.open() && w.scene.person_looks.size() == 1 && w.streaming.mods_laid().applied.problems.size() == 1 &&
                    w.streaming.mods_laid().applied.problems[0].message.find("person_looks[0].mesh") != std::string::npos);
    }
    // Mended since: list_mods says to open the world again, and then it is laid.
    GIANT_CHECK(commands.execute("list_mods").value["world"]["reopen"] == false);
    GIANT_CHECK(write_text_file(w.folder / "mods" / "guards" / "world.json", world::save_mod_world(sound) + "\n"));
    GIANT_CHECK(commands.execute("list_mods").value["world"]["reopen"] == true);
    w.streaming.close();
    GIANT_CHECK(w.open() && w.scene.person_looks.size() == 2 && w.streaming.mods_laid().applied.problems.empty());
    // A link in the mod's folder (it could lead a file of the mod's out of it): the mod is left out, and it says which.
    w.streaming.close();
    const std::filesystem::path outside = w.folder / "outside";
    const std::filesystem::path art = w.folder / "mods" / "guards" / "art";
    std::error_code error;
    std::filesystem::create_directories(outside, error);
    GIANT_CHECK(write_text_file(outside / "skin.png", "not a picture"));
    std::filesystem::create_directory_symlink(std::filesystem::absolute(outside), art, error);
    if (!error) { // (Windows makes links only for those allowed to)
        const auto left_out = [&w](const char* named) {
            const bool opened = w.open();
            const std::vector<world::ModProblem>& problems = w.streaming.mods_laid().applied.problems;
            const bool said = opened && w.scene.person_looks.size() == 1 && problems.size() == 1 &&
                              problems[0].message.find("is a link") != std::string::npos && problems[0].message.find(named) != std::string::npos;
            w.streaming.close();
            return said;
        };
        GIANT_CHECK(left_out("\"art\""));
        // A real folder in its place, and a file in it that is a link (a glTF's image beside it, say).
        std::filesystem::remove(art, error);
        std::filesystem::create_directories(art / "deep", error);
        std::filesystem::create_symlink(std::filesystem::absolute(outside / "skin.png"), art / "deep" / "skin.png", error);
        GIANT_CHECK(!error && left_out("skin.png"));
        // No link there: laid again.
        std::filesystem::remove(art / "deep" / "skin.png", error);
        GIANT_CHECK(w.open() && w.scene.person_looks.size() == 2 && w.streaming.mods_laid().applied.problems.empty());
    }
}

GIANT_TEST("mods: a mod's kits are laid over the world's, and the world is never saved needing them") {
    SavedWorld w("giant_mods_test_kits");
    const auto kit = [](const char* name, usize pieces, const char* mesh = "") {
        world::Kit k;
        k.name = name;
        k.pieces.resize(pieces);
        if (*mesh != 0) {
            k.pieces[0].shape = world::Shape::mesh;
            k.pieces[0].mesh = mesh;
        }
        return k;
    };
    // The world has a house (placed once) and a shed of its own.
    GIANT_CHECK(w.open());
    w.scene.kits = {kit("house", 2, "kits/house.glb"), kit("shed", 1)};
    world::Entity placed;
    placed.shape = world::Shape::kit;
    placed.kit = "house";
    w.scene.add(placed);
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty() && w.file().kits.size() == 2 &&
                w.file().meshes == std::vector<std::string>({"kits/house.glb", "people/villager.glb"}));
    const world::WorldIndex authored = w.file();
    const u64 without = GameSaves::world_identity(w.streaming);
    w.streaming.close();
    // The mod: another house (its own art), and a watchtower.
    world::ModWorld forts;
    forts.kits = {kit("house", 4, "mod:art/stone_house.glb"), kit("watchtower", 3, "mod:art/tower.glb")};
    w.put_world("forts", forts);
    const std::string in_mod = MeshLibrary::normalize(path_to_utf8(w.folder / "mods" / "forts")) + "/";
    GIANT_CHECK(w.open() && w.streaming.mods_laid().applied.problems.empty() && w.scene.kits.size() == 3);
    const auto pieces = [&w](const char* name) {
        const world::Kit* k = w.scene.find_kit(name);
        return k != nullptr ? k->pieces : std::vector<world::KitPiece>();
    };
    GIANT_CHECK(pieces("house").size() == 4 && pieces("house")[0].mesh == in_mod + "art/stone_house.glb" && pieces("watchtower").size() == 3 &&
                pieces("watchtower")[0].mesh == in_mod + "art/tower.glb" && pieces("shed").size() == 1);
    // A game saved with the mod's pieces is not one of the world without them.
    GIANT_CHECK(GameSaves::world_identity(w.streaming) != without);
    GIANT_CHECK(w.streaming.mods_laid().kits.size() == 2 && w.streaming.mods_laid().applied.changes.size() == 2 && !w.streaming.unsaved_settings(w.scene));
    // Saved: the world's own house as its file had it, no kit and no mesh of the mod's.
    WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && w.file().kits == authored.kits && w.file().meshes == authored.meshes);
    // The world's own shed changed and a barn made: saved. The mod's watchtower changed: not saved, and named.
    for (world::Kit& k : w.scene.kits) {
        if (k.name == "shed") k.breakable = true;
        if (k.name == "watchtower") k.pieces.pop_back();
    }
    w.scene.kits.push_back(kit("barn", 2, "kits/barn.glb"));
    GIANT_CHECK(w.streaming.unsaved_settings(w.scene));
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept == std::vector<std::string>{"watchtower"} && !w.streaming.unsaved_settings(w.scene));
    world::WorldIndex file = w.file();
    GIANT_CHECK(file.kits.size() == 3 && file.kits[0] == authored.kits[0] && file.kits[1].breakable && file.kits[2].name == "barn" &&
                file.meshes == std::vector<std::string>({"kits/barn.glb", "kits/house.glb", "people/villager.glb"}));
    // One of the world's own entities placing a kit only the mod brings: the world would not load without the mod, so it is not saved.
    const std::string before = read_text_file(w.folder / "world.json").value_or("");
    placed.kit = "watchtower";
    const world::EntityId tower_id = w.scene.add(placed);
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.find("places the kit \"watchtower\"") != std::string::npos && saved.error.find("only a mod brings") != std::string::npos &&
                read_text_file(w.folder / "world.json") == before);
    GIANT_CHECK(w.scene.remove(tower_id).has_value());
    // So too a scatter layer of the world's.
    world::ScatterLayer layer;
    layer.name = "towers";
    world::ScatterItem item;
    item.shape = world::Shape::kit;
    item.kit = "watchtower";
    layer.items = {item};
    w.scene.scatter.push_back(layer);
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.find("scatter layer \"towers\"") != std::string::npos && read_text_file(w.folder / "world.json") == before);
    // The house the mod replaced is the world's too: placing it is fine.
    w.scene.scatter.back().items[0].kit = "house";
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && w.file().scatter.size() == 1);
    // So is an entity placing it, and the world saves again after the refusals.
    placed.kit = "house";
    placed.transform.position = {5.0, 0.0, 5.0};
    w.scene.add(placed);
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && !w.streaming.unsaved_settings(w.scene));
    // A kit that is nobody's (in neither list) is as it is without mods: placed, and not there.
    w.scene.scatter.back().items[0].kit = "house";
    placed.kit = "nowhere";
    const world::EntityId lost = w.scene.add(placed);
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty() && w.scene.remove(lost).has_value() && w.streaming.save(w.scene, {}).error.empty());
    // With the mod off, the world is its own.
    w.streaming.close();
    GIANT_CHECK(!set_mod_on(w.folder, "forts", false) && w.open() && w.scene.kits.size() == 3 && pieces("house").size() == 2 &&
                w.scene.find_kit("watchtower") == nullptr && pieces("barn").size() == 2);
}

GIANT_TEST("mods: with a mod on that brings no kit, the world's kits are saved as the scene has them") {
    SavedWorld w("giant_mods_test_no_kits");
    const auto kit = [](const char* name, usize pieces) {
        world::Kit k;
        k.name = name;
        k.pieces.resize(pieces);
        return k;
    };
    GIANT_CHECK(w.open());
    w.scene.kits = {kit("house", 2), kit("shed", 1)};
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty());
    w.streaming.close();
    world::ModWorld winter;
    winter.places = {place("tavern", 30, "rest")};
    w.put_world("winter", winter);
    GIANT_CHECK(w.open() && w.streaming.mods_laid().any && w.streaming.mods_laid().kits.empty() && !w.streaming.unsaved_settings(w.scene));
    // The house deleted and made again comes after the shed: saved so, and nothing is left unsaved.
    w.scene.kits = {kit("shed", 1), kit("house", 3)};
    GIANT_CHECK(w.streaming.unsaved_settings(w.scene));
    const WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && !w.streaming.unsaved_settings(w.scene) && w.file().kits == w.scene.kits);
}

GIANT_TEST("mods: save_world lists no mesh of a mod's kit, and open_world asks for them") {
    SavedWorld w("giant_mods_test_kit_commands");
    world::ModWorld forts;
    forts.kits.emplace_back();
    forts.kits[0].name = "watchtower";
    forts.kits[0].pieces.resize(1);
    forts.kits[0].pieces[0].shape = world::Shape::mesh;
    forts.kits[0].pieces[0].mesh = "mod:art/tower.glb";
    w.put_world("forts", forts);
    EditorState state;
    WorldStreaming streaming;
    MeshLibrary meshes{nullptr};
    state.world = &streaming;
    state.meshes = &meshes;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    const std::string folder = path_to_utf8(w.folder);
    const ai::CommandResult opened = commands.execute("open_world", {{"folder", folder}});
    // (The mod's art is not there: asked for, and told as a mesh that did not load.)
    GIANT_CHECK(opened.ok && state.scene.find_kit("watchtower") != nullptr && opened.value.dump().find("art/tower.glb") != std::string::npos);
    world::Kit barn;
    barn.name = "barn";
    barn.pieces.resize(1);
    barn.pieces[0].shape = world::Shape::mesh;
    barn.pieces[0].mesh = "kits/barn.glb";
    state.scene.kits.push_back(barn);
    const ai::CommandResult saved = commands.execute("save_world");
    GIANT_CHECK(saved.ok && w.file().kits == std::vector<world::Kit>{barn} &&
                w.file().meshes == std::vector<std::string>({"kits/barn.glb", "people/villager.glb"}));
}

GIANT_TEST("mods: a mod's scripts run only where they were allowed on this machine") {
    SavedWorld w("giant_mods_test_scripts");
    const std::filesystem::path settings = std::filesystem::temp_directory_path() / "giant_mods_test_scripts_settings";
    std::error_code error;
    std::filesystem::remove_all(settings, error);
    const std::filesystem::path allowed = settings / "Giant" / "mods_scripts_allowed.json";
    world::Mod guards = mod("guards");
    guards.scripts = "scripts/Guards.dll";
    w.put(guards);
    w.put(mod("plain"));
    world::Mod traders = mod("traders");
    traders.scripts = "Traders.dll";
    w.put(traders);
    GIANT_CHECK(!set_mod_on(w.folder, "guards", true) && !set_mod_on(w.folder, "plain", true) && !set_mod_on(w.folder, "traders", true));
    std::filesystem::create_directories(w.folder / "mods" / "guards" / "scripts");
    GIANT_CHECK(write_text_file(w.folder / "mods" / "guards" / "scripts" / "Guards.dll", "not really an assembly"));
    GIANT_CHECK(w.open() && w.streaming.mods_laid().scripts == std::vector<std::string>({"scripts/Guards.dll", "", "Traders.dll"}));
    const ModsLaid& laid = w.streaming.mods_laid();
    // Nothing allowed (no list at all, none there yet, or one with nothing in it): nothing runs, and each is told why.
    const std::filesystem::path empty_list = settings / "empty.json";
    std::filesystem::create_directories(settings);
    GIANT_CHECK(write_text_file(empty_list, world::save_mods_scripts_allowed({})));
    for (const std::filesystem::path& file : {std::filesystem::path(), allowed, empty_list}) {
        const ModsScripts none = mods_scripts(w.folder, laid, file);
        GIANT_CHECK(none.run.empty() && none.not_run.size() == 2 && none.not_run[0].mod == "guards" &&
                    none.not_run[0].why.find("not allowed on this machine") != std::string::npos && none.not_run[1].mod == "traders");
        GIANT_CHECK(!mod_scripts_allowed(file, w.folder, "guards"));
    }
    // Allowed: written to the machine's list, never into the world's folder.
    GIANT_CHECK(allow_mod_scripts({}, w.folder, "guards", true).value_or("").find("no settings folder") != std::string::npos &&
                !allow_mod_scripts({}, w.folder, "guards", false));
    GIANT_CHECK(allow_mod_scripts(allowed, w.folder, "plain", true).value_or("").find("has no scripts") != std::string::npos &&
                allow_mod_scripts(allowed, w.folder, "nowhere", true).value_or("").find("there is no mod") != std::string::npos &&
                allow_mod_scripts(allowed, w.folder, "No Mod", true).has_value() && !std::filesystem::exists(allowed));
    const std::string list_before = read_text_file(w.folder / "mods" / "enabled.json").value_or("");
    GIANT_CHECK(!allow_mod_scripts(allowed, w.folder, "guards", true) && !allow_mod_scripts(allowed, w.folder, "traders", true) &&
                !allow_mod_scripts(allowed, w.folder, "guards", true) && mod_scripts_allowed(allowed, w.folder, "guards") &&
                !mod_scripts_allowed(allowed, w.folder, "plain") && read_text_file(w.folder / "mods" / "enabled.json") == list_before);
    std::vector<world::ModScriptsAllowed> kept;
    GIANT_CHECK(!world::load_mods_scripts_allowed(read_text_file(allowed).value_or(""), kept) && kept.size() == 2 && kept[0].mod == "guards" &&
                std::filesystem::path(kept[0].world).is_absolute());
    // The same world by another spelling of its folder is the same world; another folder is another.
    GIANT_CHECK(mod_scripts_allowed(allowed, w.folder / "mods" / "..", "guards") && !mod_scripts_allowed(allowed, w.folder / "mods", "guards"));
    ModsScripts found = mods_scripts(w.folder, laid, allowed);
    GIANT_CHECK(found.run.size() == 1 && found.run[0].mod == "guards" &&
                std::filesystem::equivalent(path_from_utf8(found.run[0].path), w.folder / "mods" / "guards" / "scripts" / "Guards.dll", error));
    GIANT_CHECK(found.not_run.size() == 1 && found.not_run[0].mod == "traders" && found.not_run[0].why.find("Traders.dll, are not in its folder") != std::string::npos);
    // A link in the mod's folder: its scripts are not run (what they load from beside them would not be the folder's own).
    std::filesystem::create_symlink(w.folder / "world.json", w.folder / "mods" / "guards" / "scripts" / "linked.json", error);
    if (!error) {
        found = mods_scripts(w.folder, laid, allowed);
        GIANT_CHECK(found.run.empty() && found.not_run.size() == 2 && found.not_run[0].why.find("link") != std::string::npos);
        std::filesystem::remove(w.folder / "mods" / "guards" / "scripts" / "linked.json", error);
    }
    // The mods folder itself a link to elsewhere: nothing runs from it.
    std::filesystem::rename(w.folder / "mods", w.folder / "_elsewhere", error);
    if (!error) {
        std::filesystem::create_directory_symlink(w.folder / "_elsewhere", w.folder / "mods", error);
        if (!error) {
            found = mods_scripts(w.folder, laid, allowed);
            GIANT_CHECK(found.run.empty() && found.not_run[0].why.find("is a link") != std::string::npos);
            std::filesystem::remove(w.folder / "mods", error);
        }
        std::filesystem::rename(w.folder / "_elsewhere", w.folder / "mods", error);
        GIANT_CHECK(!error && mods_scripts(w.folder, laid, allowed).run.size() == 1);
    }
    // Which the main loop has loaded: those of the world's opening while that world is the scene's, anew when one is forbidden.
    found = mods_scripts(w.folder, laid, allowed);
    found.session = 7;
    const ModsScriptsKey key = mods_scripts_key(found, true, 7);
    GIANT_CHECK(!(key == ModsScriptsKey{}) && mods_scripts_key(found, false, 7) == ModsScriptsKey{} && mods_scripts_key(found, true, 8) == ModsScriptsKey{} &&
                mods_scripts_key(ModsScripts{}, true, 0) == ModsScriptsKey{} && mods_scripts_key(found, true, 7) == key);
    GIANT_CHECK(!forbid_mod_scripts_now(found, "traders") && mods_scripts_key(found, true, 7) == key);
    GIANT_CHECK(forbid_mod_scripts_now(found, "guards") && found.run.empty() && found.not_run.size() == 2 &&
                found.not_run[1].why.find("no longer allowed") != std::string::npos && mods_scripts_key(found, true, 7) == ModsScriptsKey{});
    ModsScripts two = mods_scripts(w.folder, laid, allowed);
    two.session = 7;
    two.run.push_back({"other", "x.dll"});
    const ModsScriptsKey both = mods_scripts_key(two, true, 7);
    GIANT_CHECK(forbid_mod_scripts_now(two, "guards") && !(mods_scripts_key(two, true, 7) == both) && !(mods_scripts_key(two, true, 7) == ModsScriptsKey{}));
    // A world whose folder's name the list cannot keep (a line break in it): nothing can be allowed there.
    const std::filesystem::path odd = settings / "two\nlines";
    std::filesystem::create_directories(odd / "mods", error);
    if (!error) {
        GIANT_CHECK(can_allow_mod_scripts(allowed, odd, "guards").value_or("").find("no path the list") != std::string::npos &&
                    !mod_scripts_allowed(allowed, odd, "guards") && !allow_mod_scripts(allowed, odd, "guards", false));
    }
    // A list that cannot be read allows nothing, and is not written over.
    GIANT_CHECK(!allow_mod_scripts(allowed, w.folder, "traders", false) && !mod_scripts_allowed(allowed, w.folder, "traders"));
    GIANT_CHECK(write_text_file(allowed, "{ not json"));
    found = mods_scripts(w.folder, laid, allowed);
    GIANT_CHECK(found.run.empty() && found.not_run[0].why.find("cannot be used") != std::string::npos && !mod_scripts_allowed(allowed, w.folder, "guards") &&
                allow_mod_scripts(allowed, w.folder, "guards", true).value_or("").find("mend it") != std::string::npos &&
                allow_mod_scripts(allowed, w.folder, "guards", false).has_value() && read_text_file(allowed) == std::string("{ not json"));
    std::filesystem::remove_all(settings, error);
}

GIANT_TEST("mods: enable_mod allows a mod's scripts, open_world finds those that run, and list_mods tells") {
    SavedWorld w("giant_mods_test_script_commands");
    const std::filesystem::path settings = std::filesystem::temp_directory_path() / "giant_mods_test_script_commands_settings";
    std::error_code error;
    std::filesystem::remove_all(settings, error);
    world::Mod guards = mod("guards");
    guards.scripts = "Guards.dll";
    w.put(guards);
    w.put(mod("plain"));
    GIANT_CHECK(write_text_file(w.folder / "mods" / "guards" / "Guards.dll", "not really an assembly"));
    // A broken mod's scripts go with it.
    world::Mod broken = mod("broken");
    broken.scripts = "Broken.dll";
    w.put(broken);
    GIANT_CHECK(write_text_file(w.folder / "mods" / "broken" / "Broken.dll", "x") && write_text_file(w.folder / "mods" / "broken" / "world.json", "{ not json"));
    EditorState state;
    WorldStreaming streaming;
    state.world = &streaming;
    state.mods_scripts_allowed_file = path_to_utf8(settings / "allowed.json");
    state.mods_scripts_consent = ModsScriptsConsent::by_command;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_mod_commands(commands, state);
    const Json folder = {{"folder", path_to_utf8(w.folder)}};
    GIANT_CHECK(commands.execute("open_world", folder).ok);
    // Turned on without a word of scripts: not allowed. A mod that is off cannot be allowed.
    Json on = commands.execute("enable_mod", {{"id", "guards"}}).value;
    GIANT_CHECK(on["scripts"] == "Guards.dll" && on["scripts_allowed"] == false);
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", "guards"}, {"enabled", false}, {"scripts", true}}).ok);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "plain"}, {"scripts", true}}).error.find("has no scripts") != std::string::npos);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "broken"}, {"scripts", true}}).value["scripts_allowed"] == true);
    GIANT_CHECK(commands.execute("open_world", folder).ok && state.mods_scripts.session == streaming.session() && state.mods_scripts.run.empty());
    Json list = commands.execute("list_mods").value;
    GIANT_CHECK((*listed(list["mods"], "guards"))["scripts_allowed"] == false && !listed(list["mods"], "plain")->contains("scripts") &&
                list["world"]["scripts"]["run"].empty() && list["world"]["scripts"]["not_run"].size() == 2 && list["world"]["reopen"] == false);
    GIANT_CHECK(list["world"]["scripts"]["not_run"][0]["mod"] == "broken" &&
                list["world"]["scripts"]["not_run"][0]["why"].get<std::string>().find("left out") != std::string::npos);
    // Allowed: said to need the world opened again, and found when it is.
    on = commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).value;
    GIANT_CHECK(on["scripts_allowed"] == true && commands.execute("list_mods").value["world"]["reopen"] == true);
    GIANT_CHECK(commands.execute("open_world", folder).ok && state.mods_scripts.run.size() == 1 && state.mods_scripts.run[0].mod == "guards");
    list = commands.execute("list_mods").value;
    GIANT_CHECK(list["world"]["scripts"]["run"].size() == 1 && list["world"]["scripts"]["run"][0]["mod"] == "guards" && list["world"]["reopen"] == false &&
                (*listed(list["mods"], "guards"))["scripts_allowed"] == true);
    // Turned on again without a word: still allowed. Told not to: taken back. Turned off: taken back too.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}}).value["scripts_allowed"] == true);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", false}}).value["scripts_allowed"] == false &&
                state.mods_scripts.run.empty() && commands.execute("list_mods").value["world"]["reopen"] == false);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).ok &&
                commands.execute("enable_mod", {{"id", "guards"}, {"enabled", false}}).ok &&
                commands.execute("enable_mod", {{"id", "guards"}}).value["scripts_allowed"] == false);
    // Forbidden while the world runs them: stopped at once (no longer among those to run), and nothing is left to open again for.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).ok && commands.execute("open_world", folder).ok &&
                state.mods_scripts.run.size() == 1);
    const ModsScriptsKey running = mods_scripts_key(state.mods_scripts, true, streaming.session());
    on = commands.execute("enable_mod", {{"id", "guards"}, {"scripts", false}}).value;
    GIANT_CHECK(on["enabled"] == true && on["scripts_allowed"] == false && state.mods_scripts.run.empty() &&
                !(mods_scripts_key(state.mods_scripts, true, streaming.session()) == running) &&
                commands.execute("list_mods").value["world"]["reopen"] == false);
    // Only told to forbid: a mod that is off stays off.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"enabled", false}}).ok);
    on = commands.execute("enable_mod", {{"id", "guards"}, {"scripts", false}}).value;
    GIANT_CHECK(on["enabled"] == false && !read_mods(w.folder).mods.empty() && !found(read_mods(w.folder), "guards")->on);
    // It is the person's to allow: where the editor asks them, a command only asks, and allows nothing.
    state.mods_scripts_consent = ModsScriptsConsent::ask;
    on = commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).value;
    GIANT_CHECK(on["enabled"] == true && on["scripts_allowed"] == false && on["scripts_asked"] == true && state.mods_scripts_request &&
                state.mods_scripts_request->mod == "guards" && path_from_utf8(state.mods_scripts_request->world) == w.folder);
    GIANT_CHECK((*listed(commands.execute("list_mods").value["mods"], "guards"))["scripts_asked"] == true &&
                !mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "guards"));
    // (What cannot be allowed is not asked about; forbidding takes the question back.)
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", "plain"}, {"scripts", true}}).ok && state.mods_scripts_request->mod == "guards");
    // One question at a time: what the person is reading is not changed under them. The same asked again is the same question.
    world::Mod scouts = mod("scouts");
    scouts.scripts = "Scouts.dll";
    w.put(scouts);
    GIANT_CHECK(write_text_file(w.folder / "mods" / "scouts" / "Scouts.dll", "x"));
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "scouts"}, {"scripts", true}}).error.find("still being asked") != std::string::npos &&
                state.mods_scripts_request->mod == "guards" && commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).ok &&
                state.mods_scripts_request->mod == "guards");
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", false}}).ok && !state.mods_scripts_request);
    // The person's answer: no allows nothing, yes allows the mod and world they were asked about, and there is nothing to answer twice.
    GIANT_CHECK(answer_mods_scripts_request(state, true).find("nothing was asked") != std::string::npos);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "scouts"}, {"scripts", true}}).value["scripts_asked"] == true);
    GIANT_CHECK(answer_mods_scripts_request(state, false).find("not allowed") != std::string::npos && !state.mods_scripts_request &&
                !mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "scouts"));
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "scouts"}, {"scripts", true}}).value["scripts_asked"] == true);
    GIANT_CHECK(answer_mods_scripts_request(state, true).find("are allowed") != std::string::npos && !state.mods_scripts_request &&
                mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "scouts") &&
                !mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "guards"));
    on = commands.execute("enable_mod", {{"id", "scouts"}, {"scripts", true}}).value;
    GIANT_CHECK(on["scripts_allowed"] == true && !on.contains("scripts_asked") && !state.mods_scripts_request);
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "scouts"}, {"enabled", false}}).ok &&
                !mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "scouts"));
    // What is no mod's id is refused before anything is done.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "../guards"}, {"scripts", false}}).error.find("no mod's id") != std::string::npos);
    // Where there is no window to ask in, none can be allowed.
    state.mods_scripts_consent = ModsScriptsConsent::none;
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).error.find("no window to ask") != std::string::npos &&
                !state.mods_scripts_request && !mod_scripts_allowed(path_from_utf8(state.mods_scripts_allowed_file), w.folder, "guards"));
    // One already allowed needs no asking again.
    state.mods_scripts_consent = ModsScriptsConsent::by_command;
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).ok);
    state.mods_scripts_consent = ModsScriptsConsent::none;
    on = commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).value;
    GIANT_CHECK(on["scripts_allowed"] == true && !on.contains("scripts_asked"));
    // The machine's list broken: a mod is still turned off, with a warning that its allowance stays in the list.
    GIANT_CHECK(write_text_file(path_from_utf8(state.mods_scripts_allowed_file), "{ not json"));
    ai::CommandResult off = commands.execute("enable_mod", {{"id", "guards"}, {"enabled", false}});
    GIANT_CHECK(off.ok && off.value["enabled"] == false && off.value["warning"].get<std::string>().find("mend it") != std::string::npos);
    // (Told to forbid them, that is what was not done: a failure. And none can be allowed while it is broken.)
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", false}}).error.find("could not be taken") != std::string::npos);
    state.mods_scripts_consent = ModsScriptsConsent::by_command;
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).error.find("mend it") != std::string::npos);
    state.mods_scripts_consent = ModsScriptsConsent::ask;
    GIANT_CHECK(!commands.execute("enable_mod", {{"id", "guards"}, {"scripts", true}}).ok && !state.mods_scripts_request);
    std::filesystem::remove(path_from_utf8(state.mods_scripts_allowed_file), error);
    // A loaded mod's mod.json names other scripts than when the world was opened: open it again.
    GIANT_CHECK(commands.execute("enable_mod", {{"id", "guards"}}).ok && commands.execute("open_world", folder).ok &&
                commands.execute("list_mods").value["world"]["reopen"] == false);
    guards.scripts = "Other.dll";
    w.put(guards);
    GIANT_CHECK(commands.execute("list_mods").value["world"]["reopen"] == true);
    // A file is in a world's mods folder or it is not, however the path is written.
    GIANT_CHECK(in_mods_folder(w.folder, w.folder / "mods" / "guards" / "Guards.dll") && in_mods_folder(w.folder, w.folder / "mods" / "x" / ".." / "guards" / "a.dll") &&
                !in_mods_folder(w.folder, w.folder / "scripts.dll") && !in_mods_folder(w.folder, w.folder / "mods2" / "a.dll") &&
                !in_mods_folder(w.folder, w.folder / "mods" / ".." / "a.dll"));
    // The world closed: what was found is another opening's.
    GIANT_CHECK(commands.execute("close_world").ok && state.mods_scripts.session != streaming.session());
    std::filesystem::remove_all(settings, error);
}
