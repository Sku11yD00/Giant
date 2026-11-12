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
//   const CharacterId player = physics.add_character({.position = {3, 0.5, 0}});
//   physics.set_character_input(player, {.velocity = {1.5f, 0, 0}});
//   physics.step(1.0);                     // walked 1.5 m along x
//
// Time moves in fixed steps of 1/60 s (the same result at any frame rate),
// each spread over the job system's workers; step() runs as many as the
// time given holds, at most `max_steps_per_call`, and keeps the remainder.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/math.h"
#include "giant/core/types.h"

#include <memory>
#include <functional>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace giant::world {
struct Water;
struct Swell;
}

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
    // Contacts name the part hit exactly (hits(): a building's pieces),
    // at the cost of a little more contact work.
    bool report_parts = false;
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
    u32 part = 0; // which of a compound's parts (0 for one shape), as ContactHit's
};

// Something struck something (a contact begun this step, hard enough):
// what breaks buildings. `normal` points from a to b; `impulse` (N s) is
// about how much the blow would take to stop (the closing speed at the
// point times the pair's effective mass there: masses, and inertia as they
// turn, so a falling log's tip strikes softer than a block as heavy).
struct ContactHit {
    BodyId a = 0;
    BodyId b = 0;
    u32 part_a = 0; // which of a compound's parts (0 for one shape)
    u32 part_b = 0;
    float impulse = 0.0f;
    DVec3 point{};
    Vec3 normal{};
};

struct PhysicsSettings {
    u32 max_bodies = 1u << 18;
    u32 max_body_pairs = 1u << 18;
    u32 max_contacts = 1u << 17;
    // Fixed steps a step() call runs at most (time past them is dropped,
    // so a long frame slows the world down rather than freezing the game).
    u32 max_steps_per_call = 4;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    // Hits under this (N s) are not reported (resting, rolling, a nudge).
    float min_hit_impulse = 50.0f;
};

struct PhysicsStats {
    u32 bodies = 0;     // characters' bodies left out
    u32 characters = 0;
    u32 active = 0;  // dynamic and kinematic bodies awake
    u64 steps = 0;   // fixed steps run since made
    u32 dropped = 0; // steps a step() call left out (over max_steps_per_call), in all
    double last_step_ms = 0.0; // CPU time of the last step() call
};

// A walking character (the player, later people in the town): a capsule
// that is moved, not pushed about like a body. It walks up steps, stays on
// the ground walking down slopes and stairs, stops at walls and slides off
// slopes too steep, jumps, rides what it stands on, and pushes dynamic
// bodies it walks into. Its position is its feet.
struct CharacterDesc {
    float height = 1.8f; // feet to the top of the head
    float radius = 0.3f;
    float max_slope_degrees = 50.0f; // steeper is a wall it slides down
    float step_height = 0.4f;        // steps it walks up (and down without leaving the ground); at most half its height
    float mass = 80.0f;              // kilograms: how hard what it stands on is pressed
    float strength = 500.0f;         // newtons it pushes dynamic bodies with
    float climb_speed = 2.0f;        // metres a second up, down or across a wall
    // In water (set_water) this deep over its feet (at most three quarters
    // of its height) it swims, head and shoulders out at rest, until 0.3 m
    // shallower; shallower it wades, slowed the deeper it is.
    float swim_depth = 1.3f;
    float swim_speed = 1.6f; // metres a second, any way, through the water
    DVec3 position{};
    u64 user = 0; // handed back by queries that hit it
};

// A character's handle; 0 is none.
using CharacterId = u32;
constexpr CharacterId k_no_character = 0;

// What the character is told to do, held until told otherwise.
struct CharacterInput {
    // Metres a second over the ground (y left out; each at most 100): reached at
    // once on the ground (within `acceleration`), slowly in the air (there
    // over the world, not over what it jumped off). Swimming, through the
    // water (carried by a river's flow) at most swim_speed, and y is up
    // (to the surface, no higher) or down (diving); with none it floats
    // up to the surface.
    Vec3 velocity{};
    float acceleration = 60.0f;     // metres a second squared, on the ground
    float air_acceleration = 6.0f;  // and in the air
    // Climbs what is too steep to walk (a wall, a cliff, a giant's leg)
    // when walked into: then `velocity` into the wall climbs up, away
    // from it climbs down, across it along it (at climb_speed). At the top
    // it pulls itself up onto the ledge; a jump pushes off; down on the
    // ground, or told not to climb, it lets go.
    bool climb = false;
};

