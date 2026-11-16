// Motion matching's database: joints found by name, loops told from
// one-off clips, each frame's numbers (feet, velocities, the way ahead) in
// the character's space, loops going round (turning ones too), and the
// search against checking every frame.
#include "giant/anim/motion.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::anim;

namespace {

constexpr float k_tau = 2.0f * std::numbers::pi_v<float>;

// Hips a metre up with a foot under each side (left is +x, facing +z).
Skeleton biped(const char* hips = "Hips", const char* left = "LeftFoot", const char* right = "RightFoot") {
    Skeleton s;
    s.names = {hips, left, right};
    s.parents = {-1, 0, 0};
    s.rest = {JointTransform{{0.0f, 1.0f, 0.0f}}, JointTransform{{0.1f, -1.0f, 0.0f}},
              JointTransform{{-0.1f, -1.0f, 0.0f}}};
    s.inverse_bind = {Mat4{}, Mat4{}, Mat4{}};
    return s;
}

Channel channel(u32 joint, Path path) {
    Channel c;
    c.joint = joint;
    c.path = path;
    return c;
}

Vec4 q4(const Quat& q) { return {q.x, q.y, q.z, q.w}; }

// Feet swinging fore and aft `stride` either way, `steps` cycles over the clip.
void swing_feet(Clip& clip, float stride, float steps) {
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
Clip go(const char* name, float speed, float seconds, const Vec3& along = {0.0f, 0.0f, 1.0f}) {
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

// Standing, shifting its feet a little.
Clip idle() {
    Clip c;
    c.name = "idle";
    c.duration = 2.0f;
    swing_feet(c, 0.01f, 1.0f);
    return c;
}

// Slowing from `speed` to a stand over `seconds` (a one-off), stepping once a second.
Clip stop(float speed, float seconds) {
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

constexpr float k_turn_speed = 1.5f;
constexpr float k_turn_rate = 0.25f * std::numbers::pi_v<float>; // a quarter turn in 2 s

// Walking a quarter of a circle toward +x: a loop that turns.
Clip turn() {
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

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// Checking every startable frame.
MotionMatch slow_search(const MotionDatabase& db, const MotionFeatures& query, bool pose = true) {
    MotionMatch best;
    for (const MotionRange& r : db.ranges) {
        for (u32 f = r.first; f < r.first + r.startable; ++f) {
            const float cost = motion_cost(db, f, query, pose);
            if (cost < best.cost) best = {f, cost};
        }
    }
    return best;
}

MotionAhead straight(float speed) {
    MotionAhead ahead;
    const MotionSettings settings;
    for (usize h = 0; h < k_motion_horizons; ++h) ahead.place[h] = {0.0f, speed * settings.horizons[h]};
    return ahead;
}

} // namespace

GIANT_TEST("motion: the hips and feet are found by the usual names, whatever the prefix and case") {
    const MotionJoints plain = find_motion_joints(biped());
    GIANT_CHECK(plain.found());
    GIANT_CHECK(plain.hips == 0 && plain.left_foot == 1 && plain.right_foot == 2);

    const MotionJoints mixamo =
        find_motion_joints(biped("mixamorig:Hips", "mixamorig:LeftFoot", "mixamorig:RightFoot"));
    GIANT_CHECK(mixamo == plain);
    GIANT_CHECK(find_motion_joints(biped("pelvis", "foot_l", "foot_r")) == plain);
    GIANT_CHECK(find_motion_joints(biped("Armature|PELVIS", "L_Foot", "R_Foot")) == plain);

    const MotionJoints none = find_motion_joints(biped("spine", "LeftFoot", "hand_r"));
    GIANT_CHECK(!none.found());
    GIANT_CHECK(none.hips == k_no_joint && none.left_foot == 1 && none.right_foot == k_no_joint);
    GIANT_CHECK(!find_motion_joints(Skeleton{}).found());
}

GIANT_TEST("motion: a clip that ends posed as it began loops, wherever it has got to") {
    const Skeleton s = biped();
    const MotionJoints joints = find_motion_joints(s);
    GIANT_CHECK(clip_loops(s, go("walk", 1.5f, 1.0f), joints));
    GIANT_CHECK(clip_loops(s, turn(), joints));
    GIANT_CHECK(clip_loops(s, idle(), joints));
    GIANT_CHECK(clip_loops(s, Clip{}, joints)); // a pose

    // Three quarters of a stride: the feet end apart.
    Clip part = go("part", 1.5f, 1.0f);
    part.channels.resize(1);
    swing_feet(part, 0.3f, 0.75f);
    GIANT_CHECK(!clip_loops(s, part, joints));
    GIANT_CHECK(!clip_loops(s, part, MotionJoints{}));
}

GIANT_TEST("motion: each clip's frames, and which of them may be jumped into") {
    Set set;
    GIANT_CHECK(set.problem.empty());
    const MotionDatabase& db = set.db;
    GIANT_CHECK(db.ranges.size() == 5);
    // idle 2 s, walk 1 s, run 0.5 s, turn 2 s as loops; stop 1.5 s and its last frame.
    const u32 counts[] = {60, 30, 15, 60, 46};
    u32 first = 0;
    for (usize i = 0; i < 5; ++i) {
        GIANT_CHECK(db.ranges[i].clip == i);
        GIANT_CHECK(db.ranges[i].first == first);
        GIANT_CHECK(db.ranges[i].count == counts[i]);
        GIANT_CHECK(db.ranges[i].startable == (i < 4 ? counts[i] : 16)); // a second before the stop's end
        first += counts[i];
    }
    GIANT_CHECK(db.frames() == first);
    GIANT_CHECK(db.features.size() == usize{first} * k_motion_features);

    GIANT_CHECK(&db.range_of(0) == &db.ranges[0]);
    GIANT_CHECK(&db.range_of(59) == &db.ranges[0]);
    GIANT_CHECK(&db.range_of(60) == &db.ranges[1]);
    GIANT_CHECK(&db.range_of(first - 1) == &db.ranges[4]);
    GIANT_CHECK(near(db.time_of(75), 0.5f, 1e-5f));
    GIANT_CHECK(db.next(60) == 61);
    GIANT_CHECK(db.next(89) == 60);                // the walk goes round
    GIANT_CHECK(db.next(first - 2) == first - 1);
    GIANT_CHECK(db.next(first - 1) == first - 1);  // the stop holds its end
}

GIANT_TEST("motion: a walk's frames say where the feet are, how fast it goes and where it will be") {
    Set set;
    const MotionDatabase& db = set.db;
    const MotionRange& walk = db.ranges[1];
    for (const u32 f : {walk.first, walk.first + 7, walk.first + walk.count - 1}) {
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 0), 0.0f, 1e-3f));
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 1), 0.0f, 1e-3f));
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 2), 1.5f, 1e-3f));
        for (usize h = 0; h < k_motion_horizons; ++h) {
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 2 * h), 0.0f, 1e-3f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 2 * h + 1), 1.5f * db.settings.horizons[h], 2e-3f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_facings + 2 * h), 0.0f, 1e-4f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_facings + 2 * h + 1), 1.0f, 1e-4f));
        }
        // The feet a tenth of a metre either side, on the ground.
        GIANT_CHECK(near(db.raw(f, k_motion_foot_places + 0), 0.1f, 1e-4f));
        GIANT_CHECK(near(db.raw(f, k_motion_foot_places + 1), 0.0f, 1e-4f));
        GIANT_CHECK(near(db.raw(f, k_motion_foot_places + 3), -0.1f, 1e-4f));
    }
    // A quarter through, the left foot is furthest forward (0.3 m) and going as the hips do.
    const u32 quarter = walk.first + walk.count / 4;
    GIANT_CHECK(db.raw(quarter, k_motion_foot_places + 2) > 0.28f);
    GIANT_CHECK(db.raw(quarter, k_motion_foot_places + 5) < -0.28f);
    GIANT_CHECK(near(db.raw(quarter, k_motion_foot_velocities + 2), 1.5f, 0.25f));
    // At its start the left foot swings forward fastest: the hips' speed and 0.3 * tau more.
    GIANT_CHECK(near(db.raw(walk.first, k_motion_foot_velocities + 2), 1.5f + 0.3f * k_tau, 0.1f));
    GIANT_CHECK(near(db.raw(walk.first, k_motion_foot_velocities + 5), 1.5f - 0.3f * k_tau, 0.1f));

    // The root runs on along the clip.
    GIANT_CHECK(near(db.roots[walk.first + 15].z, 0.75f, 1e-3f));
    GIANT_CHECK(near(db.roots[walk.first + 15].x, 0.0f, 1e-4f));
    GIANT_CHECK(near(db.roots[walk.first + 15].yaw, 0.0f, 1e-4f));
}

