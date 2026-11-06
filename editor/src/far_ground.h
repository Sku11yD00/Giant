// The far ground of a streamed world: past the cells loaded in full, the
// ground is drawn from the world's coarse chunks (world/far_ground.h), so
// it reaches the horizon for little memory. Level 1 chunks (512 m, heights
// 16 m apart) near the camera, level 2 (4096 m, 128 m apart) beyond.
//
// Chunk files are read on a worker thread and added a few a frame; each
// level is a world::Terrain of chunks with its own TerrainMeshes, so its
// meshes build on workers too and swap in only once they can draw
// (AGENTS.md rule 9).
//
// What draws where (a quadtree of branching 8): a level-2 chunk draws its
// mesh unless every level-1 chunk in it can stand in (its mesh is drawn, or
// every cell of it with ground has its full tile drawn, or it has no file);
// a level-1 chunk not hidden by its parent draws the full tiles of its
// cells when every one of them is drawn, else its own mesh (else what
// tiles it has). A full tile draws only where its level-1 chunk says so.
// Tiles of different levels meet without gaps by their edge walls
// (TerrainMeshes::skirt_depth).
//
// A chunk is the world's files: edits show in the far ground once saved
// (WorldStreaming keeps the full tiles of a level-1 chunk with unsaved
// edits loaded, so near edits always draw in full).
#pragma once

#include "terrain_meshes.h"
#include "world_streaming.h"

#include "giant/core/jobs.h"
#include "giant/world/far_ground.h"

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

struct FarGroundSettings {
    // Chunks with any part this near the camera (on the ground) load, by
    // level (1, 2), and unload past radius + margin. Level 1's must reach
    // past the load radius by a level-2 chunk's diagonal (5793 m), so a
    // level-2 chunk near the camera can always hand over to its children:
    // it is taken as at least load_radius + 5800 (and level 2 as at least level 1).
    std::array<double, 2> radius{8192.0, 32768.0};
    std::array<double, 2> margin{512.0, 4096.0};
    u32 reads_in_flight = 8;
    u32 chunks_per_frame = 16; // added to a level, a frame
    u32 unloads_per_frame = 64;
};

struct FarGroundStats {
    std::array<u32, 2> files{};   // chunk files, by level
    std::array<u32, 2> loaded{};  // chunks held
    std::array<u32, 2> drawn{};   // chunks drawing their mesh
    u32 hidden = 0;               // level-1 chunks under a level-2 chunk drawing
    std::array<u32, 2> meshes{};  // chunks with a mesh built
    std::array<u32, 2> building{};
    u32 reading = 0;
    u32 failed = 0; // chunk files that could not be read (see the log)
};

class FarGround {
public:
    // Without a renderer (tests), meshes are built and counted only.
    explicit FarGround(render::Renderer* renderer);
    ~FarGround();
    FarGround(const FarGround&) = delete;
    FarGround& operator=(const FarGround&) = delete;

    // Once a frame, after `ground` (the scene's tiles) was updated and before
    // the scene's frame is built: follows the streamed world (nothing while
    // !streaming.active(generation)), reads and adds chunks near the camera,
    // lets far ones go, builds their meshes and works out what draws.
    // `terrain` is the scene's (whose tiles `ground` draws).
    void update(const DVec3& camera, const WorldStreaming& streaming, u64 generation, TerrainMeshes& ground,
                const world::Terrain& terrain);
    // After a save: the chunks it wrote replace the ones held.
    void saved(const WorldStreaming::Saved& saved);

    // Whether the cell's full tile draws (its level-1 chunk says so).
    bool draws_tile(world::CellCoord cell) const;
    // The far chunks drawn from a cell (the one at a chunk's corner).
    struct Piece {
        u32 level = 0;
        world::CellCoord chunk;
        render::MeshId mesh = render::k_no_mesh;
    };
    void pieces(world::CellCoord cell, std::vector<Piece>& out) const;
    // Every cell pieces() gives something for.
    std::vector<world::CellCoord> piece_cells() const;
    // Cells whose ground drawing changed since the last call: `ground`'s
    // changed tiles (taken from it in update), tiles shown or hidden, and
    // corner cells whose pieces changed.
    std::vector<world::CellCoord> take_changed();

    // Blocks until every chunk near the camera is read, added and built,
    // and what draws is worked out (tests and tools; never in a frame).
    void finish(const DVec3& camera, const WorldStreaming& streaming, u64 generation, TerrainMeshes& ground,
                const world::Terrain& terrain);
    // Blocks until reads in flight are done (before a save writes the files they read).
    void wait_reads();

    FarGroundStats stats() const;
    // The level's chunks and meshes (1, 2).
    const world::Terrain& chunks(u32 level) const { return m_levels[level - 1].terrain; }
    TerrainMeshes& meshes(u32 level) { return *m_levels[level - 1].meshes; }
    FarGroundSettings settings;

private:
    enum class Mode : u8 { tiles, mesh, hidden }; // a level-1 chunk: what draws over it
    struct Level {
        world::Terrain terrain;
        std::unique_ptr<TerrainMeshes> meshes;
        std::unordered_map<world::CellCoord, u64, world::CellCoordHash> reading; // chunk -> its read's serial
        std::vector<world::CellCoord> wanted; // to read, the nearest at the back
        world::CellCoord wanted_at{};
        double wanted_radius = -1.0;
        bool wanted_stale = true;
    };
    struct Read {
        u32 level = 0;
        world::CellCoord chunk;
        u64 serial = 0;
        std::optional<world::Terrain::Heights> heights; // none: could not be read
    };

    void clear(const TerrainMeshes& ground);
    double radius(u32 level, const WorldStreaming& streaming) const; // as used: level 1 at least load_radius + 5800
    void take_reads(const WorldStreaming& streaming);
    void start_reads(const DVec3& camera, const WorldStreaming& streaming);
    void unload(const DVec3& camera, const WorldStreaming& streaming);
    void select(TerrainMeshes& ground, const world::Terrain& terrain, const WorldStreaming& streaming);
    bool covered(world::CellCoord chunk, const TerrainMeshes& ground, const world::Terrain& terrain,
                 const WorldStreaming& streaming) const;
    void mark_tiles(world::CellCoord chunk, const TerrainMeshes& ground);

    std::array<Level, 2> m_levels;
    u64 m_session = 0;
    std::filesystem::path m_folder;
    bool m_open = false;
    u64 m_serial = 0;
    std::array<std::unordered_set<world::CellCoord, world::CellCoordHash>, 2> m_failed;
    std::vector<Read> m_arrived; // read, waiting for the per-frame budget (still in Level::reading)
    std::array<u32, 2> m_files{};

    // What draws.
    std::unordered_map<world::CellCoord, Mode, world::CellCoordHash> m_mode;   // level 1 (absent: tiles)
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_draw2;        // level 2 chunks drawing
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_covered;      // level 1: every tile drawn
    std::array<std::unordered_set<world::CellCoord, world::CellCoordHash>, 2> m_dirty; // to look at again
    std::vector<world::CellCoord> m_changed;

    std::mutex m_mutex; // protects m_finished
    std::vector<Read> m_finished;
    WaitGroup m_reads;
    JobSystem m_jobs{1}; // last: destroyed (finishing its jobs) first
};

} // namespace giant::editor
