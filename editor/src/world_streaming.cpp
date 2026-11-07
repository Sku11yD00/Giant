#include "world_streaming.h"

#include "mesh_library.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/core/text.h"
#include "giant/world/far_ground.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace giant::editor {

namespace {

using world::CellCoord;
using world::Entity;


std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return text.str();
}

// Written beside, then renamed over: a crash mid-save never leaves half a file.
bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path partial = path;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    return !error;
}

// How far `p` is from the cell's square, on the ground.
double square_distance(const DVec3& p, CellCoord c, double size) {
    const double x0 = c.x * size;
    const double z0 = c.z * size;
    const double dx = std::max({x0 - p.x, 0.0, p.x - (x0 + size)});
    const double dz = std::max({z0 - p.z, 0.0, p.z - (z0 + size)});
    return std::sqrt(dx * dx + dz * dz);
}
double ground_distance(const DVec3& p, CellCoord c) { return square_distance(p, c, world::k_cell_size); }

// How far a cell counts as from `p` for loading: a cell with a terrain tile
// goes by its level-1 far ground chunk, so whole chunks load and unload
// together (a chunk draws its full tiles only once it has them all).
double load_distance(const DVec3& p, CellCoord c, bool ground) {
    return ground ? square_distance(p, world::far_chunk_of_cell(c, 1), world::far_tile_size(1)) : ground_distance(p, c);
}

// The entities of `cell` as the scene holds them now (the frame's list,
// less those since moved out or deleted).
std::vector<const Entity*> cell_entities(const world::Scene& scene, const SceneFrame& frame, CellCoord cell) {
    std::vector<const Entity*> out;
    const auto it = frame.grid.find(cell);
    if (it == frame.grid.end()) {
        return out;
    }
    for (const world::EntityId id : it->second.ids) {
        const Entity* e = scene.find(id);
        if (e != nullptr && world::cell_of(e->transform.position) == cell) {
            out.push_back(e);
        }
    }
    return out;
}

bool by_id(const Entity& a, const Entity& b) { return a.id < b.id; }

// Whether the scene has any entity in `cell` now.
bool has_entities(const world::Scene& scene, const SceneFrame::Cell& built, CellCoord cell) {
    for (const world::EntityId id : built.ids) {
        const Entity* e = scene.find(id);
        if (e != nullptr && world::cell_of(e->transform.position) == cell) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WorldStreaming::matches(std::vector<const Entity*>& now, const std::vector<Entity>& saved) {
    if (now.size() != saved.size()) {
        return false;
    }
    std::sort(now.begin(), now.end(), [](const Entity* a, const Entity* b) { return a->id < b->id; });
    for (usize i = 0; i < now.size(); ++i) {
        if (!(*now[i] == saved[i])) {
            return false;
        }
    }
    return true;
}

bool WorldStreaming::same_damage(const world::Scene& scene, CellCoord coord, const Cell& cell) {
    const world::CellDamage* now = scene.damage_in(coord);
    return now == nullptr ? cell.saved_damage.empty() : *now == cell.saved_damage;
}

bool WorldStreaming::same_tile(const world::Terrain& terrain, CellCoord coord, const Cell& cell) {
    const u64 version = terrain.version(coord);
    if (!cell.saved_tile) {
        return version == 0;
    }
    return version != 0 && version == cell.saved_version;
}

namespace {
// What a tile weighs against entities_per_frame: as many entities as its
// heights over 64 (64 at 64 samples, 256 at 128).
usize tile_weight(u32 samples) { return std::max<usize>(1, usize{samples} * samples / 64); }
} // namespace

std::optional<u32> WorldStreaming::ground_samples() const {
    if (m_ground.empty()) {
        return std::nullopt;
    }
    return m_index.terrain_samples;
}

bool WorldStreaming::ground_on_disk(CellCoord cell) const {
    if (!m_ground.contains(cell)) {
        return false;
    }
    const auto it = m_cells.find(cell);
    return it != m_cells.end() && it->second.state != State::loaded;
}

WorldStreaming::WorldStreaming() = default;

WorldStreaming::~WorldStreaming() { m_jobs.wait(m_reads); }

std::filesystem::path WorldStreaming::cell_path(CellCoord cell) const {
    return m_folder / "cells" / world::cell_file_name(cell);
}

std::filesystem::path WorldStreaming::far_chunk_path(u32 level, CellCoord chunk) const {
    return m_folder / "ground" / std::to_string(level) / world::far_chunk_file_name(chunk);
}

bool WorldStreaming::ground_coming(CellCoord cell) const {
    if (!m_ground.contains(cell)) return false;
    const auto it = m_cells.find(cell);
    return it != m_cells.end() && it->second.state != State::loaded && it->second.state != State::failed;
}

const std::unordered_set<CellCoord, world::CellCoordHash>& WorldStreaming::far_chunks(u32 level) const {
    GIANT_ASSERT(level >= 1 && level <= world::k_far_levels, "far ground levels are 1 and 2");
    return m_far[level - 1];
}

namespace {

// The far ground chunk files in a level's folder ("<x>_<z>.ground.json").
std::unordered_set<CellCoord, world::CellCoordHash> list_far_chunks(const std::filesystem::path& folder) {
    std::unordered_set<CellCoord, world::CellCoordHash> out;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const std::string name = path_to_utf8(it->path().filename());
        constexpr std::string_view suffix = ".ground.json";
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - suffix.size());
        const usize bar = stem.find('_', 1);
        if (bar == std::string::npos) continue;
        i32 x = 0;
        i32 z = 0;
        const char* a = stem.data();
        const auto [xe, xc] = std::from_chars(a, a + bar, x);
        const auto [ze, zc] = std::from_chars(a + bar + 1, a + stem.size(), z);
        if (xc != std::errc{} || zc != std::errc{} || xe != a + bar || ze != a + stem.size()) continue;
        if (world::far_chunk_file_name({x, z}) == name) out.insert({x, z});
    }
    return out;
}

} // namespace

