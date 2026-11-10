// Worlds bigger than memory: a world is a folder of cell files
// (world::WorldIndex and world::save_cell), and only the cells around the
// camera are in the scene. Files are read and parsed on a worker thread;
// the main thread only adds what is ready, within a per-frame budget, and
// never waits on disk (AGENTS.md rule 9).
//
// Edits are never lost by streaming: a cell is unloaded only when what the
// scene holds of it (its entities, its terrain tile and its kept damage) is
// exactly what its file holds. A cell with unsaved edits stays loaded however far the camera
// goes, until save().
#pragma once

#include "scene_render.h"

#include "giant/core/jobs.h"
#include "giant/world/scene.h"

#include <array>
#include <filesystem>
#include <memory>
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
    // Cells let go in one frame (each frees its entities and tile, and its
    // tile's mesh goes the next frames): the rest go in the frames after.
    u32 unloads_per_frame = 128;
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
    u32 ground = 0;            // cells whose file holds a terrain tile
    u64 tiles_loaded = 0;      // terrain tiles, ever, since open
    u64 tiles_unloaded = 0;
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
        u32 chunks_written = 0; // far ground chunk files (written or removed)
        // The far ground chunks written (or removed: no heights), for the
        // editor's far ground to take.
        struct Chunk {
            u32 level = 0;
            world::CellCoord chunk;
            std::optional<world::Terrain::Heights> heights;
        };
        std::vector<Chunk> chunks = {};
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
    // New on each open, create and close.
    u64 session() const { return m_session; }
    const world::WorldIndex& index() const { return m_index; }
    // The samples a side of the terrain tiles in the world's files, while
    // any file holds one (the scene's terrain must use the same).
    std::optional<u32> ground_samples() const;
    // Whether the cell's file holds a terrain tile the scene does not hold
    // in full yet (the cell is not loaded): terrain commands leave such a
    // cell alone, since what they would record as "before" is not what
    // the file holds.
    bool ground_on_disk(world::CellCoord cell) const;
    // Whether the cell's file is in the scene (loaded).
    bool cell_loaded(world::CellCoord cell) const { return m_loaded.contains(cell); }
    // Whether the cell's file holds a tile not in the scene yet that streaming
    // will bring (on disk, being read or waiting; not one that failed).
    bool ground_coming(world::CellCoord cell) const;
    // The far ground chunks (world/far_ground.h) with a file, by level (1, 2).
    const std::unordered_set<world::CellCoord, world::CellCoordHash>& far_chunks(u32 level) const;
    std::filesystem::path far_chunk_path(u32 level, world::CellCoord chunk) const;

    // Once a frame, after `frame` was built from `scene`: adds what is ready,
    // starts reads near the camera, and now and then unloads far cells that
    // have no unsaved edits. Does nothing unless active(generation).
    void update(world::Scene& scene, u64 generation, const DVec3& camera, const SceneFrame& frame);

    // Cells whose contents differ from their file, or that have no file
    // yet. Walks the whole scene.
    u32 unsaved_cells(const world::Scene& scene) const;
    // Whether the scene's world-wide settings (lighting, kits, scatter
    // layers, weather) differ from what world.json holds.
    bool unsaved_settings(const world::Scene& scene) const;

    // What streaming knows of the scene now, to keep with a copy of it that
    // an undo or redo may put back (swap_scene): restore() then makes the
    // bookkeeping match that copy again. A memo from another world (or one
    // closed since) is ignored.
    // `scene` is the world's scene now: every id it has handed out is
    // never handed out again by a scene restore() puts back.
    struct Memo;
    std::shared_ptr<const Memo> memo(const world::Scene& scene) const;
    void restore(const Memo& memo, world::Scene& scene);
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
        // Loaded: whether its file holds a terrain tile, and the version
        // (world::Terrain::version) of the scene's tile when it was the
        // file's (0: the scene's tile is not, or was not known to be).
        bool saved_tile = false;
        u64 saved_version = 0;
        // And the tile's paint version (world::Terrain::paint_version) when
        // its paint was the file's (0: neither has paint).
        u64 saved_paint_version = 0;
        world::CellDamage saved_damage; // loaded: the kept damage its file holds
        u64 written = 0; // when this editor last wrote its file (0: not since open)
    };
    struct Read {
        world::CellCoord cell;
        u64 epoch = 0;
        std::vector<world::Entity> entities; // sorted by id
        std::optional<world::Terrain::Heights> heights;
        world::Terrain::Paint paint; // the tile's (empty: none)
        world::CellDamage damage;
        std::string error;
        bool missing = false; // the file is not there
    };
