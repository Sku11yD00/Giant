#include "world_streaming.h"

#include "mesh_library.h"

#include "giant/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace giant::editor {

namespace {

using world::CellCoord;
using world::Entity;

std::string path_text(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

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
        return "could not open " + path_text(index_path);
    }
    world::WorldIndex index;
    if (const auto error = world::load_world_index(*text, index)) {
        return path_text(index_path) + ": " + error->message;
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
    m_folder = folder;
    m_generation = generation;
    m_index = std::move(index);
    for (const CellCoord c : m_index.cells) {
        m_cells[c] = Cell{};
    }
    return std::nullopt;
}

void WorldStreaming::close() {
    m_jobs.wait(m_reads); // their results are dropped (older epoch)
    ++m_epoch;
    m_folder.clear();
    m_generation = 0;
    m_index = {};
    m_cells.clear();
    m_ready.clear();
    m_totals = {};
    m_frames = 0;
    const std::lock_guard lock(m_mutex);
    m_finished.clear();
}

void WorldStreaming::start_read(CellCoord cell) {
    m_cells[cell].state = State::reading;
    m_jobs.submit(m_reads, [this, cell, path = cell_path(cell), epoch = m_epoch] {
        Read read{cell, epoch, {}, {}};
        if (const auto text = read_text(path)) {
            if (const auto error = world::load_cell(*text, cell, read.entities)) {
                read.error = path_text(path) + ": " + error->message;
            } else {
                for (Entity& e : read.entities) {
                    if (!e.mesh.empty()) e.mesh = MeshLibrary::normalize(e.mesh);
                }
                std::sort(read.entities.begin(), read.entities.end(), by_id);
            }
        } else {
            read.error = "could not open " + path_text(path);
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
        Cell& cell = m_cells[r.cell];
        if (!r.error.empty()) {
            log_error("world", r.error);
            cell.state = State::failed; // not read again until the world is opened again
            continue;
        }
        cell.state = State::ready;
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
        Cell& cell = m_cells[r.cell];
        cell.state = State::loaded;
        cell.saved = std::move(r.entities);
        for (const Entity& e : cell.saved) {
            // Already in the scene (moved here, or put back by an undo,
            // while the cell was on disk): the scene's is the newer.
            if (scene.find(e.id) == nullptr) {
                scene.add(e);
                ++added;
            }
        }
    }
    m_ready.erase(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(taken));
    m_totals.entities_loaded += added;
}

void WorldStreaming::unload(world::Scene& scene, const DVec3& camera, const SceneFrame& frame) {
    const double limit = settings.load_radius + settings.unload_margin;
    std::vector<world::EntityId> gone;
    for (auto& [coord, cell] : m_cells) {
        if (cell.state != State::loaded || ground_distance(camera, coord) <= limit) {
            continue;
        }
        std::vector<const Entity*> now = cell_entities(scene, frame, coord);
        if (!matches(now, cell.saved)) {
            continue; // unsaved edits: stays
        }
        for (const Entity* e : now) {
            gone.push_back(e->id);
        }
        cell.state = State::on_disk;
        cell.saved = {};
    }
    if (!gone.empty()) {
        scene.remove_many(gone);
        m_totals.entities_unloaded += gone.size();
    }
}

void WorldStreaming::start_reads(const world::Scene& scene, const DVec3& camera, const SceneFrame& frame) {
    u32 in_flight = 0;
    for (const auto& [coord, cell] : m_cells) {
        in_flight += cell.state == State::reading ? 1u : 0u;
    }
    if (in_flight >= settings.reads_in_flight) {
        return;
    }
    std::vector<std::pair<double, CellCoord>> wanted;
    // Cells on disk that the scene already has entities in (moved there, or
    // put back by an undo): loaded whatever the distance, so their file's
    // other entities join them and a save keeps everything.
    for (const auto& [coord, built] : frame.grid) {
        const auto it = m_cells.find(coord);
        if (it != m_cells.end() && it->second.state == State::on_disk && has_entities(scene, built, coord)) {
            wanted.push_back({-1.0, coord});
        }
    }
    const double r = settings.load_radius;
    const CellCoord at = world::cell_of(camera);
    const i64 reach = static_cast<i64>(std::ceil(r / world::k_cell_size)) + 1;
    if (m_cells.size() < static_cast<usize>((2 * reach + 1) * (2 * reach + 1))) {
        for (const auto& [coord, cell] : m_cells) {
            if (cell.state == State::on_disk && ground_distance(camera, coord) <= r) {
                wanted.push_back({ground_distance(camera, coord), coord});
            }
        }
    } else {
        for (i64 z = at.z - reach; z <= at.z + reach; ++z) {
            for (i64 x = at.x - reach; x <= at.x + reach; ++x) {
                if (x < INT32_MIN || x > INT32_MAX || z < INT32_MIN || z > INT32_MAX) continue;
                const CellCoord c{static_cast<i32>(x), static_cast<i32>(z)};
                const auto it = m_cells.find(c);
                if (it != m_cells.end() && it->second.state == State::on_disk && ground_distance(camera, c) <= r) {
                    wanted.push_back({ground_distance(camera, c), c});
                }
            }
        }
    }
    const auto nearest = [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first
                                  : (a.second.z != b.second.z ? a.second.z < b.second.z : a.second.x < b.second.x);
    };
    std::sort(wanted.begin(), wanted.end(), nearest);
    for (usize i = 0; i < wanted.size() && in_flight < settings.reads_in_flight; ++i) {
        if (m_cells[wanted[i].second].state == State::on_disk) {
            start_read(wanted[i].second);
            ++in_flight;
        }
    }
}

void WorldStreaming::update(world::Scene& scene, u64 generation, const DVec3& camera, const SceneFrame& frame) {
    if (!active(generation)) {
        return;
    }
    take_reads();
    // The frame must be of this scene, or its cell lists mean nothing.
    const bool frame_current = frame.scene_identity == scene.identity();
    if (frame_current && ++m_frames >= std::max<u32>(1, settings.unload_every)) {
        m_frames = 0;
        unload(scene, camera, frame);
    }
    if (frame_current) {
        start_reads(scene, camera, frame);
    }
    insert(scene);
}

WorldStreaming::Saved WorldStreaming::create(const std::filesystem::path& folder, const world::Scene& scene,
                                             u64 generation, std::vector<std::string> meshes) {
    std::error_code error;
    if (std::filesystem::exists(folder / "world.json", error)) {
        return {"a world is already saved in " + path_text(folder) + "; open it with open_world"};
    }
    std::filesystem::create_directories(folder / "cells", error);
    if (error) {
        return {"could not make " + path_text(folder / "cells") + ": " + error.message()};
    }
    close();
    m_folder = folder;
    m_generation = generation;
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
    for (const auto& [coord, cell] : m_cells) {
        if (cell.state == State::failed && members.contains(coord)) {
            return {"cell [" + std::to_string(coord.x) + ", " + std::to_string(coord.z) +
                    "]'s file could not be read (see the log): fix or remove " + path_text(cell_path(coord)) +
                    " before saving"};
        }
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
                return {path_text(cell_path(coord)) + ": " + bad->message};
            }
        } else {
            return {"could not open " + path_text(cell_path(coord))};
        }
        for (Entity& e : from_file) {
            if (scene.find(e.id) == nullptr) list.push_back(std::move(e));
        }
    }
    // Write what differs from its file. Every loaded cell is looked at,
    // whether or not it still has entities.
    std::vector<std::pair<CellCoord, std::vector<Entity>>> kept; // as now on disk
    const auto put = [&](CellCoord coord, std::vector<Entity>& list) -> bool {
        const auto it = m_cells.find(coord);
        const bool had_file = it != m_cells.end();
        if (list.empty()) {
            if (had_file) {
                std::error_code error;
                std::filesystem::remove(cell_path(coord), error);
                if (error) {
                    saved.error = "could not remove " + path_text(cell_path(coord)) + ": " + error.message();
                    return false;
                }
                ++saved.cells_removed;
            }
            return true;
        }
        std::sort(list.begin(), list.end(), by_id);
        if (!had_file || it->second.state != State::loaded || list != it->second.saved) {
            if (!write_text(cell_path(coord), world::save_cell(coord, list))) {
                saved.error = "could not write " + path_text(cell_path(coord));
                return false;
            }
            ++saved.cells_written;
        }
        kept.push_back({coord, std::move(list)});
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
        m_cells.erase(coord);
    }
    // The bookkeeping follows the files.
    for (auto& [coord, list] : kept) {
        const auto it = m_cells.find(coord);
        if (it == m_cells.end()) {
            m_cells[coord] = Cell{State::loaded, std::move(list)}; // new: all of it is in the scene
        } else if (it->second.state == State::loaded) {
            it->second.saved = std::move(list);
        } else {
            // On disk with some entities in the scene: stays on disk, and
            // is read in full next (the scene's entities win at insert).
            it->second.state = State::on_disk;
            std::erase_if(m_ready, [&](const Read& r) { return r.cell == coord; });
        }
    }
    m_index.next_id = scene.next_id();
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
        saved.error = "could not write " + path_text(m_folder / "world.json");
        return saved;
    }
    saved.cells = static_cast<u32>(m_cells.size());
    return saved;
}

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
    for (const auto& [coord, cell] : m_cells) {
        unsaved += cell.state == State::loaded && !members.contains(coord) ? 1u : 0u; // emptied
    }
    return unsaved;
}

StreamingStats WorldStreaming::stats() const {
    StreamingStats s = m_totals;
    s.cells = static_cast<u32>(m_cells.size());
    for (const auto& [coord, cell] : m_cells) {
        s.loaded += cell.state == State::loaded ? 1u : 0u;
        s.reading += cell.state == State::reading ? 1u : 0u;
        s.waiting += cell.state == State::ready ? 1u : 0u;
        s.failed += cell.state == State::failed ? 1u : 0u;
    }
    return s;
}

} // namespace giant::editor