std::optional<std::string> WorldStreaming::open(const std::filesystem::path& folder, world::Scene& scene,
                                                u64 generation) {
    const std::filesystem::path index_path = folder / "world.json";
    const auto text = read_text(index_path);
    if (!text) {
        return "could not open " + path_to_utf8(index_path);
    }
    world::WorldIndex index;
    if (const auto error = world::load_world_index(*text, index)) {
        return path_to_utf8(index_path) + ": " + error->message;
    }
    for (std::string& m : index.meshes) {
        m = MeshLibrary::normalize(m);
    }
    for (world::Kit& kit : index.kits) {
        for (world::KitPiece& p : kit.pieces) {
            if (!p.mesh.empty()) p.mesh = MeshLibrary::normalize(p.mesh);
        }
    }
    for (world::ScatterLayer& layer : index.scatter) {
        for (world::ScatterItem& i : layer.items) {
            if (!i.mesh.empty()) i.mesh = MeshLibrary::normalize(i.mesh);
        }
    }
    close();
    world::Scene base;
    base.lighting = index.lighting;
    base.kits = index.kits;
    base.scatter = index.scatter;
    base.weather = index.weather;
    base.reserve_ids_below(index.next_id);
    base.terrain.set_samples(index.terrain_samples);
    base.terrain.material = index.terrain_material;
    scene = std::move(base);
    m_scene = scene.identity();
    m_folder = folder;
    m_generation = generation;
    m_index = std::move(index);
    for (const CellCoord c : m_index.cells) {
        m_cells[c] = Cell{};
    }
    m_ground.insert(m_index.terrain_cells.begin(), m_index.terrain_cells.end());
    for (u32 level = 1; level <= world::k_far_levels; ++level) {
        m_far[level - 1] = list_far_chunks(folder / "ground" / std::to_string(level));
    }
    m_far_owed.insert(m_index.far_owed.begin(), m_index.far_owed.end());
    m_far_owed2.insert(m_index.far_owed_2.begin(), m_index.far_owed_2.end());
    m_count[static_cast<usize>(State::on_disk)] = static_cast<u32>(m_cells.size());
    m_wanted_stale = true;
    return std::nullopt;
}

void WorldStreaming::close() {
    ++m_session;
    ++m_epoch; // reads still in flight are dropped when they finish
    m_count = {};
    m_loaded.clear();
    m_wanted.clear();
    m_touched.clear();
    m_folder.clear();
    m_generation = 0;
    m_index = {};
    m_cells.clear();
    m_ground.clear();
    for (auto& level : m_far) level.clear();
    m_far_owed.clear();
    m_far_owed2.clear();
    m_ready.clear();
    m_origin.clear();
    m_ids_seen = 1;
    m_scene = 0;
    m_totals = {};
    m_frames = 0;
    const std::lock_guard lock(m_mutex);
    m_finished.clear();
}

void WorldStreaming::set_state(CellCoord coord, Cell& cell, State state) {
    --m_count[static_cast<usize>(cell.state)];
    ++m_count[static_cast<usize>(state)];
    if (cell.state == State::loaded) m_loaded.erase(coord);
    if (state == State::loaded) m_loaded.insert(coord);
    if (state == State::on_disk) m_wanted_stale = true;
    cell.state = state;
}

void WorldStreaming::forget(CellCoord coord) {
    const auto it = m_cells.find(coord);
    if (it != m_cells.end()) {
        --m_count[static_cast<usize>(it->second.state)];
        m_loaded.erase(coord);
        m_cells.erase(it);
    }
    m_ground.erase(coord);
    std::erase_if(m_ready, [&](const Read& r) { return r.cell == coord; });
}

bool WorldStreaming::follows(const world::Scene& scene) {
    if (scene.identity() == m_scene) {
        return true;
    }
    // Replaced, not by an undo or redo that put back its bookkeeping with
    // it (a script, say): what it holds of each cell is unknown, so
    // streaming stops rather than guess (open_world again to go on).
    log_warning("world", "the world's scene was replaced: streaming stopped (open the world again)");
    m_generation = 0;
    return false;
}

std::shared_ptr<const WorldStreaming::Memo> WorldStreaming::memo(const world::Scene& scene) const {
    m_ids_seen = std::max(m_ids_seen, scene.next_id());
    auto out = std::make_shared<Memo>();
    out->session = m_session;
    out->cells = m_cells;
    return out;
}

void WorldStreaming::restore(const Memo& memo, world::Scene& scene) {
    if (!is_open() || memo.session != m_session) {
        return; // another world (or none) since: that scene is no longer streamed
    }
    ++m_epoch; // reads in flight were for the bookkeeping replaced here
    m_ready.clear();
    m_touched.clear();
    // Which cells have a file, and what they hold, is as it is now (origins
    // too); the memo says only which of them that scene holds in full.
    // A cell whose file was written since (by a save the memo does not
    // know of) is read again and merged, as a cell on disk the scene has
    // entities in is, so nothing in the scene or the file is lost.
    m_count = {};
    m_loaded.clear();
    for (auto& [coord, cell] : m_cells) {
        const auto then = memo.cells.find(coord);
        if (then != memo.cells.end() && then->second.state == State::loaded &&
            then->second.written == cell.written) {
            cell.state = State::loaded;
            cell.saved = then->second.saved;
            cell.saved_tile = then->second.saved_tile;
            cell.saved_version = then->second.saved_version;
            cell.saved_damage = then->second.saved_damage;
        } else {
            cell.state = State::on_disk;
            cell.saved = {};
            cell.saved_tile = false;
            cell.saved_version = 0;
            cell.saved_damage = {};
        }
        ++m_count[static_cast<usize>(cell.state)];
        if (cell.state == State::loaded) m_loaded.insert(coord);
    }
    m_wanted_stale = true;
    // Ids saved, or handed out by another side, since that scene was
    // current must not be handed out again.
    scene.reserve_ids_below(std::max(m_index.next_id, m_ids_seen));
    m_ids_seen = std::max(m_ids_seen, scene.next_id());
    m_scene = scene.identity();
}

