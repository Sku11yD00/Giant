#include "giant/anim/matcher.h"

#include <algorithm>
#include <cmath>

namespace giant::anim {

namespace {

constexpr float k_moment = 1.0f / 60.0f; // velocities at a jump are measured over this
constexpr float k_ln2 = 0.69314718056f;
constexpr float k_hair = 1e-4f; // of cost: less is no reason to jump

bool fits(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips) {
    if (db.frames() == 0 || db.ranges.empty() || skeleton.size() == 0 || skeleton.rest.size() != skeleton.size()) {
        return false;
    }
    if (db.joints.hips >= skeleton.size()) return false;
    return std::all_of(db.ranges.begin(), db.ranges.end(),
                       [&](const MotionRange& r) { return r.clip < clips.size(); });
}

// A rotation as a turn about an axis: the axis times the angle (radians), the short way.
Vec3 to_scaled_axis(Quat q) {
    if (q.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
    const Vec3 v{q.x, q.y, q.z};
    const float len = length(v);
    if (len < 1e-8f) return v * 2.0f;
    return v * (2.0f * std::atan2(len, q.w) / len);
}

Quat from_scaled_axis(const Vec3& v) {
    const float angle = length(v);
    if (angle < 1e-8f) return normalize(Quat{0.5f * v.x, 0.5f * v.y, 0.5f * v.z, 1.0f});
    return quat_from_axis_angle(v * (1.0f / angle), angle);
}

// A critically damped spring pulling `x` (moving at `v`) to nothing: both halve in about `half`.
void spring_to_zero(Vec3& x, Vec3& v, float half, float seconds) {
    if (!(half > 1e-5f)) {
        x = {};
        v = {};
        return;
    }
    const float y = 2.0f * k_ln2 / half; // half the damping
    const Vec3 j = v + x * y;
    const float e = std::exp(-y * seconds);
    x = (x + j * seconds) * e;
    v = (v - j * (y * seconds)) * e;
}

// The root at `seconds` into range `r`: between its frames' roots (past
// the last frame of a loop, on as it was going).
MotionRoot root_at(const MotionDatabase& db, const MotionRange& r, float seconds) {
    const float at = std::max(seconds, 0.0f) / r.frame_seconds;
    const u32 i = std::min(static_cast<u32>(at), r.count - 1);
    const float part = std::min(at - static_cast<float>(i), 1.0f);
    const MotionRoot a = db.roots[r.first + i];
    MotionRoot step{};
    if (i + 1 < r.count) {
        const MotionRoot b = db.roots[r.first + i + 1];
        step = {b.x - a.x, b.z - a.z, b.yaw - a.yaw};
    } else if (r.loop && i > 0) {
        const MotionRoot b = db.roots[r.first + i - 1];
        step = {a.x - b.x, a.z - b.z, a.yaw - b.yaw};
    }
    return {a.x + step.x * part, a.z + step.z * part, a.yaw + step.yaw * part};
}

// `seconds` into a range, kept within it: round a loop, held in a one-off clip.
float within(const MotionRange& r, float seconds) {
    const float span = static_cast<float>(r.count) * r.frame_seconds;
    if (r.loop) {
        seconds = std::fmod(seconds, span);
        return seconds < 0.0f ? seconds + span : seconds;
    }
    return std::clamp(seconds, 0.0f, static_cast<float>(r.count - 1) * r.frame_seconds);
}

} // namespace

void motion_pose(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips, usize range,
                 float seconds, std::span<JointTransform> local) {
    const usize n = std::min(skeleton.size(), local.size());
    if (range >= db.ranges.size() || db.ranges[range].clip >= clips.size()) {
        std::copy_n(skeleton.rest.begin(), std::min(n, skeleton.rest.size()), local.begin());
        return;
    }
    const MotionRange& r = db.ranges[range];
    const Clip& clip = clips[r.clip];
    sample(skeleton, clip, std::min(seconds, clip.duration), local);

    // The root's travel and turning taken out of every root joint.
    const MotionRoot root = root_at(db, r, seconds);
    const Quat back = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -root.yaw);
    const Mat4 undo = mat4_from_transform(rotate(back, {-root.x, 0.0f, -root.z}), back, {1.0f, 1.0f, 1.0f});
    // Seen from under skeleton.root that is a rigid move too (root scaled evenly): a turn and a
    // shift of each root joint, its scale left alone (a mirrored joint stays as it was).
    const Mat4 into = affine_inverse(skeleton.root) * undo * skeleton.root;
    const JointTransform above = decompose(skeleton.root);
    Quat turned = normalize(conjugate(above.rotation) * back * above.rotation);
    // (decompose puts a mirror in x: seen through one, a turn runs the other way about y and z.)
    if (above.scale.x < 0.0f) turned = {turned.x, -turned.y, -turned.z, turned.w};
    for (usize j = 0; j < n; ++j) {
        if (skeleton.parents[j] >= 0) continue;
        const Vec3 t = local[j].translation;
        const Vec4 moved = into * Vec4{t.x, t.y, t.z, 1.0f};
        local[j].translation = {moved.x, moved.y, moved.z};
        local[j].rotation = normalize(turned * local[j].rotation);
    }
}

MotionAhead motion_ahead(const MotionSettings& settings, std::array<float, 2> velocity, std::array<float, 2> wanted,
                         float turn, float ease_seconds) {
    const auto number = [](float v, float most) { return std::isfinite(v) ? std::clamp(v, -most, most) : 0.0f; };
    for (float& v : velocity) v = number(v, 100.0f);
    for (float& v : wanted) v = number(v, 100.0f);
    turn = number(turn, 100.0f);
    const float y = 2.0f * k_ln2 / std::clamp(number(ease_seconds, 10.0f), 0.01f, 10.0f);
    MotionAhead ahead;
    for (usize h = 0; h < k_motion_horizons; ++h) {
        const float t = settings.horizons[h];
        const float e = std::exp(-y * t);
        // The velocity eases as a critically damped spring does; this is how far that has carried it.
        const float eased = (2.0f - 2.0f * e - y * t * e) / y;
        for (usize i = 0; i < 2; ++i) ahead.place[h][i] = wanted[i] * t + (velocity[i] - wanted[i]) * eased;
        const float turned = turn * (1.0f - e * (1.0f + y * t));
        ahead.facing[h] = {std::sin(turned), std::cos(turned)};
    }
    return ahead;
}

void MotionMatcher::reset() {
    m_frame = k_no_frame;
    m_time = 0.0f;
    m_since_search = 0.0f;
    m_cost = 0.0f;
    m_jumps = 0;
    m_ended = false;
    std::fill(m_place.begin(), m_place.end(), Vec3{});
    std::fill(m_place_velocity.begin(), m_place_velocity.end(), Vec3{});
    std::fill(m_turn.begin(), m_turn.end(), Quat{});
    std::fill(m_turn_velocity.begin(), m_turn_velocity.end(), Vec3{});
}

void MotionMatcher::decay(float seconds) {
    // (Past a second the offsets would drift on their velocities instead of dying.)
    const float half = settings.blend_seconds >= 0.0f ? std::min(settings.blend_seconds, 1.0f) : 0.0f;
    for (usize j = 0; j < m_place.size(); ++j) {
        spring_to_zero(m_place[j], m_place_velocity[j], half, seconds);
        Vec3 turned = to_scaled_axis(m_turn[j]);
        spring_to_zero(turned, m_turn_velocity[j], half, seconds);
        m_turn[j] = from_scaled_axis(turned);
    }
}

void MotionMatcher::jump(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips, u32 to) {
    const usize n = skeleton.size();
    const MotionRange& from_range = db.range_of(m_frame);
    const MotionRange& to_range = db.range_of(to);
    const usize from_index = static_cast<usize>(&from_range - db.ranges.data());
    const usize to_index = static_cast<usize>(&to_range - db.ranges.data());
    const float to_time = static_cast<float>(to - to_range.first) * to_range.frame_seconds;

    m_from.resize(n);
    m_from_before.resize(n);
    m_to.resize(n);
    m_to_before.resize(n);
    // (A moment before, held within a one-off clip: its start has no before.)
    const float from_before = within(from_range, m_time - k_moment);
    const float to_before = within(to_range, to_time - k_moment);
    motion_pose(db, skeleton, clips, from_index, m_time, m_from);
    motion_pose(db, skeleton, clips, from_index, from_before, m_from_before);
    motion_pose(db, skeleton, clips, to_index, to_time, m_to);
    motion_pose(db, skeleton, clips, to_index, to_before, m_to_before);

    const float per = 1.0f / k_moment;
    for (usize j = 0; j < n; ++j) {
        // What showed (the pose left and the offsets still on it) less what will show.
        m_place[j] += m_from[j].translation - m_to[j].translation;
        m_place_velocity[j] += ((m_from[j].translation - m_from_before[j].translation) -
                                (m_to[j].translation - m_to_before[j].translation)) * per;
        // (Each pose's spin as it showed or will show: turned by the offset that is on it.)
        const Vec3 from_spin = rotate(m_turn[j], to_scaled_axis(m_from[j].rotation * conjugate(m_from_before[j].rotation)));
        m_turn[j] = normalize(m_turn[j] * m_from[j].rotation * conjugate(m_to[j].rotation));
        if (m_turn[j].w < 0.0f) m_turn[j] = {-m_turn[j].x, -m_turn[j].y, -m_turn[j].z, -m_turn[j].w};
        const Vec3 to_spin = rotate(m_turn[j], to_scaled_axis(m_to[j].rotation * conjugate(m_to_before[j].rotation)));
        m_turn_velocity[j] += (from_spin - to_spin) * per;
    }
    m_frame = to;
    m_time = to_time;
    m_ended = false;
    ++m_jumps;
}

void MotionMatcher::update(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips,
                           const MotionAhead& ahead, float seconds, bool search_now) {
    if (!fits(db, skeleton, clips) || (m_frame != k_no_frame && m_frame >= db.frames())) {
        reset();
        if (!fits(db, skeleton, clips)) return;
    }
    const usize n = skeleton.size();
    if (m_place.size() != n) {
        m_place.assign(n, Vec3{});
        m_place_velocity.assign(n, Vec3{});
        m_turn.assign(n, Quat{});
        m_turn_velocity.assign(n, Vec3{});
    }
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 1.0f) : 0.0f;

