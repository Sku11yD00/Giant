// The editor's sound: the mixer (engine/audio) on the speakers when there
// are any, the listener at the camera, and sounds traced through the scene
// as they are played: bodies and kits by the physics' rays near, the
// terrain marched far (a cliff 2 km off answers), each surface as hard as
// what it is made of. View state: playing is not undoable.
#pragma once

#include "giant/audio/mixer.h"
#include "giant/audio/propagation.h"
#include "giant/audio/shot.h"
#include "giant/platform/audio_output.h"
#include "giant/sim/physics.h"
#include "giant/world/scene.h"
#include "giant/world/water.h"
#include "giant/world/weather.h"

#include <array>
#include <functional>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace giant::editor {

struct EditorCamera;
class PhysicsState;

// What the listener hears of the water round it (water plan W5d): little
// waves lapping where lakes meet land, the sea's waves on its shore (louder
// with the wind and its `waves`), rivers running (louder the faster), each
// from where it is (`*_dir` world, level, from the listener; `*_spread` 0:
// from there, 1: all round) and how loud (0 to about 1.5), and whether the
// listener is under the surface (`depth` metres).
struct WaterHeard {
    float shore = 0.0f, sea = 0.0f, river = 0.0f;
    Vec3 shore_dir{0, 0, 1}, sea_dir{0, 0, 1}, river_dir{0, 0, 1};
    float shore_spread = 1.0f, sea_spread = 1.0f, river_spread = 1.0f;
    bool under = false;
    float depth = 0.0f;
};
// The water at a place (x, z) and the ground's height there (none: no terrain).
using WaterLookup = std::function<std::optional<world::WaterHere>(double x, double z)>;
using GroundLookup = std::function<std::optional<double>(double x, double z)>;
// Looked for round the listener (k_water_ways ways out, k_water_steps places
// along each to k_water_heard_reach metres, 1 m apart near, 9 m far): a
// lake's or the sea's shore where water meets land along a way (dry ground:
// higher than the water; a river's banks looked past), each heard by 1 over
// its distance (2 m at least, the height over it counted), a river where it
// runs, by its speed. Water narrower than the gaps between places (a pond a
// few metres across 30 m off) can be missed: it would be faint there. A few
// ways a call (`ways`; what each found kept, heard again from where the
// listener is now), so a frame looks up a few dozen places, not all of them
// (a lake's lookup runs over its outline: a few microseconds at most); every
// way on the first call and after a jump (k_water_jump metres in a call). `wind` the wind's speed (m/s) over the water. `reached`: the
// water reaches the listener when under its surface (not in a room under
// it: physics' water_reaches; there no lapping round it either); none: always.
inline constexpr double k_water_heard_reach = 48.0;
inline constexpr u32 k_water_ways = 16;
inline constexpr usize k_water_steps = 13;
inline constexpr double k_water_jump = 8.0; // moved further in a call: every way looked along again
class WaterHearing {
public:
    WaterHeard update(const WaterLookup& water, const GroundLookup& ground, const DVec3& listener, float wind,
                      const std::function<bool(double level)>& reached = {}, u32 ways = 2);

private:
    struct Found {
        bool there = false;
        DVec3 at{};          // where (its surface's height)
        double weight = 0.0; // how loud at 1 m
    };
    struct Way {
        Found shore, sea, river;
    };
    std::array<Way, k_water_ways> m_ways{};
    u32 m_next = 0;
    DVec3 m_last{};      // the listener at the last call
    bool m_looked = false;
};

class AudioState {
public:
    AudioState(); // the made sounds added: gunshot, crack, impact, click, tone; lapping, sea, river, underwater
    ~AudioState();
    AudioState(const AudioState&) = delete;
    AudioState& operator=(const AudioState&) = delete;

    // The speakers (main, not hidden). False: none; the mixer is rendered by update.
    bool open_device();
    bool device() const { return m_output != nullptr; }
    std::string device_name() const;

    audio::Mixer& mixer() { return m_mixer; }
    const audio::Mixer& mixer() const { return m_mixer; }

    // Named sounds.
    std::optional<audio::SoundId> find(const std::string& name) const;
    std::vector<std::string> names() const;
    // A WAV file as `name` (replacing none: a name is kept for good). Empty: done; else why not.
    std::string load(const std::string& name, const std::string& path);
    static constexpr usize k_max_sounds = 256;

    // Each frame: the listener at the camera; with no speakers, `seconds` of
    // the mix rendered and dropped (so what plays ends; at most 0.25 s).
    void update(const EditorCamera& camera, double seconds);
    const DVec3& listener() const { return m_listener; }

    // Each frame: the weather shown (null: none) and the ground under it. Rain, snow and fog take the
    // highs sooner (air: 1 dry, about 4 at most: a blizzard in fog); snow lying on the ground deadens
    // its echoes (up to 80% less given back).
    void set_weather(const world::Weather* shown, const world::Ground& ground);
    float air() const { return m_air; }
    float snow_cover() const { return m_snow; }

    // The water heard round the listener (WaterHearing::update) as sounds played round
    // and round (audio::Ambience slots 0 to 3: shore, sea, river, under
    // water), and under the surface the whole mix muffled (k_under_muffle
    // Hz). Each frame (cheap: the mixer eases what it is given).
    void set_water(const WaterHeard& heard);
    const WaterHeard& water_heard() const { return m_water_heard; }
    static constexpr float k_under_muffle = 400.0f;