void WorldStreaming::start_read(CellCoord cell) {
    set_state(cell, m_cells[cell], State::reading);
    m_jobs.submit(m_reads, [this, cell, path = cell_path(cell), epoch = m_epoch,
                            samples = m_index.terrain_samples] {
        Read read;
        read.cell = cell;
        read.epoch = epoch;
        if (const auto text = read_text(path)) {
            if (const auto error = world::load_cell(*text, cell, samples, read.entities, read.heights, &read.damage)) {
                read.error = path_to_utf8(path) + ": " + error->message;
            } else {
                for (Entity& e : read.entities) {
                    if (!e.mesh.empty()) e.mesh = MeshLibrary::normalize(e.mesh);
                }
                std::sort(read.entities.begin(), read.entities.end(), by_id);
            }
        } else {
            std::error_code error;
            read.missing = !std::filesystem::exists(path, error) && !error;
            read.error = "could not open " + path_to_utf8(path);
        }
        const std::lock_guard lock(m_mutex);
        m_finished.push_back(std::move(read));
    });
}

void WorldStreaming::take_reads() {
    std::vector<Read> finished;
    {
        const std::lock_guard lock(m_mutex);
        finished.swap(m_finished);
    }
    for (Read& r : finished) {
        if (r.epoch != m_epoch) {
            continue;
        }
        if (r.missing) {
            // Listed but gone (a save cut short): an empty cell.
            log_warning("world", r.error + "; the cell is taken as empty");
            forget(r.cell);
            continue;
        }
        const auto found = m_cells.find(r.cell);
        if (found == m_cells.end()) {
            continue; // its file was removed since the read began
        }
        Cell& cell = found->second;
        if (!r.error.empty()) {
            log_error("world", r.error);
            set_state(r.cell, cell, State::failed); // not read again until the world is opened again
            continue;
        }
        set_state(r.cell, cell, State::ready);
        m_ready.push_back(std::move(r));
    }
}

void WorldStreaming::finish_reads() {
    m_jobs.wait(m_reads);
    take_reads();
}

void WorldStreaming::insert(world::Scene& scene) {
    usize added = 0;
    usize taken = 0;
    for (; taken < m_ready.size(); ++taken) {
        Read& r = m_ready[taken];
        const usize weight = r.entities.size() + (r.heights ? tile_weight(scene.terrain.samples()) : 0);
        if (taken > 0 && added + weight > settings.entities_per_frame) {
            break;
        }
        const auto found = m_cells.find(r.cell);
        if (found == m_cells.end()) {
            continue; // its file was removed since (never happens: forget drops its read)
        }
        Cell& cell = found->second;
        const usize samples = scene.terrain.samples();
        if (r.heights && r.heights->size() != samples * samples) {
            // Only when the scene's terrain took another spacing while
            // tiles of the world's were on disk (see update): not loaded,
            // so a save cannot write over the file with less.
            log_error("world", "cell [" + std::to_string(r.cell.x) + ", " + std::to_string(r.cell.z) +
                                   "]'s terrain tile is not " + std::to_string(samples) +
                                   " samples a side, as the scene's terrain is: not loaded");
            set_state(r.cell, cell, State::failed);
            continue;
        }
        set_state(r.cell, cell, State::loaded);
        cell.saved = std::move(r.entities);
        cell.saved_tile = r.heights.has_value();
        cell.saved_version = 0;
        // Kept damage in the scene already (put back by an undo while the
        // cell was on disk) is newer than the file's.
        cell.saved_damage = std::move(r.damage);
        if (scene.damage_in(r.cell) == nullptr) scene.set_damage(r.cell, cell.saved_damage);
        if (r.heights) {
            // A tile in the scene already (made or put back by an undo while
            // the cell was on disk) is newer than the file's: clean only if
            // it holds the same heights.
            if (const world::Terrain::Heights* there = scene.terrain.find(r.cell); there == nullptr) {
                scene.terrain.set(r.cell, std::move(*r.heights));
                ++m_totals.tiles_loaded;
                added += tile_weight(scene.terrain.samples());
                cell.saved_version = scene.terrain.version(r.cell);
            } else if (*there == *r.heights) {
                cell.saved_version = scene.terrain.version(r.cell);
            }
        }
        for (const Entity& e : cell.saved) {
            const Entity* there = scene.find(e.id);
            const auto origin = m_origin.find(e.id);
            if (there == nullptr) {
                scene.add(e);
                m_origin[e.id] = r.cell;
                ++added;
            } else if (world::cell_of(there->transform.position) == r.cell ||
                       (origin != m_origin.end() && origin->second == r.cell)) {
                // The same entity, in the scene already (moved, or put back
                // by an undo, while the cell was on disk): the scene's is newer.
                m_origin[e.id] = r.cell;
            } else {
                // Another entity has its id (two files share it, or a new
                // entity took it): this one gets a new id, so neither is lost.
                Entity copy = e;
                copy.id = 0;
                const world::EntityId id = scene.add(std::move(copy));
                if (id != 0) {
                    log_warning("world", "cell [" + std::to_string(r.cell.x) + ", " + std::to_string(r.cell.z) +
                                             "]: entity id " + std::to_string(e.id) +
                                             " is taken; loaded as " + std::to_string(id) + " (save to keep it)");
                    m_origin[id] = r.cell;
                    ++added;
                    ++m_totals.ids_changed;
                }
            }
        }
    }
    m_ready.erase(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(taken));
    m_totals.entities_loaded += added;
}

