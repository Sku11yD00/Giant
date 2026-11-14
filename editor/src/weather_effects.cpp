#include "weather_effects.h"

#include "giant/sim/physics.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

// A well-mixed 32-bit hash (lowbias32), and 0 to 1 from one.
u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float unit(u32 h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

u32 seed_of(u64 count, u32 kind) {
    return mix(mix(static_cast<u32>(count) ^ (kind * 0x9e3779b9u)) ^ static_cast<u32>(count >> 32));
}

// A point in a disc of `radius` round `centre` (x, z), from a seed.
void in_disc(u32 seed, double cx, double cz, double radius, double& x, double& z) {
    const double r = radius * std::sqrt(static_cast<double>(unit(mix(seed ^ 0x51u))));
    const double a = 6.283185307179586 * static_cast<double>(unit(mix(seed ^ 0x52u)));
    x = cx + r * std::cos(a);
    z = cz + r * std::sin(a);
}

// Owed a whole number this frame: the fraction carried, at most `most` (the rest dropped, not owed).
u32 owed(double& carry, double rate, double seconds, u32 most) {
    carry += rate * seconds;
    const double whole = std::floor(carry);
    carry -= whole;
    return static_cast<u32>(std::min(whole, static_cast<double>(most)));
}

} // namespace

float WeatherEffects::rain(const world::Weather& w) { return world::falling(w).rain; }

std::optional<double> WeatherEffects::surface_in(const sim::PhysicsWorld& physics, double x, double z, double from_y,
                                                 double distance) {
    if (!(distance > 0.0)) return std::nullopt;
    const std::optional<sim::RayHit> hit = physics.raycast({x, from_y, z}, {0.0f, -1.0f, 0.0f}, distance);
    if (!hit) return std::nullopt;
    return hit->point.y;
}

bool WeatherEffects::sheltered_in(const sim::PhysicsWorld& physics, const DVec3& point) {
    return physics.raycast({point.x, point.y + 0.05, point.z}, {0.0f, 1.0f, 0.0f}, 100.0).has_value();
}

std::optional<double> WeatherEffects::water_in(const sim::PhysicsWorld& physics, double x, double z) {
    const std::optional<world::WaterHere> w = physics.water_at(x, z);
    if (!w) return std::nullopt;
    return static_cast<double>(w->level);
}

float WeatherEffects::storm(const WindState& wind) {
    const float speed = std::sqrt(wind.velocity.x * wind.velocity.x + wind.velocity.z * wind.velocity.z);
    const float s = (speed - k_storm_wind) / (k_gale_wind - k_storm_wind);
    return std::isfinite(s) ? std::clamp(s, 0.0f, 1.0f) : 0.0f;
}

