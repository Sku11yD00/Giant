// Motion matching's player: a first frame by the way ahead, clips played
// round in place, jumps when the way ahead changes (blended, never
// popping), one-off clips left at their end, and the way ahead eased from
// a velocity.
#include "giant/anim/matcher.h"

#include "motion_test_clips.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::anim;
using namespace giant::anim::testing;

namespace {

constexpr float k_step = 1.0f / 60.0f;

struct Walker {
    Set set;
    MotionMatcher matcher;
    std::vector<JointTransform> local = std::vector<JointTransform>(3);
    std::vector<Mat4> model = std::vector<Mat4>(3);

    void update(const MotionAhead& ahead, float seconds = k_step, bool now = false) {
        matcher.update(set.db, set.skeleton, set.clips, ahead, seconds, now);
    }
    void run(const MotionAhead& ahead, float seconds) {
        for (float t = 0.0f; t < seconds - 0.5f * k_step; t += k_step) update(ahead);
    }
    const std::vector<JointTransform>& pose() {
        matcher.evaluate(set.db, set.skeleton, set.clips, local);
        model_matrices(set.skeleton, local, model);
        return local;
    }
    Vec3 place(usize joint) {
        pose();
        return {model[joint].m[12], model[joint].m[13], model[joint].m[14]};
    }
    u32 clip() const { return set.db.range_of(matcher.frame()).clip; }
};

MotionAhead curve() {
    MotionAhead ahead;
    const MotionSettings settings;
    const float radius = k_turn_speed / k_turn_rate;
    for (usize h = 0; h < k_motion_horizons; ++h) {
        const float a = k_turn_rate * settings.horizons[h];
        ahead.place[h] = {radius * (1.0f - std::cos(a)), radius * std::sin(a)};
        ahead.facing[h] = {std::sin(a), std::cos(a)};
    }
    return ahead;
}

float apart(const Vec3& a, const Vec3& b) { return length(a - b); }

} // namespace

GIANT_TEST("matcher: nothing plays until its first update, which picks by the way ahead") {
    Walker w;
    GIANT_CHECK(!w.matcher.playing());
    GIANT_CHECK(w.matcher.frame() == k_no_frame);
    GIANT_CHECK(w.pose() == w.set.skeleton.rest);
    GIANT_CHECK(w.matcher.root_motion(w.set.db).z == 0.0f);

    w.update(straight(1.5f));
    GIANT_CHECK(w.matcher.playing());
    GIANT_CHECK(w.clip() == 1);
    GIANT_CHECK(w.matcher.jumps() == 0);
    // Shown at once: just the clip's pose in place.
    std::vector<JointTransform> clip_pose(3);
    motion_pose(w.set.db, w.set.skeleton, w.set.clips, 1, w.matcher.seconds(), clip_pose);
    GIANT_CHECK(w.pose() == clip_pose);

    Walker runner;
    runner.update(straight(4.0f));
    GIANT_CHECK(runner.clip() == 2);
    Walker stander;
    stander.update(straight(0.0f));
    GIANT_CHECK(stander.clip() == 0);
}

GIANT_TEST("matcher: a clip that fits plays on round and round, in place") {
    Walker w;
    w.update(straight(1.5f));
    float furthest = 0.0f;
    float least_time = 1e9f;
    float most_time = 0.0f;
    for (int i = 0; i < 300; ++i) { // five seconds: five rounds of the walk
        w.update(straight(1.5f));
        GIANT_CHECK(w.clip() == 1);
        const Vec3 hips = w.place(0);
        furthest = std::max(furthest, std::hypot(hips.x, hips.z));
        GIANT_CHECK(near(hips.y, 1.0f, 1e-4f));
        least_time = std::min(least_time, w.matcher.seconds());
        most_time = std::max(most_time, w.matcher.seconds());
        const RootMotion motion = w.matcher.root_motion(w.set.db);
        GIANT_CHECK(near(motion.z, 1.5f, 1e-2f));
        GIANT_CHECK(near(motion.x, 0.0f, 1e-3f));
        GIANT_CHECK(near(motion.yaw, 0.0f, 1e-3f));
        // The feet where the database says this frame has them (between frames: nearly).
        const Vec3 left = w.place(1);
        GIANT_CHECK(near(left.z, w.set.db.raw(w.matcher.frame(), k_motion_foot_places + 2), 0.07f));
        GIANT_CHECK(near(left.x, 0.1f, 1e-3f));
    }
    GIANT_CHECK(furthest < 1e-3f);
    GIANT_CHECK(w.matcher.jumps() == 0);
    GIANT_CHECK(least_time < 0.05f && most_time > 0.95f && most_time < 1.0f);
}

GIANT_TEST("matcher: a new way ahead jumps to the clip that fits, when a search is due") {
    Walker w;
    w.update(straight(1.5f));
    w.run(straight(1.5f), 0.55f);
    GIANT_CHECK(w.clip() == 1);
    // Nothing is due for a tenth of a second after a search.
    w.update(straight(4.0f), 0.0f, true);
    GIANT_CHECK(w.clip() == 2);
    GIANT_CHECK(w.matcher.jumps() == 1);
    w.update(straight(0.0f));
    w.update(straight(0.0f));
    GIANT_CHECK(w.clip() == 2);
    w.run(straight(0.0f), 0.1f);
    GIANT_CHECK(w.clip() != 2);
    GIANT_CHECK(w.matcher.jumps() == 2);
    GIANT_CHECK(std::isfinite(w.matcher.last_cost()));

    // Searching every update.
    Walker eager;
    eager.matcher.settings.search_seconds = 0.0f;
    eager.update(straight(1.5f));
    eager.update(straight(4.0f));
    GIANT_CHECK(eager.clip() == 2);
}