bool WorldStreaming::unload(world::Scene& scene, const DVec3& camera, const SceneFrame& frame) {
    const double limit = settings.load_radius + settings.unload_margin;
    std::vector<world::EntityId> gone;
    std::vector<CellCoord> far;
    for (const CellCoord coord : m_loaded) {
        const bool ground = m_cells[coord].saved_tile || scene.terrain.find(coord) != nullptr;
        if (load_distance(camera, coord, ground) > limit) far.push_back(coord);
    }
    // Level-1 chunks with an unsaved tile keep all their tiles: the chunk's
    // file is older than the edit, so it must keep drawing them in full.
    std::unordered_set<CellCoord, world::CellCoordHash> edited;
    if (!far.empty()) {
        for (const auto& [coord, heights] : scene.terrain.tiles()) {
            const auto it = m_cells.find(coord);
            if (it == m_cells.end() || (it->second.state == State::loaded && !same_tile(scene.terrain, coord, it->second))) {
                edited.insert(world::far_chunk_of_cell(coord, 1));
            }
        }
        for (const CellCoord coord : m_loaded) {
            const Cell& cell = m_cells[coord];
            if (cell.saved_tile && scene.terrain.find(coord) == nullptr) edited.insert(world::far_chunk_of_cell(coord, 1));
        }
    }
    u32 let_go = 0;
    bool more = false;
    for (const CellCoord coord : far) {
        if (let_go >= std::max<u32>(1, settings.unloads_per_frame)) {
            more = true;
            break;
        }
        Cell& cell = m_cells[coord];
        if (cell.saved_tile && edited.contains(world::far_chunk_of_cell(coord, 1))) continue;
        std::vector<const Entity*> now = cell_entities(scene, frame, coord);
        if (!same_tile(scene.terrain, coord, cell) || !same_damage(scene, coord, cell) || !matches(now, cell.saved)) {
            continue; // unsaved edits: stays
        }
        for (const Entity* e : now) {
            gone.push_back(e->id); // its origin stays: met again, it is the same entity
        }
        if (cell.saved_tile) {
            scene.terrain.remove(coord);
            ++m_totals.tiles_unloaded;
        }
        if (scene.damage_in(coord) != nullptr) scene.set_damage(coord, {}); // (an empty entry held too)
        set_state(coord, cell, State::on_disk);
        cell.saved = {};
        cell.saved_tile = false;
        cell.saved_version = 0;
        cell.saved_damage = {};
        ++let_go;
    }
    if (!gone.empty()) {
        scene.remove_many(gone);
        m_totals.entities_unloaded += gone.size();
    }
    // Cells on disk that the scene already has entities in (moved there, or
    // put back by an undo): loaded first, whatever the distance, so their
    // file's other entities join them and a save keeps everything.
    m_touched.clear();
    for (const auto& [coord, built] : frame.grid) {
        const auto it = m_cells.find(coord);
        if (it != m_cells.end() && it->second.state == State::on_disk && has_entities(scene, built, coord)) {
            m_touched.push_back(coord);
        }
    }
    // And those the scene has a terrain tile or kept damage in.
    for (const auto& [coord, heights] : scene.terrain.tiles()) {
        const auto it = m_cells.find(coord);
        if (it != m_cells.end() && it->second.state == State::on_disk) {
            m_touched.push_back(coord);
        }
    }
    for (const auto& [coord, damage] : scene.damage()) {
        const auto it = m_cells.find(coord);
        if (it != m_cells.end() && it->second.state == State::on_disk) {
            m_touched.push_back(coord);
        }
    }
    return more;
}

void WorldStreaming::start_reads(const DVec3& camera) {
    const double r = std::min(settings.load_radius, k_max_load_radius);
    const CellCoord at = world::cell_of(camera);
    // The cells to read, made again only when the camera enters another
    // cell, the radius changes or a cell went back on disk.
    if (m_wanted_stale || !(at == m_wanted_at) || r != m_wanted_radius) {
        m_wanted_stale = false;
        m_wanted_at = at;
        m_wanted_radius = r;
        std::vector<std::pair<double, CellCoord>> near;
        const auto consider = [&](CellCoord c, const Cell& cell) {
            if (cell.state == State::on_disk) {
                const double d = load_distance(camera, c, m_ground.contains(c));
                if (d <= r) near.push_back({d, c});
            }
        };
        // Cells with ground go by their chunk: up to a chunk side farther.
        const i64 reach = static_cast<i64>(std::ceil(r / world::k_cell_size)) + 1 + (m_ground.empty() ? 0 : world::k_far_branch);
        if (m_cells.size() < static_cast<usize>((2 * reach + 1) * (2 * reach + 1))) {
            for (const auto& [coord, cell] : m_cells) consider(coord, cell);
        } else {
            for (i64 z = at.z - reach; z <= at.z + reach; ++z) {
                for (i64 x = at.x - reach; x <= at.x + reach; ++x) {
                    if (x < INT32_MIN || x > INT32_MAX || z < INT32_MIN || z > INT32_MAX) continue;
                    const CellCoord c{static_cast<i32>(x), static_cast<i32>(z)};
                    if (const auto it = m_cells.find(c); it != m_cells.end()) consider(c, it->second);
                }
            }
        }
        // Farthest first: the nearest is at the back, read first.
        std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first
                                      : (a.second.z != b.second.z ? a.second.z > b.second.z : a.second.x > b.second.x);
        });
        m_wanted.clear();
        for (const auto& [d, c] : near) m_wanted.push_back(c);
    }
    // Cells asked for whatever the distance (see unload) go first.
    while ((!m_touched.empty() || !m_wanted.empty()) &&
           m_count[static_cast<usize>(State::reading)] < settings.reads_in_flight) {
        std::vector<CellCoord>& from = m_touched.empty() ? m_wanted : m_touched;
        const CellCoord c = from.back();
        from.pop_back();
        const auto it = m_cells.find(c);
        if (it != m_cells.end() && it->second.state == State::on_disk) {
            start_read(c);
        }
    }
}

