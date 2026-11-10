#include "terrain_paint.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace giant::render {

usize TerrainPaintStore::KeyHash::operator()(const Key& k) const { return terrain_paint_hash(k.x, k.z); }

TerrainPaintStore::~TerrainPaintStore() {
    if (m_pool.valid()) m_device.destroy(m_pool);
}

void TerrainPaintStore::set_grid(double tile_size, u32 samples) {
    if (tile_size == m_tile_size && samples == m_samples) return;
    clear();
    if (m_pool.valid()) m_device.destroy(m_pool); // blocks of another size
    m_pool = {};
    m_blocks = 0;
    m_free.clear();
    m_tile_size = tile_size;
    m_samples = samples;
}

bool TerrainPaintStore::set(i32 x, i32 z, std::vector<u8> weights) {
    if (m_samples == 0) return false;
    const Key key{x, z};
    if (weights.empty()) {
        const auto it = m_tiles.find(key);
        if (it == m_tiles.end()) return true;
        if (it->second.block != ~0u) {
            m_free.push_back(it->second.block);
            m_full = false;
        }
        m_tiles.erase(it);
        std::erase(m_waiting, key);
        m_table_stale = true;
        return true;
    }
    if (weights.size() != block_bytes()) return false;
    Tile& t = m_tiles[key];
    t.weights = std::move(weights); // a tile shown keeps drawing its block until uploaded
    if (!t.waiting) {
        t.waiting = true;
        m_waiting.push_back(key);
    }
    return true;
}

void TerrainPaintStore::clear() {
    for (const auto& [key, tile] : m_tiles) {
        if (tile.block != ~0u) m_free.push_back(tile.block);
    }
    m_tiles.clear();
    m_waiting.clear();
    m_table_stale = true;
    m_full_logged = false;
    m_full = false;
}

bool TerrainPaintStore::grow(rhi::CommandList& cmd, u32 blocks) {
    const u64 bytes = block_bytes();
    const u32 most = static_cast<u32>(std::max<u64>(1, m_max_pool_bytes / bytes));
    if (blocks > most) return false;
    const u32 cap = std::min(most, std::max({blocks, m_blocks * 2, 16u}));
    rhi::BufferDesc desc;
    desc.size = u64{cap} * bytes;
    desc.usage = rhi::BufferUsage::shader_read;
    desc.name = "terrain paint";
    const rhi::BufferHandle grown = m_device.create_buffer(desc);
    if (!grown.valid()) return false;
    if (m_pool.valid()) {
        cmd.barrier(m_pool, rhi::ResourceState::copy_source);
        cmd.barrier(grown, rhi::ResourceState::copy_dest);
        cmd.copy_buffer(grown, 0, m_pool, 0, u64{m_blocks} * bytes);
        m_device.destroy(m_pool); // destruction waits for frames in flight
    }
    for (u32 b = cap; b > m_blocks; --b) m_free.push_back(b - 1);
    m_pool = grown;
    m_blocks = cap;
    return true;
}

bool TerrainPaintStore::update(rhi::CommandList& cmd, u64 budget, bool table, Frame& out) {
    out = {};
    const u64 bytes = block_bytes();
    if (!m_waiting.empty() && m_samples != 0) {
        bool barriered = false;
        u64 sent = 0;
        usize kept = 0; // m_waiting[0, kept): still waiting, in order
        usize next = 0;
        for (; next < m_waiting.size(); ++next) {
            if (sent > 0 && sent + bytes > budget) break; // at least one a frame
            Tile& t = m_tiles.at(m_waiting[next]);
            if (t.block == ~0u) {
                // No block to be had: tiles without one wait, the rest still go.
                if (!m_full && m_free.empty() && !grow(cmd, m_blocks + 1)) {
                    m_full = true;
                    if (!m_full_logged) {
                        log_warning("render", "terrain paint: out of room for " + std::to_string(m_tiles.size()) +
                                                  " painted tiles; the rest draw unpainted");
                        m_full_logged = true;
                    }
                }
                if (m_free.empty()) {
                    m_waiting[kept++] = m_waiting[next];
                    continue;
                }
                t.block = m_free.back();
                m_free.pop_back();
            }
            const rhi::TransientMemory up = cmd.allocate(static_cast<u32>(bytes));
            if (up.data == nullptr) break; // out of upload memory: the rest next frame
            std::memcpy(up.data, t.weights.data(), bytes);
            t.weights = {}; // the GPU holds it now
            if (!barriered) {
                cmd.barrier(m_pool, rhi::ResourceState::copy_dest);
                barriered = true;
            }
            cmd.copy_buffer(m_pool, u64{t.block} * bytes, up.buffer, up.offset, bytes);
            sent += bytes;
            m_table_stale = m_table_stale || !t.shown;
            t.shown = true;
            t.waiting = false;
        }
        // Those not reached keep their place after those passed over.
        for (; next < m_waiting.size(); ++next) m_waiting[kept++] = m_waiting[next];
        m_waiting.resize(kept);
    }
    if (m_pool.valid()) cmd.barrier(m_pool, rhi::ResourceState::shader_read);
    if (!table) return false;
    if (m_table_stale) {
        m_table_stale = false;
        usize shown = 0;
        for (const auto& [key, tile] : m_tiles) shown += tile.shown ? 1 : 0;
        const usize entries = std::bit_ceil(std::max<usize>(16, shown * 2)); // at most half full: short probes
        // A header first: the painted tiles' bounds (min x, min z, max x, max z),
        // so pixels away from them skip the lookups.
        m_table.assign((entries + 1) * 4, 0);
        i32 lo_x = std::numeric_limits<i32>::max(), lo_z = lo_x;
        i32 hi_x = std::numeric_limits<i32>::min(), hi_z = hi_x;
        for (usize e = 1; e <= entries; ++e) m_table[e * 4 + 2] = ~0u;
        const u32 mask = static_cast<u32>(entries - 1);
        for (const auto& [key, tile] : m_tiles) {
            if (!tile.shown) continue;
            lo_x = std::min(lo_x, key.x);
            lo_z = std::min(lo_z, key.z);
            hi_x = std::max(hi_x, key.x);
            hi_z = std::max(hi_z, key.z);
            u32 at = terrain_paint_hash(key.x, key.z) & mask;
            while (m_table[(at + 1) * 4 + 2] != ~0u) at = (at + 1) & mask;
            m_table[(at + 1) * 4] = static_cast<u32>(key.x);
            m_table[(at + 1) * 4 + 1] = static_cast<u32>(key.z);
            m_table[(at + 1) * 4 + 2] = static_cast<u32>(u64{tile.block} * bytes);
        }
        m_table[0] = static_cast<u32>(lo_x);
        m_table[1] = static_cast<u32>(lo_z);
        m_table[2] = static_cast<u32>(hi_x);
        m_table[3] = static_cast<u32>(hi_z);
        m_shown = shown;
    }
    if (m_shown == 0 || !m_pool.valid()) return false;
    out.table = cmd.upload({reinterpret_cast<const u8*>(m_table.data()), m_table.size() * 4});
    if (out.table.data == nullptr) return false;
    out.table_mask = static_cast<u32>(m_table.size() / 4 - 2); // entries - 1 (after the header)
    out.pool = m_pool;
    return true;
}

} // namespace giant::render
