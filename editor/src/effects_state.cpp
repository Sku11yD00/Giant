#include "effects_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

// A well-mixed 32-bit hash (lowbias32).
u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

u32 seed_of(u64 key, u32 emitter, u64 update, u32 part) {
    u32 h = mix(static_cast<u32>(key) ^ 0x9e3779b9u);
    h = mix(h ^ static_cast<u32>(key >> 32));
    h = mix(h ^ (emitter * 0x85ebca6bu));
    h = mix(h ^ static_cast<u32>(update));
    h = mix(h ^ static_cast<u32>(update >> 32));
    return mix(h ^ (part * 0xc2b2ae35u));
}

// 0 to 1 from a hash.
float unit(u32 h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

constexpr double k_no_ground = -1.0e30;
constexpr double k_most_seconds = 1.0; // a long frame owes no more than this of its rates (as the particles' clock)
constexpr float k_most_carry = 1.0e6f;

DVec3 plus(const DVec3& a, const Vec3& b) {
    return {a.x + static_cast<double>(b.x), a.y + static_cast<double>(b.y), a.z + static_cast<double>(b.z)};
}

} // namespace

render::ParticleStyle to_style(const world::ParticleLook& l) {
    render::ParticleStyle s;
    s.life_min = l.life_min;
    s.life_max = l.life_max;
    s.size_start = l.size_start;
    s.size_end = l.size_end;
    s.color_start = l.color_start;
    s.color_end = l.color_end;
    s.opacity_start = l.opacity_start;
    s.opacity_end = l.opacity_end;
    s.emissive = l.emissive;
    s.emissive_end = l.emissive_end;
    s.gravity = l.gravity;
    s.buoyancy = l.buoyancy;
    s.drag = l.drag;
    s.wind = l.wind;
    s.wobble = l.wobble;
    s.shape = l.form == world::ParticleForm::streak ? render::ParticleShape::streak : render::ParticleShape::puff;
    s.lit = l.lit;
    s.soft = l.soft;
    return s;
}

EffectsState::EffectsState() {
    m_bursts.reserve(k_max_bursts);
    m_styles.reserve(render::k_max_particle_styles);
    m_spawned.reserve(k_max_spawned);
    m_jobs.reserve(k_max_spawned);
}

void EffectsState::clear() {
    m_runners.clear();
    m_spawned.clear();
    m_bursts.clear();
    m_jobs.clear();
    m_stats = {};
    m_born = 0;
    m_grounded = 0;
    m_taken = true;
    m_scene = 0; // the next follow looks over every entity again
    m_spawn_scene = 0;
    walked = 0.0;
}

void EffectsState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !e->visible || !e->effect.on) {
        m_runners.erase(id);
        return;
    }
    m_runners.try_emplace(id);
}

void EffectsState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        if (scene.identity() != m_scene) {
            m_runners.clear(); // another scene's: nothing carries over (the same ids are other things)
            m_scene = scene.identity();
        }
        spawned_in(scene);
        std::vector<world::EntityId> ids;
        for (const auto& [id, r] : m_runners) ids.push_back(id);
        for (const world::Entity& e : scene.entities()) {
            if (e.effect.on) ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
    } else {
        for (const world::EntityId id : changed.ids) follow_one(scene, id);
    }
    if (m_jobs.capacity() < m_runners.size() + k_max_spawned) m_jobs.reserve(m_runners.size() + k_max_spawned);
}

void EffectsState::spawned_in(const world::Scene& scene) {
    if (scene.identity() != m_spawn_scene) {
        m_spawned.clear();
        m_spawn_scene = scene.identity();
    }
}