void WorldStreaming::update(world::Scene& scene, u64 generation, const DVec3& camera, const SceneFrame& frame) {
    if (!active(generation)) {
        return;
    }
    if (!follows(scene)) {
        return;
    }
    m_ids_seen = std::max(m_ids_seen, scene.next_id());
    // The scene's terrain takes the spacing of the tiles on disk while it
    // has none of its own (an undo can put another back while it is empty).
    if (!m_ground.empty() && scene.terrain.empty() && scene.terrain.samples() != m_index.terrain_samples) {
        scene.terrain.set_samples(m_index.terrain_samples);
    }
    take_reads();
    // The frame must be of this scene, or its cell lists mean nothing.
    const bool frame_current = frame.scene_identity == scene.identity();
    if (frame_current && ++m_frames >= std::max<u32>(1, settings.unload_every)) {
        m_frames = 0;
        // Cut short by the budget: looked at again next frame.
        if (unload(scene, camera, frame)) m_frames = std::max<u32>(1, settings.unload_every) - 1;
    }
    start_reads(camera);
    insert(scene);
}

WorldStreaming::Saved WorldStreaming::create(const std::filesystem::path& folder, const world::Scene& scene,
                                             u64 generation, std::vector<std::string> meshes) {
    if (is_open()) {
        return {"a world is open (" + path_to_utf8(m_folder) + "): save it, or close_world, before making another"};
    }
    std::error_code error;
    if (std::filesystem::exists(folder / "world.json", error)) {
        return {"a world is already saved in " + path_to_utf8(folder) + "; open it with open_world"};
    }
    if (std::filesystem::exists(folder / "ground", error)) {
        return {"the folder " + path_to_utf8(folder) + " holds a ground folder (another world's far ground): "
                "remove it, or pick another folder"};
    }
    std::filesystem::create_directories(folder / "cells", error);
    if (error) {
        return {"could not make " + path_to_utf8(folder / "cells") + ": " + error.message()};
    }
    close();
    m_folder = folder;
    m_generation = generation;
    m_scene = scene.identity();
    m_index.lighting = scene.lighting;
    m_index.kits = scene.kits;
    m_index.scatter = scene.scatter;
    m_index.weather = scene.weather;
    m_index.terrain_samples = scene.terrain.samples();
    m_index.terrain_material = scene.terrain.material;
    Saved saved = write(scene, std::move(meshes));
    if (!saved.error.empty()) {
        close();
    }
    return saved;
}

WorldStreaming::Saved WorldStreaming::save(const world::Scene& scene, std::vector<std::string> meshes) {
    if (!is_open()) {
        return {"no world is open"};
    }
    if (!follows(scene)) {
        return {"the world's scene was replaced outside undo; streaming stopped: open the world again"};
    }
    finish_reads();
    return write(scene, std::move(meshes));
}