GIANT_TEST("motion: a one-off clip's velocities and way ahead reach its end and hold there") {
    Set set;
    const MotionDatabase& db = set.db;
    const MotionRange& stop_range = db.ranges[4];
    // It starts at 1.5 m/s and slows evenly to a stand at 1.5 s.
    const u32 at_half = stop_range.first + 15; // 0.5 s in: 1 m/s
    GIANT_CHECK(near(db.raw(at_half, k_motion_hips_velocity + 2), 1.0f, 0.05f));
    // A second on it stands 1.125 m along; half a second in it is 0.625 m (the root is smoothed: nearly).
    GIANT_CHECK(near(db.raw(at_half, k_motion_ahead_places + 5), 0.5f, 0.05f));
    const u32 last = stop_range.first + stop_range.count - 1;
    GIANT_CHECK(near(db.raw(last, k_motion_hips_velocity + 2), 0.0f, 0.06f));
    for (usize h = 0; h < k_motion_horizons; ++h) {
        GIANT_CHECK(near(db.raw(last, k_motion_ahead_places + 2 * h + 1), 0.0f, 1e-4f));
    }
}

GIANT_TEST("motion: a loop that turns goes round turned: every frame sees the same curve ahead") {
    Set set;
    const MotionDatabase& db = set.db;
    const MotionRange& turning = db.ranges[3];
    const float radius = k_turn_speed / k_turn_rate;
    for (u32 f = turning.first; f < turning.first + turning.count; ++f) {
        for (usize h = 0; h < k_motion_horizons; ++h) {
            const float a = k_turn_rate * db.settings.horizons[h];
            // (The root is the hips averaged along the curve: a little inside it.)
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 2 * h), radius * (1.0f - std::cos(a)), 0.03f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 2 * h + 1), radius * std::sin(a), 0.03f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_facings + 2 * h), std::sin(a), 2e-3f));
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_facings + 2 * h + 1), std::cos(a), 2e-3f));
            // And just what the first frame sees.
            GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 2 * h),
                             db.raw(turning.first, k_motion_ahead_places + 2 * h), 5e-3f));
        }
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 2), k_turn_speed, 0.02f));
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 0), 0.0f, 0.03f));
    }
    // The root's yaw runs on through the turn.
    GIANT_CHECK(near(db.roots[turning.first + 30].yaw, k_turn_rate * 1.0f, 2e-3f));
}

