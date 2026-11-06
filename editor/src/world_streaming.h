// Worlds bigger than memory: a world is a folder of cell files
// (world::WorldIndex and world::save_cell), and only the cells around the
// camera are in the scene. Files are read and parsed on a worker thread;
// the main thread only adds what is ready, within a per-frame budget, and
// never waits on disk (AGENTS.md rule 9).
//
// Edits are never lost by streaming: a cell is unloaded only when what the
// scene holds of it is exactly what its file holds. A cell with unsaved
// edits stays loaded however far the camera goes, until save().
#pragma once

#include "scene_render.h"

#include "giant/core/jobs.h"
#include "giant/world/scene.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

constexpr double k_max_load_radius = 8192.0;

struct StreamingSettings {
    // Cells with any part this near the camera (on the ground, x and z) load.
    double load_radius = 1536.0; // at most k_max_load_radius
    // They unload past load_radius + unload_margin, so a camera on a cell
    // border does not load and unload the same cells over and over.
    double unload_margin = 128.0;
    // Entities added to the scene per frame (at least one whole cell).
    u32 entities_per_frame = 4096;
    u32 reads_in_flight = 16;
    // Frames between looks for cells to unload (each look walks the loaded
    // cells, and an unload is a pass over the scene's entities).
    u32 unload_every = 30;
};

struct StreamingStats {
    u32 cells = 0;   // with a file
    u32 loaded = 0;  // in the scene
    u32 reading = 0; // being read and parsed
    u32 waiting = 0; // parsed, waiting for the per-frame budget
    u32 failed = 0;  // files that could not be read (see the log)
    u64 entities_loaded = 0;   // ever, since open
    u64 entities_unloaded = 0; // ever, since open
    u64 ids_changed = 0;       // entities given a new id: their id was taken (see the log)
};

class WorldStreaming {
public:
    WorldStreaming();
    ~WorldStreaming();
    WorldStreaming(const WorldStreaming&) = delete;
    WorldStreaming& operator=(const WorldStreaming&) = delete;

    // Opens the world in `folder`: on success `scene` becomes the world with
    // no cells loaded yet (its lighting, kits and next id), and streaming runs
    // while the editor's scene generation is `generation`. Returns an error
    // (changing nothing) if world.json is missing or invalid.
    std::optional<std::string> open(const std::filesystem::path& folder, world::Scene& scene, u64 generation);
    // Makes a new world in `folder` (which must not hold one) from `scene`,
    // writes every cell, and opens it.
    struct Saved {
        std::string error; // empty on success
        u32 cells_written = 0;
        u32 cells_removed = 0; // files of cells left empty
        u32 cells = 0;         // with a file, after saving
    };
    Saved create(const std::filesystem::path& folder, const world::Scene& scene, u64 generation,
                 std::vector<std::string> meshes);
    // Writes the open world: the index and every cell whose contents differ
    // from its file. `meshes` are the files the scene uses (kept with the
    // ones the index already lists). Waits for reads in flight.
    Saved save(const world::Scene& scene, std::vector<std::string> meshes);
    void close();

    bool is_open() const { return !m_folder.empty(); }
    // Open, and the scene is the world's (not replaced by new_scene,
    // load_scene or a script since).
    bool active(u64 generation) const { return is_open() && generation == m_generation; }
    const std::filesystem::path& folder() const { return m_folder; }
    const world::WorldIndex& index() const { return m_index; }

    // Once a frame, after `frame` was built from `scene`: adds what is ready,
    // starts reads near the camera, and now and then unloads far cells that
    // have no unsaved edits. Does nothing unless active(generation).
    void update(world::Scene& scene, u64 generation, const DVec3& camera, const SceneFrame& frame);

    // Cells whose contents differ from their file, or that have no file
    // yet. Walks the whole scene.
    u32 unsaved_cells(const world::Scene& scene);
    // Every cell with a file is in the scene (so the scene sees every kit
    // placement, say).
    bool all_loaded() const;

    // Blocks until reads in flight are parsed (tests; save does it too).
    void finish_reads();

    StreamingStats stats() const;
    StreamingSettings settings;

private:
    enum class State { on_disk, reading, ready, loaded, failed };
    struct Cell {
        State state = State::on_disk;
        // Loaded: what its file holds, sorted by id (what the scene must
        // match, entity for entity, for the cell to be clean).
        std::vector<world::Entity> saved;
    };
    struct Read {
        world::CellCoord cell;
        u64 epoch = 0;
        std::vector<world::Entity> entities; // sorted by id
        std::string error;
        bool missing = false; // the file is not there
    };
    // Whether `now` (any order) is exactly `saved`.
    static bool matches(std::vector<const world::Entity*>& now, const std::vector<world::Entity>& saved);

    void follow(const world::Scene& scene);
    void start_read(world::CellCoord cell);
    void take_reads();
    void insert(world::Scene& scene);
    void unload(world::Scene& scene, const DVec3& camera, const SceneFrame& frame);
    void start_reads(const DVec3& camera);
    void set_state(world::CellCoord coord, Cell& cell, State state); // keeps the counts
    void forget(world::CellCoord coord);                            // no file any more
    Saved write(const world::Scene& scene, std::vector<std::string> meshes);
    std::filesystem::path cell_path(world::CellCoord cell) const;

    std::filesystem::path m_folder;
    u64 m_generation = 0;
    // The scene object the bookkeeping matches (world::Scene::identity):
    // another one (an undo or redo put back a copy) is followed again.
    u64 m_scene = 0;
    // Which cell's file each entity in the scene was read from or last
    // saved to: an id met again in that cell's file is the same entity
    // (maybe moved since); met in another cell's file, it is a clash.
    std::unordered_map<world::EntityId, world::CellCoord> m_origin;
    u64 m_epoch = 0; // reads from an older epoch (a closed world) are dropped
    world::WorldIndex m_index;
    std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> m_cells; // cells with a file
    std::vector<Read> m_ready;  // parsed, in the order they finished
    std::array<u32, 5> m_count{}; // cells per State
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_loaded;
    std::vector<world::CellCoord> m_wanted; // to read, the next at the back
    world::CellCoord m_wanted_at{};
    double m_wanted_radius = -1.0;
    bool m_wanted_stale = true;
    u32 m_frames = 0;
    StreamingStats m_totals;

    std::mutex m_mutex; // protects m_finished
    std::vector<Read> m_finished;
    WaitGroup m_reads;
    JobSystem m_jobs{1}; // last: destroyed (finishing its jobs) first
};

} // namespace giant::editor
