// Game saves in the editor: gathered at once, written and read on a job,
// applied over frames.
#include "game_saves.h"

#include "physics_state.h"
#include "play_state.h"
#include "weather_state.h"
#include "world_streaming.h"

#include "giant/ai/json.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace giant::editor {

namespace fs = std::filesystem;
using ai::Json;

namespace {

save::CellKey key_of(world::CellCoord c) { return {c.x, c.z}; }

std::string camera_text(const EditorCamera& c, bool playing) {
    const Json j = {{"position", {c.position.x, c.position.y, c.position.z}},
                    {"yaw_degrees", c.yaw_degrees},
                    {"pitch_degrees", c.pitch_degrees},
                    {"vertical_fov_degrees", c.vertical_fov_degrees},
                    {"playing", playing}};
    return j.dump(1) + "\n";
}

std::string weather_text(const std::optional<world::Weather>& shown, const world::Ground& ground) {
    Json j = Json::object();
    j["weather"] = shown ? Json::parse(world::save_weather(*shown)) : Json(nullptr);
    j["wetness"] = ground.wetness;
    j["snow_cover"] = ground.snow_cover;
    return j.dump(1) + "\n";
}

bool finite_in(const Json& j, double lo, double hi, double& out) {
    if (!j.is_number() || j.is_boolean()) return false;
    out = j.get<double>();
    return std::isfinite(out) && out >= lo && out <= hi;
}

} // namespace

// What a save or load gathered (the main thread) and its job's result.
struct GameSaves::Sections {
    std::optional<EditorCamera> camera;
    bool playing = false;
    bool has_weather = false; // the section was there
    std::optional<world::Weather> weather;
    world::Ground ground;
};

struct GameSaves::Pending {
    bool load = false;
    bool remove = false; // a delete's
    std::string slot;
    fs::path path;
    std::string error;
    u64 generation = 0, session = 0; // the scene's and the world's when it began
    // A save's.
    save::Committed committed;
    std::shared_ptr<const std::map<save::CellKey, GameCell>> cells;
    // A load's.
    save::Manifest manifest;
    std::vector<std::pair<world::CellCoord, GameCell>> loaded;
    Sections sections;
};

// What a save's records depend on of the world: its kits (pieces) and scatter (tree points) and
// its ground's samples; not its lighting, weather, cells or next id, which every edit moves.
u64 GameSaves::world_identity(const world::WorldIndex& index) {
    world::WorldIndex kept;
    kept.kits = index.kits;
    kept.scatter = index.scatter;
    kept.terrain_samples = index.terrain_samples;
    return save::hash_text(world::save_world_index(kept));
}

GameSaves::GameSaves() = default;

GameSaves::~GameSaves() { m_jobs.wait(m_group); }

fs::path GameSaves::root(const EditorState& state) {
    if (state.world == nullptr || !state.world->active(state.scene_generation)) return {};
    return state.world->folder() / "saves";
}

bool GameSaves::busy() const { return m_in_flight || !m_applying.empty() || m_sections != nullptr; }

void GameSaves::wait() { m_jobs.wait(m_group); }

