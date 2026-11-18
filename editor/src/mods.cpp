#include "mods.h"

#include "giant/core/text.h"
#include "giant/core/text_files.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace giant::editor {

namespace {

constexpr const char* k_manifest = "mod.json";
constexpr const char* k_enabled = "enabled.json";

// Nothing is there under the name (not: something is, and it cannot be looked at).
bool is_absent(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::symlink_status(path, error).type() == std::filesystem::file_type::not_found;
}

// The folder's name as text, or nothing when it is not text (Windows: a name that is not Unicode).
std::optional<std::string> name_of(const std::filesystem::path& folder) {
    try {
        return path_to_utf8(folder.filename());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The ids on, or why the list cannot be used (no file: none on).
std::optional<std::string> read_enabled(const std::filesystem::path& mods, std::vector<std::string>& ids) {
    ids.clear();
    const std::filesystem::path file = mods / k_enabled;
    if (is_absent(file)) {
        return std::nullopt;
    }
    const std::optional<std::string> text = read_text_file(file, world::k_max_mod_file_bytes);
    if (!text) {
        return path_to_utf8(file) + " could not be read (or is over " + std::to_string(world::k_max_mod_file_bytes) + " bytes)";
    }
    if (const auto bad = world::load_enabled_mods(*text, ids)) {
        ids.clear();
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
    std::vector<std::string> enabled;
    if (const auto bad = read_enabled(mods, enabled)) {
        list.list_error = *bad;
    }
    std::vector<world::Mod> manifests;
    std::vector<u8> on;
    for (ModList::Found& found : list.mods) {
        found.on = std::binary_search(enabled.begin(), enabled.end(), found.mod.id);
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

std::optional<std::string> set_mod_on(const std::filesystem::path& world_folder, const std::string& id, bool on) {
    if (!world::mod_id_ok(id)) {
        return "\"" + id + "\" is no mod's id";
    }
    const std::filesystem::path mods = mods_folder(world_folder);
    std::vector<std::string> enabled;
    if (const auto bad = read_enabled(mods, enabled)) {
        return *bad + " (mend it, or delete it to start with every mod off)";
    }
    const auto at = std::lower_bound(enabled.begin(), enabled.end(), id);
    const bool was = at != enabled.end() && *at == id;
    if (on == was) {
        return std::nullopt;
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
        if (enabled.size() >= world::k_max_mods) {
            return "at most " + std::to_string(world::k_max_mods) + " mods can be on";
        }
        enabled.insert(at, id);
    } else {
        enabled.erase(at);
    }
    std::error_code error;
    std::filesystem::create_directories(mods, error);
    if (error || !write_text_file(mods / k_enabled, world::save_enabled_mods(enabled))) {
        return path_to_utf8(mods / k_enabled) + " could not be written";
    }
    return std::nullopt;
}

} // namespace giant::editor