    if (m_frame == k_no_frame) {
        // A first frame: by the way ahead alone, shown at once.
        const MotionMatch first = motion_search(db, motion_query(db, k_no_frame, ahead), {}, 0.0f);
        if (first.frame == k_no_frame) return;
        const MotionRange& r = db.range_of(first.frame);
        m_frame = first.frame;
        m_time = static_cast<float>(first.frame - r.first) * r.frame_seconds;
        m_cost = first.cost;
        m_since_search = 0.0f;
        m_ended = false;
        return;
    }

    {
        const MotionRange& r = db.range_of(m_frame);
        m_time = within(r, m_time + seconds);
        // (A thousandth of a frame early: a time worked out as a frame's own must not fall short of it.)
        m_frame = r.first + std::min(static_cast<u32>(m_time / r.frame_seconds + 1e-3f), r.count - 1);
        m_ended = !r.loop && m_frame == r.first + r.count - 1;
    }
    decay(seconds);

    m_since_search += seconds;
    if (!(search_now || m_ended || !(m_since_search < settings.search_seconds))) return; // (no number: every update)
    m_since_search = 0.0f;
    const MotionFeatures query = motion_query(db, m_frame, ahead);
    // (A clip that has ended is not stayed on: any frame that may be jumped into is better.)
    const float staying = motion_cost(db, m_frame, query, settings.pose_weight);
    const float bias = settings.stay_bias >= 0.0f ? std::min(settings.stay_bias, 1.0f) : 0.0f; // (no number: 0)
    // (Better by a hair is rounding: twins of a frame, a stride on, cost the same.)
    const MotionMatch stay = m_ended ? MotionMatch{} : MotionMatch{m_frame, staying * (1.0f - bias) - k_hair};
    const MotionMatch best = motion_search(db, query, stay, settings.pose_weight);
    m_cost = staying;
    if (best.frame == k_no_frame || best.frame == m_frame) return;
    const MotionRange& r = db.range_of(m_frame);
    const bool same_clip = best.frame >= r.first && best.frame < r.first + r.count;
    u32 apart = best.frame > m_frame ? best.frame - m_frame : m_frame - best.frame;
    if (same_clip && r.loop) apart = std::min(apart, r.count - apart); // (round a loop's end)
    if (same_clip && !m_ended && apart <= settings.near_frames) return;
    m_cost = best.cost;
    jump(db, skeleton, clips, best.frame);
}

