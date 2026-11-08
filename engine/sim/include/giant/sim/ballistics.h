// Bullets: point masses flying through the physics world, falling and
// slowing as they go, going through what they have the energy for and
// glancing off hard things struck at a shallow angle.
//
//   Ballistics bullets(physics);
//   bullets.target_of = [&](const RayHit& hit) { return what_it_is(hit); };
//   bullets.fire({.origin = muzzle, .velocity = aim * 880.0f, .mass = 0.0095f});
//   bullets.step(delta);                 // each frame, after the physics step
//   for (const BulletHit& h : bullets.hits()) apply(h); // blows, decals, damage
//
// Each bullet moves in fixed steps of 1/240 s (the same flight at any frame
// rate: a remainder waits for the next call) under gravity and quadratic
// drag (slowing by `drag` times its speed squared), and casts a ray along
// each step. What a ray meets is a surface (target_of; world::Surface):
//
//   - Struck at a shallow angle (under the surface's ricochet angle), a
//     hard surface sends it on mirrored off it, slower (a ricochet).
//   - Otherwise it goes in as far as its energy takes it: each metre of the
//     surface takes `resistance` joules. Where the part of the body it went
//     into ends along its path (a ray cast back at that part from that far
//     in) is how thick it is there: if the bullet gets through it comes out
//     the far side with the energy left, on the same line (penetration),
//     and meets whatever is next (another part of the same body, its own
//     surface) as a new layer; if not it stops inside. The terrain (a height
//     field: no far side to find) always stops it. A ray that starts inside
//     something (a muzzle in a wall) is inside it: it never glances off.
//
// Every meeting is a BulletHit: where, what, how fast in and out, the
// energy it left there and the blow (N s) it gave the body, which the game
// turns into pushes, broken pieces and wounds. A bullet's flight ends when
// it stops, slows under min_speed, goes past max_range or flies max_seconds.
// Work is bounded (rule 9): at most max_bullets fly at once (more are
// refused), each step's ray goes through at most max_layers surfaces (the
// rest of that step's way is dropped), and a step() call casts about
// max_rays_per_call rays at most (the fixed steps past them wait for the
// next call: bullets fall behind rather than stall the frame).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/sim/physics.h"
#include "giant/world/scene.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::sim {

// How a surface meets a bullet.
struct SurfaceBallistics {
    float resistance = 15000.0f; // joules each metre through it takes (> 0)
    // Struck at under this angle to the surface (degrees, 0 to 90), it
    // glances off (0: never), keeping `ricochet_keep` of its speed (0 to 1).
    float ricochet_degrees = 0.0f;
    float ricochet_keep = 0.6f;
    bool operator==(const SurfaceBallistics&) const = default;
};

// The surfaces as shipped (by world::Surface): flesh takes a rifle round
// through a torso, a wooden wall a pistol's; concrete and brick stop most
// in a hand's breadth; metal stops all but armour piercing in a few
// millimetres; glass and leaves hardly slow them.
std::array<SurfaceBallistics, world::k_surface_count> default_surfaces();

struct BallisticsSettings {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    // Quadratic drag (1/m): slowing of drag * speed^2 m/s^2. 0.0007 takes a
    // 880 m/s rifle round to about 500 m/s at 800 m.
    float drag = 0.0007f;
    float min_speed = 40.0f;     // m/s: slower, it is spent
    double max_range = 4000.0;   // metres flown
    double max_seconds = 10.0;
    u32 max_bullets = 4096;
    u32 max_layers = 8;          // surfaces one step's ray goes through, at most
    float max_thickness = 4.0f;  // metres: thicker never goes through (and the ray back starts at most this far in)
    std::array<SurfaceBallistics, world::k_surface_count> surfaces = default_surfaces();
};

// A shot fired.
struct Shot {
    DVec3 origin{};
    Vec3 velocity{};       // m/s
    float mass = 0.008f;   // kilograms (> 0)
    // Its strength against surfaces (> 0): resistances are divided by it
    // (2: armour piercing, through twice as much; 0.5: soft hollow point).
    float penetration = 1.0f;
    u64 tag = 0;           // the game's own (who fired): handed back with its hits
    BodyId ignore = k_no_body; // the shooter's body: passed through
};

using BulletId = u32;
constexpr BulletId k_no_bullet = 0;

