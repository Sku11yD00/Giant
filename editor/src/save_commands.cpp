// Game save commands: save_game, load_game, list_saves, delete_save,
// get_save, close_game, set_autosave (game_saves.h). View state, not undoable.
#include "editor_commands.h"
#include "game_saves.h"
#include "physics_state.h"
#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/save/slot.h"

#include <cmath>
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
                {"failures", s.failures},
                {"written", s.written},
                {"removed", s.removed},
                {"cells", s.cells},
                {"applied", s.applied},
                {"tiles", s.tiles},
                {"skipped", s.skipped},
                {"ground_waiting", g.ground_waiting()},
                {"game_tiles", state.physics != nullptr ? state.physics->game_ground().tiles() : usize{0}},
                {"bodies", s.bodies},
                {"bodies_applied", s.bodies_applied},
                {"bodies_skipped", s.bodies_skipped},
                {"bodies_waiting", g.bodies_waiting()},
                {"walkers", s.walkers},
                {"walkers_applied", s.walkers_applied},
                {"walkers_skipped", s.walkers_skipped},
                {"walkers_waiting", g.walkers_waiting()},
                {"autosave_seconds", g.autosave_seconds},
                {"autosaves", s.autosaves},
                {"autosave_error", s.autosave_error},
                {"gather_ms", s.gather_ms},
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
    const usize was = g.cells_per_frame, tiles = g.ground_tiles_per_frame;
    g.cells_per_frame = std::numeric_limits<usize>::max();
    g.ground_tiles_per_frame = std::numeric_limits<usize>::max();
    g.update(state, 0.0);
    g.cells_per_frame = was;
    g.ground_tiles_per_frame = tiles;
}

} // namespace

void register_save_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"save_game",
         "Saves the game to `slot` beside the open world (<world>/saves/<slot>/), never into the world's own "
         "files: the player (feet, speed, climbing, carried by), whether simulating and where the dynamic bodies it moved are, the walkers (where each stands, faces and moves, where it is going, how far it got); by cell, the damage shown (broken buildings' pieces where they lie, felled trees; what "
         "simulating broke and a loaded save's) and the ground dented (craters, footprints: each tile whole), and the camera, whether playing, and the weather it is "
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
         "broke since goes), the save's damage and dented ground show over the world's (a share of cells each frame; "
         "a tile not streamed in yet is pressed in when it is), the "
         "camera and the weather come back (shown over the scene's own until set_weather, clear_weather or "
         "close_game), and play (or simulating) starts again if it was on (the player as it was), the moved bodies put back where they were (a share a "
         "frame, after `wait` too: each once its body is made) and the walkers walking again from where they were "
         "(the way looked for again; each once its entity and the ground under it are in). A save of another world, or of the world with "
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
         "`loads`, the last save's chunk files `written` and `removed`, `cells` and `applied`, `tiles` (of those, "
         "with ground), `skipped` (ground not pressed: another sample count), `ground_waiting` (for its tile), "
         "`game_tiles` (pressed in now), `bodies` (moved bodies of the last save or load), `bodies_applied`, "
         "`bodies_skipped` (gone, moved in the world since, or the simulation stopped), `bodies_waiting` (for "
         "their body), `walkers` (the last save's or load's), `walkers_applied` (walking again), `walkers_skipped` "
         "(moved in the world since, given a body, sent walking since, or the simulation stopped), "
         "`walkers_waiting` (for their entity or the ground under them), `autosave_seconds`, `autosaves`, `autosave_error`, `gather_ms` (the last save's cost on "
         "the frame), "
         "`play_seconds`, `weather_override`, `game_cells` (cells showing a game's damage) and `folder`.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"set_autosave",
         "Sets how often the game is saved to the slot \"autosave\" while playing: every `seconds` of play "
         "(0: never; default 300). Its errors are `autosave_error`, apart from `error`; a save to another slot "
         "still writes only changes. Returns get_save.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"seconds"}},
              {"properties", {{"seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", 86400}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.saves == nullptr) return no_saves();
             const double seconds = args["seconds"].get<double>();
             if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 86400.0)
                 return CommandResult::failure("seconds must be from 0 to 86400");
             state.saves->autosave_seconds = seconds;
             return CommandResult::success(status_json(state));
         }});

    add(registry,
        {"close_game",
         "Closes the game shown: its damage and dented ground go (the world shows as it is kept), the weather shown and the "
         "time of day are the scene's again, and the next save is whole. Saves on the disk are kept. Returns get_save.",
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
