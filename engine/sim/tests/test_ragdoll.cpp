// Ragdolls: bones held together at joints with limits, limp or driven
// toward a pose by their joints' motors.
#include "giant/sim/physics.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using namespace giant;
using namespace giant::sim;

namespace {

constexpr float k_degrees = 57.29578f;

BodyDesc ground_box() {
    BodyDesc d;
    d.parts.push_back({BoxShape{{50.0f, 0.5f, 50.0f}}, {0.0f, -0.5f, 0.0f}, {}});
    return d;
}

// A heavy block standing on the ground (1 m cube, its origin at its middle)
// with an arm out of its side: a capsule 0.6 m long along the arm's own +y,
// which points along the world's +x from the joint at the block's side.
// The whole of it turned `yaw` radians about the world's up (the arm's turn
// in the block's space stays k_level).
const Quat k_level = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -1.5707963f); // the arm's +y to the block's +x
RagdollDesc block_and_arm(float swing_degrees, float twist_degrees = 20.0f, float yaw = 0.0f) {
    const Quat turned = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw);
    RagdollDesc d;
    d.joint_friction = 0.0f;
    RagdollBone block;
    block.part = {BoxShape{{0.5f, 0.5f, 0.5f}}, {}, {}};
    block.position = {0.0, 0.5, 0.0};
    block.rotation = turned;
    block.mass = 500.0f;
    RagdollBone arm;
    arm.parent = 0;
    arm.part = {CapsuleShape{0.25f, 0.05f}, {0.0f, 0.35f, 0.0f}, {}};
    const Vec3 joint = rotate(turned, Vec3{0.55f, 0.4f, 0.0f});
    arm.position = block.position + DVec3{joint.x, joint.y, joint.z};
    arm.rotation = normalize(turned * k_level);
    arm.mass = 2.0f;
    arm.swing_degrees = swing_degrees;
    arm.twist_degrees = twist_degrees;
    d.bones = {block, arm};
    return d;
}

// Degrees the arm is twisted about its own length from how the block carries it level.
float twist(const PhysicsWorld& physics, RagdollId id) {
    const BodyState block = *physics.state(physics.ragdoll_bodies(id)[0]);
    const BodyState arm = *physics.state(physics.ragdoll_bodies(id)[1]);
    const Quat q = normalize(conjugate(normalize(block.rotation * k_level)) * arm.rotation); // in the arm's level space
    return 2.0f * std::atan2(q.y, q.w) * k_degrees; // about its own +y
}

// Degrees the arm's own +y leans below level (0 level, 90 straight down).
float droop(const PhysicsWorld& physics, RagdollId id) {
    const BodyState arm = *physics.state(physics.ragdoll_bodies(id)[1]);
    const Vec3 along = rotate(arm.rotation, Vec3{0.0f, 1.0f, 0.0f});
    return std::asin(std::clamp(-along.y, -1.0f, 1.0f)) * k_degrees;
}

void run(PhysicsWorld& physics, double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 60.0 + 0.5); ++i) physics.step(1.0 / 60.0);
}

// How far the arm's joint has come apart (metres): its origin against
// where the block carries the joint.
double gap(const PhysicsWorld& physics, RagdollId id) {
    const BodyState block = *physics.state(physics.ragdoll_bodies(id)[0]);
    const BodyState arm = *physics.state(physics.ragdoll_bodies(id)[1]);
    const Vec3 carried = rotate(block.rotation, Vec3{0.55f, 0.4f, 0.0f});
    return length(arm.position - (block.position + DVec3{carried.x, carried.y, carried.z}));
}

// A chain of `count` capsules hanging down from `top`, each 0.4 m long.
RagdollDesc chain(DVec3 top, usize count, u64 user = 0) {
    RagdollDesc d;
    d.user = user;
    for (usize i = 0; i < count; ++i) {
        RagdollBone b;
        b.parent = static_cast<i32>(i) - 1;
        b.part = {CapsuleShape{0.12f, 0.06f}, {0.0f, -0.2f, 0.0f}, {}};
        b.position = top + DVec3{0.0, -0.4 * static_cast<double>(i), 0.0};
        b.mass = 3.0f;
        b.twist_axis = {0.0f, -1.0f, 0.0f};
        b.swing_degrees = 60.0f;
        d.bones.push_back(b);
    }
    return d;
}

} // namespace

