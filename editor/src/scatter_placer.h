// The scene's scatter layers (world/scatter.h) placed over its ground,
// cell by cell, on a worker thread (AGENTS.md rule 9): a cell is placed
// again when its tile or a neighbour's changes (the placements read the
// ground a little past the cell) or the layers change, and its new
// placements swap in whole, so a frame never waits and a cell never shows
// half its points. scene_frame draws them with the cell's entities.
#pragma once

#include "giant/core/jobs.h"
#include "giant/world/scatter.h"
#include "giant/world/scene.h"

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

struct ScatterStats {
    u32 cells = 0;      // with placements
    u64 points = 0;     // placements in all
    u32 building = 0;   // cells waiting to be placed, or being placed
    u32 cut_short = 0;  // cells with more than k_max_points_per_cell (the rest left out)
    u64 placed = 0;     // cells placed, ever
};

class ScatterPlacer {
public:
    using Layers = std::vector<world::ScatterLayer>;
    // A cell's placements and the layers they were made from (their items).
    struct Cell {
        std::shared_ptr<const Layers> layers;
        std::vector<world::ScatterPoint> points;
    };
    static constexpr usize k_max_points_per_cell = 1u << 18;

    ScatterPlacer();
    ~ScatterPlacer();
    ScatterPlacer(const ScatterPlacer&) = delete;
    ScatterPlacer& operator=(const ScatterPlacer&) = delete;

    // Once a frame, after the terrain's changes were taken (`changes`, from
    // TerrainMeshes::last_changes): looks at the layers (compared with the
    // last frame's) and the cells to place again (at most looks_per_frame),
    // starts placements (cells_in_flight at once), and swaps in finished
    // ones (at most points_per_frame points, and one cell at least).
    void update(const world::Scene& scene, const world::TerrainChanges& changes);
    // The cell's placements drawn now (none: no points).
    const Cell* find(world::CellCoord cell) const;
    // Cells whose placements changed since the last call.
    std::vector<world::CellCoord> take_changed();
    // Every cell with placements.
    std::vector<world::CellCoord> cells() const;
    // Blocks until every cell is placed (tests and tools; never in a frame).
    // `changes` as for update (what the terrain changed since the last update).
    void finish(const world::Scene& scene, const world::TerrainChanges& changes);

    ScatterStats stats() const;
    u32 looks_per_frame = 256;
    u32 cells_in_flight = 4;
    usize points_per_frame = 65536;

private:
    struct Placed {
        Cell cell;
        u64 key = 0; // layers and ground it was made from
    };
    struct Result {
        world::CellCoord cell;
        u64 serial = 0;
        u64 key = 0;
        Cell placed;
        bool cut_short = false;
    };

    void want(world::CellCoord cell);
    void want_around(world::CellCoord cell);
    u64 key_of(const world::Terrain& terrain, world::CellCoord cell) const;

    std::shared_ptr<const Layers> m_layers = std::make_shared<const Layers>();
    u64 m_layers_serial = 1;
    u64 m_terrain = 0; // the terrain's identity, as last seen
    u64 m_serial = 0;
    u64 m_placed_count = 0;
    std::unordered_map<world::CellCoord, Placed, world::CellCoordHash> m_cells;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_cut_short;
    std::deque<world::CellCoord> m_queue;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_queued;
    struct Flight {
        u64 serial = 0;
        u64 key = 0;
    };
    std::unordered_map<world::CellCoord, Flight, world::CellCoordHash> m_wanted; // the newest placement in flight
    std::deque<Result> m_arrived;
    std::vector<world::CellCoord> m_changed;
    u32 m_in_flight = 0;

    std::mutex m_mutex; // protects m_done
    std::vector<Result> m_done;
    WaitGroup m_jobs_done;
    JobSystem m_jobs{1}; // last: destroyed (finishing its jobs) first
};

} // namespace giant::editor
