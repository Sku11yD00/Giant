// Animation: clips sampled (linear, step, cubic; clamped and between keys),
// poses blended, joints' matrices, skinning, bounds, and the player's
// looping, fades, held blends and speeds.
#include "giant/anim/player.h"
#include "giant/anim/skeleton.h"

#include "giant_test.h"

#include <cmath>
#include <numbers>
#include <string>

using namespace giant;
using namespace giant::anim;

namespace {

constexpr float k_half_pi = 0.5f * std::numbers::pi_v<float>;

bool near(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

// An arm: a shoulder at the origin and an elbow 1 m up it; bound as it rests.
Skeleton arm() {
    Skeleton s;
    s.names = {"shoulder", "elbow"};
    s.parents = {-1, 0};
    s.rest = {JointTransform{}, JointTransform{{0.0f, 1.0f, 0.0f}}};
    s.inverse_bind = {Mat4{}, affine_inverse(mat4_from_transform({0.0f, 1.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f}))};
    return s;
}

Vec4 q4(const Quat& q) { return {q.x, q.y, q.z, q.w}; }

// The elbow turning a quarter about z over a second.
Clip bend() {
    Clip c;
    c.name = "bend";
    c.duration = 1.0f;
    Channel ch;
    ch.joint = 1;
    ch.path = Path::rotation;
    ch.times = {0.0f, 1.0f};
    ch.values = {q4({}), q4(quat_from_axis_angle({0.0f, 0.0f, 1.0f}, k_half_pi))};
    c.channels.push_back(ch);
    return c;
}

// The shoulder jumping 1 m along x at half a second (a step).
Clip hop() {
    Clip c;
    c.name = "hop";
    c.duration = 2.0f;
    Channel ch;
    ch.joint = 0;
    ch.path = Path::translation;
    ch.interpolation = Interpolation::step;
    ch.times = {0.0f, 0.5f, 2.0f};
    ch.values = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}};
    c.channels.push_back(ch);
    return c;
}

struct Posed {
    std::vector<JointTransform> local;
    std::vector<Mat4> model;
    std::vector<Mat4> skin;
};

Posed pose(const Skeleton& s, const Clip* clip, float time) {
    Posed p;
    p.local.resize(s.size());
    p.model.resize(s.size());
    p.skin.resize(s.size());
    if (clip != nullptr) {
        sample(s, *clip, time, p.local);
    } else {
        p.local = s.rest;
    }
    model_matrices(s, p.local, p.model);
    skin_matrices(s, p.model, p.skin);
    return p;
}

Vec3 hand(const Posed& p) { return skin_point(p.skin, {1, 0, 0, 0}, {255, 0, 0, 0}, {0.0f, 2.0f, 0.0f}); }

} // namespace

GIANT_TEST("anim: a linear rotation bends the arm; clamped before and after; the rest pose binds") {
    const Skeleton s = arm();
    GIANT_CHECK(validate(s).empty());
    const Clip c = bend();
    GIANT_CHECK(validate(c, s).empty());
    // Bound as it rests: skinning moves nothing.
    const Posed rest = pose(s, nullptr, 0.0f);
    GIANT_CHECK(near(hand(rest), {0.0f, 2.0f, 0.0f}));
    // A quarter turn at the elbow: the hand (1 m past it) points along -x.
    GIANT_CHECK(near(hand(pose(s, &c, 1.0f)), {-1.0f, 1.0f, 0.0f}));
    // Halfway: an eighth of a turn, at constant angular speed.
    const float r = std::sqrt(0.5f);
    GIANT_CHECK(near(hand(pose(s, &c, 0.5f)), {-r, 1.0f + r, 0.0f}));
    // Before and after the keys: held at them.
    GIANT_CHECK(near(hand(pose(s, &c, -3.0f)), {0.0f, 2.0f, 0.0f}));
    GIANT_CHECK(near(hand(pose(s, &c, 9.0f)), {-1.0f, 1.0f, 0.0f}));
    // A point half on each joint, at the elbow's height: between their moves.
    const Posed bent = pose(s, &c, 1.0f);
    const Vec3 mid = skin_point(bent.skin, {0, 1, 0, 0}, {128, 127, 0, 0}, {0.0f, 1.0f, 0.0f});
    GIANT_CHECK(near(mid, {0.0f, 1.0f, 0.0f}));
    // A normal along +y on the forearm turns with it.
    GIANT_CHECK(near(skin_direction(bent.skin, {1, 0, 0, 0}, {255, 0, 0, 0}, {0.0f, 1.0f, 0.0f}), {-1.0f, 0.0f, 0.0f}));
}