GIANT_TEST("ragdoll: a limp arm droops to its joint's limit and no further, and stays joined") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    const RagdollId id = physics.add_ragdoll(block_and_arm(30.0f));
    GIANT_CHECK(id != k_no_ragdoll && physics.contains_ragdoll(id) && physics.ragdolls() == 1);
    GIANT_CHECK(physics.ragdoll_bodies(id).size() == 2 && physics.size() == 3);
    GIANT_CHECK(std::fabs(droop(physics, id)) < 0.5f && !physics.ragdoll_driven(id));
    run(physics, 3.0);
    GIANT_CHECK(droop(physics, id) > 27.0f && droop(physics, id) < 33.0f);
    GIANT_CHECK(gap(physics, id) < 0.01);
    // A wider joint lets it hang further; the block has not moved.
    const RagdollId wide = physics.add_ragdoll([] {
        RagdollDesc d = block_and_arm(80.0f);
        for (RagdollBone& b : d.bones) b.position.z += 5.0;
        return d;
    }());
    run(physics, 3.0);
    GIANT_CHECK(droop(physics, wide) > 75.0f && droop(physics, wide) < 84.0f);
    const BodyState block = *physics.state(physics.ragdoll_bodies(wide)[0]);
    GIANT_CHECK(std::fabs(block.position.y - 0.5) < 0.02 && std::fabs(block.position.x) < 0.05);
}

GIANT_TEST("ragdoll: a joint's limits are about its centre, not about where the bone was placed") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    // Placed already drooping 20 degrees, its range (30) centred on level.
    // (The block turned about the world's up: the centre is in its space, not the world's.)
    RagdollDesc d = block_and_arm(30.0f, 20.0f, 1.2f);
    d.bones[1].rotation = normalize(d.bones[0].rotation * quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -1.5707963f - 0.34906585f));
    d.bones[1].centre = k_level;
    const RagdollId id = physics.add_ragdoll(d);
    GIANT_CHECK(id != k_no_ragdoll && droop(physics, id) > 19.0f && droop(physics, id) < 21.0f);
    run(physics, 3.0);
    GIANT_CHECK(droop(physics, id) > 27.0f && droop(physics, id) < 33.0f); // not 50
    // Straight down from level, as the block carries it: not pulled aside toward a centre anywhere else.
    const BodyState block = *physics.state(physics.ragdoll_bodies(id)[0]);
    const BodyState arm = *physics.state(physics.ragdoll_bodies(id)[1]);
    const Vec3 along = rotate(conjugate(block.rotation), rotate(arm.rotation, Vec3{0.0f, 1.0f, 0.0f}));
    GIANT_CHECK(along.x > 0.8f && std::fabs(along.z) < 0.03f);
}