GIANT_TEST("matcher: a jump does not show; what it left dies away") {
    // A walk into a run mid-stride, with and without blending.
    const auto worst_step = [](float blend_seconds, Vec3& settled, Vec3& clip_pose) {
        Walker w;
        w.matcher.settings.blend_seconds = blend_seconds;
        w.update(straight(1.5f));
        w.run(straight(1.5f), 0.3f);
        Vec3 before = w.place(1);
        float worst = 0.0f;
        for (int i = 0; i < 90; ++i) {
            w.update(straight(4.0f), k_step, i == 0);
            const Vec3 now = w.place(1);
            worst = std::max(worst, apart(now, before));
            before = now;
        }
        GIANT_CHECK(w.clip() == 2);
        GIANT_CHECK(w.matcher.jumps() == 1);
        settled = before;
        std::vector<JointTransform> local(3);
        std::vector<Mat4> model(3);
        motion_pose(w.set.db, w.set.skeleton, w.set.clips, 2, w.matcher.seconds(), local);
        model_matrices(w.set.skeleton, local, model);
        clip_pose = {model[1].m[12], model[1].m[13], model[1].m[14]};
        return worst;
    };
    Vec3 settled{}, clip_pose{};
    const float popped = worst_step(0.0f, settled, clip_pose);
    GIANT_CHECK(apart(settled, clip_pose) < 1e-5f);
    const float blended = worst_step(0.1f, settled, clip_pose);
    // The run's foot swings 0.8 m either way twice a second: up to 0.17 m a step. The pop is the feet's
    // difference at the jump, three times that.
    GIANT_CHECK(popped > 0.45f);
    GIANT_CHECK(blended < 0.2f);
    // A second and a half on, the offsets are gone.
    GIANT_CHECK(apart(settled, clip_pose) < 1e-3f);
}

GIANT_TEST("matcher: a one-off clip plays to its end and is left there") {
    Walker w;
    w.update(straight(1.5f));
    w.run(straight(1.5f), 1.0f + 2.0f * k_step); // into the walk's third frame, where the stop starts posed alike
    MotionAhead slowing;
    slowing.place = {{{0.0f, 0.4f}, {0.0f, 0.7f}, {0.0f, 0.9f}}};
    w.update(slowing, 0.0f, true);
    GIANT_CHECK(w.clip() == 4);
    // Wanting to stand from here on: the stop is played out (nothing fits a slowing pose better), then left.
    bool reached_end = false;
    for (int i = 0; i < 240 && w.clip() == 4; ++i) {
        w.update(straight(0.0f));
        if (w.clip() == 4 && w.matcher.seconds() > 1.4f) reached_end = true;
    }
    GIANT_CHECK(w.clip() == 0);
    w.run(straight(0.0f), 1.0f);
    GIANT_CHECK(w.clip() == 0);
    GIANT_CHECK(reached_end);
    const Vec3 hips = w.place(0);
    GIANT_CHECK(std::hypot(hips.x, hips.z) < 1e-3f);
    GIANT_CHECK(near(w.matcher.root_motion(w.set.db).z, 0.0f, 1e-3f));
}