GIANT_TEST("anim: step keys hold until the next; cubic keys follow their tangents") {
    const Skeleton s = arm();
    const Clip h = hop();
    GIANT_CHECK(validate(h, s).empty());
    std::vector<JointTransform> local(2);
    sample(s, h, 0.49f, local);
    GIANT_CHECK(local[0].translation.x == 0.0f);
    sample(s, h, 0.5f, local);
    GIANT_CHECK(local[0].translation.x == 1.0f);
    sample(s, h, 1.9f, local);
    GIANT_CHECK(local[0].translation.x == 1.0f);
    sample(s, h, 2.0f, local);
    GIANT_CHECK(local[0].translation.x == 2.0f);
    // The elbow untouched: its rest place.
    GIANT_CHECK(near(local[1].translation, {0.0f, 1.0f, 0.0f}));
    // Cubic: 0 to 2 over two seconds. Flat tangents: halfway is 1. Tangents
    // of 1 m/s in and out (a straight line): every point on the line.
    Clip c;
    c.name = "wave";
    c.duration = 2.0f;
    Channel ch;
    ch.joint = 0;
    ch.path = Path::translation;
    ch.interpolation = Interpolation::cubic;
    ch.times = {0.0f, 2.0f};
    ch.values = {{}, {0, 0, 0, 0}, {}, {}, {2, 0, 0, 0}, {}};
    c.channels.push_back(ch);
    GIANT_CHECK(validate(c, s).empty());
    sample(s, c, 1.0f, local);
    GIANT_CHECK(std::fabs(local[0].translation.x - 1.0f) < 1e-5f);
    sample(s, c, 0.5f, local);
    GIANT_CHECK(local[0].translation.x < 0.5f); // eases in
    c.channels[0].values = {{1, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {1, 0, 0, 0}};
    for (const float t : {0.25f, 0.5f, 1.3f}) {
        sample(s, c, t, local);
        GIANT_CHECK(std::fabs(local[0].translation.x - t) < 1e-5f);
    }
    // Bad clips are refused.
    Clip bad = c;
    bad.channels[0].values.pop_back();
    GIANT_CHECK(!validate(bad, s).empty());
    bad = c;
    bad.channels[0].times = {1.0f, 1.0f};
    GIANT_CHECK(!validate(bad, s).empty());
    bad = c;
    bad.channels[0].joint = 2;
    GIANT_CHECK(!validate(bad, s).empty());
    bad = c;
    bad.duration = 4000.0f;
    GIANT_CHECK(!validate(bad, s).empty());
    Skeleton wrong = s;
    wrong.parents = {1, -1}; // a child before its parent
    GIANT_CHECK(!validate(wrong).empty());
    wrong = s;
    wrong.names.pop_back();
    GIANT_CHECK(!validate(wrong).empty());
}

GIANT_TEST("anim: blends take the shorter way; matrices invert and come apart again") {
    const Quat a = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.2f);
    const Quat b = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.6f);
    const Quat b_flipped{-b.x, -b.y, -b.z, -b.w}; // the same rotation
    std::vector<JointTransform> pa{{{0, 0, 0}, a, {1, 1, 1}}}, pb{{{2, 0, 0}, b_flipped, {3, 3, 3}}}, out(1);
    blend(pa, pb, 0.5f, out);
    GIANT_CHECK(near(out[0].translation, {1.0f, 0.0f, 0.0f}) && near(out[0].scale, {2.0f, 2.0f, 2.0f}));
    const Vec3 turned = rotate(out[0].rotation, {1.0f, 0.0f, 0.0f});
    GIANT_CHECK(near(turned, rotate(quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.4f), {1.0f, 0.0f, 0.0f}), 1e-3f));
    blend(pa, pb, 0.0f, out);
    GIANT_CHECK(out[0].translation.x == 0.0f);
    blend(pa, pb, 1.0f, out);
    GIANT_CHECK(out[0].translation.x == 2.0f);
    // Inverse and decomposition round trips (a mirror into -x scale).
    const JointTransform t{{1.0f, -2.0f, 3.5f}, quat_from_axis_angle({1.0f, 2.0f, 0.5f}, 1.1f), {-0.5f, 2.0f, 1.5f}};
    const Mat4 m = to_matrix(t);
    const Mat4 back = m * affine_inverse(m);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) GIANT_CHECK(std::fabs(back.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-5f);
    }
    const JointTransform d = decompose(m);
    const Mat4 again = to_matrix(d);
    for (int i = 0; i < 16; ++i) GIANT_CHECK(std::fabs(again.m[i] - m.m[i]) < 1e-4f);
    GIANT_CHECK(near(d.translation, t.translation) && d.scale.x < 0.0f);
    const Mat4 none = affine_inverse(mat4_from_transform({}, {}, {0.0f, 1.0f, 1.0f}));
    GIANT_CHECK(none.m[0] == 1.0f && none.m[5] == 1.0f); // no inverse: the identity
}