struct CharacterState {
    DVec3 position{}; // feet
    // Where it was before the last fixed step: draw it at
    // lerp(previous, position, step_fraction()) so it moves smoothly at any frame rate.
    DVec3 previous{};
    bool held = false; // see hold_character
    Vec3 velocity{};
    bool on_ground = false; // stands on ground it can walk on
    bool on_steep = false;  // on a slope too steep: sliding
    Vec3 ground_normal{0.0f, 1.0f, 0.0f};
    BodyId ground = k_no_body; // the body stood on (or touched, sliding)
    bool climbing = false;     // on a wall (`wall`: the body, `wall_normal`: out of it)
    bool mantling = false;     // pulling itself up onto a ledge
    BodyId wall = k_no_body;
    Vec3 wall_normal{};
    BodyId carrier = k_no_body; // carried by this body (carry_character)
    bool swimming = false;
    float water_depth = 0.0f; // of the water over its feet (0: none)
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
    // old one is let go on a thread of its own. Every body id handed out
    // before is stale, and may name a new body later: forget them all.
    // Characters stay (made again where they were; their bodies' ids change).
    void clear();
    bool contains(BodyId id) const;
    usize size() const;

    std::optional<BodyState> state(BodyId id) const;
    u64 user(BodyId id) const;
    // Puts a body somewhere at once (a teleport; wakes it, and what rested
    // on a fixed body where it was).
    void set_transform(BodyId id, const DVec3& position, const Quat& rotation);
    void set_velocity(BodyId id, const Vec3& linear, const Vec3& angular);
    // A dynamic body pushed by `impulse` (N s) at `point` (a bullet's
    // blow: it may turn as well as move); wakes it. Other bodies (and
    // characters') are left alone.
    void add_impulse(BodyId id, const Vec3& impulse, const DVec3& point);
    // A kinematic body reaches this place by the end of the next fixed
    // step, pushing what is in the way.
    void move_kinematic(BodyId id, const DVec3& position, const Quat& rotation);

    // Moves time on by `seconds` (see the top of this file); returns the
    // fixed steps run.
    u32 step(double seconds);
    // Bodies that moved in the last step() call (dynamic and kinematic ones
    // awake in any of its fixed steps, characters' bodies too), in id order.
    const std::vector<BodyId>& moved() const { return m_moved; }
    // Contacts begun in the last step() call at least min_hit_impulse
    // hard, in a fixed order.
    const std::vector<ContactHit>& hits() const { return m_hits; }
    // How far time is into the next fixed step (0 to 1): the share of the
    // way from where things were to where they are to draw them at.
    double step_fraction() const;