GIANT_TEST("ragdoll: driven, the arm holds and reaches the turns asked for; relaxed, it falls again") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    const RagdollDesc d = block_and_arm(80.0f, 20.0f, -2.0f); // (turned: the turns asked for are in the block's space)
    const RagdollId id = physics.add_ragdoll(d);
    const std::vector<Quat> hold{Quat{}, k_level};
    // (A motor is a spring: under the arm's weight it sags a little, less the stiffer it is.)
    GIANT_CHECK(physics.drive_ragdoll(id, hold) && physics.ragdoll_driven(id));
    run(physics, 2.0);
    const float sag = droop(physics, id);
    GIANT_CHECK(sag > 0.5f && sag < 8.0f);
    RagdollDrive stiff;
    stiff.frequency = 20.0f;
    GIANT_CHECK(physics.drive_ragdoll(id, hold, stiff));
    run(physics, 2.0);
    GIANT_CHECK(droop(physics, id) > 0.0f && droop(physics, id) < sag * 0.25f);
    // Raised 40 degrees.
    const std::vector<Quat> raised{Quat{}, quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -1.5707963f + 0.6981317f)};
    GIANT_CHECK(physics.drive_ragdoll(id, raised, stiff));
    run(physics, 2.0);
    GIANT_CHECK(droop(physics, id) < -38.0f && droop(physics, id) > -40.5f);
    // A motor allowed too little cannot hold it up.
    RagdollDrive weak;
    weak.max_torque = 0.5f; // the arm's weight is about 7 N m about the joint
    GIANT_CHECK(physics.drive_ragdoll(id, hold, weak));
    run(physics, 3.0);
    GIANT_CHECK(droop(physics, id) > 60.0f);
    GIANT_CHECK(physics.drive_ragdoll(id, hold, stiff));
    run(physics, 2.0);
    GIANT_CHECK(std::fabs(droop(physics, id)) < 2.0f);
    // Held still it sleeps; a new target wakes it.
    run(physics, 5.0);
    GIANT_CHECK(!physics.state(physics.ragdoll_bodies(id)[1])->active);
    GIANT_CHECK(physics.drive_ragdoll(id, hold, stiff)); // the same again: left asleep
    GIANT_CHECK(!physics.state(physics.ragdoll_bodies(id)[1])->active);
    const std::vector<Quat> same{Quat{}, Quat{-k_level.x, -k_level.y, -k_level.z, -k_level.w}}; // the same turn
    GIANT_CHECK(physics.drive_ragdoll(id, same, stiff));
    GIANT_CHECK(!physics.state(physics.ragdoll_bodies(id)[1])->active);
    // A turn of another length is the turn it is: twice as long and raised, it is raised.
    const Quat up = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -1.5707963f + 0.6981317f);
    GIANT_CHECK(physics.drive_ragdoll(id, std::vector<Quat>{Quat{}, Quat{up.x * 2, up.y * 2, up.z * 2, up.w * 2}}, stiff));
    GIANT_CHECK(physics.state(physics.ragdoll_bodies(id)[1])->active);
    run(physics, 2.0);
    GIANT_CHECK(droop(physics, id) < -38.0f);
    GIANT_CHECK(physics.drive_ragdoll(id, hold, stiff));
    run(physics, 7.0);
    GIANT_CHECK(!physics.state(physics.ragdoll_bodies(id)[1])->active);
    GIANT_CHECK(physics.drive_ragdoll(id, raised, stiff));
    GIANT_CHECK(physics.state(physics.ragdoll_bodies(id)[1])->active);
    run(physics, 2.0);
    GIANT_CHECK(droop(physics, id) < -38.0f);
    run(physics, 5.0);
    GIANT_CHECK(!physics.state(physics.ragdoll_bodies(id)[1])->active);
    physics.relax_ragdoll(id);
    GIANT_CHECK(!physics.ragdoll_driven(id) && physics.state(physics.ragdoll_bodies(id)[1])->active);
    run(physics, 3.0);
    GIANT_CHECK(droop(physics, id) > 75.0f);

    // What cannot be driven changes nothing.
    GIANT_CHECK(!physics.drive_ragdoll(id + 100, hold));
    GIANT_CHECK(!physics.drive_ragdoll(id, std::vector<Quat>{Quat{}}));
    GIANT_CHECK(!physics.drive_ragdoll(id, std::vector<Quat>{Quat{}, Quat{0.0f, 0.0f, 0.0f, 0.0f}}));
    GIANT_CHECK(!physics.drive_ragdoll(id, std::vector<Quat>{Quat{}, Quat{std::nanf(""), 0.0f, 0.0f, 1.0f}}));
    RagdollDrive bad;
    bad.frequency = 0.0f;
    GIANT_CHECK(!physics.drive_ragdoll(id, hold, bad));
    bad = {};
    bad.max_torque = std::nanf("");
    GIANT_CHECK(!physics.drive_ragdoll(id, hold, bad));
    GIANT_CHECK(!physics.ragdoll_driven(id));
}

