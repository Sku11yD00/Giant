#include "hair_state.h"

#include "giant/core/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

namespace giant::editor {

namespace {

// What a groom is grown from: a change to any of it grows another.
struct Seed {
    world::Shape shape = world::Shape::cube;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    i32 strands = 0;
    i32 guides = 0;
    float top = 0.0f;
    float length = 0.0f;
    float length_variation = 0.0f;
    float droop = 0.0f;
    float curl_radius = 0.0f;
    float curl_turns = 0.0f;
    float frizz = 0.0f;
    float clump = 0.0f;
    i32 seed = 0;
    bool operator==(const Seed&) const = default;
};

Seed seed_of(const world::Entity& e) {
    const world::Hair& h = e.hair;
    return {e.shape,     e.transform.scale, h.strands,     h.guides, h.top,   h.length, h.length_variation,
            h.droop,     h.curl_radius,     h.curl_turns,  h.frizz,  h.clump, h.seed};
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool wears(const world::Entity& e) {
    // (A scale that is no number grows nothing, and would never equal itself.)
    return e.visible && e.hair.on && (e.shape == world::Shape::cube || e.shape == world::Shape::sphere) &&
           finite(e.transform.scale);
}

geometry::Vertex between(const geometry::Vertex& a, const geometry::Vertex& b, float t) {
    geometry::Vertex v = a;
    v.position = a.position + (b.position - a.position) * t;
    v.normal = a.normal + (b.normal - a.normal) * t;
    v.uv = {a.uv.u + (b.uv.u - a.uv.u) * t, a.uv.v + (b.uv.v - a.uv.v) * t};
    return v;
}

} // namespace

geometry::TriangleMesh hair_scalp(world::Shape shape, const Vec3& scale, float top) {
    geometry::TriangleMesh whole;
    if (shape == world::Shape::cube) {
        whole = geometry::make_cube();
    } else if (shape == world::Shape::sphere) {
        whole = geometry::make_sphere();
    } else {
        return {};
    }
    const float share = std::isfinite(top) ? std::clamp(top, 0.01f, 1.0f) : 0.5f;
    const Vec3 size{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
    const bool sized = finite(size) && size.x > 0.0f && size.y > 0.0f && size.z > 0.0f;
    // (The shapes are 1 m, centred: from y = 0.5 down.)
    const float from = 0.5f - share;
    const auto above = [from](const geometry::Vertex& v) { return v.position.y >= from - 1e-6f; };
    geometry::TriangleMesh scalp;
    const auto place = [&](geometry::Vertex v) {
        v.position = {v.position.x * size.x, v.position.y * size.y, v.position.z * size.z};
        // (A sized shape's normals turn the other way: by the inverse.)
        const Vec3 n = sized ? normalize_or_zero(Vec3{v.normal.x / size.x, v.normal.y / size.y, v.normal.z / size.z})
                             : normalize_or_zero(v.normal);
        v.normal = n.x == 0.0f && n.y == 0.0f && n.z == 0.0f ? Vec3{0.0f, 1.0f, 0.0f} : n;
        scalp.indices.push_back(static_cast<u32>(scalp.vertices.size()));
        scalp.vertices.push_back(v);
    };
    for (usize i = 0; i + 2 < whole.indices.size(); i += 3) {
        const geometry::Vertex* v[3] = {&whole.vertices[whole.indices[i]], &whole.vertices[whole.indices[i + 1]],
                                        &whole.vertices[whole.indices[i + 2]]};
        // The triangle cut at the height: what is left has 3 or 4 corners.
        std::array<geometry::Vertex, 4> kept;
        usize count = 0;
        for (usize c = 0; c < 3; ++c) {
            const geometry::Vertex& a = *v[c];
            const geometry::Vertex& b = *v[(c + 1) % 3];
            const bool a_in = above(a);
            const bool b_in = above(b);
            if (a_in && count < kept.size()) kept[count++] = a;
            if (a_in != b_in && count < kept.size()) {
                const float fall = b.position.y - a.position.y;
                kept[count++] = between(a, b, fall != 0.0f ? std::clamp((from - a.position.y) / fall, 0.0f, 1.0f) : 0.0f);
            }
        }
        for (usize c = 2; c < count; ++c) {
            place(kept[0]);
            place(kept[c - 1]);
            place(kept[c]);
        }
    }
    return scalp;
}

geometry::GroomParams hair_params(const world::Hair& hair) {
    geometry::GroomParams p;
    p.strands = static_cast<u32>(std::clamp(hair.strands, 1, world::k_max_hair_strands));
    p.guides = std::min(static_cast<u32>(std::clamp(hair.guides, 1, world::k_max_hair_guides)), p.strands);
    p.length = hair.length;
    p.length_variation = hair.length_variation;
    p.droop = hair.droop;
    p.curl_radius = hair.curl_radius;
    p.curl_turns = hair.curl_turns;
    p.frizz = hair.frizz;
    p.clump = hair.clump;
    p.seed = static_cast<u32>(std::clamp(hair.seed, 1, 1000000));
    return p;
}

render::HairLook hair_look(const world::Hair& hair) {
    render::HairLook look;
    look.melanin = hair.melanin;
    look.redness = hair.redness;
    look.tint = hair.tint;
    look.roughness = hair.roughness;
    look.width = hair.width;
    look.tip_width = hair.tip_width;
    look.variation = hair.variation;
    return look;
}

sim::HairSettings hair_motion(const world::Hair& hair) {
    sim::HairSettings s;
    s.root_stiffness = hair.stiffness;
    s.tip_stiffness = hair.stiffness / 8.0f;
    s.damping = hair.damping;
    return s;
}

// A groom being grown: shared with the worker growing it, which leaves
// what the editor keeps of it (the whole groom is the worker's to free).
struct HairState::Grown {
    WaitGroup done;
    std::atomic<bool> cancel{false}; // not wanted any more: a job not yet started does nothing
    Seed seed;
    geometry::GroomParams params;
    bool pack = false; // for a renderer
    // The worker's, read once `done`:
    bool made = false;
    u32 strands = 0;
    float longest = 0.0f;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::vector<Vec3> rest; // its guides' points
    std::shared_ptr<render::PreparedGroom> packed;
};

namespace {

// A grown groom.
struct Coat {
    bool grown = false;
    Seed seed; // what it was grown from
    render::GroomId groom = render::k_no_groom;
    u32 strands = 0;
    float longest = 0.0f;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::vector<Vec3> rest; // its guides' points at rest
};

} // namespace

struct HairState::Worn {
    world::EntityId id = 0;
    world::Hair hair; // as the entity has it
    world::Transform transform;
    Seed wanted;
    Coat coat;                  // shown
    Coat next;                  // grown since, shown once the renderer has it (with `has_next`)
    bool has_next = false;
    std::shared_ptr<Grown> job; // being grown (one at a time)
    std::unique_ptr<sim::HairSim> sim; // of `coat`
    sim::HairFrame frame;       // its next step's
    bool simulated = false;     // in the last update
    u32 idle = 0;               // updates `sim` has not moved
    bool left_out = false;
};

HairState::HairState(render::Renderer* renderer) : m_renderer(renderer) {}

HairState::~HairState() {
    // Jobs not yet started do nothing; those running are waited for.
    for (const auto& [id, worn] : m_worn) {
        if (worn->job != nullptr) worn->job->cancel.store(true);
    }
    for (const auto& [id, worn] : m_worn) {
        if (worn->job != nullptr) m_jobs.wait(worn->job->done);
    }
    for (const std::shared_ptr<Grown>& job : m_abandoned) m_jobs.wait(job->done);
    if (m_renderer != nullptr) {
        for (const auto& [id, worn] : m_worn) {
            if (worn->coat.groom != render::k_no_groom) m_renderer->remove_groom(worn->coat.groom);
            if (worn->next.groom != render::k_no_groom) m_renderer->remove_groom(worn->next.groom);
        }
    }
}

// Everything grown or being grown for it let go.
void HairState::shed(Worn& worn) {
    if (m_renderer != nullptr) {
        if (worn.coat.groom != render::k_no_groom) m_renderer->remove_groom(worn.coat.groom);
        if (worn.next.groom != render::k_no_groom) m_renderer->remove_groom(worn.next.groom);
    }
    if (worn.sim != nullptr) m_resets += worn.sim->stats().resets;
    if (worn.job != nullptr) {
        worn.job->cancel.store(true);
        m_abandoned.push_back(std::move(worn.job)); // (its worker may still be at it)
        worn.job = nullptr;
    }
    worn.sim = nullptr;
    worn.simulated = false;
    worn.idle = 0;
    worn.coat = {};
    worn.next = {};
    worn.has_next = false;
}

void HairState::drop(world::EntityId id) {
    const auto it = m_worn.find(id);
    if (it == m_worn.end()) return;
    shed(*it->second);
    m_worn.erase(it);
}

// Who wears hair and who is left out: lowest ids first, under the bounds.
void HairState::rank() {
    m_left_out = 0;
    usize count = 0;
    u64 strands = 0;
    bool full = false;
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        const u64 own = static_cast<u64>(std::clamp(worn.hair.strands, 1, world::k_max_hair_strands));
        // (Past the first that does not fit all are left out: lowest ids first, strictly.)
        const bool left = full || count >= k_max_hair || strands + own > k_max_strands;
        if (left) {
            full = true;
            ++m_left_out;
            if (!worn.left_out) shed(worn);
        } else {
            ++count;
            strands += own;
        }
        worn.left_out = left;
    }
}

void HairState::grow(Worn& worn) {
    auto job = std::make_shared<Grown>();
    job->seed = worn.wanted;
    job->params = hair_params(worn.hair);
    job->pack = m_renderer != nullptr;
    worn.job = job;
    m_jobs.submit(job->done, [job] {
        if (job->cancel.load()) return;
        const geometry::Groom groom =
            geometry::generate_groom(hair_scalp(job->seed.shape, job->seed.scale, job->seed.top), job->params);
        job->strands = groom.strand_count();
        job->longest = groom.longest;
        job->bounds_min = groom.bounds_min;
        job->bounds_max = groom.bounds_max;
        const usize guide_points = std::min(static_cast<usize>(groom.guide_count) * geometry::k_strand_points, groom.points.size());
        job->rest.assign(groom.points.begin(), groom.points.begin() + static_cast<std::ptrdiff_t>(guide_points));
        if (job->pack && job->strands > 0) job->packed = render::Renderer::prepare_groom(groom);
        job->made = true;
    });
}

// A finished job's groom kept as the next to show, if it is still the one wanted.
void HairState::take(Worn& worn) {
    const std::shared_ptr<Grown> job = std::move(worn.job);
    worn.job = nullptr;
    if (!job->made || !(job->seed == worn.wanted)) return; // (the next is grown instead)
    Coat coat;
    coat.grown = true;
    coat.seed = job->seed;
    coat.strands = job->strands;
    coat.longest = job->longest;
    coat.bounds_min = job->bounds_min;
    coat.bounds_max = job->bounds_max;
    coat.rest = std::move(job->rest);
    if (coat.strands == 0) {
        log_warning("editor", "hair of entity " + std::to_string(worn.id) + ": nothing to grow from (no hair)");
    } else if (m_renderer != nullptr && job->packed != nullptr) {
        coat.groom = m_renderer->add_groom(*job->packed);
        if (coat.groom == render::k_no_groom) {
            log_warning("editor", "hair of entity " + std::to_string(worn.id) + ": the renderer refused its groom");
        }
    }
    if (m_renderer != nullptr && worn.next.groom != render::k_no_groom) m_renderer->remove_groom(worn.next.groom);
    worn.next = std::move(coat);
    worn.has_next = true;
}

// The next groom shown in place of the old: once the renderer has all of
// it (at once when there is nothing to show meanwhile, or when told `now`).
void HairState::swap(Worn& worn, bool now) {
    if (!worn.has_next) return;
    const bool waits = m_renderer != nullptr && worn.next.groom != render::k_no_groom &&
                       worn.coat.groom != render::k_no_groom && !m_renderer->groom_ready(worn.next.groom);
    if (waits && !now) return;
    if (m_renderer != nullptr && worn.coat.groom != render::k_no_groom) m_renderer->remove_groom(worn.coat.groom);
    if (worn.sim != nullptr) m_resets += worn.sim->stats().resets;
    worn.sim = nullptr; // (of the old groom's guides)
    worn.simulated = false;
    worn.idle = 0;
    worn.coat = std::move(worn.next);
    worn.next = {};
    worn.has_next = false;
}

void HairState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !wears(*e)) {
        drop(id);
        return;
    }
    auto it = m_worn.find(id);
    if (it == m_worn.end()) {
        it = m_worn.emplace(id, std::make_unique<Worn>()).first;
        it->second->id = id;
    }
    Worn& worn = *it->second;
    worn.hair = e->hair;
    worn.transform = e->transform;
    worn.wanted = seed_of(*e);
}

