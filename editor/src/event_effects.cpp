#include "event_effects.h"

#include "editor_commands.h"
#include "effects_state.h"
#include "physics_state.h"
#include "play_state.h"
#include "weather_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

constexpr float k_rifle_joules = 3000.0f; // a rifle round's energy: the effects' bursts as built
constexpr double k_stride = 1.6;          // metres between the steps that kick dust
constexpr float k_running = 3.0f;         // m/s: slower kicks none

void spawn(EditorState& state, const char* effect, const DVec3& at, const Quat& turn, float bursts) {
    if (effect == nullptr || state.effects == nullptr || !(bursts > 0.0f)) return;
    if (!state.effects->spawn(state.scene, effect, at, turn, 1.0f, std::min(bursts, world::k_max_effect_rate_scale))) {
        ++state.effects->events.refused;
    }
}

} // namespace

HitEffect hit_effect(world::Surface surface, float energy) {
    const float share = energy >= 0.0f ? std::clamp(energy / k_rifle_joules, 0.3f, 2.0f) : 0.3f; // (NaN: the least)
    switch (surface) {
    case world::Surface::metal: return {"sparks", nullptr, share, 0.0f};
    case world::Surface::concrete:
    case world::Surface::brick:
    case world::Surface::standard: return {"debris_chips", "dust_kick", share, 0.5f * share};
    case world::Surface::glass: return {"debris_chips", nullptr, 0.5f * share, 0.0f};
    case world::Surface::wood: return {"splinters", nullptr, share, 0.0f};
    case world::Surface::foliage: return {"leaves", nullptr, 0.15f * share, 0.0f};
    case world::Surface::dirt: return {"dust_kick", "debris_chips", share, 0.3f * share};
    case world::Surface::flesh: return {};
    }
    return {};
}

Quat turn_up_to(const Vec3& n) {
    const float d = n.y;
    if (!std::isfinite(d) || d > 0.99999f) return {};
    if (d < -0.99999f) return {1.0f, 0.0f, 0.0f, 0.0f}; // upside down: half a turn about x
    return normalize(Quat{n.z, 0.0f, -n.x, 1.0f + d});
}

std::optional<DVec3> running_step(double& walked, const sim::CharacterState& player, world::Surface under,
                                  const world::Ground& ground, double seconds) {
    if (!player.on_ground || player.climbing || player.swimming || player.water_depth > 0.05f ||
        player.carrier != sim::k_no_body || under != world::Surface::dirt || !std::isfinite(walked)) {
        walked = 0.0;
        return std::nullopt;
    }
    const float speed = std::sqrt(player.velocity.x * player.velocity.x + player.velocity.z * player.velocity.z);
    if (!std::isfinite(seconds) || !(seconds > 0.0) || !std::isfinite(speed) || !(speed >= k_running) ||
        ground.wetness > 0.3f || ground.snow_cover > 0.2f) {
        return std::nullopt;
    }
    walked += static_cast<double>(speed) * seconds;
    if (walked < k_stride) return std::nullopt;
    walked = std::fmod(walked, k_stride);
    return player.position + DVec3{0.0, 0.05, 0.0};
}

void connect_event_effects(EditorState& state) {
    if (state.physics == nullptr || state.effects == nullptr || state.effects->events_connected) return;
    state.effects->events_connected = true;
    std::function<void(std::span<const PhysicsState::ShotHit>)> before = state.physics->on_hits;
    state.physics->on_hits = [&state, before](std::span<const PhysicsState::ShotHit> hits) {
        if (before) before(hits);
        if (state.effects == nullptr) return;
        for (const PhysicsState::ShotHit& h : hits) {
            const HitEffect fx = hit_effect(h.character ? world::Surface::flesh : h.hit.surface, h.hit.energy);
            if (fx.effect == nullptr) continue;
            ++state.effects->events.hits;
            const Quat turn = turn_up_to(normalize_or_zero(h.hit.normal));
            spawn(state, fx.effect, h.hit.point, turn, fx.bursts);
            spawn(state, fx.second, h.hit.point, turn, fx.second_bursts);
        }
    };
    std::function<void(std::span<const sim::WaterEntry>)> before_splash = state.physics->on_splashes;
    state.physics->on_splashes = [&state, before_splash](std::span<const sim::WaterEntry> entries) {
        if (before_splash) before_splash(entries);
        if (state.effects == nullptr) return;
        for (const sim::WaterEntry& e : entries) {
            ++state.effects->events.splashes;
            // A person jumping in (1 m at 4 m/s) throws the effect as built; a boulder more.
            const float bursts = std::clamp(std::max(e.size, 0.1f) * e.speed / 4.0f, 0.3f, 4.0f);
            spawn(state, "spray", e.point, {}, bursts);
        }
    };
}

void event_effects_frame(EditorState& state, double seconds) {
    if (state.physics == nullptr) return;
    if (state.effects != nullptr) {
        for (const sim::DustEvent& d : state.physics->dust().events()) {
            ++state.effects->events.dust;
            switch (d.source) {
            case sim::DustSource::rubble: spawn(state, "debris_chips", d.at, {}, std::clamp(d.radius / 4.0f, 0.5f, 4.0f)); break;
            case sim::DustSource::tree: spawn(state, "leaves", d.at, {}, std::clamp(d.radius / 4.0f, 0.5f, 2.0f)); break;
            case sim::DustSource::ground:
                spawn(state, "dust_kick", d.at, {}, std::clamp(d.radius / 3.0f, 0.5f, 4.0f));
                spawn(state, "debris_chips", d.at, {}, 0.5f);
                break;
            }
        }
    }
    state.physics->clear_dust_events();
    // The player's feet: running on dry, bare ground kicks a little dust every stride.
    if (state.effects == nullptr || state.play == nullptr || !state.play->playing() || !(seconds > 0.0)) return;
    const std::optional<sim::CharacterState> p = state.play->player();
    if (!p) {
        state.effects->walked = 0.0;
        return;
    }
    const world::Ground ground = state.weather != nullptr ? state.weather->ground() : world::Ground{};
    sim::RayHit stood;
    stood.body = p->ground; // (none: the ground's tiles)
    const world::Surface under = state.physics->surface_of(stood);
    const std::optional<DVec3> step = running_step(state.effects->walked, *p, under, ground, seconds);
    if (!step) return;
    ++state.effects->events.steps;
    spawn(state, "dust_kick", *step, {}, 0.25f);
}

} // namespace giant::editor
