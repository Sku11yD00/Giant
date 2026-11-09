// Game saves in the editor (engine/save's slots): what play made of the
// open world, saved beside it (<world>/saves/<slot>/) and loaded back,
// never written into the world's own files. A save holds, by cell, the
// damage shown (the game's, with what the bodies broke and the forest
// felled, and the world's kept records of the cells those touch) and the
// ground a game dented (craters, footprints: each such tile whole), and
// sections for the camera (where the player's eyes were), whether play was
// on, the weather it was going to with its ground, and the bodies
// (whether simulating, and where the moved dynamic entities are). Loading stops play
// and simulating (what they broke since goes), shows the save's damage as
// the game's (world::Scene::set_game_damage, a share of cells a frame),
// presses the save's ground in (PhysicsState::press_game_ground, a few
// tiles a frame; a tile not streamed in yet waits for it), puts the camera
// and the weather back (the weather shown over the scene's
// own until set_weather, clear_weather or close_game), plays (or
// simulates) again if it was, and puts the bodies back where they were (a
// share a frame; one not streamed in yet waits while the simulation runs).
//
// Autosave: while playing, every autosave_seconds of play, a save to the
// slot "autosave" (its own base: a save to another slot stays a save of
// changes), never while a save or load is under way.
//
// The disk is only touched on a job: a save gathers what to keep at once
// (copies, no text), and the job makes the text and commits it; a load's
// job opens the slot and reads and checks every chunk, and the frames
// apply them. A save to the slot last saved or loaded writes only the
// cells that changed since. View state: nothing here is undoable.
#pragma once

#include "editor_commands.h"

#include "giant/core/jobs.h"
#include "giant/save/slot.h"
#include "giant/world/damage.h"
#include "giant/world/terrain.h"
#include "giant/world/weather.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace giant::editor {

class GameSaves {
public:
    // A cell of a game: its damage shown and its ground (none: as authored).
    struct GameCell {
        world::CellDamage damage;
        std::optional<world::Terrain::Heights> ground;
        bool operator==(const GameCell&) const = default;
    };

    GameSaves();
    ~GameSaves(); // waits for the job
    GameSaves(const GameSaves&) = delete;
    GameSaves& operator=(const GameSaves&) = delete;

    // Where the open world's slots are (empty: no world open, or the scene is not its).
    static std::filesystem::path root(const EditorState& state);
    // Which world a save is of: a hash of what its records depend on (kits, scatter, ground samples).
    static u64 world_identity(const world::WorldIndex& index);

    // Starts saving to `slot` (made if missing, replaced if not). Empty: started; else why not.
    std::string begin_save(EditorState& state, const std::string& slot);
    // Starts loading `slot`; `force`: even when it is of another version of the world.
    std::string begin_load(EditorState& state, const std::string& slot, bool force);
    // Each frame: a finished job's result taken (dropped if the scene or the
    // world was replaced since it began), and a load's cells applied, at most
    // cells_per_frame of them.
    void update(EditorState& state, double seconds);
    usize cells_per_frame = 64;
    usize ground_tiles_per_frame = 8; // of those, the ones with ground (each a whole tile pressed in)
    // A loaded game's bodies put back a frame (8192 looked at): most at once, so few land among
    // others not put back yet (the simulation steps between frames).
    usize poses_per_frame = 4096;
    double autosave_seconds = 300.0;  // of play between autosaves (0: none)
    static constexpr const char* k_autosave_slot = "autosave";
    // Saved bodies not put back yet (their entity's body not made yet).
    usize bodies_waiting() const { return m_poses_waiting.size(); }
    // Until the job in flight is done (tests, shutdown, a command that waits).
    void wait();
    // A save or load under way (its job, or a load's cells still being applied).
    bool busy() const;

    // Starts deleting the slot's folder (the base forgotten if it is the base).
    std::string begin_delete(EditorState& state, const std::string& slot);

    // The game shown closed: its damage, the weather override and what the
    // last save or load was are forgotten (the world shows as it is kept).
    void close(EditorState& state);

    // A loaded game's ground for tiles not in the scene yet (pressed in as they stream in).
    usize ground_waiting() const { return m_ground_waiting.size(); }

