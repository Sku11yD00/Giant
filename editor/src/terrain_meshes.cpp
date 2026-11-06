#include "terrain_meshes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::editor {

TileHeights tile_heights(const world::Terrain& terrain, world::CellCoord cell) {
    TileHeights out;
    out.samples = terrain.samples();
    out.spacing = terrain.spacing();
    const i64 n = out.samples;
    const i64 side = n + 3;
    out.heights.resize(static_cast<usize>(side * side));
    const i64 i0 = i64{cell.x} * n;
    const i64 j0 = i64{cell.z} * n;
    const world::Terrain::Heights* own = terrain.find(cell);
    for (i64 j = -1; j <= n + 1; ++j) {
        float* row = out.heights.data() + (j + 1) * side;
        if (own != nullptr && j >= 0 && j < n) {
            // The tile's own samples straight from its row; only the ring
            // around it is looked up.
            std::copy_n(own->data() + j * n, n, row + 1);
            for (const i64 i : {i64{-1}, n, n + 1}) {
                const std::optional<float> h = terrain.sample(i0 + i, j0 + j, &cell);
                row[i + 1] = h ? *h : std::numeric_limits<float>::quiet_NaN();
            }
            continue;
        }
        for (i64 i = -1; i <= n + 1; ++i) {
            const std::optional<float> h = terrain.sample(i0 + i, j0 + j, &cell);
            row[i + 1] = h ? *h : std::numeric_limits<float>::quiet_NaN();
        }
    }
    return out;
}

namespace {

// The rise per metre at a sample from those either side (`before` or
// `after` NaN where there is no ground: one-sided there).
float slope(float before, float at, float after, float spacing) {
    const bool b = !std::isnan(before);
    const bool a = !std::isnan(after);
    if (a && b) return (after - before) / (2.0f * spacing);
    if (a) return (after - at) / spacing;
    if (b) return (at - before) / spacing;
    return 0.0f;
}

} // namespace

TileSurface tile_surface(const TileHeights& in) {
    TileSurface out;
    const u32 n = in.samples;
    const u32 side = n + 3;
    const u32 row = n + 1; // vertices a row
    const float s = static_cast<float>(in.spacing);
    const auto h = [&](u32 i, u32 j) { return in.heights[usize{j} * side + i]; }; // i, j from -1, shifted by one
    out.mesh.vertices.resize(usize{row} * row);
    out.edge.resize(usize{row} * row);
    for (u32 j = 0; j <= n; ++j) {
        for (u32 i = 0; i <= n; ++i) {
            const usize v = usize{j} * row + i;
            const float dx = slope(h(i, j + 1), h(i + 1, j + 1), h(i + 2, j + 1), s);
            const float dz = slope(h(i + 1, j), h(i + 1, j + 1), h(i + 1, j + 2), s);
            out.mesh.vertices[v].position = {static_cast<float>(i) * s, h(i + 1, j + 1), static_cast<float>(j) * s};
            out.mesh.vertices[v].normal = normalize_or_zero(Vec3{-dx, 1.0f, -dz}); // y is 1: never zero
            out.edge[v] = (i == 0 || j == 0 || i == n || j == n) ? 1 : 0;
        }
    }
    out.mesh.indices.reserve(usize{n} * n * 6);
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const u32 v00 = j * row + i;
            const u32 v10 = v00 + 1;
            const u32 v01 = v00 + row;
            const u32 v11 = v01 + 1;
            // Split on the (0, 0)-(1, 1) diagonal, as world::Terrain::height_at;
            // counter-clockwise seen from above.
            out.mesh.indices.insert(out.mesh.indices.end(), {v00, v01, v11, v00, v11, v10});
        }
    }
    return out;
}

namespace {

// Tells apart the heights two builds were made from (FNV-1a over the bytes).
u64 hash_of(const TileHeights& h) {
    u64 x = 1469598103934665603ull ^ h.samples;
    const auto* bytes = reinterpret_cast<const u8*>(h.heights.data());
    for (usize i = 0; i < h.heights.size() * sizeof(float); ++i) {
        x = (x ^ bytes[i]) * 1099511628211ull;
    }
    return x == 0 ? 1 : x; // 0 means "none"
}

} // namespace

TerrainMeshes::TerrainMeshes(render::Renderer* renderer) : m_renderer(renderer) {}

