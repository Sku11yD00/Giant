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
#include "giant/world/scene.h"
#include "giant/world/weather.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace giant::editor {

struct EditorCamera;
class PhysicsState;

class AudioState {
public:
    AudioState(); // the made sounds added: gunshot, crack, impact, click, tone
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
    u64 m_shots = 0, m_impacts = 0;
    u32 m_impacts_this_frame = 0, m_shots_this_frame = 0;
public:
    bool sounds_connected = false; // connect_sounds done (once only)
private:
    float m_air = 1.0f, m_snow = 0.0f;
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
