// Motion matching's player: it keeps one frame of a MotionDatabase
// playing, looks every tenth of a second for a frame that fits the wanted
// way ahead better, and jumps there when one does. A jump never shows:
// the difference between the pose it left and the pose it landed on (and
// how fast each was changing) is kept as an offset per joint that dies
// away in a fraction of a second (inertial blending; two clips are never
// sampled and mixed).
//
// The pose it gives stands in place: the root's travel and turning (the
// database's smoothed hips) are taken out, so the character faces +z over
// its origin and whoever moves it (a walker's body, the player) owns where
// it is. root_motion says how the clip would have moved it.
//
// One matcher per character; the database, skeleton and clips are handed
// in. The database must be as build_motion_database left it and the
// clips the ones it was built from (they are not validated again: only
// what cannot belong together at a glance is turned away). Nothing is
// allocated once its buffers have grown to the skeleton.
#pragma once

#include "giant/anim/motion.h"
#include "giant/anim/player.h"

#include <span>
#include <vector>

namespace giant::anim {

struct MatcherSettings {
    float search_seconds = 0.1f;   // between looks for a better frame (0: every update)
    float blend_seconds = 0.1f;    // a jump's offsets halve in this long (0: jumps show; to 1)
    u32 near_frames = 4;           // a better frame this near the one playing, in its clip, is no jump
    // How much being posed alike counts beside going the wanted way (0 to
    // 1000). Under 1 a character changes its mind sooner: what few clips
    // need, with none recorded going from one to the next.
    float pose_weight = 0.5f;
    float stay_bias = 0.05f;       // another frame must be this much better (of the cost, 0 to 1) to jump to
};

// How the playing clip moves its root now, in the character's space.
struct RootMotion {
    float x = 0.0f;   // metres a second across
    float z = 0.0f;   // and along its facing
    float yaw = 0.0f; // radians a second about +y
};

// The clip of `range` (one of the database's) at `seconds` into it, with
// its root's travel and turning taken out: `local` is one per joint.
// (Exact under a skeleton.root that moves, turns, mirrors and scales
// evenly; under one that scales unevenly the pose is turned wrongly:
// such a file's character is not for matching.)
void motion_pose(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips, usize range,
                 float seconds, std::span<JointTransform> local);

// A way ahead for a character moving at `velocity` (x across, z along its
// facing, metres a second) that should come to move at `wanted` and to
// have turned by `turn` radians (about +y, +z toward +x), easing over
// `ease_seconds` (the time to cover half the change, about).
MotionAhead motion_ahead(const MotionSettings& settings, std::array<float, 2> velocity, std::array<float, 2> wanted,
                         float turn, float ease_seconds = 0.25f);

class MotionMatcher {
public:
    MatcherSettings settings;

    // Nothing playing (the rest pose); the next update starts afresh, with no blend.
    void reset();

    // Moves `seconds` on (0 to 1; else as near as that allows), looking for
    // a better frame when one is due, `search_now` asks, or a one-off clip
    // has ended. An empty database, or one that is not the skeleton's and
    // clips', resets.
    void update(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips,
                const MotionAhead& ahead, float seconds, bool search_now = false);

    // The pose now, into `local` (one per joint): the rest pose with nothing playing.
    void evaluate(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips,
                  std::span<JointTransform> local) const;

    bool playing() const { return m_frame != k_no_frame; }
    u32 frame() const { return m_frame; }     // the database's, or k_no_frame
    float seconds() const { return m_time; }  // into its clip
    u64 jumps() const { return m_jumps; }     // since reset
    float last_cost() const { return m_cost; } // of the frame the last search stayed on or jumped to
    RootMotion root_motion(const MotionDatabase& db) const;

private:
    void jump(const MotionDatabase& db, const Skeleton& skeleton, std::span<const Clip> clips, u32 to);
    void decay(float seconds);

    u32 m_frame = k_no_frame;
    float m_time = 0.0f;         // seconds into its range
    float m_since_search = 0.0f;
    float m_cost = 0.0f;
    u64 m_jumps = 0;
    bool m_ended = false;        // a one-off clip at its end
    // The offsets a jump left, per joint: added to the pose, dying away.
    std::vector<Vec3> m_place;
    std::vector<Vec3> m_place_velocity;
    std::vector<Quat> m_turn;
    std::vector<Vec3> m_turn_velocity; // radians a second, about its length
    // Scratch for a jump: the pose left and the pose landed on, now and a moment before.
    std::vector<JointTransform> m_from, m_from_before, m_to, m_to_before;
};

} // namespace giant::anim