    // The scene as a sound meets it: the physics' ray (null: none) within
    // physics_reach, the terrain marched past it, the nearer hit taken.
    audio::RayCast ray_cast(const world::Scene& scene, const PhysicsState* physics) const;
    double physics_reach = 1000.0;

    // `sound` made at `at`, `loudness` its level at 1 m, heard at the listener now.
    audio::Heard play(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound, const DVec3& at,
                      float loudness, const audio::EchoSettings& settings);
    u64 played() const { return m_played; }
    const std::optional<audio::Heard>& last() const { return m_last; }

    // A shot fired (sim::Shot; `drag` the bullets'): the report from the muzzle (loudness from its
    // energy: a rifle's 3100 J about 100), and, when the bullet is faster than sound and its shock
    // cone sweeps over the listener, the crack from where it left the flight (muffled when the
    // listener cannot see that point). The flight ends at `range` metres when the caller knows where
    // the bullet stopped or glanced off (a resolved shot), else where a ray along the aim meets
    // something. Past k_full_shots a frame, a shot's report is traced with no echoes or bending
    // (bounded work, rule 9).
    struct ShotHeard {
        audio::Heard report;
        float report_loudness = 0.0f;
        audio::Crack crack;
        float crack_loudness = 0.0f;
        bool crack_occluded = false;
        double range = 0.0; // metres the flight was taken to (where it was met, or spent)
    };
    ShotHeard shot(const world::Scene& scene, const PhysicsState* physics, const DVec3& origin, const Vec3& velocity,
                   float mass, float drag, std::optional<double> range = std::nullopt);
    static constexpr u32 k_full_shots = 8;
    u64 shots() const { return m_shots; }
    // Where bullets struck (energy left there, joules): a knock each (16 echo rays within 300 m, no
    // bending: at most about 33 casts), at most k_max_impacts a frame, none past 1 km from the
    // listener. "A frame" is counted by update() with seconds > 0 (the main loop's every frame).
    void impact(const world::Scene& scene, const PhysicsState* physics, const DVec3& point, float energy);
    static constexpr u32 k_max_impacts = 8;
    u64 impacts() const { return m_impacts; }
    // Something gone into water (sim::WaterEntry, water plan W5e) heard
    // where it broke the surface: a splash, or a plop for something small
    // (under k_plop_mass kg or k_plop_size m), as loud as the root of its
    // energy going in (0.25 sqrt(m v^2 / 2), v^2 its speed down squared and
    // a quarter of its speed across squared: a 300 kg crate from 4.5 m
    // about 29, a person jumping off 2 m about 9.9, a stone tossed in about
    // 1), with its echoes (16 rays within 300 m, as a knock); none past 1 km
    // or with bad numbers (false: not heard).
    bool splash(const world::Scene& scene, const PhysicsState* physics, const sim::WaterEntry& entry);
    // A physics step's: at most k_max_splashes of them, the loudest where the
    // listener is first (energy over distance squared), and at most
    // k_max_splashes_frame a frame (update with seconds > 0; a command's
    // many fixed steps are one frame, as for impacts).
    void splashes_from(const world::Scene& scene, const PhysicsState* physics,
                       std::span<const sim::WaterEntry> entries);
    static constexpr u32 k_max_splashes = 8;
    static constexpr u32 k_max_splashes_frame = 16;
    static constexpr float k_plop_mass = 2.0f, k_plop_size = 0.25f;
    u64 splashes() const { return m_splashes; }
    bool last_splash_small() const { return m_last_splash_small; } // (a plop)
    float last_splash_loudness() const { return m_last_splash; }

    // The same heard alone, rendered for `seconds` into stereo frames (not the speakers).
    struct Recording {
        audio::Heard heard;
        std::vector<float> frames; // interleaved stereo at k_sample_rate
        float peak = 0.0f;
        double first_heard = -1.0; // seconds: the first sample over 1e-4 (-1: none)
    };
    Recording record(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound, const DVec3& at,
                     float loudness, const audio::EchoSettings& settings, double seconds);

private:
    audio::Heard trace_only(const world::Scene& scene, const PhysicsState* physics, const DVec3& at,
                            audio::EchoSettings settings) const;

    audio::Mixer m_mixer;
    std::unique_ptr<platform::AudioOutput> m_output; // after the mixer: closed first
    std::map<std::string, audio::SoundId> m_names;
    DVec3 m_listener{};
    Vec3 m_right{1, 0, 0}, m_up{0, 1, 0};
    u64 m_played = 0;
    u64 m_shots = 0, m_impacts = 0, m_splashes = 0;
    u32 m_impacts_this_frame = 0, m_shots_this_frame = 0, m_splashes_this_frame = 0;
    float m_last_splash = 0.0f;
    bool m_last_splash_small = false;
    struct SplashOrder {
        const sim::WaterEntry* entry = nullptr;
        float heard = 0.0f;
    };
    std::vector<SplashOrder> m_splash_order; // (splashes_from's scratch)
public:
    bool sounds_connected = false; // connect_sounds done (once only)
private:
    float m_air = 1.0f, m_snow = 0.0f;
    WaterHeard m_water_heard;
    std::optional<audio::Heard> m_last;
    std::vector<float> m_scratch; // headless renders
};

// The terrain's surface along a ray (a march with growing steps, then
// halving onto it), at most `max_distance`: none when it meets none.
std::optional<audio::RayHit> march_terrain(const world::Terrain& terrain, const DVec3& from, const Vec3& direction,
                                           double max_distance);
// How much of a sound each surface gives back.
float hardness_of(world::Surface surface);

} // namespace giant::editor