public:
    struct Memo {
        u64 session = 0;
        std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> cells;
    };

private:
    // Whether `now` (any order) is exactly `saved`.
    static bool matches(std::vector<const world::Entity*>& now, const std::vector<world::Entity>& saved);
    // Whether the scene's tile in a loaded cell is the file's (or neither has one).
    static bool same_tile(const world::Terrain& terrain, world::CellCoord coord, const Cell& cell);
    // Whether the tile's paint is as its file holds it.
    static bool same_paint(const world::Terrain& terrain, world::CellCoord coord, const Cell& cell);
    // Whether the scene's kept damage in a loaded cell is the file's.
    static bool same_damage(const world::Scene& scene, world::CellCoord coord, const Cell& cell);

    bool follows(const world::Scene& scene); // false: replaced, streaming stops
    void start_read(world::CellCoord cell);
    void take_reads();
    void insert(world::Scene& scene);
    // True when it stopped at unloads_per_frame with more far cells to look at.
    bool unload(world::Scene& scene, const DVec3& camera, const SceneFrame& frame);
    void start_reads(const DVec3& camera);
    void set_state(world::CellCoord coord, Cell& cell, State state); // keeps the counts
    void forget(world::CellCoord coord);                            // no file any more
    Saved write(const world::Scene& scene, std::vector<std::string> meshes);
    // Writes the far ground chunks over the cells whose tile changed (m_far_owed; after
    // their files are written: m_ground is the files'). False on an error (in saved).
    bool write_far(const world::Scene& scene,
                   const std::unordered_map<world::CellCoord, world::Terrain::Heights, world::CellCoordHash>& kept,
                   Saved& saved);
    std::filesystem::path cell_path(world::CellCoord cell) const;

    std::filesystem::path m_folder;
    u64 m_generation = 0;
    // The scene object the bookkeeping matches (world::Scene::identity).
    u64 m_scene = 0;
    u64 m_session = 0; // new on each open, create and close
    // Which cell's file each entity was read from or last saved to (kept
    // when its cell unloads): an id met again in that cell's file is the
    // same entity (maybe moved, or put back by a redo, since); met in
    // another cell's file, it is a clash.
    std::unordered_map<world::EntityId, world::CellCoord> m_origin;
    // Past every id any of the world's scenes has handed out (an undo side
    // put back may be behind ones another side has since).
    mutable world::EntityId m_ids_seen = 1;
    u64 m_writes = 0; // cell files written since open (Cell::written)
    u64 m_epoch = 0; // reads from an older epoch (a closed world) are dropped
    world::WorldIndex m_index;
    std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> m_cells; // cells with a file
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_ground;     // whose file holds a tile
    std::array<std::unordered_set<world::CellCoord, world::CellCoordHash>, 2> m_far; // chunk files, levels 1 and 2
    // Cells written whose level-1 chunk is not yet, and level-1 chunks written
    // whose level-2 chunk is not yet (a save cut short owes them to the next).
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_far_owed;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_far_owed2;
    std::vector<Read> m_ready;  // parsed, in the order they finished
    std::array<u32, 5> m_count{}; // cells per State
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_loaded;
    std::vector<world::CellCoord> m_wanted;  // near cells to read, the next at the back
    std::vector<world::CellCoord> m_touched; // cells to read whatever the distance
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
