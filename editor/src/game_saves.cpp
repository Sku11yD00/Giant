// Game saves in the editor: gathered at once, written and read on a job,
// applied over frames.
#include "game_saves.h"

#include "people_state.h"

#include "physics_state.h"
#include "play_state.h"
#include "ragdoll_state.h"
#include "walker_state.h"
#include "clock_state.h"
#include "weather_state.h"
#include "world_streaming.h"

#include "giant/ai/json.h"
#include "giant/world/people.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <set>

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

constexpr usize k_max_saved_poses = 65536;
constexpr usize k_max_reason = 256;

// A save's people as text (made on the job: there may be tens of thousands).
std::string people_text(const std::vector<GameSaves::SavedPerson>& people) {
    Json list = Json::array();
    for (const GameSaves::SavedPerson& s : people) {
        Json j = {{"id", s.id}};
        if (s.moved) j["at"] = Json::array({s.at.position.x, s.at.position.y, s.at.position.z, s.at.yaw});
        if (s.trip) {
            Json trip = {{"to", Json::array({s.to.x, s.to.y, s.to.z})}, {"speed", s.speed}, {"within", s.within}};
            if (!s.place.empty()) trip["place"] = s.place;
            j["trip"] = std::move(trip);
        }
        if (s.did && s.kept.said != 0) {
            Json did = {{"said", s.kept.said}, {"entry", s.kept.entry}};
            if (!s.kept.place.empty()) did["place"] = s.kept.place;
            if (s.kept.failed) did["failed"] = true;
            if (s.kept.own_trip) did["own_trip"] = true;
            if (s.kept.own) did["own"] = true;
            j["did"] = std::move(did);
        }
        if (s.did && !s.kept.behaviour.empty()) {
            Json in = {{"behaviour", s.kept.behaviour}, {"state", s.kept.state}, {"seconds", s.kept.in_state}};
            if (s.kept.settled) in["settled"] = true;
            j["in"] = std::move(in);
        }
        list.push_back(std::move(j));
    }
    return Json{{"people", std::move(list)}}.dump(1) + "\n";
}
constexpr float k_max_bone_speed = 1000.0f; // m/s and rad/s, each part (what the physics takes back)
constexpr double k_max_lying = 1e9;         // seconds
constexpr u32 k_max_joint = 65535;

// As the physics takes a bone's speed back (PhysicsWorld::place_ragdoll).
bool slow_enough(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && length(v) <= k_max_bone_speed;
}

bool sound_scale(const Vec3& v) {
    return std::fabs(v.x) >= 1e-3f && std::fabs(v.y) >= 1e-3f && std::fabs(v.z) >= 1e-3f && std::fabs(v.x) <= 1e3f &&
           std::fabs(v.y) <= 1e3f && std::fabs(v.z) <= 1e3f;
}

std::optional<WalkerState::Phase> phase_of(const Json& j) {
    if (!j.is_string()) return std::nullopt;
    for (const WalkerState::Phase p : {WalkerState::Phase::searching, WalkerState::Phase::walking,
                                       WalkerState::Phase::arrived, WalkerState::Phase::failed}) {
        if (j.get<std::string>() == WalkerState::to_string(p)) return p;
    }
    return std::nullopt;
}

Json vec_json(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }
Json quat_json(const Quat& q) { return Json::array({q.x, q.y, q.z, q.w}); }

bool read_vec(const Json& j, DVec3& out) {
    double v[3];
    if (!j.is_array() || j.size() != 3) return false;
    for (usize i = 0; i < 3; ++i)
        if (!finite_in(j[i], -1e8, 1e8, v[i])) return false;
    out = {v[0], v[1], v[2]};
    return true;
}

bool read_quat(const Json& j, Quat& out) {
    double v[4];
    if (!j.is_array() || j.size() != 4) return false;
    for (usize i = 0; i < 4; ++i)
        if (!finite_in(j[i], -1.01, 1.01, v[i])) return false;
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (std::fabs(len - 1.0) > 1e-3) return false;
    out = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])};
    return true;
}

bool read_vec3f(const Json& j, double limit, Vec3& out) {
    double v[3];
    if (!j.is_array() || j.size() != 3) return false;
    for (usize i = 0; i < 3; ++i)
        if (!finite_in(j[i], -limit, limit, v[i])) return false;
    out = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
    return true;
}

Json camera_json(const EditorCamera& c) {
    return {{"position", {c.position.x, c.position.y, c.position.z}},
            {"yaw_degrees", c.yaw_degrees},
            {"pitch_degrees", c.pitch_degrees},
            {"vertical_fov_degrees", c.vertical_fov_degrees}};
}

bool read_camera(const Json& j, EditorCamera& c) {
    double v[3];
    DVec3 p;
    if (!j.is_object() || !j.contains("position") || !read_vec(j["position"], p) ||
        !finite_in(j.value("yaw_degrees", Json(0.0)), -1e6, 1e6, v[0]) ||
        !finite_in(j.value("pitch_degrees", Json(0.0)), -89.0, 89.0, v[1]) ||
        !finite_in(j.value("vertical_fov_degrees", Json(60.0)), 1.0, 170.0, v[2]))
        return false;
    c.position = p;
    c.yaw_degrees = static_cast<float>(v[0]);
    c.pitch_degrees = static_cast<float>(v[1]);
    c.vertical_fov_degrees = static_cast<float>(v[2]);
    return true;
}

// Only the keys named (a typo must not silently drop data).
bool only_keys(const Json& j, std::initializer_list<const char*> keys) {
    for (const auto& [k, v] : j.items()) {
        (void)v;
        if (std::none_of(keys.begin(), keys.end(), [&](const char* n) { return k == n; })) return false;
    }
    return true;
}

} // namespace

// What a save or load gathered (the main thread) and its job's result.
struct GameSaves::Sections {
    std::optional<EditorCamera> camera;
    bool playing = false;
    bool has_weather = false; // the section was there
    std::optional<world::Weather> weather;
    world::Ground ground;
    std::optional<SavedPlayer> player;
    std::optional<std::pair<double, i32>> time; // the clock's hours and day
    bool has_bodies = false;
    bool simulating = false;
    std::vector<SavedPose> poses;
    std::vector<SavedWalker> walkers;
    std::vector<SavedRagdoll> ragdolls;
    std::vector<SavedPerson> people;
};

