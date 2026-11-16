// Motion matching's database: every frame of a character's clips described
// by a few numbers (where its feet are and how they move, how its hips
// move, and where it will be and face over the next second), and the search
// for the frame that best fits what is wanted now. A character that keeps
// playing from the best frame walks, turns and stops as its clips do, with
// no state machine deciding which clip.
//
// The character's space. A frame's numbers are relative to its root: the
// hips' place on the ground (model y = 0) and the way they face, both
// smoothed over the frames about it (hips sway and twist with each step).
// x and z are across and along the facing, y is up. A clip must move its
// hips through model space (root motion): one walking in place reads as
// standing.
//
// A looping clip's frames go round: what lies ahead of its last frames is
// its first ones again, moved on (and turned, for a loop that turns) by
// what one round does. A one-off clip cannot be jumped into where less
// than the longest look ahead is left of it.
//
// Building a database samples every clip: do it on a job, not each frame.
// Searching allocates nothing.
#pragma once

#include "giant/anim/skeleton.h"

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace giant::anim {

inline constexpr u32 k_no_joint = ~u32{0};
inline constexpr u32 k_no_frame = ~u32{0};
inline constexpr usize k_motion_horizons = 3;
inline constexpr usize k_motion_features = 27;
inline constexpr usize k_max_motion_frames = 1'000'000; // over nine hours at 30 a second
inline constexpr usize k_max_motion_clips = 4096;

// Where a frame's numbers lie.
inline constexpr usize k_motion_foot_places = 0;      // left xyz, right xyz
inline constexpr usize k_motion_foot_velocities = 6;  // left xyz, right xyz
inline constexpr usize k_motion_hips_velocity = 12;   // xyz
inline constexpr usize k_motion_ahead_places = 15;    // x, z at each look ahead
inline constexpr usize k_motion_ahead_facings = 21;   // x, z at each look ahead

struct MotionJoints {
    u32 hips = k_no_joint;
    u32 left_foot = k_no_joint;
    u32 right_foot = k_no_joint;
    bool found() const { return hips != k_no_joint && left_foot != k_no_joint && right_foot != k_no_joint; }
    bool operator==(const MotionJoints&) const = default;
};

// The joints by their names as the usual rigs have them ("Hips" or
// "pelvis"; "LeftFoot", "foot_l", "L_Foot"; any prefix up to a ':' or '|'
// ignored, case and punctuation too). k_no_joint for one not found.
MotionJoints find_motion_joints(const Skeleton& skeleton);

struct MotionClip {
    u32 clip = 0;      // in the clips handed to build
    bool loop = false; // its end is its start again (clip_loops)
};

// Whether the clip ends posed as it began, seen from its root: every
// joint within `tolerance` metres of where it started. A clip of no
// length does.
bool clip_loops(const Skeleton& skeleton, const Clip& clip, const MotionJoints& joints, float tolerance = 0.03f);

struct MotionSettings {
    float frame_rate = 30.0f;                                                  // 10 to 120
    std::array<float, k_motion_horizons> horizons{1.0f / 3.0f, 2.0f / 3.0f, 1.0f}; // seconds ahead, rising, to 2
    float smoothing_seconds = 0.25f; // the root is the hips averaged this long before and after (to 1)
    Vec3 forward{0.0f, 0.0f, 1.0f};  // the way the model faces at rest (its level part is used)
    // How much each group of numbers counts in a match.
    float foot_place_weight = 0.75f;
    float foot_velocity_weight = 1.0f;
    float hips_velocity_weight = 1.0f;
    float ahead_place_weight = 1.0f;
    float ahead_facing_weight = 1.5f;
};

// A clip's frames in the database.
struct MotionRange {
    u32 clip = 0;
    u32 first = 0;     // its first frame
    u32 count = 0;     // at least 1
    u32 startable = 0; // frames [first, first + startable) may be jumped into
    bool loop = false;
    // Between its frames: 1 / frame_rate, but for a loop its length over
    // its frames (a whole number to the round).
    float frame_seconds = 0.0f;
};

// A frame's root in model space.
struct MotionRoot {
    float x = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f; // radians about +y from +z, not wrapped (it runs on through a turning clip)
};

using MotionFeatures = std::array<float, k_motion_features>;

struct MotionDatabase {
    MotionSettings settings;
    MotionJoints joints;
    std::vector<MotionRange> ranges; // in frame order
    std::vector<MotionRoot> roots;   // per frame
    std::vector<float> features;     // per frame k_motion_features, normalised: (raw - offset) * scale
    MotionFeatures offset{};
    MotionFeatures scale{};
    // The least and most of each number over the startable frames of each
    // 16 and each 64 frames (none startable: both far off, past any frame).
    std::vector<float> small_least, small_most, large_least, large_most;

    usize frames() const { return roots.size(); }
    std::span<const float, k_motion_features> frame(u32 f) const {
        return std::span<const float, k_motion_features>(features.data() + usize{f} * k_motion_features,
                                                         k_motion_features);
    }
    // The range holding frame `f` (in range).
    const MotionRange& range_of(u32 f) const;
    // Seconds into its clip.
    float time_of(u32 f) const;
    // The frame after `f`: round to its clip's first for a loop; `f` again
    // at a one-off clip's end.
    u32 next(u32 f) const;
    // A frame's number as it was measured (metres, metres a second); the
    // mean over all frames for a number whose weight is 0 (it is not kept).
    float raw(u32 f, usize feature) const;
};

// Builds `out` from `chosen` clips of `clips`. Empty when built; else why
// not (joints not found or out of range, a skeleton or clip that does not
// validate, a clip out of range, settings out of range, too many frames,
// no frame to start from, a pose over 100 km from its root), and `out` is
// empty. A looping clip shorter
// than half a frame is a pose: one frame.
[[nodiscard]] std::string build_motion_database(const Skeleton& skeleton, std::span<const Clip> clips,
                                                std::span<const MotionClip> chosen, const MotionJoints& joints,
                                                const MotionSettings& settings, MotionDatabase& out);

// The way ahead that is wanted, in the character's space now: where it
// should be (x across, z along its facing) and face (a level direction,
// unit length) at each look ahead.
struct MotionAhead {
    std::array<std::array<float, 2>, k_motion_horizons> place{};
    std::array<std::array<float, 2>, k_motion_horizons> facing{{{0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}}};
};

// What to look for: the pose of frame `now` (the frame playing) with
// `ahead` as its way ahead; normalised as the database's frames are. With
// no frame playing yet (k_no_frame) the pose is no one's: search with
// `pose` 0. A place or facing that is no number counts as 0, and a
// place is kept within a kilometre, so a search always finds a frame.
MotionFeatures motion_query(const MotionDatabase& db, u32 now, const MotionAhead& ahead);

// How far frame `f` is from `query` (squared, over the normalised
// numbers). `pose` is how much the pose's numbers count beside the way
// ahead's (0 to 1000): 0, only the way ahead counts; under 1, a frame
// posed less alike is taken sooner for going the wanted way.
float motion_cost(const MotionDatabase& db, u32 f, const MotionFeatures& query, float pose = 1.0f);

struct MotionMatch {
    u32 frame = k_no_frame;
    float cost = std::numeric_limits<float>::max();
};

// The startable frame nearest `query`, if nearer than `best` (hand in the
// frame playing and its cost to stay on it unless another is better; a
// tie keeps `best`). `pose` as motion_cost has it (0: a first frame to
// play). k_no_frame only from an empty database, or when `best` was
// handed in so (with a cost no frame is under).
MotionMatch motion_search(const MotionDatabase& db, const MotionFeatures& query, MotionMatch best = {},
                          float pose = 1.0f);

} // namespace giant::anim
