// The terrain's tiles as cluster meshes (world::Terrain -> geometry ->
// renderer), one per tile, built on a worker thread so sculpting and
// loading never stall a frame (AGENTS.md rule 9). Until a tile's new mesh
// is ready its old one keeps drawing.
//
// A tile's mesh reaches one sample into each neighbour (its far edge, and
// the normals at its edges), so a changed tile rebuilds its eight
// neighbours too. Edge vertices are locked at every level of detail, so
// tiles drawn at different detail still meet without cracks.
#pragma once

#include "more_workers.h"

#include "giant/core/jobs.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "giant/world/terrain.h"

#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

// The heights a tile's mesh is made from: samples() + 3 a side, from one
// before the tile's first sample to one past its neighbour's first (for
// normals), as world::Terrain::sample gives them (NaN where it gives none:
// no ground there, so normals look one way only).
struct TileHeights {
    u32 samples = 0;      // the terrain's samples per tile side
    double spacing = 1.0; // metres between samples
    std::vector<float> heights; // (samples + 3)^2, row by row along +z
    float skirt = 0.0f;         // metres the edge walls hang down (0: none)
};
TileHeights tile_heights(const world::Terrain& terrain, world::CellCoord cell);

// The tile's surface, in metres from its cell's corner: (samples + 1)^2
// vertices with normals from the heights around them, two triangles a
// square (split as world::Terrain::height_at; none with a corner where
// there is no ground), a wall ("skirt") down from each edge if
// heights.skirt, and which vertices are on the edge (to lock).
struct TileSurface {
    geometry::TriangleMesh mesh;
    std::vector<u8> edge;
};
TileSurface tile_surface(const TileHeights& heights);

struct TerrainMeshStats {
    u32 tiles = 0;     // with a mesh drawn
    u32 building = 0;  // waiting for or being built (newer heights than drawn)
    u32 in_flight = 0; // started and not yet taken: being built on the workers, or built (`arrived`)
    u32 arrived = 0;   // of `in_flight`: built, waiting for a frame's uploads to take them
    u32 waiting = 0;   // of `building`: built and handed to the renderer, waiting for room in its geometry pool
    u64 built = 0;     // ever
    u64 triangles = 0; // in the drawn meshes, at full detail
};

class TerrainMeshes {
public:
    // Without a renderer (tests), meshes are built and counted only.
    explicit TerrainMeshes(render::Renderer* renderer);
    ~TerrainMeshes();
    TerrainMeshes(const TerrainMeshes&) = delete;
    TerrainMeshes& operator=(const TerrainMeshes&) = delete;

    // Once a frame, before the scene's frame is built: takes the terrain's
    // changes, looks at queued tiles (a heights snapshot each, at most
    // 8 * builds_per_frame; one as drawn needs no build; one new here
    // settles first), starts builds (at most builds_per_frame,
    // builds_in_flight at once), frees the meshes of tiles gone (at most
    // drops_per_frame), uploads finished meshes (at most
    // uploads_per_frame, and for at most upload_ms_per_frame after the
    // first) and swaps each in once the renderer can draw it.
    void update(world::Terrain& terrain);
    // The tile's mesh drawn now (k_no_mesh: none yet, or no renderer).
    render::MeshId mesh(world::CellCoord cell) const;
    // Whether the tile has a mesh drawn (without a renderer: one built).
    bool drawn(world::CellCoord cell) const;
    // Cells whose mesh changed (came, went or was replaced) since the last call.
    std::vector<world::CellCoord> take_changed();
    // The terrain's changes the last update took (all: a new terrain), for
    // others following the heights (the scatter, scatter_placer.h).
    const world::TerrainChanges& last_changes() const { return m_last_changes; }
    // Blocks until every build started is done and swapped in, starting
    // the rest (tests and tools; never in a frame).
    void finish(world::Terrain& terrain);