std::string GameSaves::begin_save(EditorState& state, const std::string& slot) {
    follow(state);
    if (busy()) return "a save or load is under way";
    if (!save::valid_name(slot)) return "a slot's name is 1 to 64 of lowercase letters, digits, '-' and '_'";
    const fs::path root_path = root(state);
    if (root_path.empty()) return "no world open (game saves are kept beside a world: open_world)";
    // What to keep, gathered now (copies only).
    world::Damage shown = state.physics != nullptr ? state.physics->game_damage_now(state.scene) : state.scene.game_damage();
    auto cells = std::make_shared<std::map<save::CellKey, GameCell>>();
    for (auto& [c, d] : shown) (*cells)[key_of(c)].damage = std::move(d);
    // The ground a game shows dented (its own and a simulation's), and a loaded game's still waiting for its tile.
    if (state.physics != nullptr) {
        for (auto& [c, h] : state.physics->ground_now(state.scene)) (*cells)[key_of(c)].ground = std::move(h);
    }
    for (const auto& [k, h] : m_ground_waiting) {
        GameCell& cell = (*cells)[k];
        if (!cell.ground) cell.ground = h;
    }
    const bool playing = state.play != nullptr && state.play->playing();
    std::map<std::string, std::string> sections;
    sections["camera"] = camera_text(state.camera, playing);
    // The weather it is going to (a blend under way saved as where it ends), and the ground now.
    const std::optional<world::Weather>& target = weather() != nullptr ? *weather() : state.scene.weather;
    sections["weather"] = weather_text(target, state.weather != nullptr ? state.weather->ground() : world::Ground{});
    save::Meta meta;
    meta.world_name = state.world->folder().filename().string();
    meta.world_hash = world_identity(state.world->index());
    meta.saved_unix = static_cast<i64>(std::time(nullptr));
    meta.play_seconds = m_play_seconds;
    // Against the base when saving to the slot it is of (only what changed), else whole.
    const fs::path path = root_path / slot;
    const bool same = m_base.generation != 0 && path == m_base_path && m_session == state.world->session();
    const save::Manifest base = same ? m_base : save::Manifest{};
    std::shared_ptr<const std::map<save::CellKey, GameCell>> was =
        same && m_base_cells != nullptr ? m_base_cells : std::make_shared<const std::map<save::CellKey, GameCell>>();
    const u64 generation = state.scene_generation, session = state.world->session();

    m_in_flight = true;
    m_status.doing = "saving";
    m_status.slot = slot;
    m_status.error.clear();
    m_jobs.submit(m_group, [this, slot, path, base, meta, generation, session, sections = std::move(sections),
                            cells = std::shared_ptr<const std::map<save::CellKey, GameCell>>(std::move(cells)),
                            was = std::move(was)]() mutable {
        auto p = std::make_unique<Pending>();
        p->slot = slot;
        p->path = path;
        p->generation = generation;
        p->session = session;
        save::Changes changes;
        changes.sections = std::move(sections);
        for (const auto& [k, d] : *cells) {
            const auto it = was->find(k);
            if (it == was->end() || !(it->second == d))
                changes.cells[k] = world::save_cell_damage(d.damage, d.ground ? &*d.ground : nullptr);
        }
        for (const auto& [k, d] : *was) {
            if (cells->count(k) == 0) changes.cells[k] = std::nullopt;
        }
        p->committed = save::commit(path, base, changes, meta);
        p->error = p->committed.error;
        p->cells = std::move(cells);
        std::lock_guard lock(m_mutex);
        m_done = std::move(p);
    });
    return {};
}

