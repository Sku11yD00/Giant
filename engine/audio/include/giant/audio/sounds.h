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
// Water, made to be played round and round (Ambience: their ends
// cross-faded into their starts; RMS about 0.12 to 0.15):
// little waves lapping at a shore (8 s), a river running (6 s: rushing
// noise and bubbles), and under water (6 s: a low rumble, a bubble now
// and then).
// Made into a loop (how the water's loops are): `made` holds `n` samples
// and `fade` more, which are cross-faded (equal power) into its start, so
// played round and round there is no seam; then set to `rms` (no peak past
// 0.95). Too short a `made`: `n` and `fade` cut to what there is.
Sound make_loop(std::vector<float> made, usize n, usize fade, float rms);
Sound synth_lapping(u32 seed = 3);
Sound synth_river(u32 seed = 5);
Sound synth_underwater(u32 seed = 9);
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