GIANT_TEST("motion: the model's forward at rest can be any level way") {
    MotionSettings settings;
    settings.forward = {2.0f, 0.5f, 0.0f}; // +x (not unit, not level: made so)
    const Skeleton s = biped();
    const std::vector<Clip> clips{go("walk", 1.5f, 1.0f, {1.0f, 0.0f, 0.0f})};
    const MotionClip chosen[] = {{0, true}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), settings, db).empty());
    GIANT_CHECK(near(db.raw(3, k_motion_ahead_places + 5), 1.5f, 2e-3f)); // along its facing
    GIANT_CHECK(near(db.raw(3, k_motion_ahead_places + 4), 0.0f, 2e-3f));
    GIANT_CHECK(near(db.raw(3, k_motion_hips_velocity + 2), 1.5f, 1e-3f));
    GIANT_CHECK(near(db.roots[0].yaw, 0.25f * k_tau, 1e-4f));
}

GIANT_TEST("motion: the search finds the clip that goes as wanted") {
    Set set;
    const MotionDatabase& db = set.db;
    const auto clip_for = [&](const MotionAhead& ahead) {
        const MotionMatch m = motion_search(db, motion_query(db, k_no_frame, ahead), {}, false);
        GIANT_CHECK(m.frame != k_no_frame);
        return db.range_of(m.frame).clip;
    };
    GIANT_CHECK(clip_for(straight(0.0f)) == 0);
    GIANT_CHECK(clip_for(straight(1.5f)) == 1);
    GIANT_CHECK(clip_for(straight(4.0f)) == 2);
    GIANT_CHECK(clip_for(straight(3.2f)) == 2); // nearer a run than a walk

    MotionAhead curve;
    const float radius = k_turn_speed / k_turn_rate;
    for (usize h = 0; h < k_motion_horizons; ++h) {
        const float a = k_turn_rate * db.settings.horizons[h];
        curve.place[h] = {radius * (1.0f - std::cos(a)), radius * std::sin(a)};
        curve.facing[h] = {std::sin(a), std::cos(a)};
    }
    GIANT_CHECK(clip_for(curve) == 3);

    // Walking and wanting to slow to a stand: the stop, which starts posed as the walk is.
    const u32 walking = db.ranges[1].first + 2;
    MotionAhead slowing;
    slowing.place = {{{0.0f, 0.4f}, {0.0f, 0.7f}, {0.0f, 0.9f}}};
    const MotionMatch m = motion_search(db, motion_query(db, walking, slowing));
    GIANT_CHECK(db.range_of(m.frame).clip == 4);
    GIANT_CHECK(m.frame < db.ranges[4].first + 8);
    // Still wanting to walk on, it stays in the walk.
    const MotionMatch on = motion_search(db, motion_query(db, walking, straight(1.5f)));
    GIANT_CHECK(on.frame == walking);
    GIANT_CHECK(on.cost < 1e-4f);
}

