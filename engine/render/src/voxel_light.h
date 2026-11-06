// Which voxel bricks get their light worked out each frame (global
// illumination, docs/architecture.md "Global illumination").
//
// The light pool sits beside the voxel world's brick pool: one u32 per
// voxel (voxel::pack_light), brick after brick, in the same pool indices.
// The GPU lights a budgeted number of bricks a frame (voxel_light.slang);
// this picks them:
// - a brick just put in a pool index is first cleared to "not lit" (what
//   the index held before was another brick's), then lit as soon as the
//   budget allows, before anything else;
// - a brick built again in place keeps its old light until lit again,
//   which it is next, the same way;
// - the rest are lit again in turn, round the pool, so moving sun and
//   changing surroundings reach every brick within (bricks / budget)
//   frames, and light bounces once more each time round. A quarter of the
//   budget is kept for the round, so it goes on while new bricks keep
//   coming (the camera flying fast).
// Nothing is lit when global illumination is off; turned on again, the
// bricks put in the pool meanwhile are cleared and lit first (the voxel
// world numbers its placements), the rest keep their light until the round
// comes to them.
#pragma once

#include "giant/core/types.h"
#include "gpu_layout.h"

#include <deque>
#include <span>
#include <vector>

namespace giant::render {

class VoxelScene;

struct VoxelLightStats {
    u32 lit = 0;     // bricks lit this frame
    u32 cleared = 0; // bricks cleared this frame
    u32 waiting = 0; // new or rebuilt bricks not lit yet
};

class VoxelLight {
public:
    explicit VoxelLight(u32 capacity);
    ~VoxelLight();
    VoxelLight(const VoxelLight&) = delete;
    VoxelLight& operator=(const VoxelLight&) = delete;

    // This frame's work, after the voxel world's update: up to `budget`
    // bricks lit (at least one when any are in the pool).
    void plan(const VoxelScene& scene, u32 budget);
    // A frame without global illumination: what the voxel world places
    // meanwhile is caught up with next time.
    void off();
    // This frame's work never reached the GPU (out of memory): next time,
    // what was placed since the frame before is cleared again, and this
    // frame's bricks are lit first.
    void lost();

    std::span<const u32> clears() const { return m_clears; }       // pool indices
    std::span<const gpu::LightJob> jobs() const { return m_jobs; }
    const VoxelLightStats& stats() const { return m_stats; }

private:
    void queue(u32 index);

    u32 m_capacity = 0;
    bool m_catch_up = true; // first frame, or after off() / lost(): look for what was placed meanwhile
    u64 m_seen = 0;         // the voxel world's placements() as of the last plan
    u64 m_seen_before = 0;  // ...and as of the one before it (lost())
    std::vector<u32> m_clears;
    std::vector<gpu::LightJob> m_jobs;
    std::deque<u32> m_urgent;  // pool indices to light before the round
    std::vector<u8> m_queued;  // per pool index: in m_urgent
    std::vector<u32> m_stamp;  // per pool index: the plan() that last lit it
    std::vector<u32> m_cleared; // per pool index: the plan() that last cleared it
    u32 m_frame = 0;
    u32 m_cursor = 0;          // the round's next pool index
    VoxelLightStats m_stats;
};

} // namespace giant::render
