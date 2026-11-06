#include "voxel_light.h"

#include "gpu_layout.h"
#include "voxel_scene.h"

#include <algorithm>

namespace giant::render {

VoxelLight::VoxelLight(u32 capacity)
    : m_capacity(capacity), m_queued(capacity, 0), m_stamp(capacity, 0) {}

VoxelLight::~VoxelLight() = default;

void VoxelLight::queue(u32 index) {
    if (index < m_capacity && m_queued[index] == 0) {
        m_queued[index] = 1;
        m_urgent.push_back(index);
    }
}

void VoxelLight::off() {
    m_fresh = true;
    m_clears.clear();
    m_jobs.clear();
    m_stats = {};
}

void VoxelLight::plan(const VoxelScene& scene, u32 budget) {
    m_clears.clear();
    m_jobs.clear();
    m_stats = {};
    ++m_frame;
    if (m_frame == 0) {
        std::fill(m_stamp.begin(), m_stamp.end(), 0u); // wrapped: no stamp may match by accident
        m_frame = 1;
    }
    const u32 capacity = std::min(m_capacity, scene.capacity());
    u32 level = 0;
    voxel::BrickCoord brick;
    const bool fresh = m_fresh;
    if (fresh) {
        m_fresh = false;
        m_urgent.clear();
        std::fill(m_queued.begin(), m_queued.end(), u8{0});
        for (u32 i = 0; i < capacity; ++i) {
            if (scene.pool_brick(i, level, brick)) {
                m_clears.push_back(i);
                queue(i);
            }
        }
    }
    if (!fresh) { // (fresh: every brick in the pool is cleared and queued already)
        for (const u32 i : scene.placed()) {
            if (i < capacity) {
                m_clears.push_back(i);
                queue(i);
            }
        }
    }
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
    while (m_jobs.size() < limit && !m_urgent.empty()) {
        const u32 i = m_urgent.front();
        m_urgent.pop_front();
        m_queued[i] = 0;
        add(i);
    }
    // The round: one look at each pool index at most per frame.
    for (u32 looked = 0; looked < capacity && m_jobs.size() < limit; ++looked) {
        const u32 i = m_cursor;
        m_cursor = m_cursor + 1 < capacity ? m_cursor + 1 : 0;
        if (m_queued[i] == 0) {
            add(i);
        }
    }
    // Duplicates in the clears (placed twice in a frame) are harmless.
    m_stats.lit = static_cast<u32>(m_jobs.size());
    m_stats.cleared = static_cast<u32>(m_clears.size());
    m_stats.waiting = static_cast<u32>(m_urgent.size());
}

} // namespace giant::render