GIANT_TEST("matcher: a one-off clip at its end is left even when nothing fits better") {
    // Only a walk and a stop: at the stop's end, standing, the walk is the one frame to go to.
    const Skeleton s = biped();
    const std::vector<Clip> clips{go("walk", 1.5f, 1.0f), stop(1.5f, 1.5f)};
    const MotionClip chosen[] = {{0, true}, {1, false}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
    MotionMatcher m;
    MotionAhead slowing;
    slowing.place = {{{0.0f, 0.4f}, {0.0f, 0.7f}, {0.0f, 0.9f}}};
    m.update(db, s, clips, slowing, k_step);
    GIANT_CHECK(db.range_of(m.frame()).clip == 1);
    // Wanting to stand, it plays on through the stop's last second, where nothing may jump in...
    const MotionRange& stop_range = db.ranges[1];
    u32 furthest = 0;
    int updates = 0;
    for (; updates < 200 && m.frame() >= furthest; ++updates) {
        furthest = m.frame();
        m.update(db, s, clips, straight(0.0f), k_step);
    }
    GIANT_CHECK(furthest >= stop_range.first + stop_range.count - 3);
    // ...and at its end is not held there: standing fits nothing here, so it goes back to the
    // stillest frame it may jump to (a second before the end), blended.
    GIANT_CHECK(updates < 200);
    GIANT_CHECK(m.frame() < stop_range.first + stop_range.startable && m.frame() >= stop_range.first);
    GIANT_CHECK(m.jumps() >= 1);
    GIANT_CHECK(m.root_motion(db).z > 0.1f);
}

GIANT_TEST("matcher: a turning loop turns in place; its root motion says how") {
    Walker w;
    w.update(curve());
    GIANT_CHECK(w.clip() == 3);
    for (int i = 0; i < 400; ++i) { // more than three rounds
        w.update(curve());
        GIANT_CHECK(w.clip() == 3);
        const Vec3 hips = w.place(0);
        GIANT_CHECK(std::hypot(hips.x, hips.z) < 0.02f); // (the root is the hips averaged along the curve)
        // The hips keep facing +z.
        const Vec3 facing = rotate(w.pose()[0].rotation, {0.0f, 0.0f, 1.0f});
        GIANT_CHECK(near(facing.x, 0.0f, 0.01f));
        GIANT_CHECK(facing.z > 0.99f);
        const RootMotion motion = w.matcher.root_motion(w.set.db);
        GIANT_CHECK(near(motion.yaw, k_turn_rate, 0.01f));
        GIANT_CHECK(near(motion.z, k_turn_speed, 0.03f));
        GIANT_CHECK(near(motion.x, 0.0f, 0.05f));
    }
    GIANT_CHECK(w.matcher.jumps() == 0);
}

GIANT_TEST("matcher: a pose stands in place whatever is above the skeleton's roots") {
    Set set;
    Skeleton moved = set.skeleton;
    moved.root = mat4_from_transform({5.0f, 0.0f, -3.0f}, quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.0f),
                                     {1.0f, 1.0f, 1.0f});
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(moved, set.clips, set.chosen, find_motion_joints(moved), {}, db).empty());
    std::vector<JointTransform> local(3);
    std::vector<Mat4> model(3);
    for (const float seconds : {0.0f, 0.31f, 0.77f}) {
        motion_pose(db, moved, set.clips, 1, seconds, local);
        model_matrices(moved, local, model);
        GIANT_CHECK(std::hypot(model[0].m[12], model[0].m[14]) < 1e-3f);
        GIANT_CHECK(near(model[0].m[13], 1.0f, 1e-4f));
        // Its feet either side of it across its facing, as the database has them.
        const u32 frame = db.ranges[1].first + static_cast<u32>(seconds * 30.0f);
        GIANT_CHECK(near(model[1].m[12], db.raw(frame, k_motion_foot_places + 0), 0.02f));
        GIANT_CHECK(near(model[1].m[14], db.raw(frame, k_motion_foot_places + 2), 0.07f));
    }
    // A range or clip that is not there: the rest pose.
    motion_pose(db, moved, set.clips, 99, 0.0f, local);
    GIANT_CHECK(local == moved.rest);
}

GIANT_TEST("matcher: the way ahead eases from the velocity now to the one wanted") {
    const MotionSettings settings;
    const MotionAhead steady = motion_ahead(settings, {0.0f, 1.5f}, {0.0f, 1.5f}, 0.0f);
    for (usize h = 0; h < k_motion_horizons; ++h) {
        GIANT_CHECK(near(steady.place[h][0], 0.0f, 1e-6f));
        GIANT_CHECK(near(steady.place[h][1], 1.5f * settings.horizons[h], 1e-5f));
        GIANT_CHECK(steady.facing[h][0] == 0.0f && steady.facing[h][1] == 1.0f);
    }
    // From a stand to 2 m/s: behind where 2 m/s all along would be, catching up.
    const MotionAhead starting = motion_ahead(settings, {0.0f, 0.0f}, {0.0f, 2.0f}, 0.0f);
    float last = 0.0f;
    float lag_before = 0.0f;
    for (usize h = 0; h < k_motion_horizons; ++h) {
        const float lag = 2.0f * settings.horizons[h] - starting.place[h][1];
        GIANT_CHECK(starting.place[h][1] > last);
        GIANT_CHECK(lag > lag_before);
        last = starting.place[h][1];
        lag_before = lag;
    }
    GIANT_CHECK(lag_before < 2.0f * 0.25f * 1.5f); // it loses about the easing time's worth, no more
    GIANT_CHECK(starting.place[2][1] > 1.2f);
    // Stopping from 2 m/s: carried on a little, then no further.
    const MotionAhead stopping = motion_ahead(settings, {0.0f, 2.0f}, {0.0f, 0.0f}, 0.0f);
    GIANT_CHECK(stopping.place[0][1] > 0.2f && stopping.place[2][1] < 0.8f);
    GIANT_CHECK(stopping.place[2][1] - stopping.place[1][1] < 0.1f);
    // A quarter turn: most of it within the second, never past it.
    const float quarter = 0.25f * k_tau;
    const MotionAhead turning = motion_ahead(settings, {0.0f, 1.5f}, {1.5f, 0.0f}, quarter);
    GIANT_CHECK(turning.facing[0][0] > 0.1f && turning.facing[0][0] < turning.facing[1][0]);
    GIANT_CHECK(turning.facing[2][0] > 0.9f && turning.facing[2][0] <= 1.0f);
    GIANT_CHECK(turning.place[2][0] > 0.6f);
    // A quicker ease gets there sooner.
    const MotionAhead quick = motion_ahead(settings, {0.0f, 0.0f}, {0.0f, 2.0f}, 0.0f, 0.05f);
    GIANT_CHECK(quick.place[2][1] > starting.place[2][1]);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const MotionAhead& odd : {motion_ahead(settings, {nan, inf}, {-inf, nan}, nan, nan),
                                   motion_ahead(settings, {1e30f, 0.0f}, {0.0f, -1e30f}, 1e30f, 0.0f),
                                   motion_ahead(settings, {0.0f, 1.0f}, {0.0f, 1.0f}, inf, -1.0f)}) {
        for (usize h = 0; h < k_motion_horizons; ++h) {
            GIANT_CHECK(std::isfinite(odd.place[h][0]) && std::isfinite(odd.place[h][1]));
            GIANT_CHECK(near(std::hypot(odd.facing[h][0], odd.facing[h][1]), 1.0f, 1e-4f));
        }
    }
}

