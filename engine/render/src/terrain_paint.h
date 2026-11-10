// Painted terrain layers on the GPU (photoreal part 5c-2): each ground
// tile's paint (Renderer::set_terrain_paint, k_terrain_paint_channels bytes
// a sample) in one pool buffer, a block a tile, and each frame a small
// table from tile to block (open addressing, terrain.slang's paint_block),
// so terrain.slang finds a sample's weights by its tile. Tiles changed are
// uploaded whole, within a byte budget a frame (at least one): a tile not
// uploaded yet draws as it was (or unpainted).
#pragma once

#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"

#include <unordered_map>
#include <vector>

namespace giant::render {

class TerrainPaintStore {
public:
    explicit TerrainPaintStore(rhi::Device& device) : m_device(device) {}
    ~TerrainPaintStore();
    TerrainPaintStore(const TerrainPaintStore&) = delete;
    TerrainPaintStore& operator=(const TerrainPaintStore&) = delete;

    void set_grid(double tile_size, u32 samples); // a change drops all paint
    double tile_size() const { return m_tile_size; }
    u32 samples() const { return m_samples; }
    bool set(i32 x, i32 z, std::vector<u8> weights);
    void clear();
    usize tiles() const { return m_tiles.size(); }
    usize waiting() const { return m_waiting.size(); }

    // Inside a frame, before the terrain pass: uploads what changed (within
    // `budget` bytes), then the table. False: nothing to draw from (no
    // paint, or out of memory).
    struct Frame {
        rhi::TransientMemory table; // entries of 16 bytes: x, z, block byte offset (~0: empty), 0
        u32 table_mask = 0;         // entries - 1 (a power of two)
        rhi::BufferHandle pool;
    };
    bool update(rhi::CommandList& cmd, u64 budget, Frame& out);

private:
    struct Key {
        i32 x = 0;
        i32 z = 0;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const;
    };
    struct Tile {
        std::vector<u8> weights;
        u32 block = ~0u;      // its block in the pool (~0: none yet)
        bool shown = false;   // its block holds weights it had (it is in the table)
        bool waiting = false; // `weights` not uploaded yet (in m_waiting)
    };
    u64 block_bytes() const { return u64{m_samples} * m_samples * k_terrain_paint_channels; }
    bool grow(rhi::CommandList& cmd, u32 blocks);

    rhi::Device& m_device;
    double m_tile_size = 0.0;
    u32 m_samples = 0;
    std::unordered_map<Key, Tile, KeyHash> m_tiles;
    std::vector<Key> m_waiting;  // tiles to upload, oldest first (each once)
    std::vector<u32> m_free;     // blocks no tile has
    u32 m_blocks = 0;            // blocks the pool holds
    rhi::BufferHandle m_pool;
    std::vector<u32> m_table;    // the last table built (4 words an entry)
    bool m_table_stale = true;
    usize m_shown = 0;         // tiles in the table
    bool m_full_logged = false;
};

// terrain.slang's paint table hash of a tile.
inline u32 terrain_paint_hash(i32 x, i32 z) {
    u32 h = static_cast<u32>(x) * 0x9E3779B1u ^ (static_cast<u32>(z) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

} // namespace giant::render