GIANT_TEST("motion: the search is the nearest startable frame, as checking every one finds") {
    Set set;
    const MotionDatabase& db = set.db;
    u32 seed = 12345;
    const auto random = [&](float least, float most) {
        seed = seed * 1664525u + 1013904223u;
        return least + (most - least) * (static_cast<float>(seed >> 8) / 16777216.0f);
    };
    for (int i = 0; i < 2000; ++i) {
        MotionAhead ahead;
        const float speed = random(-1.0f, 5.0f);
        const float bend = random(-1.5f, 1.5f);
        for (usize h = 0; h < k_motion_horizons; ++h) {
            const float t = db.settings.horizons[h];
            ahead.place[h] = {bend * t * t, speed * t};
            ahead.facing[h] = {std::sin(bend * t), std::cos(bend * t)};
        }
        const u32 now = i % 3 == 0 ? k_no_frame : static_cast<u32>(random(0.0f, static_cast<float>(db.frames() - 1)));
        const MotionFeatures query = motion_query(db, now, ahead);
        const bool pose = now != k_no_frame;
        const MotionMatch fast = motion_search(db, query, {}, pose);
        const MotionMatch slow = slow_search(db, query, pose);
        GIANT_CHECK(fast.frame == slow.frame);
        GIANT_CHECK(near(fast.cost, slow.cost, 1e-4f * (1.0f + slow.cost)));
        const MotionRange& r = db.range_of(fast.frame);
        GIANT_CHECK(fast.frame < r.first + r.startable);

        // A frame handed in is kept unless another is nearer.
        const MotionMatch kept = motion_search(db, query, {7, slow.cost}, pose);
        GIANT_CHECK(kept.frame == 7);
        const MotionMatch beaten = motion_search(db, query, {7, slow.cost * 1.01f + 1e-3f}, pose);
        GIANT_CHECK(beaten.frame == slow.frame);
    }
    // The stop's last second is never jumped into, though its frames stand stillest.
    const MotionRange& stop_range = db.ranges[4];
    const u32 last = stop_range.first + stop_range.count - 1;
    MotionFeatures still{};
    const auto frame = db.frame(last);
    std::copy(frame.begin(), frame.end(), still.begin());
    const MotionMatch m = motion_search(db, still);
    GIANT_CHECK(m.frame != last);
    GIANT_CHECK(m.cost > 0.0f);
    GIANT_CHECK(motion_cost(db, last, still) == 0.0f);

    GIANT_CHECK(motion_search(MotionDatabase{}, still).frame == k_no_frame);
}