GIANT_TEST("matcher: a database that is not the skeleton's, or was changed under it, starts it afresh") {
    Walker w;
    w.update(straight(4.0f));
    w.run(straight(4.0f), 0.3f);
    GIANT_CHECK(w.clip() == 2);

    // Nothing to play.
    const MotionDatabase none;
    w.matcher.update(none, w.set.skeleton, w.set.clips, straight(4.0f), k_step);
    GIANT_CHECK(!w.matcher.playing());
    w.matcher.evaluate(none, w.set.skeleton, w.set.clips, w.local);
    GIANT_CHECK(w.local == w.set.skeleton.rest);
    GIANT_CHECK(w.matcher.root_motion(none).z == 0.0f);

    // Built again with fewer clips: the frame it played is past the end.
    w.update(straight(0.0f));
    w.run(curve(), 0.5f);
    GIANT_CHECK(w.clip() == 3);
    const u32 old_frame = w.matcher.frame();
    const MotionClip only_walk[] = {{1, true}};
    MotionDatabase smaller;
    GIANT_CHECK(build_motion_database(w.set.skeleton, w.set.clips, only_walk, find_motion_joints(w.set.skeleton), {},
                                      smaller)
                    .empty());
    GIANT_CHECK(old_frame >= smaller.frames());
    w.matcher.evaluate(smaller, w.set.skeleton, w.set.clips, w.local);
    GIANT_CHECK(w.local == w.set.skeleton.rest);
    w.matcher.update(smaller, w.set.skeleton, w.set.clips, straight(1.5f), k_step);
    GIANT_CHECK(w.matcher.playing() && w.matcher.frame() < smaller.frames());
    GIANT_CHECK(w.matcher.jumps() == 0);

    // Clips that are not the database's (fewer): nothing plays.
    const std::vector<Clip> fewer;
    w.matcher.update(smaller, w.set.skeleton, fewer, straight(1.5f), k_step);
    GIANT_CHECK(!w.matcher.playing());
    // A skeleton with other joints: the buffers follow it.
    Skeleton more = w.set.skeleton;
    more.names.push_back("Head");
    more.parents.push_back(0);
    more.rest.push_back(JointTransform{{0.0f, 0.6f, 0.0f}});
    more.inverse_bind.push_back(Mat4{});
    std::vector<JointTransform> four(4);
    w.matcher.update(smaller, more, w.set.clips, straight(1.5f), k_step);
    w.matcher.update(smaller, more, w.set.clips, straight(1.5f), k_step);
    w.matcher.evaluate(smaller, more, w.set.clips, four);
    GIANT_CHECK(four[3] == more.rest[3]);
    // A span shorter than the skeleton is filled as far as it goes.
    std::vector<JointTransform> two(2);
    w.matcher.evaluate(smaller, more, w.set.clips, two);
    GIANT_CHECK(two[1] == four[1]);
}

GIANT_TEST("matcher: time that is no number, backwards or too long is kept to what can be played") {
    Walker w;
    w.update(straight(1.5f));
    const float start = w.matcher.seconds();
    w.update(straight(1.5f), std::numeric_limits<float>::quiet_NaN());
    w.update(straight(1.5f), -5.0f);
    GIANT_CHECK(w.matcher.seconds() == start);
    w.update(straight(1.5f), 1e9f); // a second at most: once round the walk
    GIANT_CHECK(w.matcher.playing());
    GIANT_CHECK(near(w.matcher.seconds(), start, 1e-3f) || near(std::fabs(w.matcher.seconds() - start), 1.0f, 1e-3f));
    for (const JointTransform& t : w.pose()) {
        GIANT_CHECK(std::isfinite(t.translation.x) && std::isfinite(t.rotation.w));
    }
    w.matcher.reset();
    GIANT_CHECK(!w.matcher.playing() && w.matcher.jumps() == 0);
}

GIANT_TEST("matcher: root joints mirrored or scaled stand in place as they are") {
    for (const Vec3& scale : {Vec3{1.0f, -1.0f, 1.0f}, Vec3{-1.0f, -1.0f, 1.0f}, Vec3{1.0f, 1.0f, -1.0f},
                              Vec3{2.0f, 1.0f, 0.5f}}) {
        Set set;
        // The turning loop with its hips scaled so all along.
        Channel scaled = channel(0, Path::scale);
        scaled.times = {0.0f};
        scaled.values = {{scale.x, scale.y, scale.z, 0.0f}};
        set.clips[3].channels.push_back(scaled);
        MotionDatabase db;
        GIANT_CHECK(build_motion_database(set.skeleton, set.clips, set.chosen, find_motion_joints(set.skeleton), {}, db)
                        .empty());
        std::vector<JointTransform> local(3);
        std::vector<JointTransform> raw(3);
        for (const float seconds : {0.0f, 0.7f, 1.6f}) {
            motion_pose(db, set.skeleton, set.clips, 3, seconds, local);
            sample(set.skeleton, set.clips[3], seconds, raw);
            GIANT_CHECK(local[0].scale == scale);
            GIANT_CHECK(local[1] == raw[1] && local[2] == raw[2]); // only the root joint is touched
            // The hips as the clip has them, turned back by the root's yaw about the root's place.
            const u32 frame = db.ranges[3].first + static_cast<u32>(seconds * 30.0f + 0.5f);
            const MotionRoot root = db.roots[frame];
            const Quat back = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -root.yaw);
            const Vec3 want = rotate(back, raw[0].translation - Vec3{root.x, 0.0f, root.z});
            GIANT_CHECK(apart(local[0].translation, want) < 2e-3f);
            const Quat turned = back * raw[0].rotation;
            const float alike = std::fabs(turned.x * local[0].rotation.x + turned.y * local[0].rotation.y +
                                          turned.z * local[0].rotation.z + turned.w * local[0].rotation.w);
            GIANT_CHECK(alike > 0.9999f);
        }
    }
}