GIANT_TEST("ragdoll: a chain falls in a heap, stays joined and sleeps; its bones are bodies with its user") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    const BodyId ground = physics.add(ground_box());
    const RagdollDesc d = chain({0.0, 4.0, 0.0}, 6, 77);
    const RagdollId id = physics.add_ragdoll(d);
    GIANT_CHECK(id != k_no_ragdoll && physics.size() == 7);
    const std::span<const BodyId> bones = physics.ragdoll_bodies(id);
    GIANT_CHECK(bones.size() == 6);
    for (u32 i = 0; i < bones.size(); ++i) {
        const auto of = physics.ragdoll_bone(bones[i]);
        GIANT_CHECK(of && of->ragdoll == id && of->bone == i && physics.user(bones[i]) == 77);
    }
    GIANT_CHECK(!physics.ragdoll_bone(ground) && physics.ragdoll_bodies(id + 1).empty());
    // A ray names the bone it meets.
    const auto hit = physics.raycast({-5.0, 4.0 - 0.4 * 2 - 0.2, 0.0}, {1.0f, 0.0f, 0.0f}, 10.0);
    GIANT_CHECK(hit && hit->body == bones[2] && hit->user == 77);
    // A push on its lowest bone swings it (the rest follow: joined).
    physics.add_impulse(bones[5], {30.0f, 0.0f, 0.0f}, physics.state(bones[5])->position);
    run(physics, 0.2);
    GIANT_CHECK(physics.state(bones[5])->position.x > 0.2);
    run(physics, 8.0);
    for (usize i = 0; i < bones.size(); ++i) {
        const BodyState s = *physics.state(bones[i]);
        GIANT_CHECK(!s.active && s.position.y > 0.0 && s.position.y < 1.0);
        if (i == 0) continue;
        // Each joint together: the bone's origin where its parent's far end is.
        const BodyState p = *physics.state(bones[i - 1]);
        const Vec3 end = rotate(p.rotation, Vec3{0.0f, -0.4f, 0.0f});
        GIANT_CHECK(length(s.position - (p.position + DVec3{end.x, end.y, end.z})) < 0.02);
    }
    // Bones leave only with their ragdoll.
    physics.remove(bones[3]);
    GIANT_CHECK(physics.contains(bones[3]) && physics.size() == 7);
    const std::vector<BodyId> were(bones.begin(), bones.end());
    physics.remove_ragdoll(id);
    GIANT_CHECK(!physics.contains_ragdoll(id) && physics.ragdolls() == 0 && physics.size() == 1);
    for (const BodyId b : were) GIANT_CHECK(!physics.contains(b) && !physics.ragdoll_bone(b));
    physics.remove_ragdoll(id); // again: nothing
    run(physics, 0.5);
}