GIANT_TEST("motion: weights change what counts") {
    // With only the facing counted, a walk and a run are as good as each other for going straight.
    MotionSettings settings;
    settings.foot_place_weight = 0.0f;
    settings.foot_velocity_weight = 0.0f;
    settings.hips_velocity_weight = 0.0f;
    settings.ahead_place_weight = 0.0f;
    Set set(settings);
    GIANT_CHECK(set.problem.empty());
    const MotionFeatures query = motion_query(set.db, k_no_frame, straight(4.0f));
    GIANT_CHECK(near(motion_cost(set.db, set.db.ranges[1].first, query),
                     motion_cost(set.db, set.db.ranges[2].first, query), 1e-5f));
    // A number that no longer counts reads as the mean.
    GIANT_CHECK(set.db.raw(0, k_motion_hips_velocity + 2) == set.db.offset[k_motion_hips_velocity + 2]);
}

GIANT_TEST("motion: what cannot be built says why and leaves nothing") {
    const Skeleton s = biped();
    const MotionJoints joints = find_motion_joints(s);
    const std::vector<Clip> clips{go("walk", 1.5f, 1.0f), stop(1.5f, 0.5f)};
    const MotionClip walk[] = {{0, true}};
    MotionDatabase db;
    const auto fails = [&](std::span<const MotionClip> chosen, const MotionJoints& j, const MotionSettings& settings) {
        const std::string problem = build_motion_database(s, clips, chosen, j, settings, db);
        return !problem.empty() && db.frames() == 0 && db.ranges.empty() && db.features.empty();
    };
    GIANT_CHECK(build_motion_database(s, clips, walk, joints, {}, db).empty());
    GIANT_CHECK(db.frames() == 30);

    GIANT_CHECK(fails(walk, MotionJoints{}, {}));
    GIANT_CHECK(fails(walk, MotionJoints{0, 1, 3}, {}));
    GIANT_CHECK(fails({}, joints, {}));
    const MotionClip missing[] = {{2, true}};
    GIANT_CHECK(fails(missing, joints, {}));
    const MotionClip short_one[] = {{1, false}}; // half a second: nothing a second from its end
    GIANT_CHECK(fails(short_one, joints, {}));

    MotionSettings settings;
    settings.frame_rate = 5.0f;
    GIANT_CHECK(fails(walk, joints, settings));
    settings = {};
    settings.frame_rate = std::nanf("");
    GIANT_CHECK(fails(walk, joints, settings));
    settings = {};
    settings.horizons = {0.5f, 0.5f, 1.0f};
    GIANT_CHECK(fails(walk, joints, settings));
    settings.horizons = {0.5f, 1.0f, 3.0f};
    GIANT_CHECK(fails(walk, joints, settings));
    settings = {};
    settings.smoothing_seconds = -1.0f;
    GIANT_CHECK(fails(walk, joints, settings));
    settings = {};
    settings.forward = {0.0f, 1.0f, 0.0f};
    GIANT_CHECK(fails(walk, joints, settings));
    settings = {};
    settings.ahead_place_weight = -1.0f;
    GIANT_CHECK(fails(walk, joints, settings));

    // Too many frames: an hour-long clip nine times and more.
    std::vector<Clip> hour{Clip{}};
    hour[0].duration = k_max_clip_seconds;
    const std::vector<MotionClip> many(10, MotionClip{0, false});
    GIANT_CHECK(!build_motion_database(s, hour, many, joints, {}, db).empty());
    GIANT_CHECK(db.frames() == 0);

    // A short one-off clip beside a loop is kept, with no frame of it to jump into.
    const MotionClip both[] = {{0, true}, {1, false}};
    GIANT_CHECK(build_motion_database(s, clips, both, joints, {}, db).empty());
    GIANT_CHECK(db.ranges[1].startable == 0 && db.ranges[1].count == 16);
}