GIANT_TEST("matcher: a better frame near the one playing, or hardly better, is no jump") {
    // Two rounds of the same walk as one clip's worth each: every frame has a twin half a clip on.
    const Skeleton s = biped();
    Clip twice = go("walk", 1.5f, 2.0f);
    twice.channels.resize(1);
    swing_feet(twice, 0.3f, 2.0f);
    const std::vector<Clip> clips{twice};
    const MotionClip chosen[] = {{0, true}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
    MotionMatcher m;
    m.settings.search_seconds = 0.0f;
    for (int i = 0; i < 600; ++i) m.update(db, s, clips, straight(1.5f), k_step);
    GIANT_CHECK(m.jumps() == 0); // twins cost the same but for rounding: not a twentieth better
    GIANT_CHECK(m.last_cost() < 1e-3f);

    // With no bias, nearness covering the whole clip keeps it too.
    MotionMatcher near_all;
    near_all.settings.search_seconds = 0.0f;
    near_all.settings.stay_bias = 0.0f;
    near_all.settings.near_frames = 60;
    for (int i = 0; i < 600; ++i) near_all.update(db, s, clips, straight(1.5f), k_step);
    GIANT_CHECK(near_all.jumps() == 0);

    // Nearness goes round a loop's end: its last frame and its first are one apart.
    Set set;
    MotionMatcher wrap;
    wrap.settings.search_seconds = 0.0f;
    wrap.settings.stay_bias = 0.0f;
    wrap.settings.near_frames = 3;
    for (int i = 0; i < 900; ++i) wrap.update(set.db, set.skeleton, set.clips, straight(1.5f), k_step);
    GIANT_CHECK(wrap.jumps() == 0);
    GIANT_CHECK(set.db.range_of(wrap.frame()).clip == 1);
}

GIANT_TEST("matcher: jumps on jumps stay smooth, and settings out of range are kept in it") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float blend : {0.1f, 1e9f, std::numeric_limits<float>::infinity(), nan, -1.0f}) {
        Walker w;
        w.matcher.settings.blend_seconds = blend;
        w.matcher.settings.search_seconds = blend == 0.1f ? 0.1f : nan;
        w.matcher.settings.pose_weight = blend == 0.1f ? 0.5f : blend;
        w.matcher.settings.stay_bias = blend == 0.1f ? 0.05f : blend;
        w.update(straight(1.5f));
        Vec3 before = w.place(1);
        float worst = 0.0f;
        float furthest = 0.0f;
        // A new mind every 0.15 s: walk, run, stand, turn, by turns, for a minute.
        for (int i = 0; i < 3600; ++i) {
            const int mind = (i / 9) % 4;
            w.update(mind == 0 ? straight(1.5f) : mind == 1 ? straight(4.0f) : mind == 2 ? straight(0.0f) : curve(),
                     k_step, i % 9 == 0);
            const Vec3 now = w.place(1);
            worst = std::max(worst, apart(now, before));
            furthest = std::max(furthest, length(now));
            before = now;
            for (const JointTransform& t : w.local) {
                const Quat& q = t.rotation;
                GIANT_CHECK(near(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0f, 1e-3f));
            }
        }
        if (blend == 0.1f) GIANT_CHECK(w.matcher.jumps() > 50);
        GIANT_CHECK(std::isfinite(worst));
        GIANT_CHECK(furthest < 3.0f); // the foot stays about the body, however long the blend asked
        if (blend == 0.1f) GIANT_CHECK(worst < 0.2f); // no more than the run's own swing a step
        // Eight steady seconds on, what the jumps left is gone (the longest blend halves in a second).
        const u64 jumps = w.matcher.jumps();
        w.run(straight(1.5f), 8.0f);
        if (w.matcher.jumps() == jumps) { // (it stayed in its clip: nothing new to blend)
            std::vector<JointTransform> clip_pose(3);
            const usize range = static_cast<usize>(&w.set.db.range_of(w.matcher.frame()) - w.set.db.ranges.data());
            motion_pose(w.set.db, w.set.skeleton, w.set.clips, range, w.matcher.seconds(), clip_pose);
            w.pose();
            GIANT_CHECK(apart(w.local[1].translation, clip_pose[1].translation) < 0.02f);
        }
    }
}

