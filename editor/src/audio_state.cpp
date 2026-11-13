#include "audio_state.h"

#include "editor_commands.h"
#include "physics_state.h"

#include "giant/audio/sounds.h"
#include "giant/core/log.h"

#include <algorithm>
#include <array>
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

WaterHeard WaterHearing::update(const WaterLookup& water, const GroundLookup& ground, const DVec3& listener, float wind,
                                const std::function<bool(double level)>& reached, u32 ways) {
    WaterHeard out;
    if (!water || !std::isfinite(listener.x) || !std::isfinite(listener.y) || !std::isfinite(listener.z)) return out;
    constexpr std::array<double, k_water_steps + 1> k_steps{0.0,  1.0,  2.0,  3.0,  4.5,  6.5,  9.0,
                                                            12.0, 16.0, 20.0, 25.0, 31.0, 39.0, k_water_heard_reach};
    const float surf = std::isfinite(wind) ? std::clamp(wind / 8.0f, 0.15f, 1.0f) : 0.15f;
    struct Sample {
        bool wet = false;
        std::optional<world::WaterHere> here;
    };
    const auto sample = [&](double x, double z) {
        Sample s;
        s.here = water(x, z);
        if (!s.here || s.here->water == nullptr) return s;
        const std::optional<double> g = ground ? ground(x, z) : std::nullopt;
        s.wet = !g || *g < static_cast<double>(s.here->level); // (an island in a lake: dry)
        return s;
    };
    // The waves' strength: a lake's lap with the wind and its waves; the sea's more.
    const auto lap = [&](const world::Water& w) {
        const float waves = std::isfinite(w.waves) ? std::clamp(w.waves, 0.0f, 4.0f) : 1.0f;
        return w.kind == world::WaterKind::sea ? 0.5 + 1.0 * static_cast<double>(surf * waves)
                                               : 0.25 + 0.75 * static_cast<double>(surf * waves);
    };
    const auto running = [](const world::WaterHere& h) {
        const double speed = std::hypot(static_cast<double>(h.flow_x), static_cast<double>(h.flow_z));
        return std::pow(std::min(std::isfinite(speed) ? speed : 0.0, 10.0) / 2.0, 1.5);
    };
    const Sample here = sample(listener.x, listener.z);
    // (Under its surface: the water reaches the listener there, not in a room under it.)
    bool reaches_here = true;
    if (here.wet) {
        const double level = here.here->level;
        if (listener.y < level && reached) reaches_here = reached(level);
        if (listener.y < level - 0.05 && reaches_here) {
            out.under = true;
            out.depth = static_cast<float>(level - listener.y);
        }
    }
    // A few ways looked along again; moved far since the last call (a jump: another world, a game loaded,
    // a teleport): every way, so nothing found from where it was is heard.
    const DVec3 moved = listener - m_last;
    if (!m_looked || dot(moved, moved) > k_water_jump * k_water_jump) ways = k_water_ways;
    m_looked = true;
    m_last = listener;
    for (u32 n = 0; n < std::min(ways, k_water_ways); ++n) {
        const u32 way = m_next;
        m_next = (m_next + 1) % k_water_ways;
        Way& w = m_ways[way];
        w = Way{};
        const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(way) / k_water_ways;
        const double dx = std::cos(angle), dz = std::sin(angle);
        Sample last = here;
        for (usize k = 1; k < k_steps.size(); ++k) {
            const double r = k_steps[k];
            const double x = listener.x + dx * r, z = listener.z + dz * r;
            const Sample now = sample(x, z);
            // A river, by its speed (the nearest stretch this way).
            if (!w.river.there && now.wet && now.here->water->kind == world::WaterKind::river) {
                w.river = {true, {x, now.here->level, z}, running(*now.here)};
            }
            // A shore: water meets land between the last step and this one (a lake's or the sea's;
            // a river's bank is looked past, to a lake or the sea beyond it).
            if (now.wet != last.wet) {
                const Sample& wet = now.wet ? now : last;
                const world::Water& body = *wet.here->water;
                if (body.kind != world::WaterKind::river) {
                    const double mid = 0.5 * (k_steps[k - 1] + r);
                    Found& f = body.kind == world::WaterKind::sea ? w.sea : w.shore;
                    f = {true, {listener.x + dx * mid, wet.here->level, listener.z + dz * mid}, lap(body)};
                    break; // (the nearest shore this way)
                }
            }
            last = now;
        }
    }
    struct Sum {
        DVec3 toward{};
        double total = 0.0;
    };
    Sum shore, sea, river;
    // Each heard by 1 over the distance to it now (2 m at least, the height over it counted).
    const auto heard = [&](const Found& f, Sum& sum) {
        if (!f.there) return;
        const DVec3 d = f.at - listener;
        const double c = f.weight / std::max(std::sqrt(dot(d, d)), 2.0);
        // (Which way round, level: what is below on every side is all round, not from below.)
        const DVec3 flat{d.x, 0.0, d.z};
        const double across = std::sqrt(dot(flat, flat));
        if (across > 1e-6) sum.toward = sum.toward + flat / across * c;
        sum.total += c;
    };
    for (const Way& w : m_ways) {
        heard(w.shore, shore);
        heard(w.sea, sea);
        heard(w.river, river);
    }
    // On the water (swimming, afloat): it laps all round, close; a river runs round.
    if (here.wet && reaches_here && std::fabs(listener.y - static_cast<double>(here.here->level)) < 2.0) {
        const world::Water& body = *here.here->water;
        if (body.kind == world::WaterKind::river) river.total += 0.5 * running(*here.here);
        else (body.kind == world::WaterKind::sea ? sea : shore).total += 0.5 * lap(body);
    }
    const auto finish = [](const Sum& sum, float& loud, Vec3& dir, float& spread) {
        loud = static_cast<float>(std::clamp(sum.total, 0.0, 1.5));
        const double len = std::sqrt(dot(sum.toward, sum.toward));
        if (len > 1e-9) {
            dir = {static_cast<float>(sum.toward.x / len), static_cast<float>(sum.toward.y / len),
                   static_cast<float>(sum.toward.z / len)};
        }
        spread = sum.total > 1e-9 ? static_cast<float>(std::clamp(1.0 - len / sum.total, 0.0, 1.0)) : 1.0f;
    };
    finish(shore, out.shore, out.shore_dir, out.shore_spread);
    finish(sea, out.sea, out.sea_dir, out.sea_spread);
    finish(river, out.river, out.river_dir, out.river_spread);
    return out;
}

