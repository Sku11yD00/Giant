#include "mods.h"

#include "mesh_library.h"

#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace giant::editor {

namespace {

constexpr const char* k_manifest = "mod.json";
constexpr const char* k_enabled = "enabled.json";
constexpr const char* k_mod_world = "world.json";
constexpr usize k_max_mod_world_bytes = usize{64} << 20; // (a world's 65,536 places are about a quarter of it)

// Nothing is there under the name (not: something is, and it cannot be looked at).
bool is_absent(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::symlink_status(path, error).type() == std::filesystem::file_type::not_found;
}

// The path as text, or nothing when it is not text (Windows: a name that is not Unicode).
std::optional<std::string> text_of(const std::filesystem::path& path) {
    try {
        return path_to_utf8(path);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
// The folder's name as text.
std::optional<std::string> name_of(const std::filesystem::path& folder) { return text_of(folder.filename()); }

// The first of the mod's own files (paths in `folder`) that is not in it on the disk (a link in the
// mod leads out), with why, or nothing. One that is not there at all is in it.
std::optional<std::string> led_out(const std::filesystem::path& folder, const std::vector<std::string>& files) {
    if (files.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(folder, error);
    if (error) {
        return "the mod's folder could not be looked at: " + error.message();
    }
    for (const std::string& file : files) {
        // (A map may name an image in a glTF, "a.gltf#image0": the file is what is before the #.)
        for (const std::string& name : {file, file.substr(0, file.rfind('#'))}) {
            if (name.empty()) {
                continue;
            }
            const std::filesystem::path real = std::filesystem::weakly_canonical(root / path_from_utf8(name), error);
            if (error) {
                return "\"mod:" + file + "\" could not be looked at: " + error.message();
            }
            const std::filesystem::path relative = real.lexically_relative(root);
            if (relative.empty() || *relative.begin() == "..") {
                return "\"mod:" + file + "\" is not in the mod's folder: a link there leads out of it";
            }
        }
    }
    return std::nullopt;
}

// The ids on, or why the list cannot be used (no file: none on).
std::optional<std::string> read_enabled(const std::filesystem::path& mods, world::ModsOn& on) {
    on = {};
    const std::filesystem::path file = mods / k_enabled;
    if (is_absent(file)) {
        return std::nullopt;
    }
    const std::optional<std::string> text = read_text_file(file, world::k_max_mod_file_bytes);
    if (!text) {
        return path_to_utf8(file) + " could not be read (or is over " + std::to_string(world::k_max_mod_file_bytes) + " bytes)";
    }
    if (const auto bad = world::load_enabled_mods(*text, on)) {
        on = {};
        return path_to_utf8(file) + ": " + bad->message;
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path mods_folder(const std::filesystem::path& world_folder) { return world_folder / "mods"; }

ModList read_mods(const std::filesystem::path& world_folder) {
    ModList list;
    const std::filesystem::path mods = mods_folder(world_folder);
    std::error_code error;
    // The folders, by name (the disk's own order is not one to show).
    std::vector<std::filesystem::path> folders;
    if (!is_absent(mods)) {
        for (std::filesystem::directory_iterator at(mods, error), end; !error && at != end; at.increment(error)) {
            std::error_code ignored; // (what cannot be looked at is no folder)
            // (Names beginning with . or _ are not mods' and not listed: .git, __MACOSX, a mod set aside.)
            const std::filesystem::path file_name = at->path().filename();
            const std::filesystem::path::string_type& name = file_name.native();
            if (!name.empty() && name[0] != '.' && name[0] != '_' && (at->is_directory(ignored) || at->is_symlink(ignored))) {
                folders.push_back(at->path());
            }
        }
        if (error) {
            list.folder_error = path_to_utf8(mods) + " could not be read through: " + error.message();
        }
    }
    std::sort(folders.begin(), folders.end());
    if (folders.size() > world::k_max_mods) {
        folders.resize(world::k_max_mods);
        list.too_many = true;
    }
    for (const std::filesystem::path& folder : folders) {
        const std::string name = name_of(folder).value_or("(a name that is not text)");
        const auto broken = [&](std::string why) { list.broken.push_back({name, std::move(why)}); };
        if (std::filesystem::is_symlink(folder, error)) {
            if (std::filesystem::is_directory(folder, error)) {
                broken("it is a link to a folder: a mod is a folder of its own");
            }
            continue; // (a link to a file is a file: no mod, and no trouble)
        }
        if (!world::mod_id_ok(name)) {
            broken("a mod's folder is named by its id: 1 to " + std::to_string(world::k_max_mod_id_bytes) +
                   " of a-z, 0-9 and _, beginning with a letter");
            continue;
        }
        if (is_absent(folder / k_manifest)) {
            broken(std::string("it has no ") + k_manifest);
            continue;
        }
        const std::optional<std::string> text = read_text_file(folder / k_manifest, world::k_max_mod_file_bytes);
        if (!text) {
            broken(std::string(k_manifest) + " could not be read (or is over " + std::to_string(world::k_max_mod_file_bytes) + " bytes)");
            continue;
        }
        ModList::Found found;
        if (const auto bad = world::load_mod(*text, found.mod)) {
            broken(std::string(k_manifest) + ": " + bad->message);
            continue;
        }
        if (found.mod.id != name) {
            broken(std::string(k_manifest) + " says its id is \"" + found.mod.id + "\": the folder must have the same name");
            continue;
        }
        list.mods.push_back(std::move(found));
    }
    // (By folder name, and each one's id is its folder's name: by id.)
    world::ModsOn on_list;
    if (const auto bad = read_enabled(mods, on_list)) {
        list.list_error = *bad;
    }
    const std::vector<std::string>& enabled = on_list.ids;
    std::vector<world::Mod> manifests;
    std::vector<u8> on;
    for (ModList::Found& found : list.mods) {
        found.on = std::binary_search(enabled.begin(), enabled.end(), found.mod.id);
        found.slot = world::find_mod_slot(on_list, found.mod.id);
        manifests.push_back(found.mod);
        on.push_back(found.on ? 1 : 0);
    }
    for (const std::string& id : enabled) {
        if (std::none_of(list.mods.begin(), list.mods.end(), [&](const ModList::Found& f) { return f.mod.id == id; })) {
            list.missing.push_back(id);
        }
    }
    world::ModOrder order = world::order_mods(manifests, on);
    for (usize i = 0; i < order.order.size(); ++i) {
        list.mods[order.order[i]].place = i + 1;
        list.order.push_back(list.mods[order.order[i]].mod.id);
    }
    for (world::ModProblem& problem : order.problems) {
        list.mods[problem.mod].problem = std::move(problem.message);
    }
    return list;
}

ModsLaid lay_mods(const std::filesystem::path& world_folder, world::Scene& scene) {
    ModsLaid laid;
    const ModList list = read_mods(world_folder);
    if (list.order.empty()) {
        return laid;
    }
    laid.mods = list.order;
    std::vector<u32>& slots = laid.slots;
    for (const std::string& id : laid.mods) {
        const auto found = std::find_if(list.mods.begin(), list.mods.end(), [&](const ModList::Found& f) { return f.mod.id == id; });
        slots.push_back(found != list.mods.end() && found->slot ? *found->slot : world::k_no_mod_slot);
    }
    std::vector<world::ModWorld> worlds(laid.mods.size());
    std::vector<const world::ModWorld*> given(laid.mods.size(), nullptr);
    std::vector<world::ModProblem> unread;
    for (usize i = 0; i < laid.mods.size(); ++i) {
        const std::filesystem::path file = mods_folder(world_folder) / laid.mods[i] / k_mod_world;
        laid.files.push_back(mod_world_file(world_folder, laid.mods[i]));
        if (is_absent(file)) {
            continue;
        }
        const std::optional<std::string> text = read_text_file(file, k_max_mod_world_bytes);
        if (!text) {
            unread.push_back({i, std::string(k_mod_world) + " could not be read (or is over " + std::to_string(k_max_mod_world_bytes >> 20) + " MB)"});
            continue;
        }
        if (const auto bad = world::load_mod_world(*text, worlds[i])) {
            unread.push_back({i, std::string(k_mod_world) + ": " + bad->message});
            continue;
        }
        // The mod's own files ("mod:...") are in its folder, and nowhere a link in it leads.
        const std::filesystem::path own_folder = mods_folder(world_folder) / laid.mods[i];
        std::optional<std::string> wrong = led_out(own_folder, world::mod_own_files(worlds[i]));
        if (!wrong) {
            const std::optional<std::string> folder_text = text_of(own_folder);
            wrong = folder_text ? world::resolve_mod_files(worlds[i], MeshLibrary::normalize(*folder_text))
                                : std::optional<std::string>("the mod's folder has a path that is not text");
        }
        if (wrong) {
            unread.push_back({i, std::string(k_mod_world) + ": " + *wrong});
            continue;
        }
        for (world::PersonLook& look : worlds[i].person_looks) {
            look.mesh = MeshLibrary::normalize(look.mesh); // (one spelling per file, as the world's own)
        }
        given[i] = &worlds[i];
    }
    laid.applied = world::apply_mod_worlds(given, slots, scene.places, scene.person_looks, scene.routines, scene.behaviours, scene.people,
                                           scene.next_person_id);
    laid.applied.problems.insert(laid.applied.problems.end(), std::make_move_iterator(unread.begin()), std::make_move_iterator(unread.end()));
    std::sort(laid.applied.problems.begin(), laid.applied.problems.end(),
              [](const world::ModProblem& l, const world::ModProblem& r) { return l.mod < r.mod; });
    // The things mods set, each once, as they are now (the last to set it left it so).
    std::set<std::pair<world::ModThing, std::string_view>> set;
    for (const world::ModChange& change : laid.applied.changes) {
        set.emplace(change.thing, change.name);
    }
    laid.any = !set.empty() || !laid.applied.people.empty();
    // The people mods added: those of the ids of each one's slot.
    for (const world::ModPeople& added : laid.applied.people) {
        const world::PersonId first = world::mod_person_id(slots[added.mod], 0);
        const auto at = std::lower_bound(scene.people.begin(), scene.people.end(), first,
                                         [](const world::Person& l, world::PersonId r) { return l.id < r; });
        laid.people.insert(laid.people.end(), at, at + static_cast<std::ptrdiff_t>(added.count));
    }
    std::sort(laid.people.begin(), laid.people.end(), [](const world::Person& l, const world::Person& r) { return l.id < r.id; });
    // (One pass over each list: a mod may set as many things as a world keeps.)
    const auto brought = [&set](world::ModThing thing, const auto& now, auto& out) {
        for (const auto& one : now) {
            if (set.contains({thing, std::string_view(one.name)})) {
                out.push_back(one);
            }
        }
    };
    brought(world::ModThing::place, scene.places, laid.places);
    brought(world::ModThing::person_look, scene.person_looks, laid.person_looks);
    brought(world::ModThing::routine, scene.routines, laid.routines);
    brought(world::ModThing::behaviour, scene.behaviours, laid.behaviours);
    return laid;
}

ModsLaid::File mod_world_file(const std::filesystem::path& world_folder, const std::string& id) {
    const std::filesystem::path file = mods_folder(world_folder) / id / k_mod_world;
    ModsLaid::File out;
    if (is_absent(file)) {
        return out;
    }
    out.there = true;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(file, error);
    out.bytes = error ? 0 : static_cast<u64>(bytes);
    const auto written = std::filesystem::last_write_time(file, error);
    out.written = error ? 0 : static_cast<i64>(written.time_since_epoch().count());
    return out;
}

std::optional<std::string> set_mod_on(const std::filesystem::path& world_folder, const std::string& id, bool on) {
    if (!world::mod_id_ok(id)) {
        return "\"" + id + "\" is no mod's id";
    }
    const std::filesystem::path mods = mods_folder(world_folder);
    world::ModsOn on_list;
    if (const auto bad = read_enabled(mods, on_list)) {
        return *bad + " (mend it; deleting it starts with every mod off, and gives up every mod's slot: their people would have other "
                      "ids, which saved games would not know)";
    }
    std::vector<std::string>& enabled = on_list.ids;
    const auto at = std::lower_bound(enabled.begin(), enabled.end(), id);
    const bool was = at != enabled.end() && *at == id;
    if (on == was && (!on || world::find_mod_slot(on_list, id))) {
        return std::nullopt; // (one on without a slot, from a list written by hand or before slots, is given its slot)
    }
    if (on) {
        const ModList list = read_mods(world_folder);
        if (std::none_of(list.mods.begin(), list.mods.end(), [&](const ModList::Found& f) { return f.mod.id == id; })) {
            for (const ModList::Broken& broken : list.broken) {
                // (In any case of letters: "Winter" is what was meant by winter.)
                std::string lower = broken.folder;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
                if (lower == id) {
                    return "the mod in the folder \"" + broken.folder + "\" cannot be used: " + broken.error;
                }
            }
            return "there is no mod \"" + id + "\" in " + path_to_utf8(mods) + " (list_mods shows those there)" +
                   (list.too_many ? "; there are more folders there than the " + std::to_string(world::k_max_mods) +
                                        " looked at: it may be one of the rest"
                                  : std::string()) +
                   (list.folder_error.empty() ? std::string() : "; " + list.folder_error);
        }
        if (!was && enabled.size() >= world::k_max_mods) {
            return "at most " + std::to_string(world::k_max_mods) + " mods can be on";
        }
        if (!was) {
            enabled.insert(at, id);
        }
        if (!world::give_mod_slot(on_list, id)) {
            return "the world's list of mods has given all its " + std::to_string(world::k_max_mod_slots) +
                   " slots (each mod ever turned on keeps one): take those of mods long gone out of " + path_to_utf8(mods / k_enabled);
        }
    } else {
        enabled.erase(at);
    }
    std::error_code error;
    std::filesystem::create_directories(mods, error);
    if (error || !write_text_file(mods / k_enabled, world::save_enabled_mods(on_list))) {
        return path_to_utf8(mods / k_enabled) + " could not be written";
    }
    return std::nullopt;
}

} // namespace giant::editor