void HairState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        m_scene = scene.identity();
        std::vector<world::EntityId> ids;
        ids.reserve(m_worn.size());
        for (const auto& [id, worn] : m_worn) ids.push_back(id);
        for (const world::Entity& e : scene.entities()) {
            if (e.hair.on) ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
    } else if (changed.ids.empty()) {
        return;
    } else {
        for (const world::EntityId id : changed.ids) follow_one(scene, id);
    }
    // Drawn where it is now and as it looks now, this frame; nothing gone is named.
    rank();
    list_draws();
}

namespace {

bool wants_growing(const HairState::Worn& worn) {
    const Coat& latest = worn.has_next ? worn.next : worn.coat;
    return !(latest.grown && latest.seed == worn.wanted);
}

} // namespace

void HairState::finish() {
    rank();
    for (const auto& [id, worn] : m_worn) {
        if (worn->left_out) continue;
        // (A job taken may not be the one wanted any more: until one is.)
        for (int tries = 0; tries < 4; ++tries) {
            if (worn->job != nullptr) {
                m_jobs.wait(worn->job->done);
                take(*worn);
            }
            if (!wants_growing(*worn)) break;
            grow(*worn);
        }
        swap(*worn, true);
    }
    list_draws();
}

void HairState::update(const WindState& wind, const DVec3& camera, double seconds) {
    std::erase_if(m_abandoned, [](const std::shared_ptr<Grown>& job) { return job->done.done(); });
    rank();
    m_near.clear();
    u32 taken = 0;
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        const bool moving = worn.simulated;
        worn.simulated = false;
        if (worn.left_out) continue;
        // (A groom taken is a GPU buffer made: a few an update.)
        if (worn.job != nullptr && taken < k_taken_per_update && worn.job->done.done()) {
            take(worn);
            ++taken;
        }
        swap(worn, false);
        if (worn.job == nullptr && wants_growing(worn)) grow(worn);
        if (!worn.coat.grown || worn.coat.strands == 0) continue;
        if (worn.hair.moves && !worn.coat.rest.empty()) {
            const DVec3 away = worn.transform.position - camera;
            const double distance = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
            // One already moving keeps its place until another is clearly nearer.
            if (distance <= (moving ? k_keep_simulating_within : k_simulate_within)) {
                m_near.emplace_back(moving ? distance * 0.9 : distance, &worn);
            }
        }
    }
    // The nearest move (ties by id, so the same frame is the same choice).
    std::sort(m_near.begin(), m_near.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    m_stepped.clear();
    usize guides = 0;
    u32 made = 0;
    for (const auto& [distance, worn] : m_near) {
        const usize own = worn->coat.rest.size() / geometry::k_strand_points;
        if (m_stepped.size() >= k_max_simulated || (!m_stepped.empty() && guides + own > k_max_simulated_guides)) break;
        if (worn->sim == nullptr) {
            if (made >= k_sims_made_per_update) continue; // (in a later update: it hangs as grown until then)
            ++made;
            worn->sim = std::make_unique<sim::HairSim>(worn->coat.rest, hair_motion(worn->hair));
        } else {
            const sim::HairSettings motion = hair_motion(worn->hair);
            worn->sim->set_motion(motion.root_stiffness, motion.tip_stiffness, motion.damping);
            if (worn->idle > 0) worn->sim->reset(); // (it hung as grown meanwhile: from there)
        }
        guides += own;
        worn->idle = 0;
        worn->frame = {};
        worn->frame.position = worn->transform.position;
        worn->frame.rotation = worn->transform.rotation;
        worn->frame.wind = wind_at(wind, worn->transform.position);
        worn->simulated = true;
        m_stepped.push_back(worn);
    }
    m_simulated = static_cast<u32>(m_stepped.size());
    if (m_stepped.size() == 1) {
        m_stepped[0]->sim->step(seconds, m_stepped[0]->frame);
    } else if (!m_stepped.empty()) {
        // (Three shares: the two workers' and this thread's.)
        m_movers.parallel_for(m_stepped.size(), (m_stepped.size() + 2) / 3, [this, seconds](usize begin, usize end) {
            for (usize i = begin; i < end; ++i) m_stepped[i]->sim->step(seconds, m_stepped[i]->frame);
        });
    }
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        // Out of reach (or told to keep still): it hangs as grown. Its
        // simulation is kept a while, so one coming and going makes no new one each time.
        if (!worn.simulated && worn.sim != nullptr && ++worn.idle > 120) {
            m_resets += worn.sim->stats().resets;
            worn.sim = nullptr;
            worn.idle = 0;
        }
    }
    list_draws();
}

