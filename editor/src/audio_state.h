// The editor's sound: the mixer (engine/audio) on the speakers when there
// are any, the listener at the camera, and sounds traced through the scene
// as they are played: bodies and kits by the physics' rays near, the
// terrain marched far (a cliff 2 km off answers), each surface as hard as
// what it is made of. View state: playing is not undoable.
#pragma once

#include "giant/audio/mixer.h"
#include "giant/audio/propagation.h"
#include "giant/platform/audio_output.h"
#include "giant/world/scene.h"

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
    AudioState(); // the made sounds added: gunshot, crack, click, tone
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

    // The scene as a sound meets it: the physics' ray (null: none) within
    // physics_reach, the terrain marched past it, the nearer hit taken.
    audio::RayCast ray_cast(const world::Scene& scene, const PhysicsState* physics) const;
    double physics_reach = 1000.0;

    // `sound` made at `at`, `loudness` its level at 1 m, heard at the listener now.
    audio::Heard play(const world::Scene& scene, const PhysicsState* physics, audio::SoundId sound, const DVec3& at,
                      float loudness, const audio::EchoSettings& settings);
    u64 played() const { return m_played; }
    const std::optional<audio::Heard>& last() const { return m_last; }

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
    audio::Mixer m_mixer;
    std::unique_ptr<platform::AudioOutput> m_output; // after the mixer: closed first
    std::map<std::string, audio::SoundId> m_names;
    DVec3 m_listener{};
    Vec3 m_right{1, 0, 0}, m_up{0, 1, 0};
    u64 m_played = 0;
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
