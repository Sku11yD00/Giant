// Playing clips on one skeleton: a clip looping or once at a speed, a
// cross-fade from one clip to the next, or two clips held blended (a walk
// and a run by speed), their phases kept in step if asked. It owns no
// skeleton or clips, which are handed in, and allocates nothing once its
// buffers have grown to the skeleton.
//
// A fade starts from whatever showed: a single clip fades out still
// playing; a blend, or a fade not yet done, is caught as it stood (one
// still pose) and that fades out, so starting a clip never jumps.
#pragma once

#include "giant/anim/skeleton.h"

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
    // Plays `clip` from `start` seconds (wrapped into it, or held within it):
    // at once, or fading from what showed over `blend_seconds`. Ends a held
    // blend.
    void play(const Skeleton& skeleton, std::span<const Clip> clips, u32 clip, float blend_seconds = 0.0f,
              bool loop = true, float start = 0.0f);
    // Holds `a` and `b` (both looping) blended by `weight` (0 all a, 1 all
    // b). `sync`: b's time follows a's, at the same fraction of its length
    // (feet in step).
    void set_blend(std::span<const Clip> clips, u32 a, u32 b, float weight, bool sync);
    void stop(); // the rest pose

    float speed() const { return m_speed; }
    void set_speed(float speed);        // 1 normal; negative plays back
    bool paused() const { return m_paused; }
    void set_paused(bool paused) { m_paused = paused; }
    void set_time(float seconds, std::span<const Clip> clips); // the main clip's time

    // Moves time on (scaled by the speed, nothing while paused; a fade
    // goes by real time). Clips not in `clips` are dropped.
    void advance(float seconds, std::span<const Clip> clips);

    // The pose now, into `local` (one per joint): the rest pose with nothing playing.
    void evaluate(const Skeleton& skeleton, std::span<const Clip> clips, PoseScratch& scratch,
                  std::span<JointTransform> local) const;

    const ClipState& main() const { return m_main; }
    // The other clip (fading out, or held blended); none while a caught
    // pose fades out (fading() is then true).
    const ClipState& other() const { return m_other; }
    // How much of the pose is the other clip's (or the caught pose's).
    float other_weight() const { return m_other.clip != k_no_clip || m_frozen_fading ? m_weight : 0.0f; }
    bool blending() const { return m_held; } // a held blend, not a fade
    bool fading() const { return !m_held && other_weight() > 0.0f; }
    bool synced() const { return m_sync; }

private:
    static void step(ClipState& s, float seconds, std::span<const Clip> clips);
    void follow(std::span<const Clip> clips); // b's time from a's (sync)

    ClipState m_main;
    ClipState m_other;
    float m_weight = 0.0f;       // of m_other (or the caught pose)
    float m_fade_seconds = 0.0f; // fading: how long the whole fade is (m_weight falls 1 to 0)
    bool m_held = false;
    bool m_sync = false;
    float m_speed = 1.0f;
    bool m_paused = false;
    bool m_frozen_fading = false;           // the caught pose fades out
    std::vector<JointTransform> m_frozen;   // the pose caught when a fade began
    PoseScratch m_catch;                    // for catching it
};

} // namespace giant::anim