void EffectsState::restyle(const world::Scene& scene) {
    m_looks = scene.particle_looks;
    // The looks there now: the scene's, then the built-ins they do not stand in for.
    std::vector<const world::ParticleLook*> now;
    for (const world::ParticleLook& l : scene.particle_looks) now.push_back(&l);
    for (const world::ParticleLook& l : world::built_in_particle_looks()) {
        if (world::find_particle_look(scene.particle_looks, l.name) == &l) now.push_back(&l);
    }
    // Names gone: their slots kept (alive particles keep their look), marked.
    for (auto& [name, slot] : m_style_index) {
        const bool there = std::any_of(now.begin(), now.end(), [&](const world::ParticleLook* l) { return l->name == name; });
        if (!there && m_style_gone[slot] == 0) m_style_gone[slot] = m_updates + 1;
    }
    for (const world::ParticleLook* l : now) {
        if (const auto it = m_style_index.find(l->name); it != m_style_index.end()) {
            m_styles[it->second] = to_style(*l); // changed in place
            m_style_gone[it->second] = 0;
            continue;
        }
        u32 slot = static_cast<u32>(m_styles.size());
        if (slot >= render::k_max_particle_styles) {
            // Full: the slot of the name gone longest ago.
            u64 oldest = 0;
            for (u32 k = 0; k < m_style_gone.size(); ++k) {
                if (m_style_gone[k] != 0 && (oldest == 0 || m_style_gone[k] < oldest)) {
                    oldest = m_style_gone[k];
                    slot = k;
                }
            }
            if (oldest == 0) continue; // (cannot happen: at most 200 looks are there at once)
            std::erase_if(m_style_index, [&](const auto& entry) { return entry.second == slot; });
            m_styles[slot] = to_style(*l);
            m_style_gone[slot] = 0;
        } else {
            m_styles.push_back(to_style(*l));
            m_style_gone.push_back(0);
        }
        m_style_index.emplace(l->name, slot);
    }
}

std::optional<u32> EffectsState::style_index(std::string_view look) const {
    const auto it = m_style_index.find(look); // (no string made: a transparent hash)
    if (it == m_style_index.end() || m_style_gone[it->second] != 0) return std::nullopt;
    return it->second;
}

void EffectsState::reindex_effects(const world::Scene& scene) {
    m_effects = scene.effects;
    m_effect_index.clear();
    const auto builtins = world::built_in_effects();
    for (usize i = builtins.size(); i-- > 0;) m_effect_index[builtins[i].name] = -1 - static_cast<i32>(i);
    for (usize i = scene.effects.size(); i-- > 0;) m_effect_index[scene.effects[i].name] = static_cast<i32>(i); // stand in
}

const world::Effect* EffectsState::effect_named(const world::Scene& scene, const std::string& name) const {
    const auto it = m_effect_index.find(name);
    if (it == m_effect_index.end()) return nullptr;
    if (it->second >= 0) {
        const usize i = static_cast<usize>(it->second);
        return i < scene.effects.size() ? &scene.effects[i] : nullptr;
    }
    const usize i = static_cast<usize>(-1 - it->second);
    const auto builtins = world::built_in_effects();
    return i < builtins.size() ? &builtins[i] : nullptr;
}

std::optional<u32> EffectsState::spawn(const world::Scene& scene, std::string_view name, const DVec3& position,
                                       const Quat& rotation, float rate, float bursts) {
    spawned_in(scene);
    const world::Effect* effect = world::find_effect(scene.effects, name);
    if (effect == nullptr || m_spawned.size() >= k_max_spawned) return std::nullopt;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return std::nullopt;
    if (!std::isfinite(rate) || rate < 0.0f || rate > world::k_max_effect_rate_scale) return std::nullopt;
    if (!std::isfinite(bursts) || bursts < 0.0f || bursts > world::k_max_effect_rate_scale) return std::nullopt;
    Spawned s;
    s.id = m_next_spawn++;
    if (m_next_spawn == 0) m_next_spawn = 1;
    s.effect = *effect;
    s.position = position;
    s.rotation = rotation;
    s.rate = rate;
    s.burst_scale = bursts;
    s.left = effect->duration;
    m_spawned.push_back(std::move(s));
    return m_spawned.back().id;
}