TerrainMeshes::~TerrainMeshes() {
    m_jobs.wait(m_builds);
    if (m_renderer != nullptr) {
        for (const auto& [cell, tile] : m_tiles) {
            if (tile.mesh != render::k_no_mesh) m_renderer->remove_mesh(tile.mesh);
            if (tile.pending != render::k_no_mesh) m_renderer->remove_mesh(tile.pending);
        }
    }
}

render::MeshId TerrainMeshes::mesh(world::CellCoord cell) const {
    const auto it = m_tiles.find(cell);
    return it == m_tiles.end() ? render::k_no_mesh : it->second.mesh;
}

std::vector<world::CellCoord> TerrainMeshes::take_changed() {
    std::vector<world::CellCoord> out = std::move(m_changed);
    m_changed.clear();
    return out;
}

void TerrainMeshes::want(world::CellCoord cell) {
    if (m_queued.insert(cell).second) {
        m_queue.push_back(cell);
    }
}

void TerrainMeshes::drop(world::CellCoord cell, Tile& tile) {
    if (m_renderer != nullptr) {
        if (tile.mesh != render::k_no_mesh) m_renderer->remove_mesh(tile.mesh);
        if (tile.pending != render::k_no_mesh) m_renderer->remove_mesh(tile.pending);
    }
    if (tile.drawn) {
        m_changed.push_back(cell);
    }
    m_tiles.erase(cell);
}

void TerrainMeshes::start(world::CellCoord cell, Tile& tile, TileHeights heights, u64 hash) {
    tile.wanted = ++m_serial;
    tile.in_flight = true;
    ++m_in_flight;
    m_jobs.submit(m_builds, [this, cell, serial = tile.wanted, hash, heights = std::move(heights)] {
        TileSurface surface = tile_surface(heights);
        Result r;
        r.cell = cell;
        r.serial = serial;
        r.hash = hash;
        r.triangles = static_cast<u32>(surface.mesh.indices.size() / 3);
        r.mesh = geometry::build_clusters(surface.mesh, surface.edge);
        const std::lock_guard lock(m_mutex);
        m_done.push_back(std::move(r));
    });
}

void TerrainMeshes::commit(world::CellCoord cell, Tile& tile) {
    if (m_renderer != nullptr && tile.mesh != render::k_no_mesh) m_renderer->remove_mesh(tile.mesh);
    tile.mesh = tile.pending;
    tile.pending = render::k_no_mesh;
    tile.triangles = tile.pending_triangles;
    tile.hash = tile.pending_hash;
    tile.drawn = true;
    tile.waiting = false;
    m_changed.push_back(cell);
}