GIANT_TEST("matcher: a pose stands in place under a root that is turned, shrunk or mirrored") {
    Set set;
    const Quat tipped = quat_from_axis_angle(normalize_or_zero(Vec3{1.0f, 0.3f, -0.2f}), 1.1f);
    const Mat4 roots[] = {
        mat4_from_transform({0.0f, 0.0f, 0.0f}, quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 0.5f * k_tau / 2.0f),
                            {1.0f, 1.0f, 1.0f}),
        mat4_from_transform({1.0f, 2.0f, 3.0f}, tipped, {0.01f, 0.01f, 0.01f}),
        mat4_from_transform({0.0f, 0.0f, 0.0f}, {}, {-1.0f, 1.0f, 1.0f}),
        mat4_from_transform({0.0f, 0.0f, 0.0f}, {}, {1.0f, 1.0f, -1.0f}),
        mat4_from_transform({0.5f, 0.0f, 0.0f}, tipped, {3.0f, -3.0f, 3.0f}),
    };
    for (const Mat4& root : roots) {
        Skeleton s = set.skeleton;
        s.root = root;
        MotionDatabase db;
        // (Whatever such a root makes of the clips: only that the pose is the clip's, moved rigidly.)
        if (!build_motion_database(s, set.clips, set.chosen, find_motion_joints(s), {}, db).empty()) continue;
        std::vector<JointTransform> local(3);
        std::vector<JointTransform> raw(3);
        std::vector<Mat4> model(3);
        std::vector<Mat4> raw_model(3);
        for (const float seconds : {0.2f, 0.9f, 1.7f}) {
            motion_pose(db, s, set.clips, 3, seconds, local);
            sample(s, set.clips[3], seconds, raw);
            model_matrices(s, local, model);
            model_matrices(s, raw, raw_model);
            // Model space: every joint's matrix is the clip's, with the root's yaw and travel undone.
            const u32 at = static_cast<u32>(seconds / db.ranges[3].frame_seconds);
            const float part = seconds / db.ranges[3].frame_seconds - static_cast<float>(at);
            const MotionRoot a = db.roots[db.ranges[3].first + at];
            const MotionRoot b = db.roots[db.ranges[3].first + at + 1];
            const MotionRoot r{a.x + (b.x - a.x) * part, a.z + (b.z - a.z) * part, a.yaw + (b.yaw - a.yaw) * part};
            const Quat back = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -r.yaw);
            const Mat4 undo = mat4_from_transform(rotate(back, {-r.x, 0.0f, -r.z}), back, {1.0f, 1.0f, 1.0f});
            for (usize j = 0; j < 3; ++j) {
                const Mat4 want = undo * raw_model[j];
                for (int k = 0; k < 16; ++k) GIANT_CHECK(near(model[j].m[k], want.m[k], 2e-3f));
            }
        }
    }
}

