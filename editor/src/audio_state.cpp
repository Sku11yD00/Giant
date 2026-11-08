#include "audio_state.h"

#include "editor_commands.h"
#include "physics_state.h"

#include "giant/audio/sounds.h"
#include "giant/core/log.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

constexpr u32 k_march_steps = 1536;
constexpr double k_lift = 0.05; // a terrain sample this far under the ray still counts as under it

DVec3 along(const DVec3& from, const Vec3& d, double t) { return from + DVec3{d.x, d.y, d.z} * t; }

// Ground steeper is harder (bare rock on a cliff, soil on the flat).
float slope_hardness(float base, const Vec3& normal) {
    const float steep = std::clamp(1.0f - std::fabs(normal.y), 0.0f, 1.0f);
    return std::clamp(base + 0.7f * steep, 0.0f, 0.85f);
}

Vec3 terrain_normal(const world::Terrain& terrain, double x, double z) {
    const double e = std::max(terrain.spacing(), 0.25);
    // A slope from the sides there are (one-sided at the terrain's edge; flat with neither).
    const auto slope = [&](double ax, double az, double bx, double bz) {
        const std::optional<double> a = terrain.height_at(ax, az), b = terrain.height_at(bx, bz);
        const std::optional<double> c = terrain.height_at(x, z);
        if (a && b) return (*b - *a) / (2.0 * e);
        if (b && c) return (*b - *c) / e;
        if (a && c) return (*c - *a) / e;
        return 0.0;
    };
    const double dx = slope(x - e, z, x + e, z), dz = slope(x, z - e, x, z + e);
    DVec3 n{-dx, 1.0, -dz};
    n = n / std::sqrt(dot(n, n));
    return {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
}

audio::RayHit ground_hit(const world::Terrain& terrain, const DVec3& from, const Vec3& direction, double t) {
    const DVec3 p = along(from, direction, t);
    audio::RayHit hit;
    hit.hit = true;
    hit.distance = t;
    hit.normal = terrain_normal(terrain, p.x, p.z);
    if (dot(hit.normal, direction) > 0.0f && t > 0.0) hit.normal = -hit.normal;
    hit.hardness = slope_hardness(hardness_of(world::Surface::dirt), hit.normal);
    return hit;
}

} // namespace

float hardness_of(world::Surface surface) {
    switch (surface) {
    case world::Surface::flesh: return 0.1f;
    case world::Surface::foliage: return 0.1f;
    case world::Surface::dirt: return 0.3f;
    case world::Surface::wood: return 0.5f;
    case world::Surface::standard: return 0.6f;
    case world::Surface::brick: return 0.8f;
    case world::Surface::concrete: return 0.9f;
    case world::Surface::glass: return 0.9f;
    case world::Surface::metal: return 0.95f;
    }
    return 0.5f;
}

std::optional<audio::RayHit> march_terrain(const world::Terrain& terrain, const DVec3& from, const Vec3& direction,
                                           double max_distance) {
    if (!std::isfinite(max_distance) || max_distance <= 0.0) return std::nullopt;
    // Above the ground by how much at t (none: no tile there).
    const auto above = [&](double t) -> std::optional<double> {
        const DVec3 p = along(from, direction, t);
        const std::optional<double> h = terrain.height_at(p.x, p.z);
        if (!h) return std::nullopt;
        return p.y - *h;
    };
    const double base_step = std::max(terrain.spacing(), 0.5);
    double t = 0.0;
    std::optional<double> prev = above(0.0);
    // Starting under the ground: met at once. Just under (rounding): as on it.
    if (prev && *prev < -k_lift) return ground_hit(terrain, from, direction, 0.0);
    if (prev && *prev < 0.0) prev = 0.0;
    for (u32 k = 0; k < k_march_steps && t < max_distance; ++k) {
        // Steps grow with the distance (0.5% of it, up to 2% high over the ground) and with the
        // height over the ground (half of it: slopes to 63 degrees are not stepped over).
        const double clear = prev ? std::max(*prev, 0.0) : 0.0;
        const double step = std::clamp(std::max(clear * 0.5, t * 0.005), base_step, std::max(base_step, t * 0.02));
        const double next_t = std::min(t + step, max_distance);
        const std::optional<double> next = above(next_t);
        if (next && *next < 0.0) {
            double lo = t, hi = next_t;
            if (prev) {
                // Crossed between t and next_t: halve onto it.
                for (int i = 0; i < 16; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    const std::optional<double> m = above(mid);
                    if (m && *m < 0.0) hi = mid;
                    else lo = mid;
                }
            } else {
                // Into a tile, under its ground, from where there was none: met at the tile's edge.
                for (int i = 0; i < 16; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    if (above(mid)) hi = mid;
                    else lo = mid;
                }
            }
            return ground_hit(terrain, from, direction, hi);
        }
        prev = next;
        t = next_t;
    }
    return std::nullopt;
}

AudioState::AudioState() {
    m_names["gunshot"] = m_mixer.add_sound(audio::synth_gunshot());
    m_names["crack"] = m_mixer.add_sound(audio::synth_crack());
    m_names["click"] = m_mixer.add_sound(audio::synth_click());
    m_names["tone"] = m_mixer.add_sound(audio::synth_tone(440.0f, 1.0f));
    m_scratch.resize(static_cast<usize>(audio::k_sample_rate / 4) * 2);
}

