// How a sound reaches the listener through the world: the direct way
// (late by distance over the speed of sound, quieter by distance, its
// highs taken by the air, muffled when something stands between) and
// first echoes off what is round the source (a cliff 500 m off answers
// about 3 s later, from the cliff's side), plus how big and hard the place
// is, for the reverb tail. It asks the world only through a ray cast, so
// it runs over terrain, buildings, anything the caller can hit.
//
//   audio::Heard heard = audio::trace(source, listener, cast, {});
//   mixer.play(audio::to_play(shot, heard, 1.0f));
//
// Cost: settings.rays casts from the source, and one more for each that
// hits (can the listener see that spot?), plus one for the direct way:
// bounded by the settings (rule 9).
#pragma once

#include "giant/audio/mixer.h"
#include "giant/core/math.h"
#include "giant/core/types.h"

#include <functional>
#include <vector>

namespace giant::audio {

struct RayHit {
    bool hit = false;
    double distance = 0.0;    // metres along the ray
    Vec3 normal{0, 1, 0};     // of the surface, facing the ray
    float hardness = 0.5f;    // 0 to 1: how much of a sound it gives back (rock 0.7, grass 0.3, concrete 0.9)
};

// Casts from `from` along unit `direction` up to `max_distance` metres.
using RayCast = std::function<RayHit(const DVec3& from, const Vec3& direction, double max_distance)>;

struct EchoSettings {
    u32 rays = 64;              // cast from the source (at most 256)
    double max_distance = 3000; // metres an echo's surface may be from the source
    double reference = 1.0;     // metres: the distance a sound is heard at its own level
    float occluded_gain = 0.3f; // the direct way through something
    float occluded_lowpass = 900.0f;
    u32 echoes = k_max_taps - 1; // kept (the loudest, near ones merged)
};

struct Heard {
    std::vector<Tap> taps;     // the direct one first (when it is heard), then echoes, loudest first
    bool occluded = false;     // something stands on the direct way
    float openness = 1.0f;     // of the rays, those that hit nothing (sky)
    double mean_free_path = 0; // metres, of those that hit
    float hardness = 0.0f;     // mean of what they hit
    float reverb_send = 0.0f;  // the tail's level
    float reverb_seconds = 0.1f;
    u32 casts = 0;             // rays cast
};

// The cut-off the air leaves after `metres` (it takes the highs first: far thunder rumbles).
float air_lowpass(double metres);
// Amplitude after `metres` from `reference` (spreading, and a little the air takes).
float distance_gain(double metres, double reference = 1.0);

Heard trace(const DVec3& source, const DVec3& listener, const RayCast& cast, const EchoSettings& settings);

// The play for `sound` heard so, `loudness` times its own level.
Play to_play(SoundId sound, const Heard& heard, float loudness);

} // namespace giant::audio