    // The weather a load put back no more (set_weather, clear_weather).
    void drop_weather();
    // The weather a load put back, shown over the scene's own (null: the scene's shows).
    const std::optional<world::Weather>* weather() const { return m_weather_on ? &m_weather : nullptr; }
    // Seconds played since the game began (loaded: the save's, on from there).
    double play_seconds() const { return m_play_seconds; }

    struct Status {
        std::string doing;      // "", "saving", "loading", "applying", "deleting"
        std::string slot;       // the one being saved or loaded, or the last
        std::string error;      // the last one's, if it failed
        std::string base_slot;  // the slot last saved or loaded (the next save there writes only changes)
        u64 saves = 0, loads = 0;
        u32 written = 0, removed = 0; // the last save's chunk files written and cleaned
        usize cells = 0;               // the last save's or load's cells
        usize applied = 0;             // a load's cells applied so far
        usize tiles = 0;               // of the last save's or load's cells, those with ground
        usize skipped = 0;             // a loaded tile's ground not pressed (another sample count: forced)
        usize bodies = 0;              // the last save's or load's moved bodies
        usize bodies_applied = 0;      // a load's put back so far
        usize bodies_skipped = 0;      // a load's not put back: moved in the world since, not dynamic, or the simulation stopped
        u64 autosaves = 0;
        std::string autosave_error;    // the last autosave's (kept apart from `error`)
        double gather_ms = 0.0;        // the last save's gathering on the frame
    };
    const Status& status() const { return m_status; }

private:
    struct Pending;
    void take(EditorState& state);
    void forget_base();                                  // both
    void forget_base(const std::filesystem::path& slot); // the one of that slot
    // A new scene or world session seen: the game forgotten (each frame, and before each job begins).
    void follow(EditorState& state);
    void finish_load(EditorState& state);
    // A game's ground: the game ground and what waits for its tile forgotten (the ground as authored).
    void clear_ground(EditorState& state);
    // A loaded tile's ground pressed in, or kept waiting for its tile.
    void apply_ground(EditorState& state, world::CellCoord cell, world::Terrain::Heights heights);
    // Waiting tiles now in the scene pressed in (at most `budget`; looks at a share a frame).
    void press_waiting(EditorState& state, usize& budget);
    // A loaded game's bodies put back (a share a frame) while the simulation they were for runs.
    void place_bodies(EditorState& state);
    std::string save_to(EditorState& state, const std::string& slot, bool autosave);

    JobSystem m_jobs{1};
    WaitGroup m_group;
    std::mutex m_mutex;
    std::unique_ptr<Pending> m_done; // set by the job
    bool m_in_flight = false;

    Status m_status;
    // A slot as last saved or loaded, as it is on the disk, and its cells:
    // the user's slots' last, and the autosave's (each a save of changes).
    struct Base {
        std::filesystem::path path;
        save::Manifest manifest;
        std::shared_ptr<const std::map<save::CellKey, GameCell>> cells; // shared with a save's job
    };
    Base m_base, m_auto_base;
    Base& base_for(const std::filesystem::path& slot_path);
    u64 m_session = 0;    // the world's (WorldStreaming::session) when the bases were made
    u64 m_generation = 0; // the scene's

    // A load being applied.
    std::vector<std::pair<world::CellCoord, GameCell>> m_applying;
    std::map<save::CellKey, world::Terrain::Heights> m_ground_waiting;
    std::optional<save::CellKey> m_waiting_from; // where the next look at them starts (none: the first)
public:
    struct SavedPose {
        world::EntityId entity = 0;
        DVec3 position{};
        Quat rotation{};
        DVec3 of_position{}; // where the scene had it (a body moved in the world since is not this save's)
        Quat of_rotation{};
    };

private:
    std::map<world::EntityId, SavedPose> m_poses_waiting;
    std::optional<world::EntityId> m_poses_from;
    u64 m_poses_run = 0; // the simulation's run they are for
    u32 m_poses_held = 0; // frames held back while bodies are being made
    struct Sections;
    std::unique_ptr<Sections> m_sections;

    bool m_weather_on = false;
    std::optional<world::Weather> m_weather;
    std::optional<world::Weather> m_scene_weather; // the scene's when the override was put back
    double m_play_seconds = 0.0;
    double m_autosaved_at = 0.0; // play seconds at the last save (any) or load
    u64 m_play_generation = 0, m_play_session = 0; // the scene and world it counts for
};

} // namespace giant::editor
