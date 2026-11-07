#include "giant/anim/player.h"

#include <algorithm>
#include <cmath>

namespace giant::anim {

namespace {

float clip_length(u32 clip, std::span<const Clip> clips) {
    return clip < clips.size() ? clips[clip].duration : 0.0f;
}

// Within the clip: wrapped when looping, else held at its ends.
float place(float time, float length, bool loop) {
    if (!std::isfinite(time) || !(length > 0.0f)) return 0.0f;
    if (!loop) return std::clamp(time, 0.0f, length);
    const float t = std::fmod(time, length);
    return t < 0.0f ? t + length : t;
}

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

void AnimationPlayer::play(u32 clip, float blend_seconds, bool loop, float start) {
    blend_seconds = finite_or(blend_seconds, 0.0f);
    const bool fade = blend_seconds > 0.0f && m_main.clip != k_no_clip && clip != k_no_clip;
    if (fade) {
        // What plays now fades out from where it is (a held blend: its main clip).
        m_other = m_main;
        m_weight = 1.0f;
        m_fade_seconds = blend_seconds;
    } else {
        m_other = {};
        m_weight = 0.0f;
        m_fade_seconds = 0.0f;
    }
    m_main = {clip, std::max(0.0f, finite_or(start, 0.0f)), loop};
    m_held = false;
    m_sync = false;
}

void AnimationPlayer::set_blend(u32 a, u32 b, float weight, bool sync) {
    if (m_main.clip != a) m_main = {a, 0.0f, true};
    if (!m_held || m_other.clip != b) m_other = {b, 0.0f, true};
    m_weight = std::clamp(finite_or(weight, 0.0f), 0.0f, 1.0f);
    m_fade_seconds = 0.0f;
    m_held = true;
    m_sync = sync;
}

void AnimationPlayer::stop() {
    m_main = {};
    m_other = {};
    m_weight = 0.0f;
    m_fade_seconds = 0.0f;
    m_held = false;
    m_sync = false;
}

void AnimationPlayer::set_speed(float speed) { m_speed = std::clamp(finite_or(speed, 1.0f), -100.0f, 100.0f); }

void AnimationPlayer::set_time(float seconds, std::span<const Clip> clips) {
    if (m_main.clip == k_no_clip) return;
    m_main.time = place(finite_or(seconds, 0.0f), clip_length(m_main.clip, clips), m_main.loop);
    if (m_held && m_sync) follow(clips);
}

void AnimationPlayer::step(ClipState& s, float seconds, std::span<const Clip> clips) {
    if (s.clip == k_no_clip) return;
    if (s.clip >= clips.size()) {
        s = {};
        return;
    }
    s.time = place(s.time + seconds, clips[s.clip].duration, s.loop);
}

void AnimationPlayer::follow(std::span<const Clip> clips) {
    const float a = clip_length(m_main.clip, clips);
    const float b = clip_length(m_other.clip, clips);
    m_other.time = a > 0.0f ? place(m_main.time / a * b, b, m_other.loop) : 0.0f;
}

void AnimationPlayer::advance(float seconds, std::span<const Clip> clips) {
    seconds = finite_or(seconds, 0.0f);
    if (m_main.clip != k_no_clip && m_main.clip >= clips.size()) m_main = {};
    if (m_other.clip != k_no_clip && m_other.clip >= clips.size()) m_other = {};
    if (m_paused || seconds <= 0.0f) return;
    const float t = seconds * m_speed;
    step(m_main, t, clips);
    if (m_held && m_sync) {
        follow(clips);
    } else {
        step(m_other, t, clips);
    }
    if (!m_held && m_other.clip != k_no_clip) {
        // Fading out: by real time, whatever the speed.
        m_weight -= m_fade_seconds > 0.0f ? seconds / m_fade_seconds : 1.0f;
        if (m_weight <= 0.0f) {
            m_other = {};
            m_weight = 0.0f;
            m_fade_seconds = 0.0f;
        }
    }
}

void AnimationPlayer::evaluate(const Skeleton& skeleton, std::span<const Clip> clips, PoseScratch& scratch,
                               std::span<JointTransform> local) const {
    const usize n = std::min(local.size(), skeleton.size());
    const bool has_main = m_main.clip < clips.size();
    const bool has_other = m_other.clip < clips.size() && m_weight > 0.0f;
    if (!has_main && !has_other) {
        std::copy_n(skeleton.rest.begin(), n, local.begin());
        return;
    }
    if (!has_other) {
        sample(skeleton, clips[m_main.clip], m_main.time, local);
        return;
    }
    scratch.a.resize(n);
    scratch.b.resize(n);
    if (has_main) {
        sample(skeleton, clips[m_main.clip], m_main.time, scratch.a);
    } else {
        std::copy_n(skeleton.rest.begin(), n, scratch.a.begin());
    }
    sample(skeleton, clips[m_other.clip], m_other.time, scratch.b);
    blend(scratch.a, scratch.b, m_weight, local.first(n));
}

} // namespace giant::anim
