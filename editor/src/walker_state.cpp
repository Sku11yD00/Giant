#include "walker_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

constexpr double k_grid = 2.0;          // metres a side of the squares others are looked for in
constexpr double k_stuck_seconds = 2.0; // looked at this often while walking
constexpr double k_stuck_metres = 0.3;  // moved less: stuck

world::CellCoord square_of(const DVec3& p) {
    const auto at = [](double v) {
        return static_cast<i32>(std::clamp(std::floor(v / k_grid), -2.0e9, 2.0e9));
    };
    return {at(p.x), at(p.z)};
}

// Its origin above its feet: a cube's or a sphere's centre stands half its
// height up; a mesh's or a kit's origin is at its feet.
double lift_of(const world::Entity& e) {
    switch (e.shape) {
    case world::Shape::cube:
    case world::Shape::sphere: return 0.5 * std::fabs(e.transform.scale.y);
    default: return 0.0;
    }
}

} // namespace

const char* WalkerState::to_string(Phase phase) {
    switch (phase) {
    case Phase::searching: return "searching";
    case Phase::walking: return "walking";
    case Phase::arrived: return "arrived";
    case Phase::failed: return "failed";
    }
    return "failed";
}

WalkerState::WalkerState(PhysicsState& physics, NavState& nav) : m_physics(physics), m_nav(nav) {}

WalkerState::~WalkerState() {
    for (const auto& [id, w] : m_walkers) {
        if (w.character != sim::k_no_character) m_physics.world().remove_character(w.character);
        if (w.search != 0) m_nav.cancel(w.search);
    }
}

bool WalkerState::make_character(Walker& w) {
    sim::CharacterDesc desc;
    desc.position = w.feet;
    desc.radius = std::min(w.settings.radius, 0.45f);
    desc.step_height = m_nav.settings().step_height;
    desc.max_slope_degrees = m_nav.settings().max_slope_degrees + 5.0f; // walks what the map says it can
    w.character = m_physics.world().add_character(desc);
    return w.character != sim::k_no_character;
}

std::optional<DVec3> WalkerState::feet_of(const world::Scene& scene, world::EntityId id) const {
    if (const Walker* w = find(id)) {
        if (const auto s = m_physics.world().character_state(w->character)) return s->position;
        return w->feet;
    }
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return std::nullopt;
    return e->transform.position - DVec3{0.0, lift_of(*e), 0.0};
}

std::string WalkerState::walk_to(const world::Scene& scene, world::EntityId id, const DVec3& to, float speed,
                                 const std::vector<DVec3>* path) {
    if (!m_physics.simulating()) return "not simulating (simulate on first): walkers move with the physics";
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return "no entity " + std::to_string(id);
    if (e->shape == world::Shape::decal || e->shape == world::Shape::cloth) return "a decal or cloth cannot walk";
    if (e->body != world::BodyKind::none) {
        return "entity " + std::to_string(id) + " has a body (set body \"none\" first: it walks as a character)";
    }
    if (!(std::fabs(to.x) <= nav::k_max_nav_coordinate) || !(std::fabs(to.z) <= nav::k_max_nav_coordinate) ||
        !std::isfinite(to.y)) {
        return "not a place";
    }
    if (!(speed > 0.0f) || !(speed <= 20.0f)) return "speed must be over 0 and at most 20 m/s";
    if (scene.identity() != m_scene) {
        clear();
        m_scene = scene.identity();
    }
    auto it = m_walkers.find(id);
    if (it == m_walkers.end()) {
        if (m_walkers.size() >= k_max_walkers) return "too many walkers (" + std::to_string(k_max_walkers) + ")";
        Walker w;
        w.lift = lift_of(*e);
        w.rotation = e->transform.rotation;
        w.scale = e->transform.scale;
        w.feet = e->transform.position - DVec3{0.0, w.lift, 0.0};
        if (!make_character(w)) return "the physics refused its character";
        // Facing as placed: its +z turned about y.
        const Vec3 f = rotate(w.rotation, Vec3{0.0f, 0.0f, 1.0f});
        w.yaw = std::atan2(f.x, f.z);
        w.rotation = normalize(quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -w.yaw) * w.rotation); // its turn about y taken out
        it = m_walkers.emplace(id, std::move(w)).first;
        m_redraw.push_back(id); // drawn as moving from now
    }
    Walker& w = it->second;
    if (w.search != 0) m_nav.cancel(w.search);
    if (const auto s = m_physics.world().character_state(w.character)) w.feet = s->position;
    w.target = to;
    w.settings.speed = speed;
    w.follow.clear();
    w.phase = Phase::searching;
    w.reason.clear();
    w.searches = 1;
    w.stuck_for = 0.0;
    w.stuck_from = w.feet;
    w.search = 0;
    if (path != nullptr && !path->empty()) {
        w.follow.set_path(*path);
        w.phase = Phase::walking;
    } else {
        w.search = m_nav.find_path(w.feet, to);
    }
    return {};
}

void WalkerState::drop(world::EntityId id) {
    const auto it = m_walkers.find(id);
    if (it == m_walkers.end()) return;
    if (it->second.character != sim::k_no_character && m_physics.world().contains_character(it->second.character)) {
        m_physics.world().remove_character(it->second.character);
    }
    if (it->second.search != 0) m_nav.cancel(it->second.search);
    m_walkers.erase(it);
    if (m_poses.erase(id) != 0) m_redraw.push_back(id);
}

bool WalkerState::stop(world::EntityId id) {
    if (!m_walkers.contains(id)) return false;
    drop(id);
    return true;
}