WorldStreaming::Saved WorldStreaming::write(const world::Scene& scene, std::vector<std::string> meshes) {
    Saved saved;
    // What the scene holds, by cell.
    std::unordered_map<CellCoord, std::vector<Entity>, world::CellCoordHash> members;
    for (const Entity& e : scene.entities()) {
        members[world::cell_of(e.transform.position)].push_back(e);
    }
    const world::Terrain& terrain = scene.terrain;
    for (const auto& [coord, heights] : terrain.tiles()) {
        members[coord]; // a cell with ground has a file
    }
    for (const auto& [coord, damage] : scene.damage()) {
        members[coord]; // and one with kept damage
    }
    // The tiles in the files of cells on disk that the scene has no tile
    // in, kept when the file is written again.
    std::unordered_map<CellCoord, world::Terrain::Heights, world::CellCoordHash> kept_heights;
    // The same for their kept damage, when the scene has none there.
    std::unordered_map<CellCoord, world::CellDamage, world::CellCoordHash> kept_damage;
    // Cells on disk that an entity of the scene was read from or saved to,
    // and has left since (moved, or put back elsewhere by an undo): their
    // file is written again without it, so it is in one file only.
    for (const Entity& e : scene.entities()) {
        const auto origin = m_origin.find(e.id);
        if (origin == m_origin.end() || origin->second == world::cell_of(e.transform.position)) continue;
        const auto cell = m_cells.find(origin->second);
        // One that could not be read refuses the save (below): written
        // around, it would keep a second copy for when it is fixed.
        if (cell != m_cells.end() && cell->second.state != State::loaded) members[origin->second];
    }
    for (const auto& [coord, cell] : m_cells) {
        if (cell.state == State::failed && members.contains(coord)) {
            return {"cell [" + std::to_string(coord.x) + ", " + std::to_string(coord.z) +
                    "]'s file could not be read (see the log): fix or remove " + path_to_utf8(cell_path(coord)) +
                    " before saving"};
        }
    }
    // The tiles must all be one spacing: the scene's, unless it has none
    // (then the world's stays). Commands keep them so; this is the guard.
    const bool on_disk_ground = std::any_of(m_ground.begin(), m_ground.end(), [&](CellCoord c) {
        const auto it = m_cells.find(c);
        return it != m_cells.end() && it->second.state != State::loaded;
    });
    if (!terrain.empty() && on_disk_ground && terrain.samples() != m_index.terrain_samples) {
        return {"the terrain is " + std::to_string(terrain.samples()) + " samples a side (an undo put back "
                "tiles of an older spacing) and the world's tiles on disk are " +
                std::to_string(m_index.terrain_samples) + ": undo further, or remove_terrain all, to save"};
    }
    const u32 samples = !terrain.empty() || !on_disk_ground ? terrain.samples() : m_index.terrain_samples;
    // While files hold tiles of the old spacing, the first index keeps it
    // (a save cut short then leaves files and index agreeing); the last
    // one, after every file is written, has the new.
    if (m_ground.empty()) m_index.terrain_samples = samples;
    m_index.terrain_material = terrain.material;
    // The index first, with the next id past every id about to be written
    // and every cell about to have a file, so a save cut short never hands
    // out ids on disk nor loses track of a file (a listed file that is
    // missing loads as an empty cell).
    m_ids_seen = std::max(m_ids_seen, scene.next_id());
    m_index.next_id = std::max(m_index.next_id, m_ids_seen);
    for (const auto& [coord, list] : members) {
        if (!m_cells.contains(coord)) m_index.cells.push_back(coord);
    }
    // Every cell about to hold a tile too (listed with a file, or not, is
    // harmless; a tile not listed would be missed by what reads the list).
    m_index.terrain_cells.assign(m_ground.begin(), m_ground.end());
    for (const auto& [coord, heights] : terrain.tiles()) {
        if (!m_ground.contains(coord)) m_index.terrain_cells.push_back(coord);
    }
    // The far ground owed if the save is cut short: every tile that may change
    // (put says exactly which do), and what was owed already.
    std::unordered_set<CellCoord, world::CellCoordHash> may_owe = m_far_owed;
    for (const auto& [coord, heights] : terrain.tiles()) {
        const auto it = m_cells.find(coord);
        if (it == m_cells.end() || it->second.state != State::loaded || !same_tile(terrain, coord, it->second)) {
            may_owe.insert(coord);
        }
    }
    for (const CellCoord coord : m_loaded) {
        if (m_cells[coord].saved_tile && terrain.find(coord) == nullptr) may_owe.insert(coord);
    }
    m_index.far_owed.assign(may_owe.begin(), may_owe.end());
    m_index.far_owed_2.assign(m_far_owed2.begin(), m_far_owed2.end());
    if (!write_text(m_folder / "world.json", world::save_world_index(m_index))) {
        return {"could not write " + path_to_utf8(m_folder / "world.json")};
    }
    // Cells on disk (or parsed, not yet added) that the scene has entities
    // in: their file's other entities are kept.
    for (auto& [coord, list] : members) {
        const auto it = m_cells.find(coord);
        if (it == m_cells.end() || it->second.state == State::loaded) {
            continue;
        }
        std::vector<Entity> from_file;
        std::optional<world::Terrain::Heights> heights_from_file;
        world::CellDamage damage_from_file;
        if (it->second.state == State::ready) {
            for (const Read& r : m_ready) {
                if (r.cell == coord) {
                    from_file = r.entities;
                    heights_from_file = r.heights;
                    damage_from_file = r.damage;
                }
            }
        } else if (const auto text = read_text(cell_path(coord))) {
            if (const auto bad = world::load_cell(*text, coord, m_index.terrain_samples, from_file, heights_from_file,
                                                  &damage_from_file)) {
                return {path_to_utf8(cell_path(coord)) + ": " + bad->message};
            }
        } else if (std::error_code error; !std::filesystem::exists(cell_path(coord), error) && !error) {
            // Listed but gone (a save cut short, or removed by hand): an
            // empty cell, as streaming takes it.
            log_warning("world", "could not open " + path_to_utf8(cell_path(coord)) + "; the cell is taken as empty");
        } else {
            return {"could not open " + path_to_utf8(cell_path(coord))};
        }
        for (Entity& e : from_file) {
            if (scene.find(e.id) == nullptr) list.push_back(std::move(e));
        }
        if (heights_from_file && terrain.find(coord) == nullptr) {
            if (heights_from_file->size() != usize{terrain.samples()} * terrain.samples()) {
                return {path_to_utf8(cell_path(coord)) + ": its terrain tile is not " +
                        std::to_string(terrain.samples()) + " samples a side, as the terrain is"};
            }
            kept_heights[coord] = std::move(*heights_from_file);
        }
        if (!damage_from_file.empty() && scene.damage_in(coord) == nullptr) {
            kept_damage[coord] = std::move(damage_from_file);
        }
    }
    // Write what differs from its file. Every loaded cell is looked at,
    // whether or not it still has entities. The bookkeeping follows each
    // file as it is written, so a save cut short leaves it true.
    const auto put = [&](CellCoord coord, std::vector<Entity>& list) -> bool {
        const auto it = m_cells.find(coord);
        const world::Terrain::Heights* heights = terrain.find(coord);
        if (heights == nullptr) {
            const auto kept = kept_heights.find(coord);
            if (kept != kept_heights.end()) heights = &kept->second;
        }
        const bool tile_changed = it == m_cells.end()                 ? heights != nullptr
                                  : it->second.state == State::loaded ? !same_tile(terrain, coord, it->second)
                                                                      : terrain.find(coord) != nullptr;
        // The far ground over it is owed (written after the cells; kept owed
        // across a save cut short, until its chunk is written).
        if (tile_changed || (heights == nullptr && m_ground.contains(coord))) m_far_owed.insert(coord);
        const world::CellDamage* damage = scene.damage_in(coord);
        if (damage == nullptr) {
            const auto kept = kept_damage.find(coord);
            if (kept != kept_damage.end()) damage = &kept->second;
        }
        if (damage != nullptr && damage->empty()) damage = nullptr; // "none here": none written
        if (list.empty() && heights == nullptr && damage == nullptr) {
            if (it != m_cells.end()) {
                std::error_code error;
                std::filesystem::remove(cell_path(coord), error);
                if (error) {
                    saved.error = "could not remove " + path_to_utf8(cell_path(coord)) + ": " + error.message();
                    return false;
                }
                ++saved.cells_removed;
                forget(coord);
            }
            return true;
        }
        std::sort(list.begin(), list.end(), by_id);
        const bool same = it != m_cells.end() && it->second.state == State::loaded &&
                          same_tile(terrain, coord, it->second) && same_damage(scene, coord, it->second) &&
                          list == it->second.saved;
        if (!same) {
            if (!write_text(cell_path(coord), world::save_cell(coord, list, heights, damage))) {
                saved.error = "could not write " + path_to_utf8(cell_path(coord));
                return false;
            }
            ++saved.cells_written;
        }
        for (const Entity& e : list) m_origin[e.id] = coord;
        if (heights != nullptr) {
            m_ground.insert(coord);
        } else {
            m_ground.erase(coord);
        }
        // A loaded cell's tile is the scene's (kept ones are for cells on disk).
        const auto keep_tile = [&](Cell& cell) {
            cell.saved_tile = heights != nullptr;
            cell.saved_version = terrain.version(coord);
            cell.saved_damage = damage != nullptr ? *damage : world::CellDamage{};
        };
        if (it == m_cells.end()) {
            Cell& cell = m_cells[coord]; // new: all of it is in the scene
            ++m_count[static_cast<usize>(State::on_disk)];
            set_state(coord, cell, State::loaded);
            cell.saved = std::move(list);
            keep_tile(cell);
            cell.written = ++m_writes;
            return true;
        }
        Cell& cell = it->second;
        if (!same) cell.written = ++m_writes;
        if (cell.state == State::loaded) {
            cell.saved = std::move(list);
            keep_tile(cell);
        } else {
            // On disk with some entities in the scene: stays on disk, and
            // is read in full next (the scene's entities win at insert).
            set_state(coord, cell, State::on_disk);
            cell.saved = {};
            cell.saved_tile = false;
            cell.saved_version = 0;
            cell.saved_damage = {};
            std::erase_if(m_ready, [&](const Read& r) { return r.cell == coord; });
        }
        return true;
    };
    for (auto& [coord, list] : members) {
        if (!put(coord, list)) return saved;
    }
    std::vector<CellCoord> emptied;
    for (const auto& [coord, cell] : m_cells) {
        if (cell.state == State::loaded && !members.contains(coord)) emptied.push_back(coord);
    }
    for (const CellCoord coord : emptied) {
        std::vector<Entity> none;
        if (!put(coord, none)) return saved;
    }
    if (!write_far(scene, kept_heights, saved)) return saved;
    m_index.lighting = scene.lighting;
    m_index.kits = scene.kits;
    m_index.scatter = scene.scatter;
    m_index.weather = scene.weather;
    m_index.terrain_cells.assign(m_ground.begin(), m_ground.end());
    m_index.terrain_samples = samples;
    m_index.far_owed.assign(m_far_owed.begin(), m_far_owed.end()); // empty, unless chunks failed
    m_index.far_owed_2.assign(m_far_owed2.begin(), m_far_owed2.end());
    for (std::string& m : meshes) {
        m_index.meshes.push_back(std::move(m));
    }
    std::sort(m_index.meshes.begin(), m_index.meshes.end());
    m_index.meshes.erase(std::unique(m_index.meshes.begin(), m_index.meshes.end()), m_index.meshes.end());
    m_index.cells.clear();
    for (const auto& [coord, cell] : m_cells) {
        m_index.cells.push_back(coord);
    }
    if (!write_text(m_folder / "world.json", world::save_world_index(m_index))) {
        saved.error = "could not write " + path_to_utf8(m_folder / "world.json");
        return saved;
    }
    saved.cells = static_cast<u32>(m_cells.size());
    return saved;
}