GIANT_TEST("matcher: nearness is counted in frames, round a loop's end too") {
    // A clip that slows as it goes, flagged a loop: its stillest stretch is its end, its quickest its start.
    const Skeleton s = biped();
    const std::vector<Clip> clips{stop(1.5f, 1.5f)};
    const MotionClip chosen[] = {{0, true}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
    const auto jumps_after = [&](u32 near_frames, float start_seconds, const MotionAhead& wanted) {
        MotionMatcher m;
        m.settings.pose_weight = 0.0f; // only the way ahead: the best frame is wherever that fits
        m.settings.stay_bias = 0.0f;
        m.settings.near_frames = near_frames;
        m.settings.search_seconds = 1000.0f;
        MotionAhead first = straight(1.5f);
        m.update(db, s, clips, first, k_step);
        // (Played on to where the test wants it, with no search due.)
        const float from = m.seconds();
        float to_go = start_seconds - from;
        if (to_go < 0.0f) to_go += 1.5f;
        m.update(db, s, clips, first, to_go);
        const u64 before = m.jumps();
        m.update(db, s, clips, wanted, 0.0f, true);
        return m.jumps() - before;
    };
    // Near its end (1.4 s in, two or three frames from the wrap) and wanting to go at full speed:
    // the best frames are its first ones, a few frames on round the end.
    GIANT_CHECK(jumps_after(0, 1.4f, straight(1.5f)) == 1);
    GIANT_CHECK(jumps_after(20, 1.4f, straight(1.5f)) == 0); // (the long way round is over 20)
    // Half a second in and wanting the same: the best is fifteen frames back. Near enough only if asked.
    GIANT_CHECK(jumps_after(8, 0.5f, straight(1.5f)) == 1);
    GIANT_CHECK(jumps_after(20, 0.5f, straight(1.5f)) == 0);
}

GIANT_TEST("matcher: staying is preferred by the bias asked; the cost reported is the playing frame's") {
    const auto after_wanting_a_run = [](float bias, float weight) {
        Walker w;
        w.matcher.settings.stay_bias = bias;
        w.matcher.settings.pose_weight = weight;
        w.update(straight(1.5f));
        w.run(straight(1.5f), 0.3f);
        w.update(straight(4.0f), 0.0f, true);
        return w;
    };
    Walker eager = after_wanting_a_run(0.05f, 0.5f);
    GIANT_CHECK(eager.clip() == 2 && eager.matcher.jumps() == 1);
    // What it cost to land there: the run's frame against the walk's pose and the way wanted.
    GIANT_CHECK(eager.matcher.last_cost() > 1.0f);
    // A bias of 1 (or more): nothing is ever enough better.
    Walker stubborn = after_wanting_a_run(1.0f, 0.5f);
    GIANT_CHECK(stubborn.clip() == 1 && stubborn.matcher.jumps() == 0);
    GIANT_CHECK(after_wanting_a_run(50.0f, 0.5f).clip() == 1);
    // The cost reported is the walk's own against a run's way: what staying costs.
    const MotionFeatures query = motion_query(stubborn.set.db, stubborn.matcher.frame(), straight(4.0f));
    GIANT_CHECK(near(stubborn.matcher.last_cost(),
                     motion_cost(stubborn.set.db, stubborn.matcher.frame(), query, 0.5f), 1e-3f));
    GIANT_CHECK(stubborn.matcher.last_cost() > 5.0f);
    // No number, or under 0: no bias, not a refusal to move.
    GIANT_CHECK(after_wanting_a_run(std::numeric_limits<float>::quiet_NaN(), 0.5f).clip() == 2);
    GIANT_CHECK(after_wanting_a_run(-3.0f, 0.5f).clip() == 2);
    // With the pose counted for much the walk is stayed in (nothing here goes from one to the other).
    GIANT_CHECK(after_wanting_a_run(0.05f, 2.0f).clip() == 1);
}

GIANT_TEST("matcher: a jump keeps how fast each joint was moving and turning") {
    // A walk and a run whose left foot also pitches, at different rates and angles.
    Set set;
    const auto pitch = [](Clip& clip, float cycles, float lean) {
        Channel turning = channel(1, Path::rotation);
        const int keys = 64;
        for (int i = 0; i <= keys; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(keys);
            turning.times.push_back(u * clip.duration);
            turning.values.push_back(
                q4(quat_from_axis_angle({1.0f, 0.0f, 0.0f}, lean + 0.5f * std::sin(k_tau * cycles * u))));
        }
        clip.channels.push_back(turning);
    };
    pitch(set.clips[1], 1.0f, 0.0f);
    pitch(set.clips[2], 1.0f, 0.6f);
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(set.skeleton, set.clips, set.chosen, find_motion_joints(set.skeleton), {}, db)
                    .empty());
    MotionMatcher m;
    std::vector<JointTransform> local(3);
    const auto foot = [&] {
        m.evaluate(db, set.skeleton, set.clips, local);
        return local[1];
    };
    const auto angle = [](const Quat& a, const Quat& b) {
        const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        return 2.0f * std::acos(std::min(d, 1.0f));
    };
    m.update(db, set.skeleton, set.clips, straight(1.5f), k_step);
    for (int i = 0; i < 20; ++i) m.update(db, set.skeleton, set.clips, straight(1.5f), k_step);
    const JointTransform two_back = foot();
    m.update(db, set.skeleton, set.clips, straight(1.5f), k_step);
    const JointTransform one_back = foot();
    const float walk_seconds = m.seconds() + k_step; // where the walk is when the jump is made
    m.update(db, set.skeleton, set.clips, straight(4.0f), k_step, true);
    GIANT_CHECK(m.jumps() == 1 && db.range_of(m.frame()).clip == 2);
    const float run_seconds = m.seconds();
    const JointTransform landed = foot();
    m.update(db, set.skeleton, set.clips, straight(4.0f), k_step);
    const JointTransform next = foot();

    // At the jump the walk's pose still shows, to the bit of rounding.
    std::vector<JointTransform> from(3), from_before(3), to(3), to_before(3), to_next(3);
    const float moment = 1.0f / 60.0f;
    motion_pose(db, set.skeleton, set.clips, 1, walk_seconds, from);
    motion_pose(db, set.skeleton, set.clips, 1, walk_seconds - moment, from_before);
    motion_pose(db, set.skeleton, set.clips, 2, run_seconds, to);
    motion_pose(db, set.skeleton, set.clips, 2, run_seconds - moment, to_before);
    motion_pose(db, set.skeleton, set.clips, 2, run_seconds + k_step, to_next);
    GIANT_CHECK(apart(landed.translation, from[1].translation) < 1e-5f);
    GIANT_CHECK(angle(landed.rotation, from[1].rotation) < 2e-3f);
    GIANT_CHECK(apart(from[1].translation, to[1].translation) > 0.1f); // (the poses are well apart)

    // A step on: the run's pose and an offset that began as the difference of the two poses, moving
    // as the difference of their velocities, and is drawn in by a critically damped spring.
    const Vec3 offset = from[1].translation - to[1].translation;
    const Vec3 offset_velocity = ((from[1].translation - from_before[1].translation) -
                                  (to[1].translation - to_before[1].translation)) * (1.0f / moment);
    const float y = 2.0f * 0.69314718f / m.settings.blend_seconds;
    const Vec3 drawn = (offset + (offset_velocity + offset * y) * k_step) * std::exp(-y * k_step);
    GIANT_CHECK(apart(next.translation, to_next[1].translation + drawn) < 1e-4f);
    // (With the velocities dropped the offset would only shrink: a visibly different step.)
    const Vec3 shrunk = (offset + offset * (y * k_step)) * std::exp(-y * k_step);
    GIANT_CHECK(apart(to_next[1].translation + shrunk, to_next[1].translation + drawn) > 0.01f);

    // Its turning carries on as it was going too: the step after the jump turns as the step before it
    // did, though the run's foot leans 0.6 rad more and turns its own way.
    const float turned_before = angle(one_back.rotation, two_back.rotation);
    const float turned_after = angle(next.rotation, landed.rotation);
    GIANT_CHECK(turned_before > 0.02f);
    GIANT_CHECK(std::fabs(turned_after - turned_before) < 0.3f * turned_before);
    GIANT_CHECK(angle(next.rotation, to_next[1].rotation) > 0.3f);
}

