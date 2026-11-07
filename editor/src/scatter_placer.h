// The scene's scatter layers (world/scatter.h) placed over its ground,
// cell by cell, on worker threads (AGENTS.md rule 9): a cell is placed
// again when its tile or a neighbour's changes (the placements read the
// ground a little past the cell) or the layers change, and its new
// placements swap in whole, so a frame never waits and a cell never shows
// half its points. Only cells within `radius` of the camera are placed,
// nearest first, and all of them together hold at most max_instances
// instances (a kit item counts its pieces): a cell arriving over it takes
// room from farther cells, and cells cut short try again when room frees.
// scene_frame draws them with the cell's entities.
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
    u64 instances = 0;  // what they draw (a kit item's pieces each)
    u32 building = 0;   // cells waiting to be placed, or being placed
    u32 cut_short = 0;  // cells cut short or left out (k_max_instances_per_cell, max_instances)
    u64 placed = 0;     // cells placed, ever
};

class ScatterPlacer {
public:
    using Layers = std::vector<world::ScatterLayer>;
    // A cell's placements and the layers they were made from (their items).
    struct Cell {
        std::shared_ptr<const Layers> layers;
        std::vector<world::ScatterPoint> points;
        u64 instances = 0; // what they draw
        u64 serial = 0;    // another for each placement swapped in (never 0)
    };
    // A cell's instances at most (one swaps in whole, in a frame: about
    // 1.5 ms; a layer at 0.5 m spacing fills one).
    static constexpr usize k_max_instances_per_cell = 1u << 14;

    ScatterPlacer();
    ~ScatterPlacer();
    ScatterPlacer(const ScatterPlacer&) = delete;
    ScatterPlacer& operator=(const ScatterPlacer&) = delete;

    // Once a frame, after the terrain's changes were taken (`changes`, from
    // TerrainMeshes::last_changes) and `camera` was set: looks at the
    // layers (compared with the last frame's) and the cells to place again
    // (at most looks_per_frame), starts placements (cells_in_flight at
    // once), and swaps in finished ones (at most instances_per_frame
    // instances, and one cell at least).
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
    u64 instances() const { return m_instances; } // what the cells draw now
    DVec3 camera{};        // where the camera is (set before update)
    double radius = 2048.0; // metres from the camera to a cell's centre, at most, for it to be placed
    u64 max_instances = 1u << 19;
    u32 looks_per_frame = 256;
    u32 cells_in_flight = 4;
    usize instances_per_frame = 4096; // every part of the renderer looks at each one the frame it arrives

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
        std::vector<u32> weights; // instances a point of each item draws (by first[layer] + item)
        std::vector<u32> first;
        bool cut_short = false; // by the budget (max_instances)
        bool capped = false;    // by k_max_instances_per_cell
    };

    void want(world::CellCoord cell, bool first = false);
    void want_around(world::CellCoord cell);
    u64 key_of(const world::Terrain& terrain, world::CellCoord cell) const;
    bool in_range(world::CellCoord cell) const;
    double distance2(world::CellCoord cell) const;
    void drop_all();
    void remove(world::CellCoord cell);
    double farthest2() const; // of the cells placed (squared; -1: none)

    std::shared_ptr<const Layers> m_layers = std::make_shared<const Layers>();
    u64 m_layers_serial = 1;
    u64 m_terrain = 0; // the terrain's identity, as last seen
    u64 m_serial = 0;
    u64 m_placed_count = 0;
    u64 m_instances = 0; // in m_cells
    world::CellCoord m_camera_cell{0, 0};
    double m_radius = -1.0; // as last seen
    u64 m_kits = 0;         // the piece counts of the kits items name, as last seen
    bool m_freed = false;   // room freed this frame
    std::unordered_map<world::CellCoord, Placed, world::CellCoordHash> m_cells;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_cut_short; // by the budget, or left out: retried
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_capped;    // by the cell cap: not retried
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
    JobSystem m_jobs{2}; // last: destroyed (finishing its jobs) first
};

} // namespace giant::editor