GIANT_TEST("motion: a pose of no length is a loop of one frame, standing") {
    const Skeleton s = biped();
    const std::vector<Clip> clips{Clip{}, go("walk", 1.5f, 1.0f)};
    const MotionClip chosen[] = {{0, true}, {1, true}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
    GIANT_CHECK(db.ranges[0].count == 1 && db.ranges[0].startable == 1);
    GIANT_CHECK(db.next(0) == 0);
    GIANT_CHECK(db.time_of(0) == 0.0f);
    GIANT_CHECK(near(db.raw(0, k_motion_hips_velocity + 2), 0.0f, 1e-5f));
    GIANT_CHECK(near(db.raw(0, k_motion_ahead_places + 5), 0.0f, 1e-5f));
    const MotionMatch m = motion_search(db, motion_query(db, k_no_frame, straight(0.0f)), {}, false);
    GIANT_CHECK(m.frame == 0);
}

GIANT_TEST("motion: a loop shorter than half a frame is a pose, built at once") {
    const Skeleton s = biped();
    for (const float seconds : {1e-4f, 1e-12f, 1e-40f}) {
        Clip tiny = go("tiny", 1.5f, 1.0f);
        tiny.duration = seconds;
        for (Channel& c : tiny.channels) {
            c.times = {0.0f, seconds};
            c.values.resize(2);
        }
        const std::vector<Clip> clips{tiny, go("walk", 1.5f, 1.0f)};
        const MotionClip chosen[] = {{0, true}, {1, true}};
        MotionDatabase db;
        GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
        GIANT_CHECK(db.ranges[0].count == 1);
        GIANT_CHECK(near(db.ranges[0].frame_seconds, 1.0f / 30.0f, 1e-6f));
        GIANT_CHECK(db.raw(0, k_motion_ahead_places + 5) == 0.0f); // it goes nowhere, whatever its end says
        GIANT_CHECK(db.raw(0, k_motion_hips_velocity + 2) == 0.0f);
    }
}

GIANT_TEST("motion: a loop's frames divide its length, whatever the rate") {
    const Skeleton s = biped();
    const std::vector<Clip> clips{go("walk", 1.5f, 1.01f)};
    const MotionClip chosen[] = {{0, true}};
    MotionSettings settings;
    settings.frame_rate = 60.0f;
    settings.smoothing_seconds = 0.0f;
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), settings, db).empty());
    GIANT_CHECK(db.ranges[0].count == 61);
    GIANT_CHECK(near(db.ranges[0].frame_seconds, 1.01f / 61.0f, 1e-7f));
    GIANT_CHECK(near(db.time_of(60), 1.01f * 60.0f / 61.0f, 1e-5f));
    for (const u32 f : {0u, 30u, 60u}) {
        GIANT_CHECK(near(db.raw(f, k_motion_hips_velocity + 2), 1.5f, 2e-3f));
        GIANT_CHECK(near(db.raw(f, k_motion_ahead_places + 5), 1.5f, 0.02f)); // 60 frames on: 0.993 s
    }
    GIANT_CHECK(near(db.roots[30].z, 1.5f * 1.01f * 30.0f / 61.0f, 1e-4f)); // not smoothed: the hips themselves
}

