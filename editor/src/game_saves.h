// Game saves in the editor (engine/save's slots): what play made of the
// open world, saved beside it (<world>/saves/<slot>/) and loaded back,
// never written into the world's own files. A save holds, by cell, the
// damage shown (the game's, with what the bodies broke and the forest
// felled, and the world's kept records of the cells those touch), and
// sections for the camera (where the player's eyes were), whether play was
// on, and the weather it was going to with its ground. Loading stops play
// and simulating (what they broke since goes), shows the save's damage as
// the game's (world::Scene::set_game_damage, a share of cells a frame),
// puts the camera and the weather back (the weather shown over the scene's
// own until set_weather, clear_weather or close_game) and plays again if
// it was playing.
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
    // Until the job in flight is done (tests, shutdown, a command that waits).
    void wait();
    // A save or load under way (its job, or a load's cells still being applied).
    bool busy() const;

    // Starts deleting the slot's folder (the base forgotten if it is the base).
    std::string begin_delete(EditorState& state, const std::string& slot);

    // The game shown closed: its damage, the weather override and what the
    // last save or load was are forgotten (the world shows as it is kept).
    void close(EditorState& state);

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
    };
    const Status& status() const { return m_status; }

private:
    struct Pending;
    void take(EditorState& state);
    void forget_base();
    // A new scene or world session seen: the game forgotten (each frame, and before each job begins).
    void follow(EditorState& state);
    void finish_load(EditorState& state);

    JobSystem m_jobs{1};
    WaitGroup m_group;
    std::mutex m_mutex;
    std::unique_ptr<Pending> m_done; // set by the job
    bool m_in_flight = false;

    Status m_status;
    // The slot last saved or loaded, as it is on the disk, and its cells' damage.
    std::filesystem::path m_base_path;
    save::Manifest m_base;
    std::shared_ptr<const std::map<save::CellKey, world::CellDamage>> m_base_cells; // shared with a save's job
    u64 m_session = 0;    // the world's (WorldStreaming::session) when the base was made
    u64 m_generation = 0; // the scene's

    // A load being applied.
    std::vector<std::pair<world::CellCoord, world::CellDamage>> m_applying;
    struct Sections;
    std::unique_ptr<Sections> m_sections;

    bool m_weather_on = false;
    std::optional<world::Weather> m_weather;
    std::optional<world::Weather> m_scene_weather; // the scene's when the override was put back
    double m_play_seconds = 0.0;
    u64 m_play_generation = 0, m_play_session = 0; // the scene and world it counts for
};

} // namespace giant::editor