void MotionMatcher::evaluate(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips,
                             std::span<JointTransform> local) const {
    const usize n = std::min(skeleton.size(), local.size());
    if (m_frame == k_no_frame || m_frame >= db.frames() || !fits(db, skeleton, clips)) {
        std::copy_n(skeleton.rest.begin(), std::min(n, skeleton.rest.size()), local.begin());
        return;
    }
    const MotionRange& r = db.range_of(m_frame);
    motion_pose(db, skeleton, clips, static_cast<usize>(&r - db.ranges.data()), m_time, local);
    for (usize j = 0; j < std::min(n, m_place.size()); ++j) {
        local[j].translation += m_place[j];
        local[j].rotation = normalize(m_turn[j] * local[j].rotation);
    }
}

RootMotion MotionMatcher::root_motion(const MotionDatabase& db) const {
    if (m_frame == k_no_frame || m_frame >= db.frames()) return {};
    const MotionRange& r = db.range_of(m_frame);
    if (r.count < 2 || (m_ended && !r.loop)) return {};
    // Over the step to the next frame (from the one before, at a clip's last).
    const u32 i = m_frame - r.first;
    const u32 a = i + 1 < r.count ? i : i - 1;
    const MotionRoot from = db.roots[r.first + a];
    const MotionRoot to = db.roots[r.first + a + 1];
    const float per = 1.0f / r.frame_seconds;
    const float c = std::cos(from.yaw);
    const float s = std::sin(from.yaw);
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    return {(dx * c - dz * s) * per, (dx * s + dz * c) * per, (to.yaw - from.yaw) * per};
}

} // namespace giant::anim