void TerrainMeshes::update(world::Terrain& terrain) {
    // What changed: a changed tile's mesh and its neighbours' (they reach
    // into it; one whose heights snapshot comes out the same is not built).
    world::TerrainChanges changes = terrain.take_changes();
    if (terrain.identity() != m_identity) {
        m_identity = terrain.identity();
        changes.all = true;
        for (auto& [cell, tile] : m_tiles) tile.wanted = 0; // builds from the old terrain are not wanted
    }
    if (changes.all) {
        for (const auto& [cell, heights] : terrain.tiles()) want(cell);
        for (const auto& [cell, tile] : m_tiles) want(cell);
    } else {
        for (const world::CellCoord c : changes.tiles) {
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i64 x = i64{c.x} + dx;
                    const i64 z = i64{c.z} + dz;
                    if (x < std::numeric_limits<i32>::min() || x > std::numeric_limits<i32>::max() ||
                        z < std::numeric_limits<i32>::min() || z > std::numeric_limits<i32>::max()) {
                        continue;
                    }
                    const world::CellCoord n{static_cast<i32>(x), static_cast<i32>(z)};
                    if (terrain.find(n) != nullptr || m_tiles.contains(n)) want(n);
                }
            }
        }
    }

    // Finished builds: uploaded (a few a frame), then drawn once loaded.
    {
        std::vector<Result> done;
        {
            const std::lock_guard lock(m_mutex);
            done.swap(m_done);
        }
        for (Result& r : done) m_arrived.push_back(std::move(r));
    }
    u32 taken = 0;
    while (!m_arrived.empty() && taken < uploads_per_frame) {
        Result r = std::move(m_arrived.front());
        m_arrived.pop_front();
        --m_in_flight;
        const auto t = m_tiles.find(r.cell);
        if (t == m_tiles.end()) continue; // never: a building tile is not dropped
        Tile& tile = t->second;
        tile.in_flight = false;
        if (r.serial != tile.wanted) continue; // made from heights since replaced
        tile.wanted = 0;
        ++taken;
        ++m_built;
        render::MeshId id = render::k_no_mesh;
        if (m_renderer != nullptr) {
            id = m_renderer->add_mesh(std::move(r.mesh));
            if (id == render::k_no_mesh) {
                tile.hash = 0; // not drawn as made: build it again when it next changes
                continue;
            }
        }
        if (tile.pending != render::k_no_mesh && m_renderer != nullptr) m_renderer->remove_mesh(tile.pending);
        tile.pending = id;
        tile.pending_triangles = r.triangles;
        tile.pending_hash = r.hash;
        if (!tile.waiting) {
            tile.waiting = true;
            m_waiting.push_back(r.cell);
        }
    }
    // Uploaded meshes replace the old ones once they can draw, so a tile
    // never blinks out while its new mesh streams in.
    for (usize k = 0; k < m_waiting.size();) {
        const auto t = m_tiles.find(m_waiting[k]);
        const bool gone = t == m_tiles.end() || !t->second.waiting;
        if (!gone && (m_renderer == nullptr || m_renderer->mesh_ready(t->second.pending))) {
            commit(t->first, t->second);
        } else if (!gone) {
            ++k;
            continue;
        }
        m_waiting[k] = m_waiting.back();
        m_waiting.pop_back();
    }

    // Start builds, oldest wish first; a tile already building waits its
    // turn. Each look takes a heights snapshot (cheap); one that matches
    // what is drawn needs no build.
    u32 started = 0;
    u32 looks = 0;
    const u32 max_looks = builds_per_frame > std::numeric_limits<u32>::max() / 8 ? builds_per_frame
                                                                                 : builds_per_frame * 8;
    for (usize looked = 0, n = m_queue.size(); looked < n; ++looked) {
        if (started >= builds_per_frame || looks >= max_looks || m_in_flight >= builds_in_flight) break;
        const world::CellCoord cell = m_queue.front();
        m_queue.pop_front();
        const auto t = m_tiles.find(cell);
        if (terrain.find(cell) == nullptr) {
            if (t != m_tiles.end() && t->second.in_flight) {
                m_queue.push_back(cell); // dropped once its build is back
                continue;
            }
            m_queued.erase(cell);
            if (t != m_tiles.end()) drop(cell, t->second);
            continue;
        }
        Tile& tile = t != m_tiles.end() ? t->second : m_tiles[cell];
        if (tile.in_flight) {
            m_queue.push_back(cell); // still wanted: built again after this one
            continue;
        }
        m_queued.erase(cell);
        ++looks;
        TileHeights heights = tile_heights(terrain, cell);
        const u64 hash = hash_of(heights);
        if (hash == (tile.waiting ? tile.pending_hash : tile.hash)) continue; // as drawn (or about to be)
        start(cell, tile, std::move(heights), hash);
        ++started;
    }
}

void TerrainMeshes::finish(world::Terrain& terrain) {
    const u32 budgets[3] = {builds_per_frame, uploads_per_frame, builds_in_flight};
    builds_per_frame = uploads_per_frame = builds_in_flight = std::numeric_limits<u32>::max();
    for (;;) {
        update(terrain);
        // Not drawn yet: with no frames to stream them in, swapped in now.
        for (const world::CellCoord c : m_waiting) {
            const auto t = m_tiles.find(c);
            if (t != m_tiles.end() && t->second.waiting) commit(c, t->second);
        }
        m_waiting.clear();
        if (m_queue.empty() && m_in_flight == 0 && m_arrived.empty()) break;
        m_jobs.wait(m_builds);
    }
    builds_per_frame = budgets[0];
    uploads_per_frame = budgets[1];
    builds_in_flight = budgets[2];
}

TerrainMeshStats TerrainMeshes::stats() const {
    TerrainMeshStats s;
    for (const auto& [cell, tile] : m_tiles) {
        s.tiles += tile.drawn ? 1u : 0u;
        s.building += (tile.in_flight || tile.waiting) && !m_queued.contains(cell) ? 1u : 0u;
        s.triangles += tile.drawn ? tile.triangles : 0u;
    }
    s.building += static_cast<u32>(m_queue.size());
    s.built = m_built;
    s.in_flight = m_in_flight;
    return s;
}

} // namespace giant::editor