bool WorldStreaming::write_far(const world::Scene& scene,
                               const std::unordered_map<CellCoord, world::Terrain::Heights, world::CellCoordHash>& kept,
                               Saved& saved) {
    const auto& changed = m_far_owed;
    using Heights = world::Terrain::Heights;
    constexpr usize k_chunk = usize{world::k_far_samples} * world::k_far_samples;
    const auto none = [] { return Heights(k_chunk, std::numeric_limits<float>::quiet_NaN()); };
    const auto empty = [](const Heights& h) { return std::all_of(h.begin(), h.end(), [](float v) { return std::isnan(v); }); };
    // A chunk's file, if there is one (an unreadable one is written afresh).
    const auto read_chunk = [&](u32 level, CellCoord chunk) -> std::optional<Heights> {
        if (!m_far[level - 1].contains(chunk)) return std::nullopt;
        const auto text = read_text(far_chunk_path(level, chunk));
        Heights h;
        if (!text) {
            log_warning("world", "could not open " + path_to_utf8(far_chunk_path(level, chunk)) + "; written afresh");
            return std::nullopt;
        }
        if (const auto bad = world::load_far_chunk(*text, level, chunk, h)) {
            log_warning("world", path_to_utf8(far_chunk_path(level, chunk)) + ": " + bad->message + "; written afresh");
            return std::nullopt;
        }
        return h;
    };
    // Writes (or, with no ground left, removes) a chunk's file.
    const auto put_chunk = [&](u32 level, CellCoord chunk, Heights heights) -> bool {
        const std::filesystem::path path = far_chunk_path(level, chunk);
        std::error_code error;
        if (empty(heights)) {
            if (m_far[level - 1].contains(chunk)) {
                std::filesystem::remove(path, error);
                if (error) {
                    saved.error = "could not remove " + path_to_utf8(path) + ": " + error.message();
                    return false;
                }
                m_far[level - 1].erase(chunk);
                ++saved.chunks_written;
                saved.chunks.push_back({level, chunk, std::nullopt});
            }
            return true;
        }
        std::filesystem::create_directories(path.parent_path(), error);
        if (error || !write_text(path, world::save_far_chunk(level, chunk, heights))) {
            saved.error = "could not write " + path_to_utf8(path);
            return false;
        }
        m_far[level - 1].insert(chunk);
        ++saved.chunks_written;
        saved.chunks.push_back({level, chunk, std::move(heights)});
        return true;
    };
    // Level 1, over the changed cells: the file's heights where nothing
    // changed, the tiles (the scene's, kept ones, or read from their cell's
    // file when the chunk had no file yet) elsewhere.
    std::unordered_set<CellCoord, world::CellCoordHash> chunks1;
    for (const CellCoord c : changed) chunks1.insert(world::far_chunk_of_cell(c, 1));
    std::unordered_map<CellCoord, std::optional<Heights>, world::CellCoordHash> made1; // nullopt: no ground
    const u32 samples = scene.terrain.samples();
    const auto side = [](usize size) {
        u32 n = 1;
        while (usize{n} * n < size) n *= 2;
        return n;
    };
    for (const CellCoord chunk : chunks1) {
        const std::optional<Heights> file = read_chunk(1, chunk);
        Heights heights = file ? *file : none();
        for (i32 b = 0; b < world::k_far_branch; ++b) {
            for (i32 a = 0; a < world::k_far_branch; ++a) {
                const CellCoord cell{chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b};
                const Heights* tile = scene.terrain.find(cell);
                if (tile == nullptr) {
                    if (const auto k = kept.find(cell); k != kept.end()) tile = &k->second;
                }
                std::optional<Heights> from_file;
                if (tile == nullptr && m_ground.contains(cell)) {
                    if (file && !changed.contains(cell)) continue; // as the chunk's file has it
                    // On disk only, and the chunk's file is not to be trusted with it: its own file.
                    std::vector<Entity> entities;
                    if (const auto text = read_text(cell_path(cell))) {
                        if (const auto bad = world::load_cell(*text, cell, m_index.terrain_samples, entities, from_file)) {
                            log_warning("world", path_to_utf8(cell_path(cell)) + ": " + bad->message);
                            from_file.reset();
                        }
                    }
                    if (from_file && from_file->size() == usize{side(from_file->size())} * side(from_file->size())) {
                        tile = &*from_file;
                    }
                }
                // A tile read from its file has the file's spacing (the scene's when it has tiles).
                const u32 n = tile == nullptr ? world::k_far_samples : (from_file && tile == &*from_file) ? side(tile->size()) : samples;
                world::far_fill(1, chunk, cell, tile, n, heights);
            }
        }
        made1[chunk] = empty(heights) ? std::nullopt : std::optional<Heights>(heights);
        if (!put_chunk(1, chunk, std::move(heights))) return false;
        for (i32 b = 0; b < world::k_far_branch; ++b) {
            for (i32 a = 0; a < world::k_far_branch; ++a) {
                m_far_owed.erase({chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b});
            }
        }
        m_far_owed2.insert(chunk);
    }
    // Level 2, over the level-1 chunks written (this save or one cut short).
    std::unordered_set<CellCoord, world::CellCoordHash> chunks2;
    for (const CellCoord c : m_far_owed2) chunks2.insert(world::far_parent(c));
    for (const CellCoord chunk : chunks2) {
        const std::optional<Heights> file = read_chunk(2, chunk);
        Heights heights = file ? *file : none();
        for (i32 b = 0; b < world::k_far_branch; ++b) {
            for (i32 a = 0; a < world::k_far_branch; ++a) {
                const CellCoord child{chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b};
                std::optional<Heights> h;
                if (const auto m = made1.find(child); m != made1.end()) {
                    h = m->second;
                } else if (file && !m_far_owed2.contains(child)) {
                    continue; // as the chunk's file has it
                } else {
                    h = read_chunk(1, child);
                }
                world::far_fill(2, chunk, child, h ? &*h : nullptr, world::k_far_samples, heights);
            }
        }
        if (!put_chunk(2, chunk, std::move(heights))) return false;
        for (i32 b = 0; b < world::k_far_branch; ++b) {
            for (i32 a = 0; a < world::k_far_branch; ++a) {
                m_far_owed2.erase({chunk.x * world::k_far_branch + a, chunk.z * world::k_far_branch + b});
            }
        }
    }
    return true;
}