void EffectsState::update(const world::Scene& scene, const DVec3& camera, double seconds, const Ground& ground) {
    if (m_styles.empty() || !(scene.particle_looks == m_looks)) restyle(scene);
    if (m_effect_index.empty() || !(scene.effects == m_effects)) reindex_effects(scene);
    spawned_in(scene);
    if (m_taken) { // (else the last update's, not drawn, are given with these)
        m_bursts.clear();
        m_born = 0;
        m_grounded = 0;
        m_taken = false;
    }
    m_stats = {};
    if (!std::isfinite(seconds) || seconds <= 0.0) { // paused: nothing born, nothing spent
        m_stats.spawned = static_cast<u32>(m_spawned.size());
        return;
    }
    seconds = std::min(seconds, k_most_seconds);
    ++m_updates;
    const double reach2 = reach * reach;
    const auto distance2 = [&](const DVec3& p) {
        const double dx = p.x - camera.x, dy = p.y - camera.y, dz = p.z - camera.z;
        return dx * dx + dy * dy + dz * dz;
    };
    m_jobs.clear();
    for (auto& [id, runner] : m_runners) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible || !e->effect.on) continue; // (followed next frame)
        const double d2 = distance2(e->transform.position);
        if (!(d2 <= reach2)) {
            ++m_stats.far;
            continue;
        }
        const world::Effect* effect = effect_named(scene, e->effect.effect);
        if (effect == nullptr) {
            ++m_stats.missing;
            continue;
        }
        if (m_jobs.size() == m_jobs.capacity()) break; // (reserved by follow; never grown here)
        Job j;
        j.distance2 = d2;
        j.effect = effect;
        j.position = e->transform.position;
        j.rotation = e->transform.rotation;
        j.rate = e->effect.rate;
        j.rate_seconds = seconds;
        j.carry = runner.carry.data();
        j.key = id;
        m_jobs.push_back(j);
    }
    for (Spawned& s : m_spawned) {
        const double d2 = distance2(s.position);
        if (!(d2 <= reach2) || m_jobs.size() == m_jobs.capacity()) continue; // (its time still runs out)
        Job j;
        j.distance2 = d2;
        j.effect = &s.effect;
        j.position = s.position;
        j.rotation = s.rotation;
        j.rate = s.rate;
        j.carry = s.carry.data();
        j.key = (u64{1} << 63) | s.id; // apart from entity ids (under 2^53)
        j.bursts = s.burst;
        j.burst_scale = s.burst_scale;
        j.rates = s.left > 0.0;
        j.rate_seconds = std::min(seconds, s.left);
        m_jobs.push_back(j);
    }
    std::sort(m_jobs.begin(), m_jobs.end(), [](const Job& a, const Job& b) {
        return a.distance2 != b.distance2 ? a.distance2 < b.distance2 : a.key < b.key;
    });
    for (const Job& j : m_jobs) run(j, ground);
    for (Spawned& s : m_spawned) {
        s.burst = false;
        s.left -= seconds;
    }
    std::erase_if(m_spawned, [](const Spawned& s) { return !s.burst && s.left <= 0.0; });
    m_stats.spawned = static_cast<u32>(m_spawned.size());
    m_stats.bursts = static_cast<u32>(m_bursts.size());
}

void EffectsState::run(const Job& job, const Ground& ground) {
    const u32 born_before = m_stats.born + m_stats.cut;
    for (usize i = 0; i < job.effect->emitters.size() && i < world::k_max_emitters; ++i) {
        const world::Emitter& e = job.effect->emitters[i];
        u32 count = 0;
        if (job.rates && e.rate > 0.0f && job.rate > 0.0f) {
            float& carry = job.carry[i];
            carry = std::min(carry + static_cast<float>(static_cast<double>(e.rate) * job.rate * job.rate_seconds), k_most_carry);
            const float whole = std::floor(carry);
            carry -= whole;
            count = static_cast<u32>(whole);
        }
        if (job.bursts) {
            const double scaled = std::round(static_cast<double>(e.burst) * job.burst_scale);
            count += static_cast<u32>(std::min(scaled, static_cast<double>(world::k_max_emitter_burst)));
        }
        if (count == 0) continue;
        const std::optional<u32> style = style_index(e.look);
        if (!style) {
            ++m_stats.missing;
            continue;
        }
        emit(e, *style, count, job, static_cast<u32>(i), ground);
    }
    if (m_stats.born + m_stats.cut != born_before) ++m_stats.running;
}

