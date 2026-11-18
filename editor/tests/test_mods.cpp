// The mods beside a world (mods.h) and their commands: list_mods, enable_mod.
#include "../src/editor_commands.h"
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
    l.mesh = "people/./villager.glb";
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
    winter.person_looks[0].mesh = "mods_art/./guard.glb";
    winter.routines = {routine("smith's day", "tavern")};
    w.put_world("winter", winter);
    GIANT_CHECK(w.open());
    // The scene is the world with the mod over it; the index is still the file's.
    GIANT_CHECK(w.scene.places.size() == 3 && world::find_place(w.scene.places, "smithy")->position.x == 55.0 &&
                world::find_place(w.scene.places, "tavern") != nullptr);
    GIANT_CHECK(w.scene.person_looks.size() == 2 && w.scene.person_looks[1].mesh == "mods_art/guard.glb"); // (one spelling per file)
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
    guards.person_looks[1].mesh = "shared/winter_villager.glb"; // (not the mod's own: as a world's paths are)
    w.put_world("guards", guards);
    const std::string in_mod = MeshLibrary::normalize(path_to_utf8(w.folder / "mods" / "guards")) + "/";
    GIANT_CHECK(w.open() && w.streaming.mods_laid().applied.problems.empty());
    const world::PersonLook* guard = world::find_person_look(w.scene.person_looks, "guard");
    GIANT_CHECK(guard != nullptr && guard->mesh == in_mod + "art/guard.glb" && guard->material.maps.base_color == in_mod + "art/guard.png" &&
                world::find_person_look(w.scene.person_looks, "villager")->mesh == "shared/winter_villager.glb");
    // What the editor imports for the mods is those paths; nothing of them is the world's to save.
    GIANT_CHECK(w.streaming.mods_laid().person_looks.size() == 2 && !w.streaming.unsaved_settings(w.scene));
    const WorldStreaming::Saved saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.mods_not_kept.empty() && w.file().meshes == std::vector<std::string>{"people/villager.glb"} &&
                w.file().person_looks.size() == 1 && w.file().person_looks[0].mesh == "people/villager.glb");
    // A path that would leave the mod's folder: the mod is left out, whole, and it is said.
    w.streaming.close();
    guards.person_looks[0].mesh = "mod:../../other/guard.glb";
    GIANT_CHECK(write_text_file(w.folder / "mods" / "guards" / "world.json", world::save_mod_world(guards)));
    GIANT_CHECK(w.open() && w.scene.person_looks.size() == 1 && w.streaming.mods_laid().applied.problems.size() == 1 &&
                w.streaming.mods_laid().applied.problems[0].message.find("person_looks[0].mesh") != std::string::npos);
}
