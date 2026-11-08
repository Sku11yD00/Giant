// Navigation: where people can walk, and how to get from here to there,
// over a streamed open world.
//
//   nav::NavMap map;
//   map.set_tile(nav::build_tile(cell, terrain, obstacles, settings)); // per world cell, on a worker
//   nav::PathSearch search(map, from, to);
//   while (search.state() == nav::SearchState::searching) search.advance(4096); // a budget a frame
//   const std::vector<DVec3>& corners = search.path();
//
// The walkable ground is a grid per world cell (world::k_cell_size, 64 m):
// k_nav_side samples a side, 0.5 m apart, each with the height to stand at
// and whether it can be stood on. A sample is walkable where there is
// ground no steeper than max_slope_degrees, with nothing in the way from
// step_height above it up to `clearance` (a wall, a crate, a tree's
// trunk), kept agent_radius from what is (obstacles grow by it). Low
// things (a kerb, a pavement slab, a floor) are stood on: their top is the
// height there. Tiles are built alone (build_tile: pure, any thread), from
// the terrain and the obstacles in and around their cell, so a streamed
// world builds the tiles near what walks and drops the rest.
//
// A path is searched over the samples of the tiles there are (8 ways from
// each, no step between neighbours higher than step_height, no corner
// cut past a blocked one), A* in bounded slices (advance: rule 9, a long
// search spreads over frames), from and to the nearest walkable samples
// (within snap_distance), then pulled straight where the way is clear
// (corners only where it turns). A search that reaches a tile not built
// says which (missing): build it and go on.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/terrain.h"

#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <unordered_map>
#include <vector>

namespace giant::nav {

inline constexpr u32 k_nav_side = 128; // samples a tile side
inline constexpr double k_nav_spacing = world::k_cell_size / k_nav_side; // 0.5 m

struct NavSettings {
    float max_slope_degrees = 40.0f; // steeper ground is a wall
    float step_height = 0.4f;        // what is walked up (a step, a kerb) and stood on
    float clearance = 1.8f;          // head room needed above the ground
    float agent_radius = 0.35f;      // kept from obstacles
};

// Something in the way (or stood on, if low enough): a box in the world.
struct Obstacle {
    DVec3 center{};
    Quat rotation{};
    Vec3 half{0.5f, 0.5f, 0.5f}; // half its size along its own axes, metres
};

// One world cell's walkable ground.
struct NavTile {
    world::CellCoord cell;
    std::vector<float> height; // per sample (row by row along +z, each along +x): where one stands (NaN: no ground)
    std::vector<u8> walkable;  // 1: can be stood on
    u32 walkable_count = 0;
    u64 version = 0; // new each build, unique in the process
    // Sample (i, j)'s centre, on the ground (x, z).
    static DVec3 sample_point(world::CellCoord cell, u32 i, u32 j);
};

// The cell's tile from the terrain and the obstacles near it (those whose
// footprint reaches it, grown by agent_radius: others are skipped).
NavTile build_tile(world::CellCoord cell, const world::Terrain& terrain, std::span<const Obstacle> obstacles,
                   const NavSettings& settings);

// The tiles there are, by cell.
class NavMap {
public:
    explicit NavMap(const NavSettings& settings = {}) : m_settings(settings) {}

    void set_tile(NavTile tile);
    void remove_tile(world::CellCoord cell);
    void clear();
    const NavTile* find(world::CellCoord cell) const;
    usize size() const { return m_tiles.size(); }
    const std::unordered_map<world::CellCoord, NavTile, world::CellCoordHash>& tiles() const { return m_tiles; }
    const NavSettings& settings() const { return m_settings; }
    // Changes whenever a tile comes, goes or changes.
    u64 version() const { return m_version; }