std::string GameSaves::begin_load(EditorState& state, const std::string& slot, bool force) {
    follow(state);
    if (busy()) return "a save or load is under way";
    if (!save::valid_name(slot)) return "a slot's name is 1 to 64 of lowercase letters, digits, '-' and '_'";
    const fs::path root_path = root(state);
    if (root_path.empty()) return "no world open (game saves are kept beside a world: open_world)";
    const fs::path path = root_path / slot;
    const std::string world_name = state.world->folder().filename().string();
    const u64 world_hash = world_identity(state.world->index());
    const u64 generation = state.scene_generation, session = state.world->session();
    m_in_flight = true;
    m_status.doing = "loading";
    m_status.slot = slot;
    m_status.error.clear();
    m_jobs.submit(m_group, [this, slot, path, force, world_name, world_hash, generation, session] {
        auto p = std::make_unique<Pending>();
        p->load = true;
        p->slot = slot;
        p->path = path;
        p->generation = generation;
        p->session = session;
        const auto fail = [&](std::string why) {
            p->error = std::move(why);
            std::lock_guard lock(m_mutex);
            m_done = std::move(p);
        };
        const save::Opened opened = save::open_slot(path);
        if (!opened.error.empty()) return fail(opened.error);
        const save::Manifest& m = opened.manifest;
        if (!force && (m.world_hash != world_hash || m.world_name != world_name))
            return fail("the save is of another world or another version of it ('" + m.world_name +
                        "'): force loads it anyway");
        for (const auto& [name, file] : m.sections) {
            const std::optional<std::string> text = save::read_chunk(path, file);
            if (!text) return fail("cannot read " + file);
            Json j = Json::parse(*text, nullptr, false);
            if (!j.is_object()) return fail(file + " is not a JSON object");
            if (name == "camera") {
                EditorCamera c;
                double v[6];
                const Json* pos = j.contains("position") ? &j["position"] : nullptr;
                if (pos == nullptr || !pos->is_array() || pos->size() != 3 || !finite_in((*pos)[0], -1e8, 1e8, v[0]) ||
                    !finite_in((*pos)[1], -1e8, 1e8, v[1]) || !finite_in((*pos)[2], -1e8, 1e8, v[2]) ||
                    !finite_in(j.value("yaw_degrees", Json(0.0)), -1e6, 1e6, v[3]) ||
                    !finite_in(j.value("pitch_degrees", Json(0.0)), -89.0, 89.0, v[4]) ||
                    !finite_in(j.value("vertical_fov_degrees", Json(60.0)), 1.0, 170.0, v[5]) ||
                    !j.value("playing", Json(false)).is_boolean())
                    return fail(file + ": a bad camera");
                c.position = {v[0], v[1], v[2]};
                c.yaw_degrees = static_cast<float>(v[3]);
                c.pitch_degrees = static_cast<float>(v[4]);
                c.vertical_fov_degrees = static_cast<float>(v[5]);
                p->sections.camera = c;
                p->sections.playing = j.value("playing", false);
            } else if (name == "weather") {
                double wet = 0.0, snow = 0.0;
                if (!finite_in(j.value("wetness", Json(0.0)), 0.0, 1.0, wet) ||
                    !finite_in(j.value("snow_cover", Json(0.0)), 0.0, 1.0, snow))
                    return fail(file + ": a bad ground");
                p->sections.ground = {static_cast<float>(wet), static_cast<float>(snow)};
                p->sections.has_weather = true;
                if (j.contains("weather") && !j["weather"].is_null()) {
                    world::Weather w;
                    if (const auto e = world::load_weather(j["weather"].dump(), w)) return fail(file + ": " + e->message);
                    p->sections.weather = w;
                }
            } // (another engine's sections: left)
        }
        auto cells = std::make_shared<std::map<save::CellKey, GameCell>>();
        p->loaded.reserve(m.cells.size());
        for (const auto& [k, file] : m.cells) {
            const std::optional<std::string> text = save::read_chunk(path, file);
            if (!text) return fail("cannot read " + file);
            GameCell d;
            if (const auto e = world::load_cell_damage(*text, {k.x, k.z}, d.damage, &d.ground))
                return fail(file + ": " + e->message);
            cells->emplace(k, d);
            p->loaded.emplace_back(world::CellCoord{k.x, k.z}, std::move(d));
        }
        p->manifest = m;
        p->cells = std::move(cells);
        std::lock_guard lock(m_mutex);
        m_done = std::move(p);
    });
    return {};
}

void GameSaves::forget_base() {
    m_base = {};
    m_base_cells.reset();
    m_base_path.clear();
    m_status.base_slot.clear();
}

void GameSaves::clear_ground(EditorState& state) {
    if (state.physics != nullptr) state.physics->clear_game_ground(state.scene);
    m_ground_waiting.clear();
    m_waiting_from.reset();
}

void GameSaves::close(EditorState& state) {
    state.scene.clear_game_damage();
    clear_ground(state);
    drop_weather();
    m_applying.clear();
    m_sections.reset();
    m_generation = 0;
    forget_base();
    m_play_seconds = 0.0;
}

void GameSaves::drop_weather() {
    m_weather_on = false;
    m_weather.reset();
    m_scene_weather.reset();
}

