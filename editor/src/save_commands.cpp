// Game save commands: save_game, load_game, list_saves, delete_save,
// get_save, close_game (game_saves.h). View state, not undoable.
#include "editor_commands.h"
#include "game_saves.h"
#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/save/slot.h"

#include <limits>
#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "save command could not be registered");
}

CommandResult no_saves() { return CommandResult::failure("game saves are not available"); }

Json slot_schema() {
    return {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 64},
            {"description", "The slot: 1 to 64 of lowercase letters, digits, '-' and '_'."}};
}

Json status_json(const EditorState& state) {
    const GameSaves& g = *state.saves;
    const GameSaves::Status& s = g.status();
    Json out = {{"doing", s.doing},
                {"slot", s.slot},
                {"error", s.error},
                {"base_slot", s.base_slot},
                {"saves", s.saves},
                {"loads", s.loads},
                {"written", s.written},
                {"removed", s.removed},
                {"cells", s.cells},
                {"applied", s.applied},
                {"play_seconds", g.play_seconds()},
                {"weather_override", g.weather() != nullptr},
                {"game_cells", state.scene.game_damage().size()}};
    const std::filesystem::path root = GameSaves::root(state);
    out["folder"] = root.empty() ? Json(nullptr) : Json(root.generic_string());
    return out;
}

// Waited for: the job done and (a load) every cell applied now.
void finish_now(EditorState& state) {
    GameSaves& g = *state.saves;
    g.wait();
    const usize was = g.cells_per_frame;
    g.cells_per_frame = std::numeric_limits<usize>::max();
    g.update(state, 0.0);
    g.cells_per_frame = was;
}

} // namespace

void register_save_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"save_game",
         "Saves the game to `slot` beside the open world (<world>/saves/<slot>/), never into the world's own "
         "files: by cell, the damage shown (broken buildings' pieces where they lie, felled trees; what "
         "simulating broke and a loaded save's), and the camera, whether playing, and the weather it is "
         "going to with the ground (wet, snow). A save to the slot last saved or loaded writes only the cells "
         "changed since. Written on a job (a crash never loses the last save): `wait` (default false) "
         "returns once it is on the disk. Returns get_save.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"slot"}},
              {"properties", {{"slot", slot_schema()}, {"wait", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             const std::string why = state.saves->begin_save(state, args["slot"].get<std::string>());
             if (!why.empty()) return CommandResult::failure(why);
             if (args.value("wait", false)) {
                 finish_now(state);
                 if (!state.saves->status().error.empty()) return CommandResult::failure(state.saves->status().error);
             }
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"load_game",
         "Loads the game in `slot` (save_game) over the open world: play and simulating stop (what they "
         "broke since goes), the save's damage shows over the world's (its cells a share each frame), the "
         "camera and the weather come back (shown over the scene's own until set_weather, clear_weather or "
         "close_game), and play starts again if it was on. A save of another world, or of the world with "
         "other kits, scatter or ground samples, is refused unless `force`. Read on a job: `wait` (default false) returns once it "
         "is all shown. Returns get_save.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"slot"}},
              {"properties",
               {{"slot", slot_schema()}, {"force", {{"type", "boolean"}}}, {"wait", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             const std::string why =
                 state.saves->begin_load(state, args["slot"].get<std::string>(), args.value("force", false));
             if (!why.empty()) return CommandResult::failure(why);
             if (args.value("wait", false)) {
                 finish_now(state);
                 if (!state.saves->status().error.empty()) return CommandResult::failure(state.saves->status().error);
             }
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"list_saves",
         "The open world's game saves: each slot's `name`, `world`, `saved_unix`, `play_seconds`, `cells`, "
         "`generation`, `of_this_world` (its kits, scatter and ground samples as the world's now), "
         "`from_previous`, and `error` for one that cannot be opened. Reads each slot's manifest now (a "
         "short read of the disk on the frame).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             const std::filesystem::path root = GameSaves::root(state);
             if (root.empty()) return CommandResult::failure("no world open");
             const u64 hash = GameSaves::world_identity(state.world->index());
             Json slots = Json::array();
             for (const save::Listed& l : save::list_slots(root)) {
                 Json s = {{"name", l.name}};
                 if (!l.opened.error.empty()) {
                     s["error"] = l.opened.error;
                 } else {
                     const save::Manifest& m = l.opened.manifest;
                     s["world"] = m.world_name;
                     s["saved_unix"] = m.saved_unix;
                     s["play_seconds"] = m.play_seconds;
                     s["cells"] = m.cells.size();
                     s["generation"] = m.generation;
                     s["of_this_world"] = m.world_hash == hash;
                     s["from_previous"] = l.opened.from_previous;
                 }
                 slots.push_back(std::move(s));
             }
             return CommandResult::success({{"folder", root.generic_string()}, {"slots", std::move(slots)}});
         }});

    add(registry,
        {"delete_save",
         "Deletes the game save in `slot` (its whole folder) on a job: `wait` (default false) returns once "
         "it is gone. Not undoable. Returns get_save.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"slot"}},
              {"properties", {{"slot", slot_schema()}, {"wait", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             const std::string why = state.saves->begin_delete(state, args["slot"].get<std::string>());
             if (!why.empty()) return CommandResult::failure(why);
             if (args.value("wait", false)) {
                 finish_now(state);
                 if (!state.saves->status().error.empty()) return CommandResult::failure(state.saves->status().error);
             }
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"get_save",
         "What the game saves are doing: `doing` (\"\", saving, loading, applying, deleting), `slot`, `error` (the "
         "last one's), `base_slot` (last saved or loaded: a save there writes only changes), `saves`, "
         "`loads`, the last save's chunk files `written` and `removed`, `cells` and `applied`, "
         "`play_seconds`, `weather_override`, `game_cells` (cells showing a game's damage) and `folder`.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"close_game",
         "Closes the game shown: its damage goes (the world shows as it is kept), the weather shown is the "
         "scene's again, and the next save is whole. Saves on the disk are kept. Returns get_save.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             if (state.saves->busy()) return CommandResult::failure("a save or load is under way");
             state.saves->close(state);
             return CommandResult::success(status_json(state));
         }});
}

} // namespace giant::editor