void AudioState::set_water(const WaterHeard& heard) {
    m_water_heard = heard;
    std::array<audio::Ambience, 4> slots{};
    const auto slot = [this, &slots](u32 index, const char* name, float gain, const Vec3& dir, float spread,
                                     float lowpass) {
        audio::Ambience& a = slots[index];
        const std::optional<audio::SoundId> id = find(name);
        a.sound = id ? *id : audio::k_no_sound;
        a.gain = gain;
        a.direction = dir;
        a.spread = spread;
        a.lowpass = lowpass;
    };
    // (The air dulls them as it does every sound.)
    const float air = 12000.0f / std::max(m_air, 1.0f);
    slot(0, "lapping", 0.6f * heard.shore, heard.shore_dir, heard.shore_spread, air);
    slot(1, "sea", 0.8f * heard.sea, heard.sea_dir, heard.sea_spread, std::min(air, 4000.0f));
    slot(2, "river", 0.7f * heard.river, heard.river_dir, heard.river_spread, air);
    slot(3, "underwater", heard.under ? 0.6f : 0.0f, {0, 0, 1}, 1.0f, 20000.0f);
    m_mixer.set_ambience(0, slots, heard.under ? k_under_muffle : 20000.0f); // (one lock for all of it)
}

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
                // Over the ground at the edge: it rises after it; halve onto it there.
                if (const std::optional<double> edge = above(hi); edge && *edge >= 0.0) {
                    lo = hi;
                    hi = next_t;
                    for (int i = 0; i < 16; ++i) {
                        const double mid = 0.5 * (lo + hi);
                        const std::optional<double> h = above(mid);
                        if (h && *h < 0.0) hi = mid;
                        else lo = mid;
                    }
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
    m_names["impact"] = m_mixer.add_sound(audio::synth_impact());
    m_names["click"] = m_mixer.add_sound(audio::synth_click());
    m_names["tone"] = m_mixer.add_sound(audio::synth_tone(440.0f, 1.0f));
    // The water's, played round and round (set_water).
    m_names["lapping"] = m_mixer.add_sound(audio::synth_lapping());
    m_names["sea"] = m_mixer.add_sound(audio::synth_lapping(11));
    m_names["river"] = m_mixer.add_sound(audio::synth_river());
    m_names["underwater"] = m_mixer.add_sound(audio::synth_underwater());
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
    if (seconds > 0.0) m_impacts_this_frame = m_shots_this_frame = 0; // a frame on (commands pass 0)
    if (m_output != nullptr) return;
    // No speakers: the mix moves on with the frames (and is dropped).
    const double s = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 0.25) : 0.0;
    const u32 frames = static_cast<u32>(s * audio::k_sample_rate);
    if (frames > 0) m_mixer.render(m_scratch.data(), frames);
}

audio::RayCast AudioState::ray_cast(const world::Scene& scene, const PhysicsState* physics) const {
    const double reach = physics_reach;
    const float snow = m_snow;
    return [&scene, physics, reach, snow](const DVec3& from, const Vec3& dir, double max_distance) {
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
        // Snow lying on the ground (and what stands on it, but for walls) takes the echo.
        if (best.hit && snow > 0.0f && best.normal.y > 0.5f) best.hardness *= 1.0f - 0.8f * snow;
        return best;
    };
}

audio::Heard AudioState::play(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound,
                              const DVec3& at, float loudness, const audio::EchoSettings& settings) {
    const audio::Heard heard = trace_only(scene, physics, at, settings);
    m_mixer.play(audio::to_play(sound, heard, loudness));
    ++m_played;
    m_last = heard;
    return heard;
}

