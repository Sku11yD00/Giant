// The editor's navigation (giant/nav/nav.h): the walkable ground of the
// world cells that are asked for, built on workers from the terrain, the
// scene's fixed bodies and the scatter trees, kept up to date as they
// change, and paths searched over it a bounded share a frame.
//
// Tiles are built when a search reaches one not built, at most
// tiles_in_flight at once on two
// workers, from copies (the terrain's tiles around, the obstacles there),
// so the scene changes freely meanwhile; a tile whose inputs changed
// while it was built is built again. Built tiles are checked against
// their inputs a few a frame (the terrain's tile versions, the scatter
// placements, the entities in and around them), and those changed are
// built again; the oldest unused go past max_tiles.
//
// Obstacles: entities with a fixed body (shown or not, as the physics has them) (cube, sphere, plane,
// mesh, kit pieces: boxes of their size, turned as they are; spheres and
// meshes the box around them; a kit's pieces knocked loose left out), and
// scatter trees' trunks. Dynamic bodies, swinging things, animated meshes,
// cloth and decals are not obstacles (what moves is avoided by what walks,
// not by the map). Where there is no terrain, the lowest obstacle top with
// head room is the ground there (nav.h: a floor plane, a deck). An entity
// whose footprint covers more than k_large_cells cells (a floor under a
// whole town) is kept apart and looked at for every tile.
//
// View state: nothing of it is saved.
#pragma once

#include "mesh_library.h"
#include "scatter_placer.h"

#include "giant/core/jobs.h"
#include "giant/nav/nav.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

// The boxes an entity stands in the way with (none for those that do not).
void nav_obstacles(const world::Scene& scene, const world::Entity& e, const render::Renderer* renderer,
                   const MeshLibrary* meshes, std::vector<nav::Obstacle>& out);

// More cells than this under an entity's footprint: kept apart (m_large).
inline constexpr usize k_large_cells = 64;

class NavState {
public:
    // Without a renderer or meshes (tests), mesh entities are boxes of their scale.
    NavState(const render::Renderer* renderer, const MeshLibrary* meshes, const ScatterPlacer* scatter);
    ~NavState();
    NavState(const NavState&) = delete;
    NavState& operator=(const NavState&) = delete;

    // The scene's changes followed (as the cloth's): the tiles they touch
    // marked to be built again.
    void follow(const world::Scene& scene, const world::SceneChanges& changed);
    // Each frame: finished tiles taken, tiles checked and started, searches
    // moved on (at most nodes_per_frame samples between them).
    void update(const world::Scene& scene);
    u32 nodes_per_frame = 1536;  // about 0.7 ms
    u32 tiles_in_flight = 2;
    u32 checks_per_frame = 32;   // built tiles looked at for changed inputs
    usize max_tiles = 512;       // about 40 MB

    // A search, run over frames; its handle. One found over a tile older
    // than what it is built from is searched again once that tile is built
    // (search() then shows the new one, searching: a few times at most).
    using SearchId = u32;
    SearchId find_path(const DVec3& from, const DVec3& to);
    const nav::PathSearch* search(SearchId id) const;
    void cancel(SearchId id);
    usize searches() const { return m_searches.size(); }
    // At once (a command): the tiles it needs built here (at most
    // max_tiles_now), then searched to the end (at most `nodes`).
    // `stale`: the path found crosses a tile older than what it is built
    // from that it could not build again (past max_tiles_now).
    std::unique_ptr<nav::PathSearch> find_path_now(const world::Scene& scene, const DVec3& from, const DVec3& to,
                                                   u32 max_tiles_now = 64, u32* built = nullptr, bool* stale = nullptr);
    // The tile of `cell` built now if it is not (or is stale).
    const nav::NavTile* tile_now(const world::Scene& scene, world::CellCoord cell);

    const nav::NavMap& map() const { return m_map; }
    usize building() const { return m_building.size(); }
    usize stale() const { return m_dirty.size(); }
    u64 tiles_built() const { return m_built; } // ever
    const nav::NavSettings& settings() const { return m_map.settings(); }

private:
    struct Inputs {
        u64 terrain[9] = {}; // the terrain's tile versions around (3 x 3)
        u64 scatter[9] = {}; // the scatter placements' serials around
        u64 entities = 0;    // the cell's entity stamp
        u64 large = 0;       // the large entities' stamp
        bool operator==(const Inputs&) const = default;
    };
    struct Building {
        world::CellCoord cell;
        Inputs inputs;
        std::unique_ptr<WaitGroup> done;
        std::shared_ptr<nav::NavTile> result;
    };
    void follow_one(const world::Scene& scene, world::EntityId id);
    void follow_all(const world::Scene& scene);
    void unplace(world::EntityId id);
    void touch(world::CellCoord cell);
    void want(world::CellCoord cell); // a search waits for it
    Inputs inputs_of(const world::Scene& scene, world::CellCoord cell) const;
    // What build_tile needs, copied: the terrain around, the obstacles there.
    struct Job {
        world::Terrain terrain;
        std::vector<nav::Obstacle> obstacles;
    };
    Job job_of(const world::Scene& scene, world::CellCoord cell) const;
    void start(const world::Scene& scene, world::CellCoord cell);
    void keep(nav::NavTile tile, const Inputs& inputs);
    void evict();

    const render::Renderer* m_renderer;
    const MeshLibrary* m_meshes;
    const ScatterPlacer* m_scatter;
    JobSystem m_jobs{2};
    nav::NavMap m_map;
    u64 m_scene = 0;
    // Each entity's obstacles and the cells they reach; each cell's entities.
    struct Placed {
        std::vector<nav::Obstacle> boxes;
        std::vector<world::CellCoord> cells;
    };
    std::unordered_map<world::EntityId, Placed> m_placed;
    std::unordered_map<world::CellCoord, std::vector<world::EntityId>, world::CellCoordHash> m_in_cell;
    std::unordered_map<world::CellCoord, u64, world::CellCoordHash> m_stamp; // the cell's entities changed
    std::unordered_set<world::EntityId> m_large; // placed with no cells: looked at for every tile
    u64 m_large_stamp = 0;
    u64 m_next_stamp = 1;
    u64 m_meshes_version = 0;
    std::vector<world::Kit> m_kits; // as last followed (the scene does not track them)
    std::unordered_map<world::CellCoord, Inputs, world::CellCoordHash> m_inputs; // each built tile's
    std::unordered_map<world::CellCoord, u64, world::CellCoordHash> m_used;      // frame last used
    std::vector<Building> m_building;
    std::set<std::pair<i32, i32>> m_dirty; // built tiles to build again
    std::deque<world::CellCoord> m_wanted; // tiles searches wait for, first come first
    std::vector<world::CellCoord> m_check; // scratch: built tiles in check order
    usize m_check_next = 0;
    u64 m_frame = 0;
    u64 m_built = 0;
    struct Search {
        std::unique_ptr<nav::PathSearch> search;
        DVec3 from{}, to{};
        u32 redone = 0; // searched again, found over a stale tile
        std::vector<world::CellCoord> waits; // found over these stale tiles: searched again once they are built
    };
    std::unordered_map<SearchId, Search> m_searches;
    // Entities to look at again (their kit or mesh changed), a share a frame.
    std::deque<world::EntityId> m_refollow;
    std::unordered_set<world::EntityId> m_refollowing;
    u32 refollows_per_frame = 256;
    bool stale_at(const world::Scene& scene, world::CellCoord cell) const;
    SearchId m_next_search = 1;
};

} // namespace giant::editor