void HairState::list_draws() {
    m_draws.clear();
    for (const auto& [id, held] : m_worn) {
        const Worn& worn = *held;
        if (worn.left_out || worn.coat.groom == render::k_no_groom) continue;
        render::HairDraw draw;
        draw.groom = worn.coat.groom;
        draw.position = worn.transform.position;
        draw.rotation = worn.transform.rotation;
        draw.look = hair_look(worn.hair);
        if (worn.simulated && worn.sim != nullptr) draw.guides = worn.sim->points();
        m_draws.push_back(draw);
    }
}

HairStats HairState::stats() const {
    HairStats s;
    s.left_out = m_left_out;
    s.simulated = m_simulated;
    s.drawn = static_cast<u32>(m_draws.size());
    s.resets = m_resets;
    for (const auto& [id, worn] : m_worn) {
        ++s.wearing;
        if (worn->coat.grown && worn->coat.strands > 0) {
            ++s.grown;
            s.strands += worn->coat.strands;
        }
        if (worn->job != nullptr || worn->has_next) ++s.growing;
        if (worn->sim != nullptr) s.resets += worn->sim->stats().resets;
    }
    return s;
}

bool HairState::view(world::EntityId id, View& out) const {
    const auto it = m_worn.find(id);
    if (it == m_worn.end()) return false;
    const Worn& worn = *it->second;
    const Coat& coat = worn.coat;
    out = {};
    out.grown = coat.grown && coat.strands > 0;
    out.growing = worn.job != nullptr || worn.has_next;
    out.ready = coat.groom != render::k_no_groom && m_renderer != nullptr && m_renderer->groom_ready(coat.groom);
    out.simulated = worn.simulated;
    out.left_out = worn.left_out;
    out.strands = coat.strands;
    out.guides = static_cast<u32>(coat.rest.size() / geometry::k_strand_points);
    out.longest = coat.longest;
    out.bounds_min = coat.bounds_min;
    out.bounds_max = coat.bounds_max;
    out.guide_points = worn.simulated && worn.sim != nullptr ? worn.sim->points() : std::span<const Vec3>(coat.rest);
    return true;
}

} // namespace giant::editor