struct GameSaves::Pending {
    bool load = false;
    bool autosave = false;
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
// The kits its mods laid over it are of it too (after its own: a placement builds a mod's pieces,
// so a save made with a mod's kit is not one of the world without it).
u64 GameSaves::world_identity(const WorldStreaming& world) {
    const world::WorldIndex& index = world.index();
    world::WorldIndex kept;
    kept.kits = index.kits;
    kept.kits.insert(kept.kits.end(), world.mods_laid().kits.begin(), world.mods_laid().kits.end());
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

GameSaves::Base& GameSaves::base_for(const fs::path& slot_path) {
    return slot_path.filename() == k_autosave_slot ? m_auto_base : m_base;
}

std::string GameSaves::begin_save(EditorState& state, const std::string& slot) { return save_to(state, slot, false); }

std::string GameSaves::save_to(EditorState& state, const std::string& slot, bool autosave) {
    follow(state);
    if (busy()) return "a save or load is under way";
    if (!save::valid_name(slot)) return "a slot's name is 1 to 64 of lowercase letters, digits, '-' and '_'";
    const fs::path root_path = root(state);
    if (root_path.empty()) return "no world open (game saves are kept beside a world: open_world)";
    const auto gather_from = std::chrono::steady_clock::now();
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
    // The time of day play reached.
    if (state.clock != nullptr) {
        const world::Clock& c = state.clock->shown();
        sections["clock"] = Json{{"hours", c.hours}, {"day", c.day}}.dump(1) + "\n";
    }
    // The player as it is (put down: else a load puts it under the camera again, as play starting does).
    if (playing && state.play->placing() && state.play->pending_start()) {
        // Still being put down where a load had it: kept as that load had it.
        const SavedPlayer& at = *state.play->pending_start();
        Json p = {{"feet", vec_json(at.feet)},
                  {"velocity", Json::array({at.velocity.x, at.velocity.y, at.velocity.z})},
                  {"climbing", at.climbing ? Json::array({at.climbing->x, at.climbing->y, at.climbing->z}) : Json(nullptr)},
                  {"carried_by", at.carried_by ? Json(*at.carried_by) : Json(nullptr)},
                  {"returns_to", camera_json(at.returns_to)},
                  {"was_simulating", at.was_simulating}};
        sections["player"] = p.dump(1) + "\n";
    } else if (playing && !state.play->placing()) {
        if (const std::optional<sim::CharacterState> c = state.play->player()) {
            Json p = {{"feet", vec_json(c->position)},
                      {"velocity", Json::array({c->velocity.x, c->velocity.y, c->velocity.z})},
                      {"climbing", c->climbing && !c->mantling
                                       ? Json::array({c->wall_normal.x, c->wall_normal.y, c->wall_normal.z})
                                       : Json(nullptr)},
                      {"carried_by", Json(nullptr)},
                      {"returns_to", camera_json(state.play->returns_to())},
                      {"was_simulating", state.play->was_simulating()}};
            if (const std::optional<world::EntityId> by = state.play->carried_by()) p["carried_by"] = *by;
            const bool finite = std::isfinite(c->position.x) && std::isfinite(c->position.y) &&
                                std::isfinite(c->position.z) && std::fabs(c->position.y) <= 1e8 &&
                                std::fabs(c->position.x) <= 1e8 && std::fabs(c->position.z) <= 1e8 &&
                                std::isfinite(c->velocity.x) && std::isfinite(c->velocity.y) &&
                                std::isfinite(c->velocity.z);
            if (finite) sections["player"] = p.dump(1) + "\n";
        }
    }
    // Whether simulating, and the dynamic entities it moved (each with where the scene has it: one
    // moved in the world since is not this save's), those of a load not put back yet too.
    usize posed = 0;
    {
        Json poses = Json::array();
        const bool simulating = state.physics != nullptr && state.physics->simulating();
        const auto add = [&](world::EntityId id, const DVec3& p, const Quat& r, const DVec3& of_p, const Quat& of_r) {
            if (poses.size() >= k_max_saved_poses) return;
            // Only what a load reads back (a body fallen out of the world is not kept: it would refuse the save).
            const auto in = [](const DVec3& v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= 1e8 &&
                       std::fabs(v.y) <= 1e8 && std::fabs(v.z) <= 1e8;
            };
            const auto unit = [](const Quat& q) {
                return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
            };
            if (!in(p) || !in(of_p) || !unit(r) || !unit(of_r)) return;
            poses.push_back({{"entity", id},
                             {"position", vec_json(p)},
                             {"rotation", quat_json(r)},
                             {"of", {{"position", vec_json(of_p)}, {"rotation", quat_json(of_r)}}}});
        };
        if (simulating) {
            std::vector<world::EntityId> ids;
            for (const auto& [id, pose] : state.physics->bodies().poses()) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            for (const world::EntityId id : ids) {
                const world::Transform* placed = state.physics->bodies().placed(id);
                const world::Entity* e = state.scene.find(id);
                if (placed == nullptr || e == nullptr || !(e->transform == *placed)) continue; // stale
                const world::Transform& pose = state.physics->bodies().poses().at(id);
                add(id, pose.position, pose.rotation, placed->position, placed->rotation);
            }
            for (const auto& [id, p] : m_poses_waiting) {
                if (state.physics->bodies().poses().count(id) == 0) add(id, p.position, p.rotation, p.of_position, p.of_rotation);
            }
        }
        posed = poses.size();
        sections["bodies"] = Json{{"simulating", simulating}, {"poses", std::move(poses)}}.dump(1) + "\n";
    }
    // The walkers (only while simulating: they walk with it), those of a load not walking again yet too.
    usize walking = 0;
    {
        Json walkers = Json::array();
        const auto in = [](const DVec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= 1e8 &&
                   std::fabs(v.y) <= 1e8 && std::fabs(v.z) <= 1e8;
        };
        const auto add = [&](const SavedWalker& w) {
            const WalkerState::Resume& r = w.resume;
            const Vec3& v = r.velocity;
            const Quat& q = w.of_rotation;
            const double len = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
                                         static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w);
            // Only what a load reads back (one fallen out of the world is not kept: it would refuse the save).
            if (walkers.size() >= WalkerState::k_max_walkers || !in(r.feet) || !in(r.target) || !in(w.of_position) ||
                !(std::fabs(r.yaw) <= 10.0f) || !(std::fabs(v.x) <= 1e4f) || !(std::fabs(v.y) <= 1e4f) ||
                !(std::fabs(v.z) <= 1e4f) || !(r.speed > 0.0f && r.speed <= 20.0f) || !(std::fabs(len - 1.0) <= 1e-3))
                return;
            walkers.push_back({{"entity", w.entity},
                               {"feet", vec_json(r.feet)},
                               {"yaw", r.yaw},
                               {"velocity", Json::array({v.x, v.y, v.z})},
                               {"target", vec_json(r.target)},
                               {"speed", r.speed},
                               {"phase", WalkerState::to_string(r.phase)},
                               {"reason", r.reason.substr(0, k_max_reason)},
                               {"of", {{"position", vec_json(w.of_position)}, {"rotation", quat_json(w.of_rotation)}}}});
        };
        if (state.physics != nullptr && state.physics->simulating() && state.walkers != nullptr) {
            std::vector<world::EntityId> ids;
            // (Of a run stopped since, not this game's: let go on the next step.)
            if (state.walkers->current(state.scene))
                for (const auto& [id, w] : state.walkers->walkers()) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            for (const world::EntityId id : ids) {
                const WalkerState::Walker& w = state.walkers->walkers().at(id);
                const world::Entity* e = state.scene.find(id);
                const std::optional<sim::CharacterState> c = state.physics->world().character_state(w.character);
                if (e == nullptr || !c) continue; // (gone: let go, or its character made again, on the next step)
                if (e->person != 0) continue; // (a person's entity walking its trip: neither is the save's)
                SavedWalker saved;
                saved.entity = id;
                saved.resume = {c->position, w.yaw, c->velocity, w.target, w.settings.speed, w.phase, w.reason};
                saved.of_position = e->transform.position;
                saved.of_rotation = e->transform.rotation;
                add(saved);
            }
            if (state.physics->runs() == m_walkers_run) {
                for (const auto& [id, w] : m_walkers_waiting) {
                    if (!std::binary_search(ids.begin(), ids.end(), id)) add(w);
                }
            }
        }
        walking = walkers.size();
        sections["walkers"] = Json{{"walkers", std::move(walkers)}}.dump(1) + "\n";
    }
    // The ragdolls (only while simulating: they lie in it), those of a load not lying again yet too.
    usize lying = 0;
    {
        Json ragdolls = Json::array();
        const auto in = [](const DVec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= 1e8 &&
                   std::fabs(v.y) <= 1e8 && std::fabs(v.z) <= 1e8;
        };
        const auto unit = [](const Quat& q) {
            const double len = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
                                         static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w);
            return std::fabs(len - 1.0) <= 1e-3;
        };
        const auto slow = [](const Vec3& v) { return slow_enough(v); };
        const auto sized = [](const Vec3& v) { return sound_scale(v); };
        const auto add = [&](const SavedRagdoll& g) {
            const RagdollState::Resume& r = g.resume;
            // Only what a load reads back (one fallen out of the world is not kept: it would refuse the save).
            if (ragdolls.size() >= RagdollState::k_max_ragdolls || r.bones.empty() || r.bones.size() != r.joints.size() ||
                r.bones.size() > sim::k_max_ragdoll_bones || !in(g.of_position) || !unit(g.of_rotation) || !sized(g.of_scale) ||
                !(r.lying >= 0.0 && r.lying <= k_max_lying))
                return;
            Json bones = Json::array();
            bool awake = false;
            for (usize b = 0; b < r.bones.size(); ++b) {
                const sim::BodyState& s = r.bones[b];
                Quat q = s.rotation;
                if (!unit(q)) q = normalize(q);
                if (r.joints[b] > k_max_joint || !in(s.position) || !unit(q) || !slow(s.linear_velocity) || !slow(s.angular_velocity))
                    return;
                awake = awake || s.active;
            }
            // Each bone a row of numbers (a field of the fallen is many): its joint, where, its turn, and for
            // one not asleep how it moves and spins.
            for (usize b = 0; b < r.bones.size(); ++b) {
                const sim::BodyState& s = r.bones[b];
                const Quat q = unit(s.rotation) ? s.rotation : normalize(s.rotation);
                Json row = Json::array({r.joints[b], s.position.x, s.position.y, s.position.z, q.x, q.y, q.z, q.w});
                if (awake) {
                    for (const float v : {s.linear_velocity.x, s.linear_velocity.y, s.linear_velocity.z, s.angular_velocity.x,
                                          s.angular_velocity.y, s.angular_velocity.z})
                        row.push_back(v);
                }
                bones.push_back(std::move(row));
            }
            ragdolls.push_back({{"entity", g.entity},
                                {"asleep", !awake},
                                {"lying", r.lying},
                                {"bones", std::move(bones)},
                                {"of",
                                 {{"position", vec_json(g.of_position)},
                                  {"rotation", quat_json(g.of_rotation)},
                                  {"scale", Json::array({g.of_scale.x, g.of_scale.y, g.of_scale.z})}}}});
        };
        if (state.physics != nullptr && state.physics->simulating() && state.ragdolls != nullptr) {
            std::vector<world::EntityId> ids;
            // (Of a run stopped since, not this game's: let go on the next step.)
            if (state.ragdolls->current(state.scene))
                for (const auto& [id, r] : state.ragdolls->ragdolls()) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            for (const world::EntityId id : ids) {
                const world::Entity* e = state.scene.find(id);
                std::optional<RagdollState::Resume> r = state.ragdolls->lies(state.scene, id);
                if (e == nullptr || !r) continue; // (gone: let go on the next step)
                if (e->person != 0) continue; // (a person's entity struck down: made again standing, not the save's)
                SavedRagdoll saved;
                saved.entity = id;
                saved.resume = std::move(*r);
                saved.of_position = e->transform.position;
                saved.of_rotation = e->transform.rotation;
                saved.of_scale = e->transform.scale;
                add(saved);
            }
            if (state.physics->runs() == m_ragdolls_run) {
                for (const auto& [id, g] : m_ragdolls_waiting) {
                    if (!std::binary_search(ids.begin(), ids.end(), id)) add(g);
                }
            }
        }
        lying = ragdolls.size();
        sections["ragdolls"] = Json{{"ragdolls", std::move(ragdolls)}}.dump(1) + "\n";
    }
    // The people (only while simulating: they travel by its clock): those not where the scene has
    // them, on a trip, or following a routine; those of a load not put back yet too.
    std::vector<SavedPerson> people;
    if (state.physics != nullptr && state.physics->simulating() && state.travel != nullptr) {
        const PeopleTravel::Clock clock = PeopleTravel::clock_of(*state.physics);
        state.travel->sync(state.scene, clock.run, clock.seconds);
        const auto in = [](const DVec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= 1e8 &&
                   std::fabs(v.y) <= 1e8 && std::fabs(v.z) <= 1e8;
        };
        // (Those waiting are in the order of their ids, as the scene's people are: taken as they come.)
        usize waiting = state.physics->runs() == m_people_run ? m_people_next : m_people_waiting.size();
        const auto& moved = state.travel->moved();
        if (state.routines != nullptr) state.routines->ready(state.scene, *state.travel);
        // (Those in a later state of their behaviour are written too: at most those behaving.)
        const usize behaving = state.routines != nullptr ? state.routines->stats().behaving : 0;
        people.reserve(std::min(state.scene.people.size(),
                                moved.size() + state.travel->stats().trips + (m_people_waiting.size() - waiting) + behaving));
        for (usize i = 0; i < state.scene.people.size(); ++i) {
            const world::Person& person = state.scene.people[i];
            while (waiting < m_people_waiting.size() && m_people_waiting[waiting].id < person.id) ++waiting;
            if (waiting < m_people_waiting.size() && m_people_waiting[waiting].id == person.id) {
                people.push_back(m_people_waiting[waiting]); // (not put back yet: as the save it came from had it)
                continue;
            }
            SavedPerson s;
            s.id = person.id;
            const PeopleTravel::Trip* trip = state.travel->trip(person.id);
            const bool going = trip != nullptr && trip->phase != PeopleTravel::Phase::failed;
            if (moved.contains(person.id) || (trip != nullptr && trip->phase == PeopleTravel::Phase::travelling)) {
                s.at = state.travel->where(person);
                s.moved = in(s.at.position) && std::fabs(s.at.yaw) <= 360.0f;
            }
            if (going && in(trip->to) && trip->speed >= 0.1 && trip->speed <= 100.0) {
                s.trip = true;
                s.to = trip->to;
                s.speed = trip->speed;
                s.within = std::clamp(trip->within, 0.0, PeopleTravel::k_join_within);
                s.place = trip->place;
            }
            // (One at home that is not going anywhere is not written: its routine looks at it afresh
            // and finds it where it should be, so a world of thousands at home saves nobody.)
            // (Nor one in the first state of its behaviour: a loaded game begins it there. One in a
            // later state, or doing what a state says, is written wherever it is.)
            const bool later = state.routines != nullptr && state.routines->in_later_state(state.scene, i);
            if ((s.moved || s.trip || later) && state.routines != nullptr &&
                state.routines->kept(state.scene, *state.travel, i, s.kept)) {
                s.did = true;
                // (Its routine's trip failed and the routine has not looked since: failed, to be tried again.)
                if (s.kept.own_trip && !s.trip) s.kept.failed = true;
                if (!s.trip) s.kept.own_trip = false;
            }
            // (A trip a state began, with what began it not kept, it left that state and was not looked
            // at since: not written as one sent by hand, which nothing would stop. A routine's is
            // written as ever.)
            if (s.trip && s.kept.own_trip && s.kept.said == 0 && s.kept.own) {
                s.trip = false;
                s.kept.own_trip = false;
            }
            if (s.moved || s.trip || s.did) people.push_back(std::move(s));
        }
    }
    const usize peopled = people.size();
    save::Meta meta;
    meta.world_name = state.world->folder().filename().string();
    meta.world_hash = world_identity(*state.world);
    meta.saved_unix = static_cast<i64>(std::time(nullptr));
    meta.play_seconds = m_play_seconds;
    // Against the base when saving to the slot it is of (only what changed), else whole.
    const fs::path path = root_path / slot;
    const Base& b = base_for(path);
    const bool same = b.manifest.generation != 0 && path == b.path && m_session == state.world->session();
    const save::Manifest base = same ? b.manifest : save::Manifest{};
    std::shared_ptr<const std::map<save::CellKey, GameCell>> was =
        same && b.cells != nullptr ? b.cells : std::make_shared<const std::map<save::CellKey, GameCell>>();
    const u64 generation = state.scene_generation, session = state.world->session();