GIANT_TEST("ragdoll: a bone passes through the bone it hangs from, not through the others unless told") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs, [] {
        PhysicsSettings s;
        s.gravity = {};
        s.min_hit_impulse = 0.05f; // (every knock is told)
        return s;
    }());
    // Three fat bones in a row along x, overlapping their neighbours at the
    // joints; the first too heavy to be pushed about.
    RagdollDesc d;
    for (int i = 0; i < 3; ++i) {
        RagdollBone b;
        b.parent = i - 1;
        b.part = {SphereShape{0.3f}, {0.25f, 0.0f, 0.0f}, {}};
        b.position = {0.5 * i, 0.0, 0.0};
        b.mass = i == 0 ? 1000.0f : 1.0f;
        b.twist_axis = {1.0f, 0.0f, 0.0f};
        b.swing_degrees = 170.0f;
        d.bones.push_back(b);
    }
    RagdollDesc through = d;
    through.no_collide.push_back({2, 0});
    for (RagdollBone& b : through.bones) b.position.z += 10.0;
    const RagdollId id = physics.add_ragdoll(d);
    const RagdollId other = physics.add_ragdoll(through);
    GIANT_CHECK(id != k_no_ragdoll && other != k_no_ragdoll);
    run(physics, 1.0);
    // Overlapping neighbours did not push each other apart: all where they were, still.
    const std::span<const BodyId> bones = physics.ragdoll_bodies(id);
    for (usize i = 0; i < 3; ++i) {
        const BodyState s = *physics.state(bones[i]);
        GIANT_CHECK(length(s.position - d.bones[i].position) < 1e-3 && length(s.linear_velocity) < 1e-3f);
    }
    // The third folded back onto the first (165 degrees at its joint, the
    // second held straight): stopped by it, unless the pair is told not to collide.
    const std::vector<Quat> folded{Quat{}, Quat{}, quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 2.8797933f)};
    RagdollDrive stiff;
    stiff.frequency = 15.0f;
    GIANT_CHECK(physics.drive_ragdoll(id, folded, stiff) && physics.drive_ragdoll(other, folded, stiff));
    const auto apart = [&physics](RagdollId r) {
        const BodyState first = *physics.state(physics.ragdoll_bodies(r)[0]);
        const BodyState third = *physics.state(physics.ragdoll_bodies(r)[2]);
        const Vec3 c1 = rotate(first.rotation, Vec3{0.25f, 0.0f, 0.0f});
        const Vec3 c3 = rotate(third.rotation, Vec3{0.25f, 0.0f, 0.0f});
        return length((third.position + DVec3{c3.x, c3.y, c3.z}) - (first.position + DVec3{c1.x, c1.y, c1.z}));
    };
    // (Bones of one ragdoll knocking together are no hits to tell of.)
    usize own_hits = 0;
    usize other_hits = 0;
    const auto count_hits = [&] {
        for (const ContactHit& h : physics.hits()) {
            const auto a = physics.ragdoll_bone(h.a);
            const auto b = physics.ragdoll_bone(h.b);
            if (a && b && a->ragdoll == b->ragdoll) ++own_hits;
            else ++other_hits;
        }
    };
    for (int i = 0; i < 120; ++i) {
        physics.step(1.0 / 60.0);
        count_hits();
    }
    double least = 1e9;
    double most_through = 0.0;
    for (int i = 0; i < 60; ++i) {
        physics.step(1.0 / 60.0);
        count_hits();
        least = std::min(least, apart(id));
        most_through = std::max(most_through, apart(other));
    }
    GIANT_CHECK(least > 0.57);        // two 0.3 m balls touching
    GIANT_CHECK(most_through < 0.54); // 0.51 where the fold puts them
    // Another ragdoll's bones collide with this one's.
    RagdollDesc above = d;
    for (RagdollBone& b : above.bones) {
        b.position.y += 3.0;
        b.linear_velocity = {0.0f, -3.0f, 0.0f};
    }
    const RagdollId second = physics.add_ragdoll(above);
    GIANT_CHECK(own_hits == 0 && other_hits == 0);
    for (int i = 0; i < 120; ++i) {
        physics.step(1.0 / 60.0);
        count_hits();
    }
    GIANT_CHECK(own_hits == 0 && other_hits > 0); // one ragdoll's bones on another's
    GIANT_CHECK(physics.state(physics.ragdoll_bodies(second)[0])->position.y > physics.state(bones[0])->position.y + 0.3);
}