    TerrainMeshStats stats() const;
    // Its workers, for other terrain work (making heights) between frames.
    JobSystem& jobs() { return m_jobs; }
    // In play, and behind a game's loading screen (a frame there may take what it takes).
    static constexpr u32 k_per_frame = 8, k_loading_per_frame = 256;
    u32 builds_per_frame = k_per_frame;
    u32 uploads_per_frame = k_per_frame;
    u32 builds_in_flight = k_per_frame;
    u32 drops_per_frame = 64; // meshes of tiles gone, freed a frame
    // Built meshes waiting for a frame's uploads hold no build's place (the workers build on)
    // until this many wait.
    static constexpr u32 k_most_arrived = 32;
    // The most time (ms) a frame's uploads may take, whatever uploads_per_frame allows: handing
    // a mesh to the renderer makes its tables and buffers on this thread (0.3 to 0.5 ms a tile), and
    // eight tiles arriving together were a frame's slowest phase at speed. One is always taken.
    // Behind a loading screen: no limit (k_loading_upload_ms).
    static constexpr float k_upload_ms = 0.5f;
    static constexpr float k_loading_upload_ms = std::numeric_limits<float>::infinity();
    float upload_ms_per_frame = k_upload_ms;
    // Play's budgets, or a loading screen's (a frame there may take what it takes): how many
    // builds and uploads a frame and builds at once (`most`), and the uploads' time.
    void set_budgets(bool loading, u32 most) {
        builds_per_frame = uploads_per_frame = builds_in_flight = most;
        upload_ms_per_frame = loading ? k_loading_upload_ms : k_upload_ms;
    }
    // The clock the uploads are timed by, in ms (tests give their own; empty: the steady clock).
    std::function<double()> clock;
    // Threads that build beside the two that always do, while above zero (a
    // game's loading screen: MoreWorkers::for_loading).
    u32 more_workers = 0;
    u64 handed_to_more_workers() const { return m_more.handed(); } // builds given to them, ever (tests)
    // Frames a newly added tile (and its neighbours) wait before they are
    // looked at, so tiles arriving one after another (a world streaming
    // in) build once, not again as each neighbour arrives. Changes to tiles
    // already there (sculpting) do not wait.
    u32 settle_frames = 6;
    // Edge walls on every tile (TileHeights::skirt): hide the gaps where
    // tiles of different spacing meet (the far ground, editor/src/far_ground.h).
    float skirt_depth = 0.0f;

private:
    struct Tile {
        render::MeshId mesh = render::k_no_mesh; // drawn
        bool drawn = false;    // has a mesh (with no renderer: counted as one)
        u32 triangles = 0;     // of the drawn mesh
        u64 hash = 0;          // of the heights it was made from (0: none)
        u64 wanted = 0;        // the build in flight (0: none wanted)
        bool in_flight = false;
        // Uploaded, waiting to be loaded before it replaces `mesh`.
        bool waiting = false;
        render::MeshId pending = render::k_no_mesh;
        u32 pending_triangles = 0;
        u64 pending_hash = 0;
    };
    struct Result {
        world::CellCoord cell;
        u64 serial = 0;
        u64 hash = 0;
        geometry::ClusterMesh mesh;
        u32 triangles = 0;
    };

    void want(world::CellCoord cell, u64 not_before = 0);
    void start(world::CellCoord cell, Tile& tile, TileHeights heights, u64 hash);
    void commit(world::CellCoord cell, Tile& tile);
    void drop(world::CellCoord cell, Tile& tile);

    render::Renderer* m_renderer;
    u64 m_identity = 0;
    u64 m_serial = 0;
    u64 m_built = 0;
    std::unordered_map<world::CellCoord, Tile, world::CellCoordHash> m_tiles;
    std::deque<world::CellCoord> m_queue; // waiting to start, oldest first
    // Queued cells, each with the frame it may be looked at from.
    std::unordered_map<world::CellCoord, u64, world::CellCoordHash> m_queued;
    u64 m_frame = 0;
    bool m_finishing = false; // finish(): nothing waits to settle
    std::vector<world::CellCoord> m_changed;
    world::TerrainChanges m_last_changes;
    std::deque<Result> m_arrived;            // finished builds waiting for the upload budget
    std::vector<world::CellCoord> m_waiting; // tiles with an uploaded mesh not loaded yet
    u32 m_in_flight = 0;                     // builds started and not taken back

    std::mutex m_mutex; // protects m_done
    std::vector<Result> m_done;
    WaitGroup m_builds;
    JobSystem m_jobs{2};
    MoreWorkers m_more; // last two: destroyed (finishing their jobs) first
};

} // namespace giant::editor
