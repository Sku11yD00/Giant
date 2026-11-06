#include "far_ground.h"

#include "giant/core/log.h"
#include "giant/core/text.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace giant::editor {

namespace {

using world::CellCoord;

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) return std::nullopt;
    return text.str();
}

// How far `p` is from a chunk's square, on the ground.
double chunk_distance(const DVec3& p, CellCoord c, double size) {
    const double x0 = c.x * size;
    const double z0 = c.z * size;
    const double dx = std::max({x0 - p.x, 0.0, p.x - (x0 + size)});
    const double dz = std::max({z0 - p.z, 0.0, p.z - (z0 + size)});
    return std::sqrt(dx * dx + dz * dz);
}

// The cell at a chunk's corner (where its piece is drawn from).
CellCoord corner(CellCoord chunk, u32 level) {
    i64 scale = 1;
    for (u32 l = 0; l < level; ++l) scale *= world::k_far_branch;
    const auto axis = [&](i32 v) {
        return static_cast<i32>(std::clamp<i64>(i64{v} * scale, std::numeric_limits<i32>::min(),
                                                std::numeric_limits<i32>::max()));
    };
    return {axis(chunk.x), axis(chunk.z)};
}

// The chunk (of `level`) a cell is the corner of, if it is one.
std::optional<CellCoord> chunk_at_corner(CellCoord cell, u32 level) {
    const CellCoord chunk = world::far_chunk_of_cell(cell, level);
    if (!(corner(chunk, level) == cell)) return std::nullopt;
    return chunk;
}

constexpr double k_min_reach = 5800.0; // a level-2 chunk's diagonal (5793 m)
constexpr float k_skirts[2] = {64.0f, 256.0f}; // edge walls, by level: beyond the gaps to the next finer level

} // namespace

FarGround::FarGround(render::Renderer* renderer) {
    for (u32 l = 0; l < world::k_far_levels; ++l) {
        Level& level = m_levels[l];
        level.terrain = world::Terrain(world::far_tile_size(l + 1), true);
        level.terrain.set_samples(world::k_far_samples);
        level.meshes = std::make_unique<TerrainMeshes>(renderer);
        level.meshes->skirt_depth = k_skirts[l];
    }
}

FarGround::~FarGround() { m_jobs.wait(m_reads); }

void FarGround::clear(const TerrainMeshes& ground) {
    for (auto& [chunk, mode] : m_mode) {
        if (mode == Mode::mesh) m_changed.push_back(corner(chunk, 1));
        mark_tiles(chunk, ground); // only tiles with a mesh are drawn again
    }
    for (const CellCoord chunk : m_draw2) m_changed.push_back(corner(chunk, 2));
    m_mode.clear();
    m_draw2.clear();
    m_covered.clear();
    for (u32 l = 0; l < world::k_far_levels; ++l) {
        Level& level = m_levels[l];
        level.terrain.clear(); // the meshes go over the next frames
        level.reading.clear(); // reads in flight are dropped when they come
        level.wanted.clear();
        level.wanted_stale = true;
        m_failed[l].clear();
        m_dirty[l].clear();
    }
    m_arrived.clear();
    m_open = false;
}