void WalkerState::clear() {
    std::vector<world::EntityId> ids;
    ids.reserve(m_walkers.size());
    for (const auto& [id, w] : m_walkers) ids.push_back(id);
    for (const world::EntityId id : ids) drop(id);
}

const WalkerState::Walker* WalkerState::find(world::EntityId id) const {
    const auto it = m_walkers.find(id);
    return it != m_walkers.end() ? &it->second : nullptr;
}

void WalkerState::before_step(const world::Scene& scene, double seconds) {
    if (m_walkers.empty()) return;
    if (!m_physics.simulating() || scene.identity() != m_scene) {
        clear();
        m_scene = scene.identity();
        return;
    }
    sim::PhysicsWorld& world = m_physics.world();
    // Gone (removed, given a body, hidden): let go. A character lost (the
    // physics made again): made again where it was.
    std::vector<world::EntityId> gone;
    for (auto& [id, w] : m_walkers) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || e->body != world::BodyKind::none || !e->visible) {
            gone.push_back(id);
            continue;
        }
        w.scale = e->transform.scale; // (a scale changed meanwhile drawn too)
        if (!world.contains_character(w.character) && !make_character(w)) gone.push_back(id);
    }
    for (const world::EntityId id : gone) drop(id);
    // Where each stands now, by square, for the others to keep clear of.
    m_grid.clear();
    for (auto& [id, w] : m_walkers) {
        if (const auto s = world.character_state(w.character)) w.feet = s->position;
        m_grid[square_of(w.feet)].push_back(id);
    }
    for (auto& [id, w] : m_walkers) {
        Vec3 velocity{};
        if (w.phase == Phase::searching) {
            const nav::PathSearch* s = m_nav.search(w.search);
            if (s == nullptr) {
                w.phase = Phase::failed;
                w.reason = "its search went";
            } else if (s->state() == nav::SearchState::found) {
                w.follow.set_path(s->path());
                w.phase = Phase::walking;
                w.stuck_for = 0.0;
                w.stuck_from = w.feet;
                m_nav.cancel(w.search);
                w.search = 0;
            } else if (s->state() == nav::SearchState::failed) {
                w.phase = Phase::failed;
                w.reason = s->reason();
                m_nav.cancel(w.search);
                w.search = 0;
            }
        }
        if (w.phase == Phase::walking) {
            m_near.clear();
            const world::CellCoord c = square_of(w.feet);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const auto in = m_grid.find({c.x + dx, c.z + dz});
                    if (in == m_grid.end()) continue;
                    for (const world::EntityId other : in->second) {
                        if (other == id) continue;
                        const Walker& o = m_walkers.at(other);
                        m_near.push_back({o.feet, o.velocity, o.settings.radius});
                        if (m_near.size() >= 4 * nav::PathFollower::k_max_neighbours) break;
                    }
                }
            }
            const nav::Steering st = w.follow.steer(w.feet, m_near, w.settings);
            if (st.arrived) {
                w.phase = Phase::arrived;
            } else {
                velocity = st.velocity;
                // Stuck (a door shut, a crowd): the way looked for again.
                w.stuck_for += seconds;
                if (w.stuck_for >= k_stuck_seconds) {
                    const double moved = std::hypot(w.feet.x - w.stuck_from.x, w.feet.z - w.stuck_from.z);
                    w.stuck_for = 0.0;
                    w.stuck_from = w.feet;
                    if (moved < k_stuck_metres) {
                        if (w.searches >= k_max_searches) {
                            w.phase = Phase::failed;
                            w.reason = "stuck";
                            velocity = {};
                        } else {
                            ++w.searches;
                            w.follow.clear();
                            w.phase = Phase::searching;
                            w.search = m_nav.find_path(w.feet, w.target);
                            velocity = {};
                        }
                    }
                }
            }
        }
        w.velocity = velocity;
        const float speed = std::hypot(velocity.x, velocity.z);
        if (speed > 0.05f) w.yaw = std::atan2(velocity.x, velocity.z);
        sim::CharacterInput input;
        input.velocity = velocity;
        world.set_character_input(w.character, input);
    }
}

void WalkerState::after_step() {
    const sim::PhysicsWorld& world = m_physics.world();
    const double f = world.step_fraction();
    for (const auto& [id, w] : m_walkers) {
        const auto s = world.character_state(w.character);
        if (!s) continue;
        world::Transform t;
        const DVec3 at = s->previous + (s->position - s->previous) * f;
        t.position = at + DVec3{0.0, w.lift, 0.0};
        t.rotation = normalize(quat_from_axis_angle({0.0f, 1.0f, 0.0f}, w.yaw) * w.rotation);
        const auto old = m_poses.find(id);
        t.scale = w.scale;
        if (old != m_poses.end() && old->second.position == t.position && old->second.scale == t.scale &&
            old->second.rotation.x == t.rotation.x &&
            old->second.rotation.y == t.rotation.y && old->second.rotation.z == t.rotation.z &&
            old->second.rotation.w == t.rotation.w) {
            continue;
        }
        const bool first = old == m_poses.end();
        m_poses[id] = t;
        if (!first) m_moved.push_back(id);
    }
}

std::vector<world::EntityId> WalkerState::take_moved() {
    std::vector<world::EntityId> out;
    out.swap(m_moved);
    return out;
}

std::vector<world::EntityId> WalkerState::take_redraw() {
    std::vector<world::EntityId> out;
    out.swap(m_redraw);
    return out;
}

} // namespace giant::editor
