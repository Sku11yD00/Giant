// Characters' hit shapes: a capsule round each bone, posed with the
// character, named by zone (head, torso, arm, leg...), for bullets to meet
// (sim::Ballistics::hit_shapes). They live beside the physics world, not in
// it: a character's capsules move with its animation every frame, which
// would mean remaking a body each frame; here moving them is copying a few
// dozen numbers. A ray is tested against each character's bounding sphere,
// then its capsules.
//
//   HitShapes shapes;
//   shapes.set(entity, {.capsules = posed});     // each frame its pose changes
//   const auto hit = shapes.raycast(from, dir, 100.0); // nearest capsule met
//
// Zones are named by rules on the bones' names (HitZoneMap): the first
// rule with a word the bone's name holds (ignoring case) names it. The game
// gives its zones their meaning (a head shot's damage); the engine reports
// them.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::sim {

constexpr u16 k_no_zone = 0xFFFF;

struct HitZoneRule {
    std::string zone;               // its name ("head")
    std::vector<std::string> match; // words a bone's name may hold ("head", "skull")
    bool operator==(const HitZoneRule&) const = default;
};
struct HitZoneMap {
    std::vector<HitZoneRule> rules; // the first that matches names the bone
    // The zone (index into rules) a bone of this name is in; k_no_zone: none.
    u16 zone_of(std::string_view bone) const;
    bool operator==(const HitZoneMap&) const = default;
};
// Rules for the usual rigs (Mixamo, Unreal, Blender's Rigify): head, neck,
// torso (spine, chest, hips, pelvis, shoulders), arm (arms, hands,
// fingers), leg (legs, thighs, knees, calves, feet, toes); anything else
// is in no zone.
HitZoneMap default_hit_zones();

// A capsule in the world: the segment a to b, `radius` round it.
struct HitCapsule {
    DVec3 a{};
    DVec3 b{};
    float radius = 0.0f;
    u32 bone = 0;          // the joint it is round
    u16 zone = k_no_zone;  // in the character's zone map
};

struct HitCharacter {
    std::vector<HitCapsule> capsules;
    world::Surface surface = world::Surface::flesh;
};

struct ShapeHit {
    u64 owner = 0;          // as set()
    u32 capsule = 0;        // which of its capsules
    double distance = 0.0;  // along the ray to where it goes in (0: it starts inside)
    double exit = 0.0;      // and to where it comes out
    DVec3 point{};
    Vec3 normal{};          // out of the capsule there
};

class HitShapes {
public:
    // The character's shapes (replacing any it had); none: removed.
    void set(u64 owner, HitCharacter character);
    void remove(u64 owner);
    void clear();
    bool contains(u64 owner) const { return m_characters.contains(owner); }
    const HitCharacter* find(u64 owner) const;
    usize size() const { return m_characters.size(); }

    // The nearest capsule along the ray (direction unit) within
    // max_distance, of any character but `ignore` (0: none).
    std::optional<ShapeHit> raycast(const DVec3& origin, const Vec3& direction, double max_distance,
                                    u64 ignore = 0) const;

private:
    struct Entry {
        HitCharacter character;
        DVec3 center{};
        double radius = 0.0; // round every capsule
    };
    std::unordered_map<u64, Entry> m_characters;
};

// Where a ray (direction unit) goes into and out of a capsule: the
// distances along it (in may be negative: it starts inside); none if it
// misses.
std::optional<std::pair<double, double>> ray_capsule(const DVec3& origin, const Vec3& direction, const DVec3& a,
                                                     const DVec3& b, double radius);

} // namespace giant::sim