void FarGround::take_reads(const WorldStreaming& streaming) {
    {
        std::vector<Read> done;
        {
            const std::lock_guard lock(m_mutex);
            done.swap(m_finished);
        }
        for (Read& r : done) {
            Level& level = m_levels[r.level - 1];
            const auto it = level.reading.find(r.chunk);
            if (it == level.reading.end() || it->second != r.serial) continue; // not wanted any more
            if (!r.heights) {
                level.reading.erase(it);
                m_failed[r.level - 1].insert(r.chunk);
                m_dirty[r.level - 1].insert(r.chunk); // it stands in for nothing: what draws changes
                continue;
            }
            m_arrived.push_back(std::move(r)); // still "reading" (not read again) until added
        }
    }
    // A few a frame.
    std::array<u32, 2> added{};
    for (usize k = 0; k < m_arrived.size();) {
        Read& r = m_arrived[k];
        const u32 l = r.level - 1;
        if (added[l] >= std::max<u32>(1, settings.chunks_per_frame)) {
            ++k;
            continue;
        }
        Level& level = m_levels[l];
        const auto it = level.reading.find(r.chunk);
        if (it != level.reading.end() && it->second == r.serial) {
            level.reading.erase(it);
            if (streaming.far_chunks(r.level).contains(r.chunk)) {
                level.terrain.set(r.chunk, std::move(*r.heights));
                m_dirty[l].insert(r.chunk);
                ++added[l];
            }
        }
        m_arrived.erase(m_arrived.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

void FarGround::unload(const DVec3& camera, const WorldStreaming& streaming) {
    for (u32 l = 0; l < world::k_far_levels; ++l) {
        Level& level = m_levels[l];
        const double size = world::far_tile_size(l + 1);
        const double limit = radius(l + 1, streaming) + settings.margin[l];
        std::vector<CellCoord> far;
        for (const auto& [chunk, heights] : level.terrain.tiles()) {
            if (chunk_distance(camera, chunk, size) > limit) far.push_back(chunk);
            if (far.size() >= std::max<u32>(1, settings.unloads_per_frame)) break;
        }
        for (const CellCoord chunk : far) {
            level.terrain.remove(chunk);
            m_dirty[l].insert(chunk); // past the radius: wanted again only once the camera moves
        }
    }
}

double FarGround::radius(u32 level, const WorldStreaming& streaming) const {
    // Level 1 must reach a level-2 chunk's diagonal past the loaded cells,
    // or a level-2 chunk would draw over the full tiles by the camera.
    const double one = std::max(settings.radius[0], std::min(streaming.settings.load_radius, k_max_load_radius) + k_min_reach);
    return level <= 1 ? one : std::max(settings.radius[1], one);
}

void FarGround::start_reads(const DVec3& camera, const WorldStreaming& streaming) {
    for (u32 l = 0; l < world::k_far_levels; ++l) {
        Level& level = m_levels[l];
        const auto& files = streaming.far_chunks(l + 1);
        const double size = world::far_tile_size(l + 1);
        const double r = radius(l + 1, streaming);
        const CellCoord at = level.terrain.tile_of(camera.x, camera.z);
        // Made again in another chunk, or half a margin from where it was made
        // (chunks let go there may be in reach again), or for a new radius.
        const double moved = std::hypot(camera.x - level.wanted_from.x, camera.z - level.wanted_from.z);
        if (level.wanted_stale || !(at == level.wanted_at) || r != level.wanted_radius ||
            moved > settings.margin[l] * 0.5) {
            level.wanted_stale = false;
            level.wanted_at = at;
            level.wanted_from = camera;
            level.wanted_radius = r;
            std::vector<std::pair<double, CellCoord>> near;
            const auto consider = [&](CellCoord c) {
                if (level.terrain.find(c) != nullptr || level.reading.contains(c) || m_failed[l].contains(c)) return;
                const double d = chunk_distance(camera, c, size);
                if (d <= r) near.push_back({d, c});
            };
            // The square of chunks around the camera, or every file if there are fewer.
            const i64 reach = static_cast<i64>(std::ceil(r / size)) + 1;
            if (files.size() < static_cast<usize>((2 * reach + 1) * (2 * reach + 1))) {
                for (const CellCoord c : files) consider(c);
            } else {
                for (i64 z = i64{at.z} - reach; z <= i64{at.z} + reach; ++z) {
                    for (i64 x = i64{at.x} - reach; x <= i64{at.x} + reach; ++x) {
                        if (x < std::numeric_limits<i32>::min() || x > std::numeric_limits<i32>::max() ||
                            z < std::numeric_limits<i32>::min() || z > std::numeric_limits<i32>::max()) {
                            continue;
                        }
                        const CellCoord c{static_cast<i32>(x), static_cast<i32>(z)};
                        if (files.contains(c)) consider(c);
                    }
                }
            }
            // Farthest first: the nearest is at the back, read first.
            std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first
                                          : (a.second.z != b.second.z ? a.second.z > b.second.z : a.second.x > b.second.x);
            });
            level.wanted.clear();
            for (const auto& [d, c] : near) level.wanted.push_back(c);
        }
        // Each level its own reads, so the far one is not starved by the near.
        u32 reading = static_cast<u32>(level.reading.size());
        while (reading < std::max<u32>(1, settings.reads_in_flight) && !level.wanted.empty()) {
            const CellCoord chunk = level.wanted.back();
            level.wanted.pop_back();
            if (!files.contains(chunk) || level.terrain.find(chunk) != nullptr || level.reading.contains(chunk)) continue;
            const u64 serial = ++m_serial;
            level.reading[chunk] = serial;
            ++reading;
            m_jobs.submit(m_reads, [this, l, chunk, serial, path = streaming.far_chunk_path(l + 1, chunk)] {
                Read read;
                read.level = l + 1;
                read.chunk = chunk;
                read.serial = serial;
                world::Terrain::Heights heights;
                if (const auto text = read_text(path)) {
                    if (const auto error = world::load_far_chunk(*text, l + 1, chunk, heights)) {
                        log_warning("world", path_to_utf8(path) + ": " + error->message);
                    } else {
                        read.heights = std::move(heights);
                    }
                } else {
                    log_warning("world", "could not open " + path_to_utf8(path));
                }
                const std::lock_guard lock(m_mutex);
                m_finished.push_back(std::move(read));
            });
        }
    }
}

void FarGround::wait_reads() { m_jobs.wait(m_reads); }

void FarGround::saved(const WorldStreaming::Saved& saved) {
    if (!m_open) return;
    for (const WorldStreaming::Saved::Chunk& c : saved.chunks) {
        Level& level = m_levels[c.level - 1];
        level.wanted_stale = true;
        m_failed[c.level - 1].erase(c.chunk);
        level.reading.erase(c.chunk); // its read is of the old file: dropped
        std::erase_if(m_arrived, [&](const Read& r) { return r.level == c.level && r.chunk == c.chunk; });
        if (c.heights && level.terrain.find(c.chunk) != nullptr) {
            level.terrain.set(c.chunk, *c.heights);
        } else if (!c.heights) {
            level.terrain.remove(c.chunk);
        }
        m_dirty[c.level - 1].insert(c.chunk);
    }
}

bool FarGround::covered(CellCoord chunk, const TerrainMeshes& ground, const world::Terrain& terrain,
                        const WorldStreaming& streaming) const {
    const world::Terrain::Heights* heights = m_levels[0].terrain.find(chunk);
    if (heights == nullptr) return false;
    constexpr u32 per = world::k_far_samples / world::k_far_branch;
    bool any = false; // ground in it, and a tile of it drawn
    bool none = true; // no ground in it at all
    for (i32 b = 0; b < world::k_far_branch; ++b) {
        for (i32 a = 0; a < world::k_far_branch; ++a) {
            // A cell with a tile has every one of its samples; one without, none.
            const float h = (*heights)[usize{static_cast<u32>(b) * per} * world::k_far_samples + static_cast<u32>(a) * per];
            if (std::isnan(h)) continue;
            none = false;
            const CellCoord cell{chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b};
            if (ground.drawn(cell)) {
                any = true;
                continue;
            }
            // Waiting for its mesh, or for streaming to bring its tile; else (taken
            // away and not saved yet, or its file failed) nothing to wait for.
            if (terrain.find(cell) != nullptr || streaming.ground_coming(cell)) return false;
        }
    }
    // Every one of its tiles gone or failed: its mesh, not a hole.
    return none || any;
}

void FarGround::mark_tiles(CellCoord chunk, const TerrainMeshes& ground) {
    for (i32 b = 0; b < world::k_far_branch; ++b) {
        for (i32 a = 0; a < world::k_far_branch; ++a) {
            const CellCoord cell{chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b};
            if (ground.drawn(cell)) m_changed.push_back(cell);
        }
    }
}

void FarGround::select(TerrainMeshes& ground, const world::Terrain& terrain, const WorldStreaming& streaming) {
    TerrainMeshes& meshes1 = *m_levels[0].meshes;
    TerrainMeshes& meshes2 = *m_levels[1].meshes;
    // What changed: full tiles drawn (their chunk may now be covered, or no
    // longer), and the chunks' meshes (a mesh replaced: its piece is new).
    for (const CellCoord c : ground.take_changed()) {
        m_changed.push_back(c);
        m_dirty[0].insert(world::far_chunk_of_cell(c, 1));
    }
    for (const CellCoord c : meshes1.take_changed()) {
        m_dirty[0].insert(c);
        m_changed.push_back(corner(c, 1));
    }
    for (const CellCoord c : meshes2.take_changed()) {
        m_dirty[1].insert(c);
        m_changed.push_back(corner(c, 2));
    }
    // A cell file failing is a change no mesh reports: every chunk is looked at again (rare).
    const u32 failed = streaming.stats().failed;
    if (failed != m_cells_failed) {
        m_cells_failed = failed;
        for (const auto& [chunk, heights] : m_levels[0].terrain.tiles()) m_dirty[0].insert(chunk);
    }
    if (m_dirty[0].empty() && m_dirty[1].empty()) return;
    const auto& files1 = streaming.far_chunks(1);
    for (const CellCoord c : m_dirty[0]) {
        if (covered(c, ground, terrain, streaming)) {
            m_covered.insert(c);
        } else {
            m_covered.erase(c);
        }
        m_dirty[1].insert(world::far_parent(c));
    }
    // A level-1 chunk can stand in for its part of its parent.
    // (One whose file could not be read stands in too: it has nothing to show.)
    const auto usable = [&](CellCoord c) {
        return !files1.contains(c) || m_covered.contains(c) || meshes1.drawn(c) || m_failed[0].contains(c);
    };
    for (const CellCoord p : m_dirty[1]) {
        bool all = true;
        for (i32 b = 0; all && b < world::k_far_branch; ++b) {
            for (i32 a = 0; all && a < world::k_far_branch; ++a) {
                all = usable({p.x * world::k_far_branch + a, p.z * world::k_far_branch + b});
            }
        }
        const bool draw = !all && meshes2.drawn(p);
        if (draw == m_draw2.contains(p)) continue;
        if (draw) {
            m_draw2.insert(p);
        } else {
            m_draw2.erase(p);
        }
        m_changed.push_back(corner(p, 2));
        for (i32 b = 0; b < world::k_far_branch; ++b) {
            for (i32 a = 0; a < world::k_far_branch; ++a) {
                m_dirty[0].insert({p.x * world::k_far_branch + a, p.z * world::k_far_branch + b});
            }
        }
    }
    for (const CellCoord c : m_dirty[0]) {
        Mode mode = Mode::tiles;
        if (m_draw2.contains(world::far_parent(c))) {
            mode = Mode::hidden;
        } else if (!m_covered.contains(c) && meshes1.drawn(c)) {
            mode = Mode::mesh;
        }
        const auto it = m_mode.find(c);
        const Mode was = it == m_mode.end() ? Mode::tiles : it->second;
        if (mode == was) continue;
        if ((was == Mode::tiles) != (mode == Mode::tiles)) mark_tiles(c, ground);
        if (was == Mode::mesh || mode == Mode::mesh) m_changed.push_back(corner(c, 1));
        if (mode == Mode::tiles) {
            m_mode.erase(c);
        } else {
            m_mode[c] = mode;
        }
    }
    m_dirty[0].clear();
    m_dirty[1].clear();
}

void FarGround::update(const DVec3& camera, const WorldStreaming& streaming, u64 generation, TerrainMeshes& ground,
                       const world::Terrain& terrain) {
    if (!streaming.active(generation)) {
        if (m_open) clear(ground);
    } else if (!m_open || streaming.session() != m_session) {
        clear(ground);
        m_open = true;
        m_session = streaming.session();
    }
    if (m_open) {
        take_reads(streaming);
        unload(camera, streaming);
        start_reads(camera, streaming);
    }
    for (Level& level : m_levels) level.meshes->update(level.terrain);
    if (m_open) {
        for (u32 l = 0; l < world::k_far_levels; ++l) m_files[l] = static_cast<u32>(streaming.far_chunks(l + 1).size());
        select(ground, terrain, streaming);
    } else {
        m_files = {};
        for (const CellCoord c : ground.take_changed()) m_changed.push_back(c);
    }
}

void FarGround::finish(const DVec3& camera, const WorldStreaming& streaming, u64 generation, TerrainMeshes& ground,
                       const world::Terrain& terrain) {
    const FarGroundSettings kept = settings;
    settings.chunks_per_frame = std::numeric_limits<u32>::max();
    settings.unloads_per_frame = std::numeric_limits<u32>::max();
    for (;;) {
        update(camera, streaming, generation, ground, terrain);
        for (Level& level : m_levels) level.meshes->finish(level.terrain);
        bool busy = !m_arrived.empty();
        for (const Level& level : m_levels) busy = busy || !level.reading.empty() || !level.wanted.empty();
        if (!busy) break;
        m_jobs.wait(m_reads);
    }
    update(camera, streaming, generation, ground, terrain); // what draws, with every mesh in
    settings = kept;
}

bool FarGround::draws_tile(CellCoord cell) const {
    if (m_mode.empty()) return true;
    return !m_mode.contains(world::far_chunk_of_cell(cell, 1));
}

void FarGround::pieces(CellCoord cell, std::vector<Piece>& out) const {
    if (const auto c = chunk_at_corner(cell, 1)) {
        const auto it = m_mode.find(*c);
        if (it != m_mode.end() && it->second == Mode::mesh) out.push_back({1, *c, m_levels[0].meshes->mesh(*c)});
    }
    if (const auto c = chunk_at_corner(cell, 2); c && m_draw2.contains(*c)) {
        out.push_back({2, *c, m_levels[1].meshes->mesh(*c)});
    }
}

std::vector<CellCoord> FarGround::piece_cells() const {
    std::vector<CellCoord> out;
    for (const auto& [chunk, mode] : m_mode) {
        if (mode == Mode::mesh) out.push_back(corner(chunk, 1));
    }
    for (const CellCoord chunk : m_draw2) out.push_back(corner(chunk, 2));
    return out;
}

std::vector<CellCoord> FarGround::take_changed() {
    std::vector<CellCoord> out = std::move(m_changed);
    m_changed.clear();
    return out;
}

FarGroundStats FarGround::stats() const {
    FarGroundStats s;
    s.files = m_files;
    for (u32 l = 0; l < world::k_far_levels; ++l) {
        const Level& level = m_levels[l];
        s.loaded[l] = static_cast<u32>(level.terrain.tiles().size());
        const TerrainMeshStats m = level.meshes->stats();
        s.meshes[l] = m.tiles;
        s.building[l] = m.building;
        s.reading += static_cast<u32>(level.reading.size());
        s.failed += static_cast<u32>(m_failed[l].size());
    }
    for (const auto& [chunk, mode] : m_mode) {
        s.drawn[0] += mode == Mode::mesh ? 1u : 0u;
        s.hidden += mode == Mode::hidden ? 1u : 0u;
    }
    s.drawn[1] = static_cast<u32>(m_draw2.size());
    return s;
}

} // namespace giant::editor