AudioState::~AudioState() { m_output.reset(); }

bool AudioState::open_device() {
    if (m_output != nullptr) return true;
    m_output = platform::AudioOutput::open(audio::k_sample_rate,
                                           [this](float* out, u32 frames) { m_mixer.render(out, frames); });
    if (m_output != nullptr) log_info("audio", "playing on " + m_output->device());
    return m_output != nullptr;
}

std::string AudioState::device_name() const { return m_output != nullptr ? m_output->device() : std::string(); }

std::optional<audio::SoundId> AudioState::find(const std::string& name) const {
    const auto it = m_names.find(name);
    if (it == m_names.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> AudioState::names() const {
    std::vector<std::string> out;
    for (const auto& [name, id] : m_names) out.push_back(name);
    return out;
}

std::string AudioState::load(const std::string& name, const std::string& path) {
    if (name.empty()) return "no name";
    if (m_names.contains(name)) return "a sound is already called " + name;
    if (m_names.size() >= k_max_sounds) return "too many sounds";
    std::optional<audio::Sound> sound = audio::load_wav(path);
    if (!sound) return "not a WAV file it reads (16-bit PCM or 32-bit float): " + path;
    if (sound->samples.empty()) return "the file holds no sound: " + path;
    m_names[name] = m_mixer.add_sound(std::move(*sound));
    return {};
}

void AudioState::update(const EditorCamera& camera, double seconds) {
    m_listener = camera.position;
    const float yaw = degrees_to_radians(camera.yaw_degrees);
    m_right = {std::cos(yaw), 0.0f, -std::sin(yaw)}; // yaw 0 looks down -Z: right is +X
    m_up = {0.0f, 1.0f, 0.0f};
    m_mixer.set_listener(m_right, m_up);
    if (m_output != nullptr) return;
    // No speakers: the mix moves on with the frames (and is dropped).
    const double s = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 0.25) : 0.0;
    const u32 frames = static_cast<u32>(s * audio::k_sample_rate);
    if (frames > 0) m_mixer.render(m_scratch.data(), frames);
}

audio::RayCast AudioState::ray_cast(const world::Scene& scene, const PhysicsState* physics) const {
    const double reach = physics_reach;
    return [&scene, physics, reach](const DVec3& from, const Vec3& dir, double max_distance) {
        audio::RayHit best;
        if (physics != nullptr) {
            // Characters are let through: the listener stands in the player's capsule, a shot is made in it.
            const sim::PhysicsWorld& w = physics->world();
            sim::PhysicsWorld::RayFilter filter;
            filter.accept = [&w](sim::BodyId id, u64) { return !w.is_character_body(id); };
            const auto hit = w.raycast(from, dir, std::min(max_distance, reach), filter);
            if (hit) {
                best.hit = true;
                best.distance = hit->distance;
                best.normal = hit->normal;
                if (dot(best.normal, dir) > 0.0f) best.normal = -best.normal;
                const world::Surface surface = physics->surface_of(*hit);
                best.hardness = surface == world::Surface::dirt ? slope_hardness(hardness_of(surface), best.normal)
                                                                : hardness_of(surface);
            }
        }
        // The terrain, past what the physics holds (or nearer, where its ground is not made yet).
        const double terrain_reach = best.hit ? best.distance : max_distance;
        if (const auto ground = march_terrain(scene.terrain, from, dir, terrain_reach))
            if (!best.hit || ground->distance < best.distance) best = *ground;
        return best;
    };
}

audio::Heard AudioState::play(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound,
                              const DVec3& at, float loudness, const audio::EchoSettings& settings) {
    const audio::Heard heard = audio::trace(at, m_listener, ray_cast(scene, physics), settings);
    m_mixer.play(audio::to_play(sound, heard, loudness));
    ++m_played;
    m_last = heard;
    return heard;
}

AudioState::Recording AudioState::record(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound,
                                         const DVec3& at, float loudness, const audio::EchoSettings& settings,
                                         double seconds) {
    Recording out;
    out.heard = audio::trace(at, m_listener, ray_cast(scene, physics), settings);
    const audio::Sound* source = m_mixer.sound(sound);
    if (source == nullptr) return out;
    audio::Mixer alone;
    alone.set_listener(m_right, m_up);
    const audio::SoundId id = alone.add_sound(*source);
    alone.play(audio::to_play(id, out.heard, loudness));
    const double s = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 60.0) : 0.0;
    out.frames.assign(static_cast<usize>(s * audio::k_sample_rate) * 2, 0.0f);
    const usize frames = out.frames.size() / 2;
    for (usize done = 0; done < frames;) {
        const u32 n = static_cast<u32>(std::min<usize>(4096, frames - done));
        alone.render(out.frames.data() + done * 2, n);
        done += n;
    }
    for (usize k = 0; k < out.frames.size(); ++k) {
        const float a = std::fabs(out.frames[k]);
        out.peak = std::max(out.peak, a);
        if (out.first_heard < 0.0 && a > 1e-4f)
            out.first_heard = static_cast<double>(k / 2) / static_cast<double>(audio::k_sample_rate);
    }
    return out;
}

} // namespace giant::editor