void WeatherEffects::update(EffectsState& effects, const std::optional<world::Weather>& shown,
                            const world::Ground& ground_state, const WindState& wind, const DVec3& camera,
                            double seconds, const Ground& ground, const Water& water, const Surface& surface,
                            const Sheltered& sheltered) {
    m_stats = {};
    if (!std::isfinite(seconds) || seconds <= 0.0 || !ground) return; // paused: none (and nothing owed)
    seconds = std::min(seconds, 1.0);
    const float snow = std::clamp(ground_state.snow_cover, 0.0f, 1.0f);
    const float wet = std::clamp(ground_state.wetness, 0.0f, 1.0f);

    // Rain splashing on what it falls on: the first surface under it from
    // above the camera, else the terrain (none on snow lying past half cover).
    const float raining = shown ? rain(*shown) * std::clamp(1.0f - 2.0f * snow, 0.0f, 1.0f) : 0.0f;
    const u32 splashes = owed(m_splash_carry, k_splash_rate * raining, seconds, k_max_splashes);
    for (u32 i = 0; i < splashes; ++i) {
        const u32 seed = seed_of(m_count++, 1);
        double x = 0.0, z = 0.0;
        in_disc(seed, camera.x, camera.z, k_splash_radius, x, z);
        const std::optional<double> terrain = ground(x, z);
        const bool terrain_ok = terrain && std::isfinite(*terrain);
        // Deep under the ground (a tunnel, a cave) or flown high over it: none.
        if (terrain_ok && (*terrain > camera.y + k_splash_under || camera.y - *terrain > k_splash_over)) continue;
        // Looked down on from high over the camera and the ground there (a tall hall's roof, a high deck).
        const double top = std::max(camera.y, terrain_ok ? *terrain : camera.y) + k_splash_top;
        const double bottom = terrain_ok ? *terrain - 1.0 : camera.y - k_splash_over;
        std::optional<double> y = surface ? surface(x, z, top, top - bottom) : std::nullopt;
        if (y && !std::isfinite(*y)) y.reset();
        if (y && *y >= top - 1e-4) continue; // (begun inside something tall: blocked)
        if (terrain_ok) y = y ? std::max(*y, *terrain) : *terrain; // (not on what lies buried)
        if (!y) continue;
        if (water) {
            const std::optional<double> level = water(x, z);
            if (level && *level >= *y - 0.05) {
                ++m_stats.on_water;
                continue;
            }
        }
        render::ParticleBurst b;
        b.count = 3;
        b.position = {x, *y + 0.02, z};
        b.direction = {0.0f, 1.0f, 0.0f};
        b.cone_degrees = 50.0f;
        b.speed_min = 0.8f;
        b.speed_max = 1.6f;
        b.ground = *y - 0.005;
        b.seed = seed;
        if (effects.add_burst("rain_splash", b)) {
            ++m_stats.splashes;
        } else {
            ++m_stats.cut;
        }
    }

    // A storm: leaves and grit blown along from upwind (grit off dry, bare ground).
    const float s = storm(wind);
    if (s > 0.0f) {
        const float speed = std::sqrt(wind.velocity.x * wind.velocity.x + wind.velocity.z * wind.velocity.z);
        const double cx = camera.x - static_cast<double>(wind.velocity.x / speed) * k_upwind;
        const double cz = camera.z - static_cast<double>(wind.velocity.z / speed) * k_upwind;
        const float dry = (1.0f - wet) * (1.0f - snow);
        const u32 leaves = owed(m_leaf_carry, k_leaf_rate * s, seconds, k_max_debris);
        const u32 grit = owed(m_grit_carry, k_grit_rate * s * dry, seconds, k_max_debris);
        const double dx = static_cast<double>(wind.velocity.x / speed), dz = static_cast<double>(wind.velocity.z / speed);
        const auto blow = [&](u32 seed, const char* look, u32 count, float spread, float low, float high,
                              u32& given) {
            double x = 0.0, z = 0.0;
            in_disc(seed, cx, cz, k_debris_radius, x, z);
            const std::optional<double> terrain = ground(x, z);
            if (!terrain || !std::isfinite(*terrain)) return;
            // Off the top of what lies on the ground there (a slab, a road), else the terrain.
            std::optional<double> y = terrain;
            if (surface) {
                const double from = *terrain + k_debris_top;
                const std::optional<double> hit = surface(x, z, from, k_debris_top + 1.0);
                if (hit && std::isfinite(*hit) && *hit < from - 1e-4) y = std::max(*hit, *terrain);
            }
            if (water) {
                const std::optional<double> level = water(x, z);
                if (level && *level >= *y - 0.05) return; // (over water: none)
            }
            if (sheltered && sheltered({x, *y, z})) { // indoors, under a bridge: no wind there
                ++m_stats.sheltered;
                return;
            }
            // Where they stop: the lowest ground along their way downwind, as
            // far as the wind takes them in a leaf's life (they may sink from
            // sight over a rise, never rest in the air over a fall).
            double floor = std::min(*y, *terrain);
            for (const double t : {0.5, 1.5, 3.0, 5.0, 7.0}) {
                const double along = static_cast<double>(speed) * t;
                const std::optional<double> g = ground(x + dx * along, z + dz * along);
                if (g && std::isfinite(*g)) floor = std::min(floor, *g);
            }
            render::ParticleBurst b;
            b.count = count;
            b.spread = spread;
            const float h = low + (high - low) * unit(mix(seed ^ 0x53u));
            b.position = {x, *y + static_cast<double>(h), z};
            b.direction = {0.0f, 1.0f, 0.0f};
            b.cone_degrees = 180.0f;
            b.speed_min = 0.2f;
            b.speed_max = 1.0f;
            b.velocity = {wind.velocity.x * 0.8f, 0.0f, wind.velocity.z * 0.8f}; // already moving with it
            b.ground = floor - 0.005;
            b.seed = seed;
            if (effects.add_burst(look, b)) {
                ++given;
            } else {
                ++m_stats.cut;
            }
        };
        for (u32 i = 0; i < leaves; ++i) blow(seed_of(m_count++, 2), "leaves", 1, 0.3f, 0.3f, 2.5f, m_stats.leaves);
        // (Grit's spread within its lowest height: none born under the ground.)
        for (u32 i = 0; i < grit; ++i) blow(seed_of(m_count++, 3), "grit", 4, 0.15f, 0.2f, 0.7f, m_stats.grit);
    }
}

} // namespace giant::editor