void AudioState::set_weather(const world::Weather* shown, const world::Ground& ground) {
    float air = 1.0f;
    if (shown != nullptr) {
        const world::Falling f = world::falling(*shown);
        const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
        // Wet air and what falls through it take the highs: rain a little, snow more, thick fog too.
        air += 1.0f * unit(f.rain) + 2.0f * unit(f.snow) + 1.0f * unit(shown->fog_density);
    }
    m_air = air;
    m_snow = std::isfinite(ground.snow_cover) ? std::clamp(ground.snow_cover, 0.0f, 1.0f) : 0.0f;
}

AudioState::ShotHeard AudioState::shot(const world::Scene& scene, const PhysicsState* physics, const DVec3& origin,
                                       const Vec3& velocity, float mass, float drag, std::optional<double> range) {
    ShotHeard out;
    const float speed = length(velocity);
    if (!std::isfinite(speed) || speed <= 0.0f || !(mass > 0.0f)) return out;
    ++m_shots;
    // The report: louder with the round's energy (a rifle's 3100 J about 100 at 1 m).
    const double energy = 0.5 * static_cast<double>(mass) * static_cast<double>(speed) * speed;
    out.report_loudness = static_cast<float>(std::clamp(100.0 * std::sqrt(energy / 3100.0), 5.0, 400.0));
    const std::optional<audio::SoundId> report = find("gunshot");
    audio::EchoSettings settings;
    if (++m_shots_this_frame > k_full_shots) {
        settings.rays = 0;
        settings.diffract = false;
    }
    if (report) out.report = play(scene, physics, *report, origin, out.report_loudness, settings);
    // The crack: the flight to where it stopped, when known; else straight along the aim to where a ray meets something.
    audio::Flight flight{origin, velocity, drag, 4000.0};
    const double supersonic = audio::supersonic_range(flight);
    if (supersonic <= 0.0) return out;
    const audio::RayCast cast = ray_cast(scene, physics);
    const Vec3 aim = velocity / speed;
    if (range && !std::isnan(*range)) { // infinite: it flew on as far as it stays fast
        flight.range = std::clamp(*range, 0.0, supersonic);
    } else {
        const audio::RayHit met = cast(origin, aim, supersonic);
        flight.range = met.hit ? met.distance : supersonic;
    }
    out.range = flight.range;
    out.crack = audio::trace_crack(flight, m_listener, m_air);
    if (!out.crack.heard) return out;
    out.crack_loudness = static_cast<float>(std::clamp(60.0 * std::sqrt(static_cast<double>(mass) / 0.008), 5.0, 200.0));
    audio::Tap tap;
    tap.delay = out.crack.delay;
    tap.gain = out.crack.gain * out.crack_loudness;
    tap.lowpass = audio::air_lowpass(out.crack.miss, m_air);
    tap.direction = out.crack.direction;
    if (out.crack.miss > 0.2) {
        const audio::RayHit seen = cast(m_listener, out.crack.direction, out.crack.miss - 0.1);
        if (seen.hit) {
            out.crack_occluded = true;
            const audio::EchoSettings wall; // through it as the report would be
            tap.gain *= wall.occluded_gain;
            tap.lowpass = std::min(tap.lowpass, wall.occluded_lowpass);
        }
    }
    const std::optional<audio::SoundId> crack = find("crack");
    if (crack) {
        audio::Play p;
        p.sound = *crack;
        p.taps = {tap};
        p.reverb_send = out.report.reverb_send * out.crack_loudness * 0.3f;
        p.reverb_seconds = out.report.reverb_seconds;
        m_mixer.play(std::move(p));
    }
    return out;
}

void AudioState::impact(const world::Scene& scene, const PhysicsState* physics, const DVec3& point, float energy) {
    if (m_impacts_this_frame >= k_max_impacts || !std::isfinite(energy) || energy <= 0.0f) return;
    const DVec3 to = point - m_listener;
    if (!(dot(to, to) <= 1000.0 * 1000.0)) return; // too far to hear a knock
    const std::optional<audio::SoundId> knock = find("impact");
    if (!knock) return;
    ++m_impacts_this_frame;
    ++m_impacts;
    audio::EchoSettings s;
    s.rays = 16; // a knock: few echoes, near, no bending
    s.max_distance = 300.0;
    s.diffract = false;
    const audio::Heard heard = trace_only(scene, physics, point, s);
    m_mixer.play(audio::to_play(*knock, heard, std::clamp(0.6f * std::sqrt(energy), 0.5f, 60.0f)));
}

audio::Heard AudioState::trace_only(const world::Scene& scene, const PhysicsState* physics, const DVec3& at,
                                    audio::EchoSettings settings) const {
    settings.air *= m_air;
    return audio::trace(at, m_listener, ray_cast(scene, physics), settings);
}

AudioState::Recording AudioState::record(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound,
                                         const DVec3& at, float loudness, const audio::EchoSettings& settings,
                                         double seconds) {
    Recording out;
    out.heard = trace_only(scene, physics, at, settings);
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