GIANT_TEST("anim: pose bounds hold every vertex, in any pose and at any scale") {
    Skeleton s = arm();
    s.root = mat4_from_transform({100.0f, 0.0f, 0.0f}, {}, {50.0f, 50.0f, 50.0f}); // a giant, moved
    // Vertices up the arm: the upper arm on the shoulder, the forearm on the
    // elbow, the middle shared.
    struct V {
        Vec3 p;
        std::array<u16, 4> j;
        std::array<u8, 4> w;
    };
    const std::vector<V> verts{{{0.2f, 0.0f, 0.0f}, {0, 0, 0, 0}, {255, 0, 0, 0}},
                               {{-0.2f, 0.5f, 0.1f}, {0, 0, 0, 0}, {255, 0, 0, 0}},
                               {{0.2f, 1.0f, 0.0f}, {0, 1, 0, 0}, {100, 155, 0, 0}},
                               {{0.0f, 2.0f, 0.0f}, {1, 0, 0, 0}, {255, 0, 0, 0}},
                               {{0.3f, 1.6f, -0.2f}, {1, 0, 0, 0}, {255, 0, 0, 0}}};
    // Joint radii as the import works them out: from the joint's bound place.
    std::vector<float> radius(2, 0.0f);
    const Vec3 bound[2] = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (const V& v : verts) {
        for (usize k = 0; k < 4; ++k) {
            if (v.w[k] > 0) radius[v.j[k]] = std::max(radius[v.j[k]], length(v.p - bound[v.j[k]]));
        }
    }
    const Clip c = bend();
    for (const float t : {0.0f, 0.3f, 1.0f}) {
        const Posed p = pose(s, &c, t);
        const Bounds b = pose_bounds(p.model, p.skin, radius);
        GIANT_CHECK(b.radius > 0.0f);
        for (const V& v : verts) {
            const Vec3 at = skin_point(p.skin, v.j, v.w, v.p);
            GIANT_CHECK(length(at - b.center) <= b.radius * 1.0001f);
        }
        GIANT_CHECK(b.radius < 200.0f); // about 2 m of arm at 50 times
    }
    const Posed p = pose(s, nullptr, 0.0f);
    GIANT_CHECK(pose_bounds(p.model, p.skin, std::vector<float>(2, 0.0f)).radius == 0.0f);
    // Shear: a stretched parent and a turned child. The vertex a metre from
    // the child lands two metres out; the bounds still hold it.
    Skeleton sheared;
    sheared.names = {"a", "b"};
    sheared.parents = {-1, 0};
    sheared.rest = {JointTransform{{}, {}, {2.0f, 1.0f, 1.0f}},
                    JointTransform{{}, quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.25f * std::numbers::pi_v<float>)}};
    sheared.inverse_bind = {Mat4{}, Mat4{}};
    const Posed q = pose(sheared, nullptr, 0.0f);
    const std::vector<float> reach{0.0f, 1.0f};
    const Bounds b = pose_bounds(q.model, q.skin, reach);
    const float r = std::sqrt(0.5f);
    const Vec3 v = skin_point(q.skin, {1, 0, 0, 0}, {255, 0, 0, 0}, {r, -r, 0.0f});
    GIANT_CHECK(std::fabs(length(v) - 2.0f) < 1e-4f && length(v - b.center) <= b.radius);
}