void EffectsState::emit(const world::Emitter& e, u32 style, u32 count, const Job& job, u32 emitter,
                        const Ground& ground) {
    const DVec3 origin = plus(job.position, rotate(job.rotation, e.offset));
    const bool scattered = e.shape == world::EmitterShape::box || e.shape == world::EmitterShape::disc;
    // Where they stop: the ground under them (a box or disc's under each of
    // its points; a sphere's under its middle, less its radius), or where they
    // were born when that is under it. No ground there: they fall on.
    const auto floor_at = [&](const DVec3& p, double below) -> std::optional<double> {
        if (!e.ground || !ground) return std::nullopt;
        const std::optional<double> g = ground(p.x, p.z);
        if (!g) return std::nullopt;
        return std::min(*g, p.y - below) - 0.01;
    };
    const float spread = e.shape == world::EmitterShape::sphere ? e.size.x : 0.0f;
    const std::optional<double> middle_floor = scattered ? std::nullopt : floor_at(origin, spread);
    // A box or disc's points, and whether any lands (the grounded budget counts those).
    struct Part {
        DVec3 position;
        std::optional<double> floor;
        u32 seed;
    };
    std::array<Part, k_max_bursts_per_emitter> parts{};
    const u32 bursts_left = k_max_bursts - static_cast<u32>(m_bursts.size());
    u32 part_count = scattered ? std::min({count, k_max_bursts_per_emitter, bursts_left}) : std::min(1u, bursts_left);
    bool grounded = middle_floor.has_value();
    if (scattered) {
        for (u32 p = 0; p < part_count; ++p) {
            const u32 seed = seed_of(job.key, emitter, m_updates, p + 1);
            const float u = unit(mix(seed ^ 0x1u)), v = unit(mix(seed ^ 0x2u)), w = unit(mix(seed ^ 0x3u));
            Vec3 local;
            if (e.shape == world::EmitterShape::box) {
                local = {(2.0f * u - 1.0f) * e.size.x, (2.0f * v - 1.0f) * e.size.y, (2.0f * w - 1.0f) * e.size.z};
            } else {
                const float r = e.size.x * std::sqrt(u), a = 6.2831853f * v;
                local = {r * std::cos(a), 0.0f, r * std::sin(a)};
            }
            parts[p].position = plus(origin, rotate(job.rotation, local));
            parts[p].floor = floor_at(parts[p].position, 0.0);
            parts[p].seed = seed;
            grounded = grounded || parts[p].floor.has_value();
        }
    }
    u32 allowed = std::min(count, births_per_frame - std::min(m_born, births_per_frame));
    if (grounded) allowed = std::min(allowed, grounded_per_frame - std::min(m_grounded, grounded_per_frame));
    if (part_count == 0) allowed = 0;
    part_count = std::min(part_count, allowed);
    m_stats.cut += count - allowed;
    if (allowed == 0) return;
    m_born += allowed;
    if (grounded) m_grounded += allowed;
    m_stats.born += allowed;

    render::ParticleBurst b;
    b.style = style;
    b.direction = rotate(job.rotation, normalize_or_zero(e.direction)); // (a heading: no length lost to rounding)
    b.cone_degrees = e.cone_degrees;
    b.speed_min = e.speed_min;
    b.speed_max = e.speed_max;
    b.size_scale = e.size_scale;
    if (!scattered) {
        b.count = allowed;
        b.position = origin;
        b.spread = spread;
        b.ground = middle_floor.value_or(k_no_ground);
        b.seed = seed_of(job.key, emitter, m_updates, 0);
        m_bursts.push_back(b);
        return;
    }
    // Box and disc: each part its own point (spread 0), the count shared out.
    for (u32 p = 0; p < part_count; ++p) {
        b.count = allowed / part_count + (p < allowed % part_count ? 1u : 0u);
        b.position = parts[p].position;
        b.spread = 0.0f;
        b.ground = parts[p].floor.value_or(k_no_ground);
        b.seed = parts[p].seed;
        m_bursts.push_back(b);
    }
}

} // namespace giant::editor