GIANT_TEST("ragdoll: a joint's twist is limited, locked at nothing, and driven like its lean") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs, [] {
        PhysicsSettings s;
        s.gravity = {};
        return s;
    }());
    const auto spun = [&physics](float twist_degrees, float swing_degrees, double z) {
        RagdollDesc d = block_and_arm(swing_degrees, twist_degrees, 0.7f);
        for (RagdollBone& b : d.bones) b.position.z += z;
        // Spinning about its own length (which the block carries along its own +x).
        const Vec3 along = rotate(d.bones[1].rotation, Vec3{0.0f, 1.0f, 0.0f});
        d.bones[1].angular_velocity = along * 8.0f;
        return physics.add_ragdoll(d);
    };
    const RagdollId narrow = spun(20.0f, 30.0f, 0.0);
    const RagdollId wide = spun(170.0f, 30.0f, 4.0);
    const RagdollId locked = spun(0.0f, 0.0f, 8.0);
    GIANT_CHECK(length(physics.state(physics.ragdoll_bodies(narrow)[1])->angular_velocity) > 7.9f); // as described
    float most_narrow = 0.0f, most_wide = 0.0f, most_locked = 0.0f;
    for (int i = 0; i < 120; ++i) {
        physics.step(1.0 / 60.0);
        most_narrow = std::max(most_narrow, std::fabs(twist(physics, narrow)));
        most_wide = std::max(most_wide, std::fabs(twist(physics, wide)));
        most_locked = std::max(most_locked, std::fabs(twist(physics, locked)) + std::fabs(droop(physics, locked)));
    }
    GIANT_CHECK(most_narrow > 17.0f && most_narrow < 24.0f);
    GIANT_CHECK(most_wide > 100.0f);
    GIANT_CHECK(most_locked < 2.0f);
    // Driven to a twist within its range, it goes there.
    RagdollDrive stiff;
    stiff.frequency = 15.0f;
    const std::vector<Quat> twisted{Quat{}, normalize(k_level * quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.2617994f))};
    GIANT_CHECK(physics.drive_ragdoll(narrow, twisted, stiff));
    run(physics, 2.0);
    GIANT_CHECK(std::fabs(twist(physics, narrow) - 15.0f) < 1.0f && std::fabs(droop(physics, narrow)) < 1.0f);
    // Asked for more than the joint allows: as near as it allows.
    const std::vector<Quat> too_far{Quat{}, normalize(k_level * quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.0471976f))};
    GIANT_CHECK(physics.drive_ragdoll(narrow, too_far, stiff));
    run(physics, 2.0);
    GIANT_CHECK(twist(physics, narrow) > 17.0f && twist(physics, narrow) < 23.0f);
}

GIANT_TEST("ragdoll: friction in a joint holds a limp bone; a heavier bone is pushed less; one bone alone is a ragdoll") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    RagdollDesc stiff_joint = block_and_arm(80.0f);
    stiff_joint.joint_friction = 50.0f; // the arm's weight turns it with about 7 N m
    const RagdollId held = physics.add_ragdoll(stiff_joint);
    run(physics, 2.0);
    GIANT_CHECK(std::fabs(droop(physics, held)) < 3.0f);

    PhysicsWorld space(jobs, [] {
        PhysicsSettings s;
        s.gravity = {};
        return s;
    }());
    const auto ball = [&space](float mass, double x) {
        RagdollDesc d;
        RagdollBone b;
        b.part = {SphereShape{0.2f}, {}, {}};
        b.position = {x, 0.0, 0.0};
        b.mass = mass;
        d.bones.push_back(b);
        d.user = 5;
        return space.add_ragdoll(d);
    };
    const RagdollId light = ball(1.0f, 0.0);
    const RagdollId heavy = ball(10.0f, 3.0);
    GIANT_CHECK(light != k_no_ragdoll && heavy != k_no_ragdoll && space.ragdoll_bodies(light).size() == 1);
    for (const RagdollId id : {light, heavy}) {
        const BodyId body = space.ragdoll_bodies(id)[0];
        space.add_impulse(body, {0.0f, 0.0f, 5.0f}, space.state(body)->position);
    }
    run(space, 0.1);
    GIANT_CHECK(std::fabs(space.state(space.ragdoll_bodies(light)[0])->linear_velocity.z - 5.0f) < 0.05f);
    GIANT_CHECK(std::fabs(space.state(space.ragdoll_bodies(heavy)[0])->linear_velocity.z - 0.5f) < 0.005f);
    GIANT_CHECK(space.drive_ragdoll(light, std::vector<Quat>{Quat{}}) && space.ragdoll_driven(light)); // nothing to drive: fine
}