GIANT_TEST("anim: the player loops, holds, fades, blends in step, and runs at its speed") {
    const Skeleton s = arm();
    std::vector<Clip> clips{bend(), hop()};
    PoseScratch scratch;
    std::vector<JointTransform> local(2);
    AnimationPlayer player;
    // Nothing playing: the rest pose.
    player.evaluate(s, clips, scratch, local);
    GIANT_CHECK(local == s.rest);
    // Looping: 1.25 s into a 1 s clip is a quarter in.
    player.play(s, clips, 0);
    player.advance(1.25f, clips);
    GIANT_CHECK(std::fabs(player.main().time - 0.25f) < 1e-5f);
    // Once: held at the end.
    player.play(s, clips, 0, 0.0f, false);
    player.advance(5.0f, clips);
    GIANT_CHECK(player.main().time == 1.0f);
    // Twice as fast; paused; backwards wraps.
    player.play(s, clips, 1);
    player.set_speed(2.0f);
    player.advance(0.5f, clips);
    GIANT_CHECK(std::fabs(player.main().time - 1.0f) < 1e-5f);
    player.set_paused(true);
    player.advance(0.5f, clips);
    GIANT_CHECK(std::fabs(player.main().time - 1.0f) < 1e-5f);
    player.set_paused(false);
    player.set_speed(-1.0f);
    player.advance(1.5f, clips);
    GIANT_CHECK(std::fabs(player.main().time - 1.5f) < 1e-5f); // 1 - 1.5 wrapped into 2 s
    player.set_speed(1.0f);
    // A fade from hop (at x = 1) to bend over a second: halfway, half each.
    player.set_time(1.0f, clips);
    player.play(s, clips, 0, 1.0f);
    GIANT_CHECK(player.other().clip == 1 && player.other_weight() == 1.0f);
    player.advance(0.5f, clips);
    GIANT_CHECK(std::fabs(player.other_weight() - 0.5f) < 1e-5f);
    player.evaluate(s, clips, scratch, local);
    GIANT_CHECK(std::fabs(local[0].translation.x - 0.5f) < 1e-5f); // hop's x = 1, bend's 0
    player.advance(0.6f, clips);
    GIANT_CHECK(player.other().clip == k_no_clip && player.other_weight() == 0.0f);
    // Held blend, in step: b's time keeps a's fraction.
    player.set_time(0.5f, clips);
    player.set_blend(clips, 0, 1, 0.25f, true);
    GIANT_CHECK(std::fabs(player.other().time - 1.0f) < 1e-5f); // in step at once
    player.set_time(0.0f, clips);
    player.advance(0.4f, clips);
    GIANT_CHECK(player.blending() && std::fabs(player.other().time - 0.8f) < 1e-5f);
    player.advance(5.0f, clips); // and on: still blended
    GIANT_CHECK(player.blending() && player.other_weight() == 0.25f);
    // A clip that is gone is dropped; stop is the rest pose.
    player.advance(0.1f, std::span<const Clip>(clips).first(1));
    GIANT_CHECK(player.other().clip == k_no_clip && player.main().clip == 0);
    player.stop();
    player.evaluate(s, clips, scratch, local);
    GIANT_CHECK(local == s.rest);
    // Bad numbers do nothing harmful.
    player.play(s, clips, 0, std::nanf(""), true, std::nanf(""));
    player.set_speed(std::nanf(""));
    player.advance(std::nanf(""), clips);
    GIANT_CHECK(player.main().time == 0.0f && player.speed() == 1.0f);
}

