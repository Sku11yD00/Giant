#include "voxel_light.h"

#include "gpu_layout.h"
#include "voxel_scene.h"

#include <algorithm>

namespace giant::render {

VoxelLight::VoxelLight(u32 capacity)
    : m_capacity(capacity), m_queued(capacity, 0), m_stamp(capacity, 0), m_cleared(capacity, 0) {}

VoxelLight::~VoxelLight() = default;

void VoxelLight::queue(u32 index) {
    if (index < m_capacity && m_queued[index] == 0) {
        m_queued[index] = 1;
        m_urgent.push_back(index);
    }
}

void VoxelLight::off() {
    m_catch_up = true;
    m_clears.clear();
    m_jobs.clear();
    m_stats = {};
}

void VoxelLight::lost() {
    m_catch_up = true;
    m_seen = m_seen_before;
    for (const gpu::LightJob& job : m_jobs) {
        queue(job.index); // lit next time, before the round
    }
}

void VoxelLight::plan(const VoxelScene& scene, u32 budget) {
    m_clears.clear();
    m_jobs.clear();
    m_stats = {};
    ++m_frame;
    if (m_frame == 0) { // wrapped: no stamp may match by accident
        std::fill(m_stamp.begin(), m_stamp.end(), 0u);
        std::fill(m_cleared.begin(), m_cleared.end(), 0u);
        m_frame = 1;
    }
    const u32 capacity = std::min(m_capacity, scene.capacity());
    u32 level = 0;
    voxel::BrickCoord brick;
    const auto clear = [&](u32 i) {
        if (m_cleared[i] != m_frame) {
            m_cleared[i] = m_frame;
            m_clears.push_back(i);
        }
        queue(i);
    };
    m_seen_before = m_seen;
    if (m_catch_up) {
        // Whatever was put in the pool since last time (everything, the
        // first time): this frame's placed() among them.
        m_catch_up = false;
        for (u32 i = 0; i < capacity; ++i) {
            if (scene.placed_at(i) > m_seen && scene.pool_brick(i, level, brick)) {
                clear(i);
            }
        }
    } else {
        for (const u32 i : scene.placed()) {
            if (i < capacity) {
                clear(i);
            }
        }
    }
    m_seen = scene.placements();
    for (const u32 i : scene.rebuilt()) {
        queue(i);
    }
    const auto add = [&](u32 i) {
        if (m_stamp[i] == m_frame || !scene.pool_brick(i, level, brick)) {
            return; // lit already this frame, or freed since
        }
        m_stamp[i] = m_frame;
        gpu::LightJob job{};
        job.index = i;
        job.level = level;
        job.brick[0] = brick.x;
        job.brick[1] = brick.y;
        job.brick[2] = brick.z;
        m_jobs.push_back(job);
    };
    const u32 limit = std::max(budget, 1u);
    const auto urgent = [&](usize upto) {
        while (m_jobs.size() < upto && !m_urgent.empty()) {
            const u32 i = m_urgent.front();
            m_urgent.pop_front();
            m_queued[i] = 0;
            add(i);
        }
    };
    // New and rebuilt bricks first, leaving a quarter for the round.
    urgent(limit - limit / 4);
    // The round: one look at each pool index at most per frame, stopping
    // once every brick in the pool has been looked at.
    const u32 bricks = scene.stats().bricks;
    u32 found = 0;
    for (u32 looked = 0; looked < capacity && found < bricks && m_jobs.size() < limit; ++looked) {
        const u32 i = m_cursor;
        m_cursor = m_cursor + 1 < capacity ? m_cursor + 1 : 0;
        if (!scene.pool_brick(i, level, brick)) {
            continue;
        }
        ++found;
        if (m_queued[i] == 0) {
            add(i);
        }
    }
    urgent(limit); // the round had no use for the rest
    // Freed ones still queued are dropped when reached; not counted.
    u32 waiting = 0;
    for (const u32 i : m_urgent) {
        waiting += scene.pool_brick(i, level, brick) ? 1u : 0u;
    }
    m_stats.lit = static_cast<u32>(m_jobs.size());
    m_stats.cleared = static_cast<u32>(m_clears.size());
    m_stats.waiting = waiting;
}

} // namespace giant::render
