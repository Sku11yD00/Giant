// Hair: the guide strands of a groom (giant/geometry/groom.h), simulated.
// Each guide is a chain of k_hair_points points hanging from its root,
// which rides whatever the hair grows on (a head joint). Position-based,
// like the cloth: Verlet steps under gravity and the wind's drag, a pull
// back toward the styled rest shape (stiff at the root, loose at the tip,
// so curls stay curls and long hair still swings), every piece put back to
// exactly its length from the root down (hair does not stretch, unless a
// capsule is driven into it faster than it can get out of the way), and
// the points pushed out of the character's capsules (head, neck,
// shoulders).
//
//   HairSim hair(groom.points first guide_count * 16, settings);
//   hair.step(seconds, frame);       // each frame: where the head is, wind, capsules
//   hair.points();                   // the guides, in the groom's space: pose_strand, the renderer
//
// The points live in the world's directions about the place the hair
// hangs from (so the head's moves swing the hair, and floats stay exact
// far from the world's origin); points() gives them back in the groom's own
// space. A jump (a teleport, the first frame) carries the hair whole, at
// rest. Work is bounded (rule 9): fixed steps of step_seconds, at
// most max_steps a frame (time past that is dropped), nothing allocated
// after construction. The hair is simulated up to the last whole step
// before now and rides the head from there, each step seeing the head
// (and the capsules, carried with it) where it was at that step's time: a
// frame shorter than a step changes nothing, and frames of any length (up
// to max_steps steps' worth) show the hair the head's true speed. View
// state: nothing of it is saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>
#include <span>
#include <vector>

namespace giant::sim {

inline constexpr u32 k_hair_points = 16; // geometry::k_strand_points
inline constexpr u32 k_max_hair_capsules = 16;

struct HairSettings {
    // How firmly a point is held to the rest shape: the frequency (Hz,
    // 0..60) of the spring that pulls it back, by the root and by the tip
    // (between, in between). 0: not held.
    float root_stiffness = 8.0f;
    float tip_stiffness = 1.0f;
    float damping = 2.0f;        // 1/s, 0..60: how fast a point's speed dies away
    float gravity = 9.81f;       // m/s²
    float wind_response = 0.5f;  // 1/m: how hard moving air drags a strand along (0: not at all)
    float thickness = 0.005f;    // metres the hair stands off a capsule
    float friction = 0.5f;       // 0..1: how a capsule holds hair lying on it from sliding
    // 1/240..1/30. Part of the look: finer steps sag a little less and
    // swing longer, so tune a style at the step it will run at.
    float step_seconds = 1.0f / 60.0f;
    u32 max_steps = 4; // a frame, 1..16
};

// A rounded bar (a bone's hit shape) in the world's directions, relative to
// HairFrame::position.
struct HairCapsule {
    Vec3 a{};
    Vec3 b{};
    float radius = 0.0f;
};

// Where the hair hangs this frame, and what is around it.
struct HairFrame {
    DVec3 position{};  // of the groom's origin, in the world
    Quat rotation{};   // the groom's space to the world's directions
    // Uniform, 0.001..1000 (no number: 1). A new scale resizes the hair
    // as it hangs.
    float scale = 1.0f;
    Vec3 wind{};       // m/s, at the hair (wind_at), world
    // The first k_max_hair_capsules are felt, each known by its place in
    // the list from frame to frame. A point that rested inside one when the
    // hair was placed (roots under a head's capsule) is only kept as far
    // out as it rested then, so a capsule never pushes hair off the scalp
    // it grows from; so place hair (the first step, reset(), a jump, a
    // list of another length) with the character at rest, since a limb
    // that crosses the hair's rest shape just then is let through there
    // until it is placed again. One with
    // numbers that are none is not felt. Between frames they are taken to
    // ride the head.
    std::span<const HairCapsule> capsules{};
};

struct HairStats {
    u32 guides = 0;
    u32 steps = 0;  // last step(): fixed steps taken
    u64 resets = 0; // in all: put back at rest after a jump or numbers gone wrong
};

class HairSim {
public:
    // `rest_guides`: k_hair_points points a guide, root first, in the
    // groom's space (a count that is not a multiple drops the remainder).
    // Bad settings are clamped.
    explicit HairSim(std::span<const Vec3> rest_guides, const HairSettings& settings = {});

    // Time moves on. Seconds that are no number or negative do nothing.
    void step(double seconds, const HairFrame& frame);
    // The hair at rest again, wherever it next steps.
    void reset();

    u32 guide_count() const { return m_guides; }
    // The guides now, in the groom's space, as the rest guides were given.
    std::span<const Vec3> points() const { return m_local; }
    std::span<const Vec3> rest() const { return m_rest; }
    const HairSettings& settings() const { return m_settings; }
    HairStats stats() const { return m_stats; }
    // Changes whenever points() does (moves under a hundredth of a
    // millimetre are not shown until they add up, so hair at rest keeps
    // its version).
    u64 version() const { return m_version; }

private:
    void place_at_rest(const HairFrame& frame, u32 capsules);
    void bind(const HairFrame& frame, u32 capsules);
    void weigh_give();
    bool advance(float dt, const HairFrame& frame);
    void to_local();

    HairSettings m_settings;
    u32 m_guides = 0;
    std::vector<Vec3> m_rest;
    std::vector<float> m_lengths;  // per point: the rest length of the piece from the one before (0 for roots)
    std::vector<Vec3> m_points;    // world directions, about m_position
    std::vector<Vec3> m_previous;
    std::vector<Vec3> m_local;
    std::vector<Vec3> m_targets;   // scratch: each point's rest place this step
    std::vector<float> m_bound;    // per point and capsule: how near the axis it rested when placed
    std::vector<float> m_reach;    // per guide: its length
    std::vector<float> m_give;     // per point: the share of its length fix the point above answers for
    std::vector<u16> m_near;       // scratch, per guide: the capsules in its reach (bits)
    u32 m_bound_capsules = 0;
    std::array<HairCapsule, k_max_hair_capsules> m_capsules{}; // scratch: as of the step being taken
    DVec3 m_position{}; // the place the points are about: the head's at the last step's time
    Quat m_rotation{};
    DVec3 m_last_position{}; // the last frame's place
    Quat m_last_rotation{};
    float m_scale = 1.0f;
    bool m_placed = false;
    float m_time = 0.0f; // seconds from the last step's time to the last frame's
    HairStats m_stats;
    u64 m_version = 1;
};

// The settings as the simulation takes them.
HairSettings clean(const HairSettings& settings);

} // namespace giant::sim