    // Characters (see CharacterDesc). k_no_character when the description
    // has bad numbers (a radius too big for the height) or the world is full.
    CharacterId add_character(const CharacterDesc& desc);
    void remove_character(CharacterId id);
    bool contains_character(CharacterId id) const;
    // Followed in each fixed step until changed.
    void set_character_input(CharacterId id, const CharacterInput& input);
    // A jump at `speed` m/s: at once when on the ground, else as soon as it
    // lands within a fifth of a second (a press just before landing counts).
    void jump(CharacterId id, float speed);
    // Puts it somewhere at once, still.
    void set_character_position(CharacterId id, const DVec3& position);
    // Moving at `velocity` (m/s) from now, as if it had been (a fall goes on
    // falling, a run goes on running until its input says otherwise); at
    // most the characters' top speed. On the ground, rising (up a step, a
    // slope) is not taken as a jump. Climbing, pulling up or carried, the
    // next step sets its own speed. Bad numbers change nothing.
    void set_character_velocity(CharacterId id, const Vec3& velocity);
    // Takes hold of the wall it faces out of along `wall_normal` (flat),
    // as walking into it would: a wall too steep to walk on, not a moving
    // body, within reach. It keeps hold only while its input says climb (as
    // climbing does). False (nothing changed): none there, or carried.
    bool take_hold(CharacterId id, const Vec3& wall_normal);
    // Carried by `carrier` (a hand that picks it up, a platform it is
    // strapped to): from now it goes wherever the body takes it, held where
    // it is against it (in the body's frame), its input left aside, until
    // released (or the body goes). False (nothing changed) when either is
    // not there, or the body is a character's.
    bool carry_character(CharacterId id, BodyId carrier);
    // Let go, moving as it was carried (it falls from there).
    void release_character(CharacterId id);
    // Held: left where it is by the steps (the ground under it on its way),
    // keeping its speed (a jump asked for still lasts a fifth of a second).
    void hold_character(CharacterId id, bool held);
    std::optional<CharacterState> character_state(CharacterId id) const;
    // The body standing for the character (raycasts hit it; falling things
    // land on it): kinematic, moved with it; remove, set_transform,
    // set_velocity and move_kinematic leave it alone. k_no_body when not a character.
    BodyId character_body(CharacterId id) const;
    usize characters() const;
    // Whether the body stands for a character (character_body of one).
    bool is_character_body(BodyId id) const;

    // The lakes, rivers and sea (world/water.h), copied. In each fixed
    // step a dynamic body in water is buoyed up by the water it displaces
    // (at 1000 kg per cubic metre: lighter than that it floats, heavier it
    // sinks; a body of the default mass hangs where it is), slowed by it,
    // and carried along by a river's flow; characters wade and swim
    // (CharacterDesc). Water does not reach under something fixed between
    // a body all under it and the surface with something fixed under it
    // within 6 m, not the terrain's heightfield (a tunnel under a lake;
    // under a pier over the bed is open water). The
    // bodies where it was and where it is are woken. Lakes and rivers are
    // flat; the sea rises and falls with the swell (set_swell).
    void set_water(std::span<const world::Water> water);
    std::span<const world::Water> water() const;
    // The sea's swell (world::sea_swell, SwellTrack) at `seconds` on its
    // clock (the clock drawing it keeps: what floats rides what is seen),
    // given before step() with the time it is to step to still to come:
    // the swell's clock is set to `seconds` less the time not yet stepped
    // (step_fraction's), and each fixed step moves it on by itself. Seas
    // rise and fall under it by their `waves`; floating bodies bob and tilt
    // with the surface (kept awake while they float; a body sunk to the
    // bed sleeps) and swimmers ride it. A bad clock: 0. Sleeping bodies in
    // the water are woken when the swell first comes, and again each time
    // it has risen another quarter metre.
    void set_swell(const world::Swell& swell, double seconds);
    const world::Swell& swell() const;
    double swell_seconds() const;
    // Whether water whose surface is at `level` reaches `point` under it:
    // not in a room under something fixed (a tunnel under a lake), as
    // set_water keeps it out for bodies. (The camera under water.)
    bool water_reaches(const DVec3& point, double level) const;

    std::optional<RayHit> raycast(const DVec3& origin, const Vec3& direction, double max_distance) const;
    // Only bodies `accept` takes (called with each candidate's id and user
    // value, under the body's lock: it must not call back into the world, but for
    // is_character_body, a plain lookup),
    // and with fixed_only only fixed ones: what something can rest on for good.
    // With `body`, only that body; and with `part` too, only that part of
    // it (a compound's: RayHit::part), the others passed through.
    struct RayFilter {
        bool fixed_only = false;
        std::function<bool(BodyId, u64)> accept;
        BodyId body = k_no_body;
        std::optional<u32> part;
    };
    std::optional<RayHit> raycast(const DVec3& origin, const Vec3& direction, double max_distance,
                                  const RayFilter& filter) const;

    PhysicsStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<BodyId> m_moved;
    std::vector<ContactHit> m_hits;
    // Cleared worlds being let go, each on a thread of its own (joined once
    // done, or when this world goes).
    struct Disposal;
    std::vector<std::unique_ptr<Disposal>> m_disposals;
};

} // namespace giant::sim