std::string GameSaves::begin_delete(EditorState& state, const std::string& slot) {
    follow(state);
    if (busy()) return "a save or load is under way";
    if (!save::valid_name(slot)) return "a slot's name is 1 to 64 of lowercase letters, digits, '-' and '_'";
    const fs::path root_path = root(state);
    if (root_path.empty()) return "no world open";
    if (!fs::is_directory(root_path / slot)) return "no save '" + slot + "'";
    if (root_path / slot == m_base_path) forget_base(); // the next save there is whole
    m_in_flight = true;
    m_status.doing = "deleting";
    m_status.slot = slot;
    m_status.error.clear();
    m_jobs.submit(m_group, [this, slot, root_path] {
        auto p = std::make_unique<Pending>();
        p->remove = true;
        p->slot = slot;
        if (!save::delete_slot(root_path, slot)) p->error = "cannot delete '" + slot + "'";
        std::lock_guard lock(m_mutex);
        m_done = std::move(p);
    });
    return {};
}

void GameSaves::take(EditorState& state) {
    std::unique_ptr<Pending> p;
    {
        std::lock_guard lock(m_mutex);
        p = std::move(m_done);
    }
    if (p == nullptr) return;
    m_in_flight = false;
    m_status.doing.clear();
    m_status.slot = p->slot;
    m_status.error = p->error;
    if (p->remove) return;
    // Begun for this scene and this world session (else it is not this game's).
    const bool same = state.world != nullptr && state.world->active(state.scene_generation) &&
                      p->generation == state.scene_generation && p->session == state.world->session();
    if (!p->load) {
        if (!p->error.empty() || !same) {
            forget_base(); // (what is on the disk is not known now: the next save is whole)
            if (p->error.empty()) m_status.error = "the world was closed or replaced while saving";
            else if (!same) m_status.error += " (and the world was closed or replaced)";
            return;
        }
        ++m_status.saves;
        m_status.written = p->committed.written;
        m_status.removed = p->committed.removed;
        m_status.cells = p->cells->size();
        m_status.tiles = static_cast<usize>(
            std::count_if(p->cells->begin(), p->cells->end(), [](const auto& c) { return c.second.ground.has_value(); }));
        m_base = p->committed.manifest;
        m_base_cells = std::move(p->cells);
        m_base_path = p->path;
        m_status.base_slot = p->slot;
        m_session = p->session;
        m_generation = p->generation;
        return;
    }
    if (!p->error.empty() || !same) {
        if (p->error.empty()) m_status.error = "the world was closed or replaced while loading";
        return;
    }
    // What play and simulating broke since goes; the save's shows as the game's.
    if (state.play != nullptr && state.play->playing()) state.play->stop(state.camera);
    if (state.physics != nullptr) {
        state.physics->set_simulating(false);
        state.physics->restore_ground(state.scene);
    }
    state.scene.clear_game_damage();
    clear_ground(state);
    drop_weather();
    m_applying = std::move(p->loaded);
    m_sections = std::make_unique<Sections>(std::move(p->sections));
    m_status.doing = "applying";
    m_status.cells = m_applying.size();
    m_status.applied = 0;
    m_status.tiles = static_cast<usize>(
        std::count_if(m_applying.begin(), m_applying.end(), [](const auto& c) { return c.second.ground.has_value(); }));
    m_status.skipped = 0;
    m_base = p->manifest;
    m_base_cells = std::move(p->cells);
    m_base_path = p->path;
    m_status.base_slot = p->slot;
    m_session = p->session;
    m_generation = p->generation;
    m_play_seconds = p->manifest.play_seconds;
    m_play_generation = p->generation;
    m_play_session = p->session;
    ++m_status.loads;
}

void GameSaves::finish_load(EditorState& state) {
    const Sections s = std::move(*m_sections);
    m_sections.reset();
    m_status.doing.clear();
    if (s.camera) {
        // Where it was and where it looked (the fly speed stays the viewer's).
        state.camera.position = s.camera->position;
        state.camera.yaw_degrees = s.camera->yaw_degrees;
        state.camera.pitch_degrees = s.camera->pitch_degrees;
        state.camera.vertical_fov_degrees = s.camera->vertical_fov_degrees;
    }
    if (s.has_weather) {
        m_weather_on = true;
        m_weather = s.weather;
        m_scene_weather = state.scene.weather;
        if (state.weather != nullptr) {
            state.weather->update(m_weather, 0.0, state.camera.position, state.scene_generation);
            state.weather->set_ground(s.ground);
        }
    }
    if (s.playing && state.play != nullptr && state.physics != nullptr) {
        if (!state.play->start(state.scene, state.camera, state.scene_generation))
            m_status.error = "loaded, but the player could not be put there";
    }
}

