// Playing clips on one skeleton: a clip looping or once at a speed, a
// cross-fade from one clip to the next, or two clips held blended (a walk
// and a run by speed), their phases kept in step if asked. Plain state: it
// owns no skeleton or clips, which are handed in, and allocates nothing
// once its scratch has grown to the skeleton.
#pragma once

#include "giant/anim/skeleton.h"

#include <optional>
#include <span>
#include <vector>

namespace giant::anim {

inline constexpr u32 k_no_clip = ~u32{0};

struct ClipState {
    u32 clip = k_no_clip;
    float time = 0.0f; // seconds into it
    bool loop = true;
};

// Reused between evaluations (no allocation each frame).
struct PoseScratch {
    std::vector<JointTransform> a;
    std::vector<JointTransform> b;
};

class AnimationPlayer {
public:
    // Plays `clip` from `start` seconds: at once, or fading from what played
    // over `blend_seconds`. Ends a held blend.
    void play(u32 clip, float blend_seconds = 0.0f, bool loop = true, float start = 0.0f);
    // Holds `a` and `b` blended by `weight` (0 all a, 1 all b). `sync`: b's
    // time follows a's, at the same fraction of its length (feet in step).
    void set_blend(u32 a, u32 b, float weight, bool sync);
    void stop(); // the rest pose

    float speed() const { return m_speed; }
    void set_speed(float speed);        // 1 normal; negative plays back
    bool paused() const { return m_paused; }
    void set_paused(bool paused) { m_paused = paused; }
    void set_time(float seconds, std::span<const Clip> clips); // the main clip's time

    // Moves time on (scaled by the speed, nothing while paused). Clips not
    // in `clips` are dropped.
    void advance(float seconds, std::span<const Clip> clips);

    // The pose now, into `local` (one per joint): the rest pose with nothing playing.
    void evaluate(const Skeleton& skeleton, std::span<const Clip> clips, PoseScratch& scratch,
                  std::span<JointTransform> local) const;

    const ClipState& main() const { return m_main; }
    // The other clip (fading out, or held blended), and its weight.
    const ClipState& other() const { return m_other; }
    float other_weight() const { return m_other.clip == k_no_clip ? 0.0f : m_weight; }
    bool blending() const { return m_held; } // a held blend, not a fade
    bool synced() const { return m_sync; }

private:
    static void step(ClipState& s, float seconds, std::span<const Clip> clips);
    void follow(std::span<const Clip> clips); // b's time from a's (sync)

    ClipState m_main;
    ClipState m_other;
    float m_weight = 0.0f;       // of m_other
    float m_fade_seconds = 0.0f; // fading: how long the whole fade is (m_weight falls 1 to 0)
    bool m_held = false;
    bool m_sync = false;
    float m_speed = 1.0f;
    bool m_paused = false;
};

} // namespace giant::anim