    m_in_flight = true;
    m_status.doing = "saving";
    m_status.slot = slot;
    if (autosave) {
        m_status.autosave_error.clear();
    } else {
        m_status.error.clear();
    }
    m_status.bodies = posed;
    m_status.walkers = walking;
    m_status.ragdolls = lying;
    m_status.people = peopled;
    m_status.gather_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gather_from).count();
    m_autosaved_at = m_play_seconds;
    m_jobs.submit(m_group, [this, slot, path, base, meta, generation, session, autosave, sections = std::move(sections), people = std::move(people),
                            cells = std::shared_ptr<const std::map<save::CellKey, GameCell>>(std::move(cells)),
                            was = std::move(was)]() mutable {
        auto p = std::make_unique<Pending>();
        p->slot = slot;
        p->path = path;
        p->autosave = autosave;
        p->generation = generation;
        p->session = session;
        save::Changes changes;
        changes.sections = std::move(sections);
        changes.sections["people"] = people_text(people); // (with nobody too: the slot's last is not left to be loaded)
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
    const u64 world_hash = world_identity(*state.world);
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
            } else if (name == "clock") {
                double hours = 0.0, day = 0.0;
                if (!only_keys(j, {"hours", "day"}) || !j.contains("hours") || !finite_in(j["hours"], 0.0, 24.0, hours) ||
                    !j.contains("day") || !j["day"].is_number_integer() || !finite_in(j["day"], 1.0, 365.0, day))
                    return fail(file + ": a bad clock");
                p->sections.time = std::pair<double, i32>{hours, static_cast<i32>(day)}; // (24: set_time wraps it)
            } else if (name == "player") {
                SavedPlayer s;
                const Json none(nullptr);
                const Json& climbing = j.contains("climbing") ? j["climbing"] : none;
                const Json& by = j.contains("carried_by") ? j["carried_by"] : none;
                if (!only_keys(j, {"feet", "velocity", "climbing", "carried_by", "returns_to", "was_simulating"}) ||
                    !j.contains("feet") || !read_vec(j["feet"], s.feet) || !j.contains("velocity") ||
                    !read_vec3f(j["velocity"], 1e4, s.velocity) || !j.contains("returns_to") ||
                    !read_camera(j["returns_to"], s.returns_to) || !j.value("was_simulating", Json(false)).is_boolean() ||
                    (!climbing.is_null() && !read_vec3f(climbing, 1.01, s.climbing.emplace())) ||
                    (!by.is_null() && (!by.is_number_unsigned() || by.get<u64>() == 0 ||
                                       by.get<u64>() > world::k_max_any_entity_id)))
                    return fail(file + ": a bad player");
                if (!by.is_null()) s.carried_by = by.get<u64>();
                s.was_simulating = j.value("was_simulating", false);
                p->sections.player = s;
            } else if (name == "bodies") {
                if (!only_keys(j, {"simulating", "poses"}) || !j.value("simulating", Json(false)).is_boolean() ||
                    !j.value("poses", Json::array()).is_array() || j.value("poses", Json::array()).size() > k_max_saved_poses)
                    return fail(file + ": bad bodies");
                p->sections.has_bodies = true;
                p->sections.simulating = j.value("simulating", false);
                for (const Json& e : j.value("poses", Json::array())) {
                    SavedPose pose;
                    const Json* of = e.is_object() && e.contains("of") ? &e["of"] : nullptr;
                    if (!e.is_object() || !only_keys(e, {"entity", "position", "rotation", "of"}) || of == nullptr ||
                        !of->is_object() || !only_keys(*of, {"position", "rotation"}) || !e.contains("entity") ||
                        !e["entity"].is_number_unsigned() || e["entity"].get<u64>() == 0 ||
                        e["entity"].get<u64>() > world::k_max_any_entity_id || !e.contains("position") ||
                        !read_vec(e["position"], pose.position) || !e.contains("rotation") ||
                        !read_quat(e["rotation"], pose.rotation) || !of->contains("position") ||
                        !read_vec((*of)["position"], pose.of_position) || !of->contains("rotation") ||
                        !read_quat((*of)["rotation"], pose.of_rotation))
                        return fail(file + ": a bad body");
                    pose.entity = e["entity"].get<u64>();
                    p->sections.poses.push_back(pose);
                }
            } else if (name == "walkers") {
                if (!only_keys(j, {"walkers"}) || !j.value("walkers", Json::array()).is_array() ||
                    j.value("walkers", Json::array()).size() > WalkerState::k_max_walkers)
                    return fail(file + ": bad walkers");
                std::set<world::EntityId> ids;
                for (const Json& e : j.value("walkers", Json::array())) {
                    SavedWalker w;
                    WalkerState::Resume& r = w.resume;
                    const Json* of = e.is_object() && e.contains("of") ? &e["of"] : nullptr;
                    double yaw = 0.0, speed = 0.0;
                    std::optional<WalkerState::Phase> phase;
                    if (!e.is_object() ||
                        !only_keys(e, {"entity", "feet", "yaw", "velocity", "target", "speed", "phase", "reason", "of"}) ||
                        of == nullptr || !of->is_object() || !only_keys(*of, {"position", "rotation"}) ||
                        !e.contains("entity") || !e["entity"].is_number_unsigned() || e["entity"].get<u64>() == 0 ||
                        e["entity"].get<u64>() > world::k_max_any_entity_id || !e.contains("feet") ||
                        !read_vec(e["feet"], r.feet) || !e.contains("yaw") || !finite_in(e["yaw"], -10.0, 10.0, yaw) ||
                        !e.contains("velocity") || !read_vec3f(e["velocity"], 1e4, r.velocity) ||
                        !e.contains("target") || !read_vec(e["target"], r.target) || !e.contains("speed") ||
                        !finite_in(e["speed"], 0.0, 20.0, speed) || !(static_cast<float>(speed) > 0.0f) || !e.contains("phase") ||
                        !(phase = phase_of(e["phase"])) || !e.value("reason", Json("")).is_string() ||
                        e.value("reason", std::string()).size() > k_max_reason || !of->contains("position") ||
                        !read_vec((*of)["position"], w.of_position) || !of->contains("rotation") ||
                        !read_quat((*of)["rotation"], w.of_rotation))
                        return fail(file + ": a bad walker");
                    w.entity = e["entity"].get<u64>();
                    if (!ids.insert(w.entity).second) return fail(file + ": a walker twice");
                    r.yaw = static_cast<float>(yaw);
                    r.speed = static_cast<float>(speed);
                    r.phase = *phase;
                    r.reason = e.value("reason", std::string());
                    p->sections.walkers.push_back(std::move(w));
                }
            } else if (name == "ragdolls") {
                if (!only_keys(j, {"ragdolls"}) || !j.value("ragdolls", Json::array()).is_array() ||
                    j.value("ragdolls", Json::array()).size() > RagdollState::k_max_ragdolls)
                    return fail(file + ": bad ragdolls");
                std::set<world::EntityId> ids;
                for (const Json& e : j.value("ragdolls", Json::array())) {
                    SavedRagdoll g;
                    RagdollState::Resume& r = g.resume;
                    const Json* of = e.is_object() && e.contains("of") ? &e["of"] : nullptr;
                    const Json* bones = e.is_object() && e.contains("bones") ? &e["bones"] : nullptr;
                    if (!e.is_object() || !only_keys(e, {"entity", "asleep", "lying", "bones", "of"}) || of == nullptr ||
                        !of->is_object() || !only_keys(*of, {"position", "rotation", "scale"}) || !of->contains("scale") ||
                        !read_vec3f((*of)["scale"], 1e3, g.of_scale) || !sound_scale(g.of_scale) || !e.contains("entity") ||
                        !e["entity"].is_number_unsigned() || e["entity"].get<u64>() == 0 ||
                        e["entity"].get<u64>() > world::k_max_any_entity_id || !e.contains("asleep") || !e["asleep"].is_boolean() ||
                        !e.contains("lying") || !finite_in(e["lying"], 0.0, k_max_lying, r.lying) || bones == nullptr ||
                        !bones->is_array() || bones->empty() || bones->size() > sim::k_max_ragdoll_bones ||
                        !of->contains("position") || !read_vec((*of)["position"], g.of_position) || !of->contains("rotation") ||
                        !read_quat((*of)["rotation"], g.of_rotation))
                        return fail(file + ": a bad ragdoll");
                    const bool asleep = e["asleep"].get<bool>();
                    for (const Json& b : *bones) {
                        // A row: joint, place, turn, and for one awake its velocity and spin.
                        sim::BodyState s;
                        double v[13] = {};
                        const usize count = asleep ? 8 : 14;
                        bool good = b.is_array() && b.size() == count && b[0].is_number_unsigned() && b[0].get<u64>() <= k_max_joint;
                        for (usize k = 1; good && k < count; ++k) {
                            const double limit = k < 4 ? 1e8 : k < 8 ? 1.01 : static_cast<double>(k_max_bone_speed);
                            good = finite_in(b[k], -limit, limit, v[k - 1]);
                        }
                        if (!good || !read_quat(Json::array({v[3], v[4], v[5], v[6]}), s.rotation))
                            return fail(file + ": a bad ragdoll bone");
                        s.position = {v[0], v[1], v[2]};
                        s.linear_velocity = {static_cast<float>(v[7]), static_cast<float>(v[8]), static_cast<float>(v[9])};
                        s.angular_velocity = {static_cast<float>(v[10]), static_cast<float>(v[11]), static_cast<float>(v[12])};
                        if (!slow_enough(s.linear_velocity) || !slow_enough(s.angular_velocity))
                            return fail(file + ": a bad ragdoll bone");
                        s.active = !asleep;
                        r.joints.push_back(b[0].get<u32>());
                        r.bones.push_back(s);
                    }
                    g.entity = e["entity"].get<u64>();
                    if (!ids.insert(g.entity).second) return fail(file + ": a ragdoll twice");
                    p->sections.ragdolls.push_back(std::move(g));
                }
            } else if (name == "people") {
                if (!only_keys(j, {"people"}) || !j.value("people", Json::array()).is_array() ||
                    j.value("people", Json::array()).size() > world::k_max_people)
                    return fail(file + ": bad people");
                world::PersonId last = 0;
                for (const Json& e : j.value("people", Json::array())) {
                    SavedPerson s;
                    if (!e.is_object() || !only_keys(e, {"id", "at", "trip", "did", "in"}) || !e.contains("id") ||
                        !e["id"].is_number_unsigned() || e["id"].get<u64>() <= last || e["id"].get<u64>() > 0xffffffffull)
                        return fail(file + ": a bad person (each once, in the order of their ids)");
                    s.id = static_cast<world::PersonId>(e["id"].get<u64>());
                    last = s.id;
                    if (e.contains("at")) {
                        const Json& at = e["at"];
                        double v[4];
                        if (!at.is_array() || at.size() != 4 || !finite_in(at[0], -1e8, 1e8, v[0]) || !finite_in(at[1], -1e8, 1e8, v[1]) ||
                            !finite_in(at[2], -1e8, 1e8, v[2]) || !finite_in(at[3], -360.0, 360.0, v[3]))
                            return fail(file + ": a bad person's place");
                        s.moved = true;
                        s.at = {{v[0], v[1], v[2]}, static_cast<float>(v[3])};
                    }
                    if (e.contains("trip")) {
                        const Json& trip = e["trip"];
                        if (!trip.is_object() || !only_keys(trip, {"to", "speed", "within", "place"}) || !trip.contains("to") ||
                            !read_vec(trip["to"], s.to) || !trip.contains("speed") || !finite_in(trip["speed"], 0.1, 100.0, s.speed) ||
                            (trip.contains("within") && !finite_in(trip["within"], 0.0, PeopleTravel::k_join_within, s.within)) ||
                            !trip.value("place", Json("")).is_string() ||
                            trip.value("place", std::string()).size() > world::k_max_name_bytes)
                            return fail(file + ": a bad person's trip");
                        s.trip = true;
                        s.place = trip.value("place", std::string());
                    }
                    if (e.contains("did")) {
                        const Json& did = e["did"];
                        double said = 0.0, entry = 0.0;
                        if (!did.is_object() || !only_keys(did, {"said", "entry", "place", "failed", "own_trip", "own"}) ||
                            !did.contains("said") || !did["said"].is_number_unsigned() || !finite_in(did["said"], 1.0, 4294967295.0, said) ||
                            !did.contains("entry") || !did["entry"].is_number_unsigned() ||
                            !finite_in(did["entry"], 0.0, static_cast<double>(world::k_max_routine_entries + world::k_max_routine_alarms - 1), entry) ||
                            !did.value("place", Json("")).is_string() ||
                            did.value("place", std::string()).size() > world::k_max_name_bytes ||
                            !did.value("failed", Json(false)).is_boolean() || !did.value("own_trip", Json(false)).is_boolean() ||
                            !did.value("own", Json(false)).is_boolean() || (did.value("own", false) && !e.contains("in")))
                            return fail(file + ": a bad person's routine");
                        s.did = true;
                        s.kept.said = static_cast<u32>(said);
                        s.kept.entry = static_cast<u8>(entry);
                        s.kept.place = did.value("place", std::string());
                        s.kept.failed = did.value("failed", false);
                        s.kept.own_trip = s.trip && did.value("own_trip", false);
                        s.kept.own = did.value("own", false);
                    }
                    if (e.contains("in")) { // (the state of its behaviour it was in: older saves have none)
                        const Json& in = e["in"];
                        const auto named = [&in](const char* key) {
                            return in.contains(key) && in[key].is_string() && !in[key].get_ref<const std::string&>().empty() &&
                                   in[key].get_ref<const std::string&>().size() <= world::k_max_name_bytes;
                        };
                        if (!in.is_object() || !only_keys(in, {"behaviour", "state", "seconds", "settled"}) || !named("behaviour") ||
                            !named("state") || !in.contains("seconds") || !finite_in(in["seconds"], 0.0, 1e9, s.kept.in_state) ||
                            !in.value("settled", Json(false)).is_boolean())
                            return fail(file + ": a bad person's state");
                        s.did = true;
                        s.kept.behaviour = in["behaviour"].get<std::string>();
                        s.kept.state = in["state"].get<std::string>();
                        s.kept.settled = in.value("settled", false);
                    }
                    p->sections.people.push_back(std::move(s));
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
    m_auto_base = {};
    m_status.base_slot.clear();
}

void GameSaves::forget_base(const fs::path& slot) {
    Base& b = base_for(slot);
    if (b.path != slot) return;
    b = {};
    if (slot.filename().string() == m_status.base_slot) m_status.base_slot.clear();
}

void GameSaves::clear_ground(EditorState& state) {
    if (state.physics != nullptr) state.physics->clear_game_ground(state.scene);
    m_ground_waiting.clear();
    m_waiting_from.reset();
}

void GameSaves::close(EditorState& state) {
    state.scene.clear_game_damage();
    clear_ground(state);
    m_poses_waiting.clear();
    m_poses_from.reset();
    m_carried_feet.reset();
    forget_walkers();
    forget_ragdolls();
    forget_people();
    drop_weather();
    if (state.clock != nullptr) state.clock->show(state.scene.clock); // the scene's time again
    m_applying.clear();
    m_sections.reset();
    m_generation = 0;
    forget_base();
    m_play_seconds = 0.0;
    m_autosaved_at = 0.0;
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
    forget_base(root_path / slot); // the next save there is whole
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
    if (p->autosave) {
        m_status.autosave_error = p->error;
    } else {
        m_status.error = p->error;
    }
    if (p->remove) return;
    // Begun for this scene and this world session (else it is not this game's).
    const bool same = state.world != nullptr && state.world->active(state.scene_generation) &&
                      p->generation == state.scene_generation && p->session == state.world->session();
    if (!p->load) {
        if (!p->error.empty() || !same) {
            forget_base(p->path); // (what is on the disk is not known now: the next save there is whole)
            std::string& error = p->autosave ? m_status.autosave_error : m_status.error;
            if (p->error.empty()) error = "the world was closed or replaced while saving";
            else if (!same) error += " (and the world was closed or replaced)";
            if (!p->autosave) ++m_status.failures;
            return;
        }
        ++(p->autosave ? m_status.autosaves : m_status.saves);
        m_status.written = p->committed.written;
        m_status.removed = p->committed.removed;
        m_status.cells = p->cells->size();
        m_status.tiles = static_cast<usize>(
            std::count_if(p->cells->begin(), p->cells->end(), [](const auto& c) { return c.second.ground.has_value(); }));
        Base& b = base_for(p->path);
        b.manifest = p->committed.manifest;
        b.cells = std::move(p->cells);
        b.path = p->path;
        m_status.base_slot = p->slot;
        m_session = p->session;
        m_generation = p->generation;
        return;
    }
    if (!p->error.empty() || !same) {
        if (p->error.empty()) m_status.error = "the world was closed or replaced while loading";
        ++m_status.failures;
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
    if (state.clock != nullptr) state.clock->show(state.scene.clock); // (a save without a time shows the scene's)
    m_poses_waiting.clear();
    m_poses_from.reset();
    m_carried_feet.reset();
    forget_walkers();
    forget_ragdolls();
    forget_people();
    if (state.walkers != nullptr) state.walkers->clear(); // (walking in the game being left)
    if (state.ragdolls != nullptr && state.meshes != nullptr) state.ragdolls->clear(state.scene, *state.meshes); // (lying in it)
    forget_base(); // (another game now: the other base is not of it)
    m_applying = std::move(p->loaded);
    m_sections = std::make_unique<Sections>(std::move(p->sections));
    m_status.doing = "applying";
    m_status.cells = m_applying.size();
    m_status.applied = 0;
    m_status.tiles = static_cast<usize>(
        std::count_if(m_applying.begin(), m_applying.end(), [](const auto& c) { return c.second.ground.has_value(); }));
    m_status.skipped = 0;
    Base& b = base_for(p->path);
    b.manifest = p->manifest;
    b.cells = std::move(p->cells);
    b.path = p->path;
    m_status.base_slot = p->slot;
    m_session = p->session;
    m_generation = p->generation;
    m_play_seconds = p->manifest.play_seconds;
    m_autosaved_at = m_play_seconds;
    m_status.bodies = m_sections->poses.size();
    m_status.bodies_applied = 0;
    m_status.bodies_skipped = 0;
    m_status.walkers = m_sections->walkers.size();
    m_status.walkers_applied = 0;
    m_status.walkers_skipped = 0;
    m_status.ragdolls = m_sections->ragdolls.size();
    m_status.ragdolls_applied = 0;
    m_status.ragdolls_skipped = 0;
    m_status.people = m_sections->people.size();
    m_status.people_applied = 0;
    m_status.people_skipped = 0;
    m_play_generation = p->generation;
    m_play_session = p->session;
    ++m_status.loads;
}

void GameSaves::finish_load(EditorState& state) {
    Sections s = std::move(*m_sections);
    m_sections.reset();
    m_status.doing.clear();
    state.follow.person = 0; // a loaded game's view is its own (follow_person)
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
    if (s.time && state.clock != nullptr) state.clock->set_time(s.time->first, s.time->second); // until the scene's clock changes
    if (s.playing && state.play != nullptr && state.physics != nullptr) {
        if (!state.play->start(state.scene, state.camera, state.scene_generation,
                               s.player ? &*s.player : nullptr)) {
            m_status.error = "loaded, but the player could not be put there";
            ++m_status.failures;
        }
    }
    if (state.physics != nullptr && s.has_bodies) {
        if (s.simulating) state.physics->set_simulating(true);
        if (state.physics->simulating()) {
            for (const SavedPose& pose : s.poses) m_poses_waiting[pose.entity] = pose;
            // A carried player's carrier put back later: carried again then, from where the save had it.
            if (s.player && s.player->carried_by && m_poses_waiting.count(*s.player->carried_by) != 0)
                m_carried_feet = std::make_pair(*s.player->carried_by, s.player->feet);
            m_poses_run = state.physics->runs();
            m_poses_held = 0;
        } else {
            m_status.bodies_skipped += s.poses.size();
        }
    }
    if (state.physics != nullptr && state.travel != nullptr && state.physics->simulating()) {
        m_people_waiting = std::move(s.people);
        m_people_next = 0;
        m_people_run = state.physics->runs();
    } else {
        m_status.people_skipped += s.people.size();
    }
    if (state.routines != nullptr) state.routines->hold = !m_people_waiting.empty();
    if (state.people != nullptr) state.people->hold = !m_people_waiting.empty();
    if (state.physics != nullptr && state.walkers != nullptr && state.physics->simulating()) {
        for (const SavedWalker& w : s.walkers) m_walkers_waiting[w.entity] = w;
        m_walkers_run = state.physics->runs();
    } else {
        m_status.walkers_skipped += s.walkers.size();
    }
    if (state.physics != nullptr && state.ragdolls != nullptr && state.meshes != nullptr && state.physics->simulating()) {
        for (const SavedRagdoll& g : s.ragdolls) m_ragdolls_waiting[g.entity] = g;
        m_ragdolls_run = state.physics->runs();
        m_ragdolls_unsure = false;
    } else {
        m_status.ragdolls_skipped += s.ragdolls.size();
    }
}

void GameSaves::forget_ragdolls() {
    m_ragdolls_waiting.clear();
    m_ragdolls_from.reset();
}

void GameSaves::place_ragdolls(EditorState& state) {
    if (m_ragdolls_waiting.empty()) return;
    // Only for the simulation they were for (stopped since: ragdolls let go).
    if (state.physics == nullptr || state.ragdolls == nullptr || state.meshes == nullptr || !state.physics->simulating() ||
        state.physics->runs() != m_ragdolls_run) {
        m_status.ragdolls_skipped += m_ragdolls_waiting.size();
        forget_ragdolls();
        return;
    }
    // Each waits for what it lies on, as the walkers do for what they stand on, and then for its file
    // to be read (k_file_frames more at most); asked once: one refused then is not this save's.
    constexpr u32 k_hold_frames = 120;
    constexpr u32 k_file_frames = 600;
    // What one slept on may not be there (a body or another ragdoll of the load skipped): those made
    // from then on are made awake, to settle on what is.
    if (m_status.bodies_skipped != 0 || m_status.ragdolls_skipped != 0) m_ragdolls_unsure = true;
    const bool settling = state.physics->bodies().entities_waiting() || !m_poses_waiting.empty();
    constexpr usize k_looks = 256;
    usize budget = std::max<usize>(ragdolls_per_frame, 1);
    auto it = m_ragdolls_from ? m_ragdolls_waiting.lower_bound(*m_ragdolls_from) : m_ragdolls_waiting.begin();
    const usize looks = std::min(k_looks, m_ragdolls_waiting.size());
    for (usize looked = 0; looked < looks && budget > 0 && !m_ragdolls_waiting.empty(); ++looked) {
        if (it == m_ragdolls_waiting.end()) it = m_ragdolls_waiting.begin();
        SavedRagdoll& g = it->second;
        const world::Entity* e = state.scene.find(g.entity);
        if (e == nullptr) {
            ++it; // (its cell not streamed in yet)
            continue;
        }
        if (g.seen < k_hold_frames) ++g.seen;
        if (g.seen == 1 || (settling && g.seen < k_hold_frames)) {
            ++it;
            continue;
        }
        // Moved or scaled in the world since, hidden, or walking or limp since: not this save's.
        if (e->person != 0 || state.ragdolls->find(g.entity) != nullptr ||
            (state.walkers != nullptr && state.walkers->find(g.entity) != nullptr) ||
            !(e->transform.position == g.of_position) || !(e->transform.rotation == g.of_rotation) ||
            !(e->transform.scale == g.of_scale) || !e->visible) {
            ++m_status.ragdolls_skipped;
            m_ragdolls_unsure = true; // (another may have slept on it)
            it = m_ragdolls_waiting.erase(it);
            continue;
        }
        const DVec3& root = g.resume.bones.front().position;
        if (state.physics->bodies().ground_waiting(state.scene.terrain, root) ||
            m_ground_waiting.count(key_of(world::cell_of(root))) != 0) {
            ++it; // (its ground not in yet)
            continue;
        }
        if (!state.ragdolls->ready(state.scene, *state.meshes, g.entity) && g.unread++ < k_file_frames) {
            ++it; // (its file not read yet)
            continue;
        }
        --budget;
        if (m_ragdolls_unsure) {
            for (sim::BodyState& s : g.resume.bones) s.active = true;
        }
        if (state.ragdolls->resume(state.scene, *state.meshes, g.entity, g.resume).empty()) {
            ++m_status.ragdolls_applied;
        } else {
            ++m_status.ragdolls_skipped; // (other bones now, no room: asking again would not help)
            m_ragdolls_unsure = true;
        }
        it = m_ragdolls_waiting.erase(it);
    }
    m_ragdolls_from = it != m_ragdolls_waiting.end() ? std::optional<world::EntityId>(it->first) : std::nullopt;
}

void GameSaves::forget_people() {
    m_people_waiting.clear();
    m_people_next = 0;
}

// Each put where it was, sent on where it was going (its way looked for again from there), and its
// routine told what it had acted on.
void GameSaves::place_people(EditorState& state) {
    if (m_people_next >= m_people_waiting.size()) {
        if (!m_people_waiting.empty()) forget_people();
        return;
    }
    // Only for the simulation they were for (stopped since: everyone is back where the scene has them).
    if (state.physics == nullptr || state.travel == nullptr || !state.physics->simulating() ||
        state.physics->runs() != m_people_run) {
        m_status.people_skipped += m_people_waiting.size() - m_people_next;
        forget_people();
        return;
    }
    const PeopleTravel::Clock clock = PeopleTravel::clock_of(*state.physics);
    state.travel->sync(state.scene, clock.run, clock.seconds);
    usize budget = std::max<usize>(people_per_frame, 1), sends = std::max<usize>(people_sent_per_frame, 1);
    for (; m_people_next < m_people_waiting.size() && budget > 0; ++m_people_next, --budget) {
        const SavedPerson& s = m_people_waiting[m_people_next];
        if (world::find_person(state.scene.people, s.id) == nullptr) { // (gone from the world since)
            ++m_status.people_skipped;
            continue;
        }
        if (s.trip && sends == 0) break; // (the next frame's)
        if (s.moved && !state.travel->put(state.scene, s.id, s.at)) {
            ++m_status.people_skipped;
            continue;
        }
        bool sent = false;
        if (s.trip) {
            --sends;
            // (Part way along a long stretch it is farther from the road's nodes than `within`: one
            // put where it travelled to joins as far off as any may, people_travel.h.)
            sent = state.travel->send(state.scene, s.id, s.to, static_cast<float>(s.speed), s.within, s.place, true).empty();
        }
        // (One whose routine's trip cannot be sent again, the roads other than they were, is left to
        // its routine to send afresh; a trip of its own that cannot is not the routine's to mind.)
        if (s.did && state.routines != nullptr && (!s.trip || sent || !s.kept.own_trip)) {
            // (Its routine says something else now: the trip that was the routine's is not gone on with.)
            if (!state.routines->restore(state.scene, *state.travel, s.id, s.kept) && sent && s.kept.own_trip)
                state.travel->stop(s.id);
        } else if (s.did && state.routines != nullptr && !s.kept.behaviour.empty()) {
            // (Its trip could not be sent again: it is in its state still, and what the state or its
            // routine says is acted on afresh.)
            (void)state.routines->restore(state.scene, *state.travel, s.id, s.kept, true);
        }
        ++m_status.people_applied;
    }
    if (m_people_next >= m_people_waiting.size()) forget_people();
}

void GameSaves::forget_walkers() {
    m_walkers_waiting.clear();
    m_walkers_from.reset();
}

void GameSaves::place_walkers(EditorState& state) {
    if (m_walkers_waiting.empty()) return;
    // Only for the simulation they were for (stopped since: walkers let go).
    if (state.physics == nullptr || state.walkers == nullptr || !state.physics->simulating() ||
        state.physics->runs() != m_walkers_run) {
        m_status.walkers_skipped += m_walkers_waiting.size();
        forget_walkers();
        return;
    }
    // Each waits for what it stands on: its entity in the scene a frame (its cell's bodies and ground
    // seen by the physics), then while bodies are being made or put back (at most k_hold_frames:
    // streaming may keep some waiting anywhere for long).
    constexpr u32 k_hold_frames = 120;
    const bool settling = state.physics->bodies().entities_waiting() || !m_poses_waiting.empty();
    constexpr usize k_looks = 1024;
    usize budget = std::max<usize>(walkers_per_frame, 1);
    auto it = m_walkers_from ? m_walkers_waiting.lower_bound(*m_walkers_from) : m_walkers_waiting.begin();
    // Each looked at once at most (its `seen` counts frames, not looks).
    const usize looks = std::min(k_looks, m_walkers_waiting.size());
    for (usize looked = 0; looked < looks && budget > 0 && !m_walkers_waiting.empty(); ++looked) {
        if (it == m_walkers_waiting.end()) it = m_walkers_waiting.begin();
        SavedWalker& w = it->second;
        const world::Entity* e = state.scene.find(w.entity);
        if (e == nullptr) {
            ++it; // (its cell not streamed in yet)
            continue;
        }
        if (w.seen < k_hold_frames) ++w.seen;
        if (w.seen == 1 || (settling && w.seen < k_hold_frames)) {
            ++it;
            continue;
        }
        // Moved in the world since, given a body, hidden, or sent walking since: not this save's.
        // (Nor a person's entity, which an older save may name: it walks its trip.)
        if (e->person != 0 || state.walkers->find(w.entity) != nullptr || !(e->transform.position == w.of_position) ||
            !(e->transform.rotation == w.of_rotation) ||
            e->body != world::BodyKind::none || !e->visible) {
            ++m_status.walkers_skipped;
            it = m_walkers_waiting.erase(it);
            continue;
        }
        if (state.physics->bodies().ground_waiting(state.scene.terrain, w.resume.feet) ||
            m_ground_waiting.count(key_of(world::cell_of(w.resume.feet))) != 0) {
            ++it; // (its ground not in yet)
            continue;
        }
        --budget;
        if (state.walkers->resume(state.scene, w.entity, w.resume).empty()) {
            ++m_status.walkers_applied;
        } else {
            ++m_status.walkers_skipped;
        }
        it = m_walkers_waiting.erase(it);
    }
    m_walkers_from = it != m_walkers_waiting.end() ? std::optional<world::EntityId>(it->first) : std::nullopt;
}

void GameSaves::place_bodies(EditorState& state) {
    if (m_poses_waiting.empty()) return;
    // Only for the simulation they were put back for (stopped since: what it moved went back).
    if (state.physics == nullptr || !state.physics->simulating() || state.physics->runs() != m_poses_run) {
        m_status.bodies_skipped += m_poses_waiting.size();
        m_poses_waiting.clear();
        m_poses_from.reset();
        m_carried_feet.reset();
        return;
    }
    // Waiting on what they would rest on: the bodies being made and the ground round them (a game's too).
    // (Held at most k_hold_frames: streaming may keep some entity waiting anywhere for long.)
    constexpr u32 k_hold_frames = 120;
    if (state.physics->bodies().entities_waiting() && m_poses_held++ < k_hold_frames) return;
    constexpr usize k_looks = 8192;
    usize budget = std::max<usize>(poses_per_frame, 1);
    auto it = m_poses_from ? m_poses_waiting.lower_bound(*m_poses_from) : m_poses_waiting.begin();
    for (usize looked = 0; looked < k_looks && budget > 0 && !m_poses_waiting.empty(); ++looked) {
        if (it == m_poses_waiting.end()) it = m_poses_waiting.begin();
        const SavedPose& p = it->second;
        const world::Entity* e = state.scene.find(p.entity);
        if (e == nullptr) {
            ++it; // (its cell not streamed in yet)
            continue;
        }
        // Moved in the world since, or not a dynamic body (a kit stands fixed): not this save's.
        if (!(e->transform.position == p.of_position) || !(e->transform.rotation == p.of_rotation) ||
            e->body != world::BodyKind::dynamic || e->shape == world::Shape::kit) {
            ++m_status.bodies_skipped;
            it = m_poses_waiting.erase(it);
            continue;
        }
        const world::CellCoord under = world::cell_of(p.position);
        if (state.physics->bodies().ground_waiting(state.scene.terrain, p.position) ||
            m_ground_waiting.count(key_of(under)) != 0 || !state.physics->place_body(p.entity, p.position, p.rotation)) {
            ++it; // (its ground, or its body, not made yet)
            continue;
        }
        --budget;
        ++m_status.bodies_applied;
        if (m_carried_feet && m_carried_feet->first == p.entity) {
            // (Only while that player is still held by it: not one started again, let go or dropped since.)
            if (state.play != nullptr && state.play->carried_by() == p.entity)
                state.play->carry_from(m_carried_feet->second, p.entity);
            m_carried_feet.reset();
        }
        it = m_poses_waiting.erase(it);
    }
    m_poses_from = it != m_poses_waiting.end() ? std::optional<world::EntityId>(it->first) : std::nullopt;
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
    place_bodies(state);
    place_walkers(state);
    place_ragdolls(state);
    place_people(state);
    // Until everyone is back nobody follows a routine or is given an entity (one would be sent
    // again, or stand where the scene has it and then be elsewhere).
    if (state.routines != nullptr) state.routines->hold = !m_people_waiting.empty();
    if (state.people != nullptr) state.people->hold = !m_people_waiting.empty();
    const bool playing = state.play != nullptr && state.play->playing();
    if (playing && std::isfinite(seconds) && seconds > 0.0)
        m_play_seconds = std::min(m_play_seconds + seconds, save::k_max_play_seconds);
    // Autosave: while playing (put down), every autosave_seconds of play (bodies still waiting saved as they were).
    if (playing && autosave_seconds > 0.0 && std::isfinite(autosave_seconds) &&
        m_play_seconds - m_autosaved_at >= autosave_seconds && !busy() &&
        !state.play->waiting_for_ground() && !root(state).empty()) {
        const std::string why = save_to(state, k_autosave_slot, true);
        if (!why.empty()) {
            m_status.autosave_error = why;
            m_autosaved_at = m_play_seconds; // (tried: again the next time round)
        }
    }
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
        m_poses_waiting.clear();
        m_poses_from.reset();
        m_carried_feet.reset();
        forget_walkers();
        forget_ragdolls();
        forget_people();
        forget_base();
        if (m_status.doing == "applying") m_status.doing.clear();
        m_generation = 0;
    }
    if (m_play_generation != state.scene_generation || m_play_session != session) {
        m_play_seconds = 0.0;
        m_autosaved_at = 0.0;
        m_play_generation = state.scene_generation;
        m_play_session = session;
    }
}

} // namespace giant::editor