GIANT_TEST("matcher: a blend asked to last for ever still dies away within seconds") {
    for (const float blend : {std::numeric_limits<float>::infinity(), 1e9f, 30.0f}) {
        Walker w;
        w.matcher.settings.blend_seconds = blend;
        w.update(straight(1.5f));
        w.run(straight(1.5f), 0.3f);
        w.update(straight(4.0f), 0.0f, true);
        GIANT_CHECK(w.clip() == 2 && w.matcher.jumps() == 1);
        w.run(straight(4.0f), 10.0f);
        GIANT_CHECK(w.clip() == 2 && w.matcher.jumps() == 1);
        std::vector<JointTransform> clip_pose(3);
        motion_pose(w.set.db, w.set.skeleton, w.set.clips, 2, w.matcher.seconds(), clip_pose);
        w.pose();
        GIANT_CHECK(apart(w.local[1].translation, clip_pose[1].translation) < 0.02f);
    }
}

namespace {

// A person moved as the editor moves one: its goal's velocity eased toward
// what is wanted (half the gap in 0.2 s), the way ahead from that.
struct Moved {
    PersonSet set;
    MotionMatcher matcher;
    float going = 0.0f;
    u32 changes = 0;

    explicit Moved(bool with_run = true) : set(with_run) {}
    u32 clip() const { return set.db.range_of(matcher.frame()).clip; }
    void step(float wanted) {
        going += (wanted - going) * (1.0f - std::exp(-0.69314718f * k_step / 0.2f));
        const bool playing = matcher.frame() != k_no_frame;
        const u32 was = playing ? clip() : 0;
        matcher.update(set.db, set.skeleton, set.clips, motion_ahead(set.db.settings, {0.0f, going}, {0.0f, wanted}, 0.0f), k_step);
        if (playing && clip() != was) ++changes;
    }
    void run(float wanted, float seconds) {
        for (float t = 0.0f; t < seconds - 0.5f * k_step; t += k_step) step(wanted);
    }
};

} // namespace

GIANT_TEST("matcher: one sent walking leaves its idle though its set has a run too, and stands again when it stops") {
    // (A run beside the walk spreads the set's speeds wide: scaled by all of that spread, a walk's worth of
    // way ahead counted for too little to leave the idle, whose pose is far from any stride's.)
    for (const float speed : {1.05f, 1.4f}) {
        Moved m;
        GIANT_CHECK(m.set.problem.empty());
        m.run(0.0f, 1.0f);
        GIANT_CHECK(m.clip() == 0);
        m.run(speed, 5.0f);
        GIANT_CHECK(m.clip() == 1);
        // Steady from then on: played round, no jump.
        const u64 jumps = m.matcher.jumps();
        m.run(speed, 5.0f);
        GIANT_CHECK(m.clip() == 1 && m.matcher.jumps() == jumps);
        m.run(0.0f, 2.0f);
        GIANT_CHECK(m.clip() == 0);
    }
    // Faster, it runs; in between, it stays as it is (what plays is left only for something clearly better).
    Moved m;
    m.run(1.4f, 3.0f);
    GIANT_CHECK(m.clip() == 1);
    m.run(2.7f, 3.0f);
    GIANT_CHECK(m.clip() == 1);
    m.run(3.0f, 3.0f);
    GIANT_CHECK(m.clip() == 1);
    m.run(4.0f, 3.0f);
    GIANT_CHECK(m.clip() == 2);
    m.run(2.7f, 3.0f);
    GIANT_CHECK(m.clip() == 2);
    m.run(2.4f, 3.0f);
    GIANT_CHECK(m.clip() == 2);
    m.run(1.4f, 3.0f);
    GIANT_CHECK(m.clip() == 1);
    // A speed that wavers about the middle (a quarter of a metre a second either way, anew each step, as
    // one jostled in a crowd) does not change the clip.
    Moved wavering;
    wavering.run(1.4f, 3.0f);
    u32 seed = 12345;
    wavering.changes = 0;
    for (int i = 0; i < 3600; ++i) {
        seed = seed * 1664525u + 1013904223u;
        wavering.step(2.7f + 0.25f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f));
    }
    GIANT_CHECK(wavering.changes <= 1);
    // A set that only stands and walks is left as it was: the caps lie above its own spread.
    Moved two(false);
    two.run(0.0f, 1.0f);
    two.run(1.4f, 3.0f);
    GIANT_CHECK(two.clip() == 1);
    two.run(0.0f, 2.0f);
    GIANT_CHECK(two.clip() == 0);
}

GIANT_TEST("matcher: one that stops at once still plays its stop clip, its velocity easing off") {
    Walker w;
    float going = 1.5f;
    const auto step = [&](float wanted) {
        going += (wanted - going) * (1.0f - std::exp(-0.69314718f * k_step / 0.2f));
        w.update(motion_ahead(w.set.db.settings, {0.0f, going}, {0.0f, wanted}, 0.0f));
    };
    for (int i = 0; i < 180; ++i) step(1.5f);
    GIANT_CHECK(w.clip() == 1);
    bool stopped = false;
    for (int i = 0; i < 150; ++i) {
        step(0.0f);
        stopped = stopped || w.clip() == 4;
        GIANT_CHECK(w.clip() == 1 || w.clip() == 4 || (stopped && w.clip() == 0)); // walk, stop, then idle
    }
    GIANT_CHECK(stopped && w.clip() == 0);
}