    // Samples by whole-number coordinates from the world origin (sample
    // (gi, gj) is sample (gi - x * k_nav_side, ...) of cell (x, z)).
    struct Sample {
        bool built = false; // its tile is there
        bool walkable = false;
        float height = 0.0f;
    };
    Sample sample(i64 gi, i64 gj) const;
    // (Clamped to +-1e12 m: far past any world, inside i64.)
    static i64 sample_of(double metres) {
        const double m = metres >= -1e12 ? (metres <= 1e12 ? metres : 1e12) : -1e12; // NaN: -1e12
        return static_cast<i64>(std::floor(m / k_nav_spacing));
    }
    static world::CellCoord cell_of_sample(i64 gi, i64 gj);
    // Whether one can stand at (x, z), and the height there.
    Sample at(double x, double z) const { return sample(sample_of(x), sample_of(z)); }

private:
    NavSettings m_settings;
    std::unordered_map<world::CellCoord, NavTile, world::CellCoordHash> m_tiles;
    u64 m_version = 0;
};

enum class SearchState : u8 {
    searching, // advance more
    missing,   // waits for a tile not built (missing_tile): build it, then advance
    found,     // path() holds it
    failed,    // no way there (or no ground at either end, or too far: reason())
};
const char* to_string(SearchState state);

// Farthest from the origin (x or z, metres) a search's ends may be.
inline constexpr double k_max_nav_coordinate = 1e9;

struct SearchLimits {
    float snap_distance = 2.0f; // metres an end may be moved to stand on walkable ground
    u32 max_nodes = 1u << 20;   // samples looked at in all, before it gives up (about 0.8 km x 0.8 km of open ground)
};

class PathSearch {
public:
    PathSearch(const NavMap& map, const DVec3& from, const DVec3& to, const SearchLimits& limits = {});

    // Does up to about `nodes` samples' worth of work: samples looked at
    // while searching, then the path walked back and pulled straight (a
    // found path spreads over calls too). If a tile it read was rebuilt
    // or dropped since, it starts again. Returns the state after.
    SearchState advance(u32 nodes);
    SearchState state() const { return m_state; }
    world::CellCoord missing_tile() const { return m_missing; }
    const char* reason() const { return m_reason; }
    // The corners from the start to the end (found): heights where one
    // stands; the first the start, the last the end (each as snapped).
    const std::vector<DVec3>& path() const { return m_path; }
    double length() const; // metres along it
    u32 nodes() const { return m_nodes; } // looked at so far
    u64 work() const { return m_work; }   // samples' worth of work so far (what advance's budget counts)
    u32 restarts() const { return m_restarts; } // started again after a tile it read changed

private:
    struct Node {
        float f = 0.0f;
        i64 i = 0;
        i64 j = 0;
        bool operator>(const Node& o) const { return f > o.f; }
    };
    bool snap(const DVec3& p, i64& i, i64& j, bool& missing, u64& work);
    // Whether the tiles it has read are as they were (else start again).
    bool unchanged();
    void restart();
    // The found path made, within the budget left (false: not yet).
    bool finish(u64& looked, u64 budget);
    bool clear_line(i64 ai, i64 aj, i64 bi, i64 bj) const;
    DVec3 point(i64 i, i64 j) const;

    const NavMap& m_map;
    DVec3 m_from{};
    DVec3 m_to{};
    SearchLimits m_limits;
    SearchState m_state = SearchState::searching;
    world::CellCoord m_missing{};
    const char* m_reason = "";
    bool m_started = false;
    i64 m_si = 0, m_sj = 0, m_gi = 0, m_gj = 0;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> m_open;
    // What the search knows of a sample, kept by tile (dense: lookups are
    // most of its time).
    struct Seen {
        float g = std::numeric_limits<float>::infinity(); // metres from the start (inf: not reached)
        u8 from = 0xFF; // the neighbour (0 to 7) it was reached from; 0xFF: none (the start)
        u8 closed = 0;
    };
    Seen& seen(i64 i, i64 j);
    // m_map.sample, through the last tile looked at.
    NavMap::Sample at(i64 i, i64 j);
    const NavTile* m_tile = nullptr;
    world::CellCoord m_tile_cell{};
    u64 m_tile_version = ~u64{0}; // the map's, when m_tile was found
    std::unordered_map<world::CellCoord, u32, world::CellCoordHash> m_block_of; // cell -> into m_blocks
    std::vector<std::vector<Seen>> m_blocks;
    std::vector<u64> m_block_tile; // each block's tile's version, as it was read (0: none)
    u64 m_checked_version = 0;     // the map's, when the blocks' tiles were last found unchanged
    world::CellCoord m_last_cell{};
    u32 m_last_block = ~0u;
    u32 m_nodes = 0;
    u64 m_work = 0;
    u32 m_restarts = 0;
    // Making the path once the end is reached: walking back, then pulling straight.
    enum class Phase : u8 { search, walk, pull };
    Phase m_phase = Phase::search;
    std::vector<std::pair<i64, i64>> m_cells;
    i64 m_wi = 0, m_wj = 0;
    usize m_anchor = 0, m_best = 0, m_k = 0;
    std::vector<DVec3> m_path;
};

} // namespace giant::nav