bool WorldStreaming::all_loaded() const { return m_loaded.size() == m_cells.size(); }

u32 WorldStreaming::unsaved_cells(const world::Scene& scene) const {
    std::unordered_map<CellCoord, std::vector<const Entity*>, world::CellCoordHash> members;
    for (const Entity& e : scene.entities()) {
        members[world::cell_of(e.transform.position)].push_back(&e);
    }
    for (const auto& [coord, heights] : scene.terrain.tiles()) {
        members[coord];
    }
    for (const auto& [coord, damage] : scene.damage()) {
        members[coord];
    }
    u32 unsaved = 0;
    for (auto& [coord, list] : members) {
        const auto it = m_cells.find(coord);
        if (it == m_cells.end() || it->second.state != State::loaded) {
            ++unsaved; // no file yet, or entities, a tile or damage in a cell still on disk
        } else if (!same_tile(scene.terrain, coord, it->second) || !same_damage(scene, coord, it->second) ||
                   !matches(list, it->second.saved)) {
            ++unsaved;
        }
    }
    for (const CellCoord coord : m_loaded) {
        unsaved += members.contains(coord) ? 0u : 1u; // emptied
    }
    return unsaved;
}

bool WorldStreaming::unsaved_settings(const world::Scene& scene) const {
    return !(scene.lighting == m_index.lighting) || !(scene.kits == m_index.kits) ||
           !(scene.scatter == m_index.scatter) || !(scene.weather == m_index.weather);
}

StreamingStats WorldStreaming::stats() const {
    StreamingStats s = m_totals;
    s.cells = static_cast<u32>(m_cells.size());
    s.loaded = m_count[static_cast<usize>(State::loaded)];
    s.reading = m_count[static_cast<usize>(State::reading)];
    s.waiting = m_count[static_cast<usize>(State::ready)];
    s.failed = m_count[static_cast<usize>(State::failed)];
    s.ground = static_cast<u32>(m_ground.size());
    return s;
}

} // namespace giant::editor
