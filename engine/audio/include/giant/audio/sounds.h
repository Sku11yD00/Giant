// Sounds: made here (a gunshot's report, a bullet's crack, a click, a
// tone) until there are recorded ones, and WAV files read and written.
#pragma once

#include "giant/audio/mixer.h"
#include "giant/core/types.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace giant::audio {

// A rifle's report at the muzzle: the blast (a sharp edge, then noise
// that dies in about 0.2 s) over a low thump. Peak about 1.
Sound synth_gunshot(u32 seed = 1);
// A supersonic bullet's crack as it passes: an N-wave about half a millisecond long.
Sound synth_crack();
// A short tick (for tests and timing: its edge is one sample).
Sound synth_click();
// A bullet striking: a short knock of noise (about 40 ms).
Sound synth_impact(u32 seed = 7);
// A sine of `hz` for `seconds`, faded in and out over 5 ms.
Sound synth_tone(float hz, float seconds, float level = 0.5f);

// 16-bit PCM or 32-bit float WAV, mono or stereo (mixed to mono), any rate
// (resampled to k_sample_rate). nullopt: not one it reads.
std::optional<Sound> decode_wav(std::span<const u8> bytes);
std::optional<Sound> load_wav(const std::string& path);
// Interleaved float frames as 16-bit PCM at k_sample_rate (1 to 8 channels; else empty).
std::vector<u8> encode_wav(std::span<const float> interleaved, u32 channels);
bool save_wav(const std::string& path, std::span<const float> interleaved, u32 channels);

} // namespace giant::audio