void GameSaves::update(EditorState& state, double seconds) {
    follow(state);
    take(state);
    // The scene's weather changed: it shows again.
    if (m_weather_on && !(state.scene.weather == m_scene_weather)) drop_weather();
    usize tiles = std::max<usize>(ground_tiles_per_frame, 1);
    // A share of the cells (each cell's ground first: what lies on it settles on it), the ground's tiles fewer.
    const usize cells = std::max<usize>(cells_per_frame, 1);
    for (usize n = 0; n < cells && !m_applying.empty(); ++n) {
        auto& [c, d] = m_applying.back();
        if (d.ground) {
            if (tiles == 0) break;
            --tiles;
            apply_ground(state, c, std::move(*d.ground));
        }
        if (!d.damage.empty()) state.scene.set_game_damage(c, std::move(d.damage));
        m_applying.pop_back();
        ++m_status.applied;
    }
    press_waiting(state, tiles);
    if (m_applying.empty() && m_sections != nullptr) finish_load(state);
    if (state.play != nullptr && state.play->playing() && std::isfinite(seconds) && seconds > 0.0)
        m_play_seconds = std::min(m_play_seconds + seconds, save::k_max_play_seconds);
}

void GameSaves::apply_ground(EditorState& state, world::CellCoord cell, world::Terrain::Heights heights) {
    const usize n = state.scene.terrain.samples();
    if (state.physics == nullptr || heights.size() != n * n) {
        ++m_status.skipped; // (a forced load of a world with other samples)
        return;
    }
    if (state.scene.terrain.find(cell) == nullptr) {
        m_ground_waiting[key_of(cell)] = std::move(heights);
        return;
    }
    if (!state.physics->press_game_ground(state.scene, cell, std::move(heights))) ++m_status.skipped;
}

void GameSaves::press_waiting(EditorState& state, usize& budget) {
    if (m_ground_waiting.empty() || budget == 0 || state.physics == nullptr) return;
    // A share looked at each frame, from where the last look ended (round and round).
    constexpr usize k_looks = 256;
    auto it = m_waiting_from ? m_ground_waiting.lower_bound(*m_waiting_from) : m_ground_waiting.begin();
    for (usize looked = 0; looked < k_looks && budget > 0 && !m_ground_waiting.empty(); ++looked) {
        if (it == m_ground_waiting.end()) it = m_ground_waiting.begin();
        const world::CellCoord c{it->first.x, it->first.z};
        if (state.scene.terrain.find(c) == nullptr) {
            ++it;
            continue;
        }
        --budget;
        if (!state.physics->press_game_ground(state.scene, c, std::move(it->second))) ++m_status.skipped;
        it = m_ground_waiting.erase(it);
    }
    // (Past the last: the next look starts at the first. No key stands for that: every key is a cell.)
    m_waiting_from = it != m_ground_waiting.end() ? std::optional<save::CellKey>(it->first) : std::nullopt;
}

void GameSaves::follow(EditorState& state) {
    // Another scene or world: the game is not its (checked before a result is taken, which checks its own).
    const u64 session = state.world != nullptr ? state.world->session() : 0;
    if (m_generation != 0 && (m_generation != state.scene_generation || session != m_session)) {
        m_applying.clear();
        m_sections.reset();
        drop_weather();
        clear_ground(state);
        forget_base();
        if (m_status.doing == "applying") m_status.doing.clear();
        m_generation = 0;
    }
    if (m_play_generation != state.scene_generation || m_play_session != session) {
        m_play_seconds = 0.0;
        m_play_generation = state.scene_generation;
        m_play_session = session;
    }
}

} // namespace giant::editor