GIANT_TEST("motion: a skeleton or clip that does not validate is refused, not read") {
    const Skeleton s = biped();
    const MotionJoints joints = find_motion_joints(s);
    const MotionClip chosen[] = {{0, true}};
    MotionDatabase db;

    Skeleton broken = s;
    broken.parents = {-1, 0, 100000};
    const std::vector<Clip> walk{go("walk", 1.5f, 1.0f)};
    GIANT_CHECK(!build_motion_database(broken, walk, chosen, joints, {}, db).empty());
    GIANT_CHECK(!clip_loops(broken, walk[0], joints));
    Skeleton no_rest = s;
    no_rest.rest.clear();
    GIANT_CHECK(!build_motion_database(no_rest, walk, chosen, joints, {}, db).empty());

    std::vector<Clip> short_values{go("walk", 1.5f, 1.0f)};
    short_values[0].channels[0].times = {0.0f, 0.5f, 1.0f};
    short_values[0].channels[0].values.resize(1);
    GIANT_CHECK(!build_motion_database(s, short_values, chosen, joints, {}, db).empty());
    GIANT_CHECK(!clip_loops(s, short_values[0], joints));

    std::vector<Clip> no_number{go("walk", 1.5f, 1.0f)};
    no_number[0].channels[0].values[1].z = std::numeric_limits<float>::quiet_NaN();
    GIANT_CHECK(!build_motion_database(s, no_number, chosen, joints, {}, db).empty());
    GIANT_CHECK(db.frames() == 0);

    std::vector<Clip> endless{go("walk", 1.5f, 1.0f)};
    endless[0].duration = std::numeric_limits<float>::infinity();
    GIANT_CHECK(!build_motion_database(s, endless, chosen, joints, {}, db).empty());

    // Feet a world away: the numbers would not stay floats.
    std::vector<Clip> flung{go("walk", 1.5f, 2.0f)};
    for (Vec4& v : flung[0].channels[1].values) v.y = 3.3e38f;
    flung[0].channels[1].values[0].y = -3.3e38f;
    const MotionClip once[] = {{0, false}};
    GIANT_CHECK(!build_motion_database(s, flung, once, joints, {}, db).empty());
    std::vector<Clip> leaping{go("leap", 1e12f, 1.0f / 30.0f)}; // a one-frame loop going 2^60 m a round or so
    GIANT_CHECK(!build_motion_database(s, leaping, chosen, joints, {}, db).empty());
    GIANT_CHECK(db.frames() == 0);

    const std::vector<MotionClip> crowd(k_max_motion_clips + 1, MotionClip{0, true});
    GIANT_CHECK(!build_motion_database(s, walk, crowd, joints, {}, db).empty());

    MotionSettings far;
    far.forward = {1e20f, 0.0f, 0.0f};
    GIANT_CHECK(!build_motion_database(s, walk, chosen, joints, far, db).empty());
    far.forward = {0.0f, std::numeric_limits<float>::infinity(), 1.0f};
    GIANT_CHECK(!build_motion_database(s, walk, chosen, joints, far, db).empty());
}

GIANT_TEST("motion: a way ahead that is no number, or beyond reach, still finds a frame") {
    Set set;
    const MotionDatabase& db = set.db;
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1e20f,
                            -1e30f}) {
        MotionAhead ahead = straight(1.5f);
        ahead.place[1][1] = bad;
        ahead.facing[2][0] = bad;
        const MotionFeatures query = motion_query(db, 70, ahead);
        GIANT_CHECK(std::all_of(query.begin(), query.end(), [](float v) { return std::isfinite(v); }));
        const MotionMatch m = motion_search(db, query);
        GIANT_CHECK(m.frame < db.frames());
        GIANT_CHECK(std::isfinite(m.cost));
        GIANT_CHECK(motion_search(db, query, {}, false).frame < db.frames());
    }
    // The pose handed on is the frame's own.
    const MotionFeatures query = motion_query(db, 70, straight(1.5f));
    GIANT_CHECK(std::equal(query.begin(), query.begin() + k_motion_ahead_places, db.frame(70).begin()));
}

GIANT_TEST("motion: runs of frames with none to jump into are skipped, far from any query") {
    // A long one-off clip's last second fills whole runs of 16.
    const Skeleton s = biped();
    const std::vector<Clip> clips{go("walk", 1.5f, 4.0f)};
    const MotionClip chosen[] = {{0, false}};
    MotionDatabase db;
    GIANT_CHECK(build_motion_database(s, clips, chosen, find_motion_joints(s), {}, db).empty());
    GIANT_CHECK(db.ranges[0].count == 121 && db.ranges[0].startable == 91);
    const usize last = (db.frames() - 1) / 16; // frames 112 on: none startable
    for (usize i = 0; i < k_motion_features; ++i) {
        GIANT_CHECK(db.small_least[last * k_motion_features + i] == 1e18f);
        GIANT_CHECK(db.small_most[last * k_motion_features + i] == 1e18f);
        GIANT_CHECK(db.small_least[i] <= db.small_most[i]);
    }
    const MotionMatch m = motion_search(db, motion_query(db, 120, straight(0.0f)));
    GIANT_CHECK(m.frame < 91);
}
