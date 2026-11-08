#include "walker_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

constexpr double k_grid = 2.0;          // metres a side of the squares others are looked for in
constexpr double k_stuck_seconds = 2.0; // looked at this often while walking
constexpr double k_stuck_share = 0.33;  // moved less than this share of what it was steered to: stuck

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
    desc.radius = w.settings.radius;
    desc.height = w.height;
    desc.step_height = std::min(m_nav.settings().step_height, 0.5f * w.height);
    desc.max_slope_degrees = m_nav.settings().max_slope_degrees + 5.0f; // walks what the map says it can
    desc.user = w.entity; // a shot or a ray that meets it names it
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
    if (scene.identity() != m_scene || m_physics.runs() != m_run) {
        clear();
        m_scene = scene.identity();
        m_run = m_physics.runs();
    }
    auto it = m_walkers.find(id);
    if (it == m_walkers.end()) {
        if (m_walkers.size() >= k_max_walkers) return "too many walkers (" + std::to_string(k_max_walkers) + ")";
        Walker w;
        w.entity = id;
        w.lift = lift_of(*e);
        w.rotation = e->transform.rotation;
        w.scale = e->transform.scale;
        w.feet = e->transform.position - DVec3{0.0, w.lift, 0.0};
        // As wide as it is (within what the map keeps clear), as tall as a cube or sphere is.
        const float wide = 0.5f * std::max(std::fabs(e->transform.scale.x), std::fabs(e->transform.scale.z));
        w.settings.radius = std::clamp(wide, 0.2f, 0.45f);
        if (e->shape == world::Shape::cube || e->shape == world::Shape::sphere) {
            w.height = std::clamp(std::fabs(e->transform.scale.y), 2.0f * w.settings.radius + 0.1f, 4.0f);
        }
        if (!make_character(w)) return "the physics refused its character";
        // Facing as placed: its +z turned about y.
        const Vec3 f = rotate(w.rotation, Vec3{0.0f, 0.0f, 1.0f});
        w.yaw = std::atan2(f.x, f.z);
        w.rotation = normalize(quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -w.yaw) * w.rotation); // its turn about y taken out
        it = m_walkers.emplace(id, std::move(w)).first; // (drawn as moving from its first pose)
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
    w.expected = 0.0;
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
    if (!m_physics.simulating() || scene.identity() != m_scene || m_physics.runs() != m_run) {
        clear();
        m_scene = scene.identity();
        return;
    }
    sim::PhysicsWorld& world = m_physics.world();
    // Simulated time since the last look (a long frame drops steps: not walked, not stuck).
    const u64 steps = world.stats().steps;
    const double simulated = std::min(static_cast<double>(steps - std::min(m_steps, steps)) * sim::PhysicsWorld::k_step_seconds,
                                      0.1); // (at most a frame's worth: the first look after a while counts no more)
    m_steps = steps;
    (void)seconds;
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
    // Where each stands now, by square, for the others to keep clear of
    // (the squares' lists kept for the next step; emptied ones dropped now and then).
    if (m_grid.size() > 4 * m_walkers.size() + 64) m_grid.clear();
    for (auto& [square, ids] : m_grid) ids.clear();
    for (auto& [id, w] : m_walkers) {
        if (const auto s = world.character_state(w.character)) w.feet = s->position;
        m_grid[square_of(w.feet)].push_back(id);
    }
    for (auto& [id, w] : m_walkers) {
        Vec3 velocity{};
        if (w.phase == Phase::searching && (w.search == 0 || m_nav.settled(w.search) || m_nav.search(w.search) == nullptr)) {
            const nav::PathSearch* s = m_nav.search(w.search);
            if (s == nullptr) {
                w.phase = Phase::failed;
                w.reason = "its search went";
            } else if (s->state() == nav::SearchState::found) {
                w.follow.set_path(s->path());
                w.phase = Phase::walking;
                w.stuck_for = 0.0;
                w.expected = 0.0;
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
            // Others near: its own square first, then those around (the
            // follower keeps the nearest few), and the player.
            m_near.clear();
            const world::CellCoord c = square_of(w.feet);
            constexpr usize k_most = 4 * nav::PathFollower::k_max_neighbours;
            for (int ring = 0; ring < 9 && m_near.size() < k_most; ++ring) {
                const i32 dx = ring == 0 ? 0 : (ring - 1) % 3 - 1, dz = ring == 0 ? 0 : (ring - 1) / 3 - 1;
                if (ring > 0 && dx == 0 && dz == 0) continue;
                const auto in = m_grid.find({c.x + dx, c.z + dz});
                if (in == m_grid.end()) continue;
                for (const world::EntityId other : in->second) {
                    if (m_near.size() >= k_most) break;
                    if (other == id) continue;
                    const Walker& o = m_walkers.at(other);
                    m_near.push_back({o.feet, o.velocity, o.settings.radius});
                }
            }
            m_near.insert(m_near.end(), others.begin(), others.end());
            const nav::Steering st = w.follow.steer(w.feet, m_near, w.settings);
            if (st.arrived) {
                w.phase = Phase::arrived;
            } else {
                velocity = st.velocity;
                // Stuck (a door shut, a crowd): the way looked for again.
                w.stuck_for += simulated;
                w.expected += static_cast<double>(std::hypot(velocity.x, velocity.z)) * simulated;
                if (w.stuck_for >= k_stuck_seconds) {
                    const double moved = std::hypot(w.feet.x - w.stuck_from.x, w.feet.z - w.stuck_from.z);
                    const double expected = w.expected;
                    w.stuck_for = 0.0;
                    w.expected = 0.0;
                    w.stuck_from = w.feet;
                    if (expected > 0.1 && moved < k_stuck_share * expected) {
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
        if (first) {
            m_redraw.push_back(id); // its cell built again: drawn as moving from now
        } else {
            m_moved.push_back(id);
        }
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