GIANT_TEST("ragdoll: moved as one piece; a bone alone is not placed; a full world refuses one whole") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs, [] {
        PhysicsSettings s;
        s.max_bodies = 6;
        return s;
    }());
    physics.add(ground_box());
    GIANT_CHECK(physics.add_ragdoll(chain({0.0, 3.0, 0.0}, 6)) == k_no_ragdoll); // one too many
    GIANT_CHECK(physics.size() == 1 && physics.ragdolls() == 0);
    const RagdollId id = physics.add_ragdoll(chain({0.0, 3.0, 0.0}, 5)); // (what the refused one made was let go)
    GIANT_CHECK(id != k_no_ragdoll && physics.size() == 6);
    const std::span<const BodyId> bones = physics.ragdoll_bodies(id);
    // A bone alone stays where its joints have it.
    const BodyState was = *physics.state(bones[2]);
    physics.set_transform(bones[2], {9.0, 9.0, 9.0}, Quat{});
    physics.move_kinematic(bones[2], {9.0, 9.0, 9.0}, Quat{});
    GIANT_CHECK(length(physics.state(bones[2])->position - was.position) < 1e-6);
    // Falling, then moved: laid along +x from (20, 5, -7), its fall turned with it.
    run(physics, 0.1);
    const float falling = physics.state(bones[4])->linear_velocity.y;
    GIANT_CHECK(falling < -0.5f);
    const Quat lying = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f); // its down (-y) to +x
    GIANT_CHECK(physics.move_ragdoll(id, {20.0, 5.0, -7.0}, lying));
    for (usize i = 0; i < bones.size(); ++i) {
        const BodyState s = *physics.state(bones[i]);
        GIANT_CHECK(length(s.position - DVec3{20.0 + 0.4 * static_cast<double>(i), 5.0, -7.0}) < 0.02);
        GIANT_CHECK(std::fabs(s.linear_velocity.x + falling) < 0.3f && std::fabs(s.linear_velocity.y) < 0.3f);
    }
    GIANT_CHECK(!physics.move_ragdoll(id + 1, {}, Quat{}) && !physics.move_ragdoll(id, {std::nan(""), 0.0, 0.0}, Quat{}) &&
                !physics.move_ragdoll(id, {}, Quat{0.0f, 0.0f, 0.0f, 0.0f}));
    run(physics, 4.0);
    for (usize i = 1; i < bones.size(); ++i) {
        const BodyState s = *physics.state(bones[i]);
        const BodyState p = *physics.state(bones[i - 1]);
        const Vec3 end = rotate(p.rotation, Vec3{0.0f, -0.4f, 0.0f});
        GIANT_CHECK(length(s.position - (p.position + DVec3{end.x, end.y, end.z})) < 0.02 && s.position.y < 0.5 &&
                    s.position.x > 15.0);
    }
    // What slept on it falls when it is moved away.
    physics.remove_ragdoll(id);
    RagdollDesc slab;
    RagdollBone flat;
    flat.part = {BoxShape{{0.6f, 0.1f, 0.6f}}, {}, {}};
    flat.position = {0.0, 0.1, 0.0};
    flat.mass = 50.0f;
    slab.bones.push_back(flat);
    const RagdollId under = physics.add_ragdoll(slab);
    BodyDesc crate;
    crate.motion = Motion::dynamic;
    crate.position = {0.0, 0.45, 0.0};
    crate.parts.push_back({BoxShape{{0.25f, 0.25f, 0.25f}}, {}, {}});
    const BodyId on_top = physics.add(crate);
    run(physics, 6.0);
    GIANT_CHECK(!physics.state(on_top)->active && physics.state(on_top)->position.y > 0.4);
    GIANT_CHECK(physics.move_ragdoll(under, {30.0, 0.1, 0.0}, Quat{}));
    run(physics, 2.0);
    GIANT_CHECK(physics.state(on_top)->position.y < 0.3);
}

