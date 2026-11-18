// The mod commands: list_mods, enable_mod (mods.h). Which mods are on is
// the world's own file, not the scene: not undoable (enable_mod again).
#include "editor_commands.h"
#include "mods.h"
#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/core/text.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "mod command could not be registered");
}

// The world whose mods are meant, or nothing when none is open.
std::optional<std::filesystem::path> world_folder(const EditorState& state) {
    if (!state.mods_world.empty()) {
        return path_from_utf8(state.mods_world);
    }
    if (state.world != nullptr && state.world->is_open()) {
        return state.world->folder();
    }
    return std::nullopt;
}

CommandResult no_world() {
    return CommandResult::failure("mods belong to a world: open one first (open_world, or save_world with a folder)");
}

Json mod_json(const ModList::Found& found) {
    const world::Mod& mod = found.mod;
    Json needs = Json::array();
    for (const world::ModNeed& need : mod.needs) {
        needs.push_back({{"id", need.id}, {"at_least", world::to_string(need.at_least)}});
    }
    Json out = {{"id", mod.id},
                {"name", mod.name},
                {"version", world::to_string(mod.version)},
                {"author", mod.author},
                {"description", mod.description},
                {"needs", std::move(needs)},
                {"after", mod.after},
                {"enabled", found.on},
                {"loaded", found.place != 0}};
    if (found.place != 0) {
        out["place"] = found.place;
    }
    if (!found.problem.empty()) {
        out["problem"] = found.problem;
    }
    return out;
}

} // namespace

void register_mod_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"list_mods",
         "Lists the mods beside the open world (`folder` is where): each folder <world>/mods/<id>/ with a mod.json (format \"giant-mod\": "
         "id, name, version \"1.0.0\", and optionally author, description, needs [{id, at_least}] and after [ids]). "
         "For each: whether it is enabled, whether it is loaded and its place in the load order (a mod loads after "
         "all it needs and all it asks to come after, otherwise by id), and for one that is enabled and not loaded the "
         "problem (a mod it needs is missing, off, too old or not loaded itself, or mods wait on each other in a "
         "circle). `broken` are folders that are no mod, with why (folders whose names begin with . or _ are passed over); `missing` are ids enabled with no mod there; "
         "`load_order` is the ids loaded, first to last; `error` is there when enabled.json cannot be used (then no mod "
         "is on), `folder_error` when the mods folder could not be read through, `too_many` when it has more than 256 "
         "folders (the rest are not looked at). Reads the disk when called. So far a mod is its manifest and "
         "its place in the order: what mods hold is not loaded into the world yet.",
         Json::parse(R"({"type": "object", "additionalProperties": false})"),
         true,
         [&state](const Json&, CommandContext&) {
             const std::optional<std::filesystem::path> world = world_folder(state);
             if (!world) {
                 return no_world();
             }
             const ModList list = read_mods(*world);
             Json mods = Json::array();
             for (const ModList::Found& found : list.mods) {
                 mods.push_back(mod_json(found));
             }
             Json broken = Json::array();
             for (const ModList::Broken& b : list.broken) {
                 broken.push_back({{"folder", b.folder}, {"error", b.error}});
             }
             Json out = {{"folder", path_to_utf8(mods_folder(*world))},
                         {"mods", std::move(mods)},
                         {"broken", std::move(broken)},
                         {"missing", list.missing},
                         {"load_order", list.order}};
             if (!list.list_error.empty()) {
                 out["error"] = list.list_error;
             }
             if (!list.folder_error.empty()) {
                 out["folder_error"] = list.folder_error;
             }
             if (list.too_many) {
                 out["too_many"] = true;
             }
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"enable_mod",
         "Turns a mod of the open world on (or off, with enabled: false). Which mods are on is kept with the world, in "
         "<world>/mods/enabled.json, written at once: this writes a file and is not on the undo history (run it "
         "again to change back). A mod that is not there cannot be turned on (list_mods shows those there); one that "
         "is gone can be turned off. Answers with whether the mod is loaded now, its problem if it is on and cannot "
         "load (see list_mods), and the load order.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["id"],
             "properties": {"id": {"type": "string", "minLength": 1, "maxLength": 64},
                            "enabled": {"type": "boolean", "description": "Default true."}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             const std::optional<std::filesystem::path> world = world_folder(state);
             if (!world) {
                 return no_world();
             }
             const std::string id = args["id"].get<std::string>();
             const bool on = args.value("enabled", true);
             if (const auto error = set_mod_on(*world, id, on)) {
                 return CommandResult::failure(*error);
             }
             const ModList list = read_mods(*world);
             Json out = {{"id", id}, {"enabled", on}, {"loaded", false}, {"load_order", list.order}};
             const auto found = std::find_if(list.mods.begin(), list.mods.end(), [&](const ModList::Found& f) { return f.mod.id == id; });
             if (found != list.mods.end()) {
                 out["loaded"] = found->place != 0;
                 if (!found->problem.empty()) {
                     out["problem"] = found->problem;
                 }
             } else if (on) { // (on already, and gone since)
                 out["problem"] = "there is no such mod there now (list_mods: `missing`)";
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