GIANT_TEST("anim: starting a clip mid-fade or from a blend never jumps; starts wrap into the clip") {
    const Skeleton s = arm();
    // Three clips holding the shoulder at x = 0, 1 and 2.
    std::vector<Clip> clips;
    for (int i = 0; i < 3; ++i) {
        Clip c;
        c.name = "at " + std::to_string(i);
        c.duration = 1.0f;
        Channel ch;
        ch.joint = 0;
        ch.times = {0.0f, 1.0f};
        ch.values = {{float(i), 0, 0, 0}, {float(i), 0, 0, 0}};
        c.channels.push_back(ch);
        clips.push_back(c);
    }
    PoseScratch scratch;
    std::vector<JointTransform> local(2);
    const auto x = [&](const AnimationPlayer& p) {
        p.evaluate(s, clips, scratch, local);
        return local[0].translation.x;
    };
    AnimationPlayer player;
    player.play(s, clips, 0);
    player.play(s, clips, 1, 1.0f);
    player.advance(0.5f, clips);
    GIANT_CHECK(std::fabs(x(player) - 0.5f) < 1e-5f);
    // Again, mid-fade: it carries on from where it stood.
    player.play(s, clips, 2, 1.0f);
    GIANT_CHECK(std::fabs(x(player) - 0.5f) < 1e-5f && player.fading());
    player.advance(0.5f, clips);
    GIANT_CHECK(std::fabs(x(player) - 1.25f) < 1e-5f); // halfway from 0.5 to 2
    player.advance(0.6f, clips);
    GIANT_CHECK(std::fabs(x(player) - 2.0f) < 1e-5f && !player.fading());
    // From a held blend: the same.
    player.set_blend(clips, 0, 2, 0.8f, false);
    GIANT_CHECK(std::fabs(x(player) - 1.6f) < 1e-5f);
    player.play(s, clips, 1, 1.0f);
    GIANT_CHECK(std::fabs(x(player) - 1.6f) < 1e-5f && !player.blending());
    player.advance(1.0f, clips);
    GIANT_CHECK(std::fabs(x(player) - 1.0f) < 1e-5f);
    // A start past a looping clip's end wraps into it; past a single play's, holds at its end.
    player.play(s, clips, 0, 0.0f, true, 1.25f);
    GIANT_CHECK(std::fabs(player.main().time - 0.25f) < 1e-5f);
    player.play(s, clips, 0, 0.0f, false, 7.0f);
    GIANT_CHECK(player.main().time == 1.0f);
    // A held blend loops even if its clip played once before; paused, b stays in step.
    player.set_blend(clips, 0, 1, 0.5f, true);
    GIANT_CHECK(player.main().loop);
    player.set_paused(true);
    player.set_time(0.5f, clips);
    player.advance(0.1f, clips);
    GIANT_CHECK(std::fabs(player.other().time - 0.5f) < 1e-5f);
    // A clip that is not there plays nothing.
    player.play(s, clips, 9);
    GIANT_CHECK(player.main().clip == k_no_clip && x(player) == 0.0f);
}