GIANT_TEST("ragdoll: descriptions that cannot be are refused whole; clear lets every ragdoll go") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    const auto refused = [&physics](const auto& change) {
        RagdollDesc d = chain({0.0, 3.0, 0.0}, 3);
        change(d);
        const RagdollId id = physics.add_ragdoll(d);
        GIANT_CHECK(id == k_no_ragdoll && physics.size() == 1 && physics.ragdolls() == 0);
    };
    refused([](RagdollDesc& d) { d.bones.clear(); });
    refused([](RagdollDesc& d) { d.bones.resize(k_max_ragdoll_bones + 1, d.bones.back()); });
    refused([](RagdollDesc& d) { d.bones[0].parent = 0; });
    refused([](RagdollDesc& d) { d.bones[1].parent = -1; }); // a second root
    refused([](RagdollDesc& d) { d.bones[1].parent = 1; });  // itself
    refused([](RagdollDesc& d) { d.bones[1].parent = 2; });  // a later bone
    refused([](RagdollDesc& d) { d.bones[2].mass = 0.0f; });
    refused([](RagdollDesc& d) { d.bones[2].mass = std::nanf(""); });
    refused([](RagdollDesc& d) { d.bones[2].position.x = std::nan(""); });
    refused([](RagdollDesc& d) { d.bones[2].rotation = {0.0f, 0.0f, 0.0f, 0.0f}; });
    refused([](RagdollDesc& d) { d.bones[2].twist_axis = {}; });
    refused([](RagdollDesc& d) { d.bones[2].twist_axis = {1.0e30f, 0.0f, 0.0f}; });
    refused([](RagdollDesc& d) { d.bones[2].mass = 1.0e-6f; });
    refused([](RagdollDesc& d) { d.no_collide.push_back({0, 3}); });
    refused([](RagdollDesc& d) { d.no_collide.push_back({1, 1}); });
    refused([](RagdollDesc& d) { d.bones[2].swing_degrees = 180.0f; });
    refused([](RagdollDesc& d) { d.bones[2].swing_degrees = -1.0f; });
    refused([](RagdollDesc& d) { d.bones[2].twist_degrees = std::nanf(""); });
    refused([](RagdollDesc& d) { d.bones[2].centre = Quat{0.0f, 0.0f, 0.0f, 0.0f}; });
    refused([](RagdollDesc& d) { d.bones[2].part.shape = CapsuleShape{0.1f, -1.0f}; }); // (after two that could be made)
    refused([](RagdollDesc& d) { d.bones[2].part.shape = HeightShape{}; });
    refused([](RagdollDesc& d) { d.bones[1].linear_velocity.y = std::nanf(""); });
    refused([](RagdollDesc& d) { d.friction = -1.0f; });
    refused([](RagdollDesc& d) { d.joint_friction = std::nanf(""); });
    // The most bones there may be.
    RagdollDesc most = chain({0.0, 60.0, 0.0}, k_max_ragdoll_bones);
    const RagdollId big = physics.add_ragdoll(most);
    GIANT_CHECK(big != k_no_ragdoll && physics.size() == 1 + k_max_ragdoll_bones);
    const RagdollId small = physics.add_ragdoll(chain({3.0, 3.0, 0.0}, 2));
    GIANT_CHECK(small != k_no_ragdoll && small != big && physics.ragdolls() == 2);
    run(physics, 0.5);
    GIANT_CHECK(physics.clears() == 0 && physics.drive_ragdoll(small, std::vector<Quat>(2)));
    physics.clear();
    GIANT_CHECK(physics.clears() == 1);
    GIANT_CHECK(physics.ragdolls() == 0 && !physics.contains_ragdoll(big) && physics.ragdoll_bodies(small).empty() &&
                physics.size() == 0);
    // And the world works on: ids are not handed out again.
    const RagdollId next = physics.add_ragdoll(chain({0.0, 3.0, 0.0}, 2));
    GIANT_CHECK(next != k_no_ragdoll && next != big && next != small);
    run(physics, 0.5);
}

GIANT_TEST("ragdoll: a crowd falls together without flying apart") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    std::vector<RagdollId> ids;
    for (int i = 0; i < 40; ++i) {
        ids.push_back(physics.add_ragdoll(chain({(i % 8) * 0.7 - 2.5, 3.0 + (i / 8) * 0.9, (i % 3) * 0.3}, 8)));
        GIANT_CHECK(ids.back() != k_no_ragdoll);
    }
    run(physics, 6.0);
    for (const RagdollId id : ids) {
        for (const BodyId b : physics.ragdoll_bodies(id)) {
            const BodyState s = *physics.state(b);
            GIANT_CHECK(std::isfinite(s.position.x) && s.position.y > -0.1 && s.position.y < 4.0 &&
                        std::fabs(s.position.x) < 12.0 && std::fabs(s.position.z) < 12.0 &&
                        length(s.linear_velocity) < 3.0f);
        }
    }
    for (const RagdollId id : ids) physics.remove_ragdoll(id);
    GIANT_CHECK(physics.size() == 1);
}