enum class HitOutcome : u8 { stopped, passed, ricochet };
const char* to_string(HitOutcome outcome);

struct BulletHit {
    BulletId bullet = k_no_bullet;
    u64 tag = 0;
    BodyId body = k_no_body;
    u64 user = 0;
    u32 part = 0; // of the body (RayHit::part)
    // What target_of named, as the hit happened (bodies may change after).
    u64 owner = 0;
    std::optional<u32> piece;
    DVec3 point{};           // where it went in (or glanced off)
    Vec3 normal{};           // the surface's there, outwards
    Vec3 direction{};        // its way in, unit
    world::Surface surface = world::Surface::standard;
    HitOutcome outcome = HitOutcome::stopped;
    float speed_in = 0.0f;   // m/s
    float speed_out = 0.0f;  // as it left (0: stopped)
    float energy = 0.0f;     // joules it left there
    Vec3 impulse{};          // N s it gave the body (its momentum lost)
    float thickness = 0.0f;  // metres gone through (passed; how far in, stopped)
    DVec3 exit{};            // where it came out (passed), or stopped
    double distance = 0.0;   // metres flown from the muzzle to `point`
};

struct BulletState {
    BulletId id = k_no_bullet;
    u64 tag = 0;
    DVec3 position{};
    DVec3 previous{}; // before the last fixed step (a tracer from here)
    Vec3 velocity{};
    double distance = 0.0; // flown
    double seconds = 0.0;
};

struct BallisticsStats {
    u32 flying = 0;
    u64 fired = 0;
    u64 refused = 0; // over max_bullets, or bad numbers
    u64 hits = 0;
    u64 rays = 0;    // cast in all
    u64 steps = 0;
    double last_step_ms = 0.0;
};

class Ballistics {
public:
    static constexpr double k_step_seconds = 1.0 / 240.0;

    // `physics` must outlive this.
    explicit Ballistics(const PhysicsWorld& physics, const BallisticsSettings& settings = {});

    // What a ray met: what it is made of, and the game's own names for it
    // (an entity, a piece of it), handed back in BulletHit. None: standard,
    // nothing named.
    struct Target {
        world::Surface surface = world::Surface::standard;
        u64 owner = 0;
        std::optional<u32> piece;
    };
    std::function<Target(const RayHit&)> target_of;

    // k_no_bullet when refused (bad numbers, or max_bullets flying).
    BulletId fire(const Shot& shot);
    // Flies a shot to its end now (at most max_seconds), every hit handed
    // back in order, none in hits(); the world as it is now (bodies do not
    // move meanwhile). A shot for a command, or a hitscan weapon.
    std::vector<BulletHit> resolve(const Shot& shot);
    // Moves time on by `seconds` for every bullet flying; returns the fixed
    // steps run (at most max_steps_per_call: time past them is dropped).
    u32 step(double seconds);
    u32 max_steps_per_call = 16;
    u32 max_rays_per_call = 16384;
    // The hits of the last step() call, in the order they happened (bullet
    // by bullet within a fixed step).
    const std::vector<BulletHit>& hits() const { return m_hits; }
    std::vector<BulletState> bullets() const;
    // Every bullet gone (a scene swapped, a simulation stopped).
    void clear();

    BallisticsSettings& settings() { return m_settings; }
    const BallisticsSettings& settings() const { return m_settings; }
    BallisticsStats stats() const;

private:
    struct Bullet {
        BulletId id = k_no_bullet;
        Shot shot;
        DVec3 position{};
        DVec3 previous{};
        Vec3 velocity{};
        double distance = 0.0;
        double seconds = 0.0;
        bool done = false;
    };
    // One fixed step of a bullet: its hits appended to `out`.
    void advance(Bullet& b, std::vector<BulletHit>& out);
    // The bullet along `dir` (unit) from `from` for `length` metres: what
    // it meets, through what it can; its speed, way and place changed (done
    // when it stops or is spent).
    void fly(Bullet& b, DVec3 from, Vec3 dir, double length, std::vector<BulletHit>& out);

    const PhysicsWorld& m_physics;
    BallisticsSettings m_settings;
    std::vector<Bullet> m_bullets;
    std::vector<BulletHit> m_hits;
    BulletId m_next = 1;
    double m_carried = 0.0;
    BallisticsStats m_stats;
};

} // namespace giant::sim
