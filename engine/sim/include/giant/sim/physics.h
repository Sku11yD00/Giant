// Rigid bodies and collision on Jolt Physics: the one physics world the
// game's moving things share (debris, falling trees, the player, a
// giantess's feet). Double precision, so a body keeps millimetres anywhere
// in a 30 km world. Jolt stays behind this header: nothing else includes it.
//
//   JobSystem jobs;
//   PhysicsWorld physics(jobs);
//   BodyDesc ground;  ground.parts.push_back({BoxShape{{50, 0.5f, 50}}});
//   physics.add(ground);
//   BodyDesc crate;   crate.motion = Motion::dynamic;  crate.position = {0, 5, 0};
//   crate.parts.push_back({BoxShape{{0.5f, 0.5f, 0.5f}}});
//   const BodyId id = physics.add(crate);
//   physics.step(1.0);                     // a second: 60 fixed steps
//   const BodyState s = *physics.state(id); // resting on the ground
//
// Time moves in fixed steps of 1/60 s (the same result at any frame rate),
// each spread over the job system's workers; step() runs as many as the
// time given holds, at most `max_steps_per_call`, and keeps the remainder.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/math.h"
#include "giant/core/types.h"

#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace giant::sim {

// Fixed: never moves (the ground, buildings standing). Kinematic: moved by
// the game (move_kinematic), pushes dynamic bodies, is not pushed (a
// giantess's foot). Dynamic: moved by forces and contacts (debris).
enum class Motion : u8 { fixed, kinematic, dynamic };

// Shapes, in the body's frame (metres).
struct BoxShape {
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
};
struct SphereShape {
    float radius = 0.5f;
};
// Round-ended, along y: `half_height` is the straight part's half length.
struct CapsuleShape {
    float half_height = 0.5f;
    float radius = 0.5f;
};
// The smallest convex shape around the points (an ellipsoid, a rock).
struct ConvexShape {
    std::vector<Vec3> points;
};
// A square grid of heights (y) over x and z: sample (i, j) is at
// (i, j) * spacing from the body's origin, at heights[j * samples + i].
// A NaN height leaves a hole. Fixed bodies only.
struct HeightShape {
    u32 samples = 0;
    float spacing = 1.0f;
    std::vector<float> heights;
};
using Shape = std::variant<BoxShape, SphereShape, CapsuleShape, ConvexShape, HeightShape>;

// One shape of a body, placed in the body's frame. A body of several
// parts is one rigid compound (a house's walls and roof).
struct Part {
    Shape shape;
    Vec3 offset{};
    Quat rotation{};
};

struct BodyDesc {
    std::vector<Part> parts; // at least one
    Motion motion = Motion::fixed;
    DVec3 position{};
    Quat rotation{};
    // Kilograms (dynamic bodies); 0: from the shape's volume at the
    // density of water (1000 kg per cubic metre).
    float mass = 0.0f;
    float friction = 0.6f;
    float restitution = 0.1f;
    Vec3 linear_velocity{};
    Vec3 angular_velocity{}; // radians a second
    // The game's own tag (an entity id): handed back by queries.
    u64 user = 0;
};

// A body's handle. 0 is none; a removed body's id is not reused at once
// (Jolt's sequence numbers), so a stale id finds nothing.
using BodyId = u32;
constexpr BodyId k_no_body = 0;

struct BodyState {
    DVec3 position{};
    Quat rotation{};
    Vec3 linear_velocity{};
    Vec3 angular_velocity{};
    bool active = false; // simulated this step (a resting body sleeps)
};

struct RayHit {
    BodyId body = k_no_body;
    u64 user = 0;
    DVec3 point{};
    Vec3 normal{};
    double distance = 0.0;
};

struct PhysicsSettings {
    u32 max_bodies = 1u << 18;
    u32 max_body_pairs = 1u << 18;
    u32 max_contacts = 1u << 17;
    // Fixed steps a step() call runs at most (time past them is dropped,
    // so a long frame slows the world down rather than freezing the game).
    u32 max_steps_per_call = 4;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
};

struct PhysicsStats {
    u32 bodies = 0;
    u32 active = 0;  // dynamic and kinematic bodies awake
    u64 steps = 0;   // fixed steps run since made
    u32 dropped = 0; // steps a step() call left out (over max_steps_per_call), in all
    double last_step_ms = 0.0; // CPU time of the last step() call
};

struct PreparedShapeData;

// A body's shape made ahead (PhysicsWorld::prepare): the costly part of
// adding a body (a 128-sample height field takes about a millisecond).
// Cheap to copy; invalid when the description was not.
class PreparedShape {
public:
    bool valid() const { return m_data != nullptr; }

private:
    friend class PhysicsWorld;
    std::shared_ptr<const PreparedShapeData> m_data;
};

class PhysicsWorld {
public:
    // Steps run on `jobs` (which must outlive the world).
    explicit PhysicsWorld(JobSystem& jobs, const PhysicsSettings& settings = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    static constexpr double k_step_seconds = 1.0 / 60.0;

    // k_no_body when the description is not valid (no parts, a shape with
    // bad numbers, a height shape on a moving body) or the world is full.
    // Sizes under a millimetre are taken as a millimetre (a sign, a sheet);
    // a compound leaves out a part it cannot make, if any is left.
    BodyId add(const BodyDesc& desc);
    // Many at once (cheaper: one pass into the broad phase); ids in order.
    std::vector<BodyId> add_many(std::span<const BodyDesc> descs);
    // The shape of `desc` (its parts and motion), made now on the calling
    // thread: any thread, while this world lives. Then add_many with it.
    PreparedShape prepare(const BodyDesc& desc) const;
    // As add_many, each body made with its prepared shape (`shapes` as many
    // as `descs`; their parts are not looked at). An invalid shape is refused.
    std::vector<BodyId> add_many(std::span<const BodyDesc> descs, std::span<const PreparedShape> shapes);
    // Removes bodies (listing one twice is fine). Sleeping bodies that may
    // have rested on one are woken (`wake_neighbours`; off when the whole
    // world is going).
    void remove(BodyId id);
    void remove_many(std::span<const BodyId> ids, bool wake_neighbours = true);
    // Every body gone at once (a new scene): the world starts afresh and the
    // old one is let go on a thread of its own. Every id handed out before
    // is stale, and may name a new body later: forget them all.
    void clear();
    bool contains(BodyId id) const;
    usize size() const;

    std::optional<BodyState> state(BodyId id) const;
    u64 user(BodyId id) const;
    // Puts a body somewhere at once (a teleport; wakes it, and what rested
    // on a fixed body where it was).
    void set_transform(BodyId id, const DVec3& position, const Quat& rotation);
    void set_velocity(BodyId id, const Vec3& linear, const Vec3& angular);
    // A kinematic body reaches this place by the end of the next fixed
    // step, pushing what is in the way.
    void move_kinematic(BodyId id, const DVec3& position, const Quat& rotation);

    // Moves time on by `seconds` (see the top of this file); returns the
    // fixed steps run.
    u32 step(double seconds);
    // Bodies that moved in the last step() call (dynamic and kinematic ones
    // awake in any of its fixed steps), in id order.
    const std::vector<BodyId>& moved() const { return m_moved; }

    std::optional<RayHit> raycast(const DVec3& origin, const Vec3& direction, double max_distance) const;

    PhysicsStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<BodyId> m_moved;
    // Cleared worlds being let go, each on a thread of its own (joined once
    // done, or when this world goes).
    struct Disposal;
    std::vector<std::unique_ptr<Disposal>> m_disposals;
};

} // namespace giant::sim
