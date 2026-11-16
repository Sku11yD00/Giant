// Clips made in code for the motion matching tests: a biped of hips and two
// feet that idles, walks, runs, walks a turning loop and stops.
#pragma once

#include "giant/anim/motion.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace giant::anim::testing {

inline constexpr float k_tau = 2.0f * std::numbers::pi_v<float>;

// Hips a metre up with a foot under each side (left is +x, facing +z).
inline Skeleton biped(const char* hips = "Hips", const char* left = "LeftFoot", const char* right = "RightFoot") {
    Skeleton s;
    s.names = {hips, left, right};
    s.parents = {-1, 0, 0};
    s.rest = {JointTransform{{0.0f, 1.0f, 0.0f}}, JointTransform{{0.1f, -1.0f, 0.0f}},
              JointTransform{{-0.1f, -1.0f, 0.0f}}};
    s.inverse_bind = {Mat4{}, Mat4{}, Mat4{}};
    return s;
}

inline Channel channel(u32 joint, Path path) {
    Channel c;
    c.joint = joint;
    c.path = path;
    return c;
}

inline Vec4 q4(const Quat& q) { return {q.x, q.y, q.z, q.w}; }

// Feet swinging fore and aft `stride` either way, `steps` cycles over the clip.
inline void swing_feet(Clip& clip, float stride, float steps) {
    Channel left = channel(1, Path::translation);
    Channel right = channel(2, Path::translation);
    const int keys = 32;
    for (int i = 0; i <= keys; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(keys);
        const float swing = stride * std::sin(k_tau * steps * u);
        left.times.push_back(u * clip.duration);
        right.times.push_back(u * clip.duration);
        left.values.push_back({0.1f, -1.0f, swing, 0.0f});
        right.values.push_back({-0.1f, -1.0f, -swing, 0.0f});
    }
    clip.channels.push_back(left);
    clip.channels.push_back(right);
}

// Going straight along `along` (level, unit) at `speed` for `seconds`.
inline Clip go(const char* name, float speed, float seconds, const Vec3& along = {0.0f, 0.0f, 1.0f}) {
    Clip c;
    c.name = name;
    c.duration = seconds;
    Channel hips = channel(0, Path::translation);
    hips.times = {0.0f, seconds};
    hips.values = {{0.0f, 1.0f, 0.0f, 0.0f},
                   {along.x * speed * seconds, 1.0f, along.z * speed * seconds, 0.0f}};
    c.channels.push_back(hips);
    swing_feet(c, 0.2f * speed, 1.0f);
    return c;
}

// Going along +z at `speed` with a person's stride: each foot reaching
// `reach` fore and aft and lifted `lift` as it comes forward (the left
// first), one cycle over the clip. A loop.
inline Clip stride(const char* name, float speed, float seconds, float reach, float lift) {
    Clip c;
    c.name = name;
    c.duration = seconds;
    Channel hips = channel(0, Path::translation);
    hips.times = {0.0f, seconds};
    hips.values = {{0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, speed * seconds, 0.0f}};
    c.channels.push_back(hips);
    Channel left = channel(1, Path::translation);
    Channel right = channel(2, Path::translation);
    const int keys = 32;
    for (int i = 0; i <= keys; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(keys);
        const float swing = std::sin(k_tau * u);
        const float forward = std::cos(k_tau * u);
        left.times.push_back(u * seconds);
        right.times.push_back(u * seconds);
        left.values.push_back({0.1f, -1.0f + lift * std::max(0.0f, forward), reach * swing, 0.0f});
        right.values.push_back({-0.1f, -1.0f + lift * std::max(0.0f, -forward), -reach * swing, 0.0f});
    }
    c.channels.push_back(left);
    c.channels.push_back(right);
    return c;
}

// Standing, shifting its feet a little.
inline Clip idle() {
    Clip c;
    c.name = "idle";
    c.duration = 2.0f;
    swing_feet(c, 0.01f, 1.0f);
    return c;
}

// Slowing from `speed` to a stand over `seconds` (a one-off), stepping once a second.
inline Clip stop(float speed, float seconds) {
    Clip c;
    c.name = "stop";
    c.duration = seconds;
    Channel hips = channel(0, Path::translation);
    const int keys = 30;
    for (int i = 0; i <= keys; ++i) {
        const float t = seconds * static_cast<float>(i) / static_cast<float>(keys);
        hips.times.push_back(t);
        hips.values.push_back({0.0f, 1.0f, speed * (t - 0.5f * t * t / seconds), 0.0f});
    }
    c.channels.push_back(hips);
    swing_feet(c, 0.3f, seconds); // in step with a walk at 1.5 m/s
    return c;
}

inline constexpr float k_turn_speed = 1.5f;
inline constexpr float k_turn_rate = 0.25f * std::numbers::pi_v<float>; // a quarter turn in 2 s

// Walking a quarter of a circle toward +x: a loop that turns.
inline Clip turn() {
    Clip c;
    c.name = "turn";
    c.duration = 2.0f;
    Channel place = channel(0, Path::translation);
    Channel facing = channel(0, Path::rotation);
    const int keys = 64;
    const float radius = k_turn_speed / k_turn_rate;
    for (int i = 0; i <= keys; ++i) {
        const float t = c.duration * static_cast<float>(i) / static_cast<float>(keys);
        const float a = k_turn_rate * t;
        place.times.push_back(t);
        facing.times.push_back(t);
        place.values.push_back({radius * (1.0f - std::cos(a)), 1.0f, radius * std::sin(a), 0.0f});
        facing.values.push_back(q4(quat_from_axis_angle({0.0f, 1.0f, 0.0f}, a)));
    }
    c.channels.push_back(place);
    c.channels.push_back(facing);
    swing_feet(c, 0.3f, 2.0f);
    return c;
}

// A person's set with no clip going from one to the next: standing, a walk
// at 1.4 m/s, and (`with_run`) a run at 4.
struct PersonSet {
    Skeleton skeleton = biped();
    std::vector<Clip> clips;
    std::vector<MotionClip> chosen;
    MotionDatabase db;
    std::string problem;

    explicit PersonSet(bool with_run = true) {
        clips = {idle(), stride("walk", 1.4f, 1.0f, 0.39f, 0.16f)};
        chosen = {{0, true}, {1, true}};
        if (with_run) {
            clips.push_back(stride("run", 4.0f, 0.7f, 0.6f, 0.45f));
            chosen.push_back({2, true});
        }
        problem = build_motion_database(skeleton, clips, chosen, find_motion_joints(skeleton), MotionSettings{}, db);
    }
};

struct Set {
    Skeleton skeleton = biped();
    std::vector<Clip> clips{idle(), go("walk", 1.5f, 1.0f), go("run", 4.0f, 0.5f), turn(), stop(1.5f, 1.5f)};
    std::vector<MotionClip> chosen{{0, true}, {1, true}, {2, true}, {3, true}, {4, false}};
    MotionDatabase db;
    std::string problem;

    explicit Set(const MotionSettings& settings = {}) {
        problem = build_motion_database(skeleton, clips, chosen, find_motion_joints(skeleton), settings, db);
    }
};

inline bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

inline MotionAhead straight(float speed) {
    MotionAhead ahead;
    const MotionSettings settings;
    for (usize h = 0; h < k_motion_horizons; ++h) ahead.place[h] = {0.0f, speed * settings.horizons[h]};
    return ahead;
}

} // namespace giant::anim::testing
