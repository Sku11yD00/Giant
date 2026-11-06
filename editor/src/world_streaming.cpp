#include "world_streaming.h"

#include "mesh_library.h"

#include "giant/core/log.h"
#include "giant/core/text.h"

#include <algorithm>
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
double ground_distance(const DVec3& p, CellCoord c) {
    const double x0 = c.x * world::k_cell_size;
    const double z0 = c.z * world::k_cell_size;
    const double dx = std::max({x0 - p.x, 0.0, p.x - (x0 + world::k_cell_size)});
    const double dz = std::max({z0 - p.z, 0.0, p.z - (z0 + world::k_cell_size)});
    return std::sqrt(dx * dx + dz * dz);
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

WorldStreaming::WorldStreaming() = default;

WorldStreaming::~WorldStreaming() { m_jobs.wait(m_reads); }

std::filesystem::path WorldStreaming::cell_path(CellCoord cell) const {
    return m_folder / "cells" / world::cell_file_name(cell);
}

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
    close();
    world::Scene base;
    base.lighting = index.lighting;
    base.kits = index.kits;
    base.reserve_ids_below(index.next_id);
    scene = std::move(base);
    m_scene = scene.identity();
    m_folder = folder;
    m_generation = generation;
    m_index = std::move(index);
    for (const CellCoord c : m_index.cells) {
        m_cells[c] = Cell{};
    }
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
        } else {
            cell.state = State::on_disk;
            cell.saved = {};
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
    m_jobs.submit(m_reads, [this, cell, path = cell_path(cell), epoch = m_epoch] {
        Read read;
        read.cell = cell;
        read.epoch = epoch;
        if (const auto text = read_text(path)) {
            if (const auto error = world::load_cell(*text, cell, read.entities)) {
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
        if (taken > 0 && added + r.entities.size() > settings.entities_per_frame) {
            break;
        }
        const auto found = m_cells.find(r.cell);
        if (found == m_cells.end()) {
            continue; // its file was removed since (never happens: forget drops its read)
        }
        Cell& cell = found->second;
        set_state(r.cell, cell, State::loaded);
        cell.saved = std::move(r.entities);
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

void WorldStreaming::unload(world::Scene& scene, const DVec3& camera, const SceneFrame& frame) {
    const double limit = settings.load_radius + settings.unload_margin;
    std::vector<world::EntityId> gone;
    std::vector<CellCoord> far;
    for (const CellCoord coord : m_loaded) {
        if (ground_distance(camera, coord) > limit) far.push_back(coord);
    }
    for (const CellCoord coord : far) {
        Cell& cell = m_cells[coord];
        std::vector<const Entity*> now = cell_entities(scene, frame, coord);
        if (!matches(now, cell.saved)) {
            continue; // unsaved edits: stays
        }
        for (const Entity* e : now) {
            gone.push_back(e->id); // its origin stays: met again, it is the same entity
        }
        set_state(coord, cell, State::on_disk);
        cell.saved = {};
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
                const double d = ground_distance(camera, c);
                if (d <= r) near.push_back({d, c});
            }
        };
        const i64 reach = static_cast<i64>(std::ceil(r / world::k_cell_size)) + 1;
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
    take_reads();
    // The frame must be of this scene, or its cell lists mean nothing.
    const bool frame_current = frame.scene_identity == scene.identity();
    if (frame_current && ++m_frames >= std::max<u32>(1, settings.unload_every)) {
        m_frames = 0;
        unload(scene, camera, frame);
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
    // Cells on disk that an entity of the scene was read from or saved to,
    // and has left since (moved, or put back elsewhere by an undo): their
    // file is written again without it, so it is in one file only.
    for (const Entity& e : scene.entities()) {
        const auto origin = m_origin.find(e.id);
        if (origin == m_origin.end() || origin->second == world::cell_of(e.transform.position)) continue;
        const auto cell = m_cells.find(origin->second);
        // A file that could not be read is left as it is (it blocks only
        // saving into its own cell).
        if (cell != m_cells.end() && cell->second.state != State::loaded && cell->second.state != State::failed) {
            members[origin->second];
        }
    }
    for (const auto& [coord, cell] : m_cells) {
        if (cell.state == State::failed && members.contains(coord)) {
            return {"cell [" + std::to_string(coord.x) + ", " + std::to_string(coord.z) +
                    "]'s file could not be read (see the log): fix or remove " + path_to_utf8(cell_path(coord)) +
                    " before saving"};
        }
    }
    // The index first, with the next id past every id about to be written
    // and every cell about to have a file, so a save cut short never hands
    // out ids on disk nor loses track of a file (a listed file that is
    // missing loads as an empty cell).
    m_ids_seen = std::max(m_ids_seen, scene.next_id());
    m_index.next_id = std::max(m_index.next_id, m_ids_seen);
    for (const auto& [coord, list] : members) {
        if (!m_cells.contains(coord)) m_index.cells.push_back(coord);
    }
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
        if (it->second.state == State::ready) {
            for (const Read& r : m_ready) {
                if (r.cell == coord) from_file = r.entities;
            }
        } else if (const auto text = read_text(cell_path(coord))) {
            if (const auto bad = world::load_cell(*text, coord, from_file)) {
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
    }
    // Write what differs from its file. Every loaded cell is looked at,
    // whether or not it still has entities. The bookkeeping follows each
    // file as it is written, so a save cut short leaves it true.
    const auto put = [&](CellCoord coord, std::vector<Entity>& list) -> bool {
        const auto it = m_cells.find(coord);
        if (list.empty()) {
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
        const bool same = it != m_cells.end() && it->second.state == State::loaded && list == it->second.saved;
        if (!same) {
            if (!write_text(cell_path(coord), world::save_cell(coord, list))) {
                saved.error = "could not write " + path_to_utf8(cell_path(coord));
                return false;
            }
            ++saved.cells_written;
        }
        for (const Entity& e : list) m_origin[e.id] = coord;
        if (it == m_cells.end()) {
            Cell& cell = m_cells[coord]; // new: all of it is in the scene
            ++m_count[static_cast<usize>(State::on_disk)];
            set_state(coord, cell, State::loaded);
            cell.saved = std::move(list);
            cell.written = ++m_writes;
            return true;
        }
        Cell& cell = it->second;
        if (!same) cell.written = ++m_writes;
        if (cell.state == State::loaded) {
            cell.saved = std::move(list);
        } else {
            // On disk with some entities in the scene: stays on disk, and
            // is read in full next (the scene's entities win at insert).
            set_state(coord, cell, State::on_disk);
            cell.saved = {};
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
    m_index.lighting = scene.lighting;
    m_index.kits = scene.kits;
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

bool WorldStreaming::all_loaded() const { return m_loaded.size() == m_cells.size(); }

u32 WorldStreaming::unsaved_cells(const world::Scene& scene) const {
    std::unordered_map<CellCoord, std::vector<const Entity*>, world::CellCoordHash> members;
    for (const Entity& e : scene.entities()) {
        members[world::cell_of(e.transform.position)].push_back(&e);
    }
    u32 unsaved = 0;
    for (auto& [coord, list] : members) {
        const auto it = m_cells.find(coord);
        if (it == m_cells.end() || it->second.state != State::loaded) {
            ++unsaved; // no file yet, or entities in a cell still on disk
        } else if (!matches(list, it->second.saved)) {
            ++unsaved;
        }
    }
    for (const CellCoord coord : m_loaded) {
        unsaved += members.contains(coord) ? 0u : 1u; // emptied
    }
    return unsaved;
}

StreamingStats WorldStreaming::stats() const {
    StreamingStats s = m_totals;
    s.cells = static_cast<u32>(m_cells.size());
    s.loaded = m_count[static_cast<usize>(State::loaded)];
    s.reading = m_count[static_cast<usize>(State::reading)];
    s.waiting = m_count[static_cast<usize>(State::ready)];
    s.failed = m_count[static_cast<usize>(State::failed)];
    return s;
}

} // namespace giant::editor
