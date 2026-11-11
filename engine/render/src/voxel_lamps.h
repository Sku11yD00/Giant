// Local lights in the voxel world's light (lights part L4): which lamps
// light each voxel brick lit this frame (src/voxel_light.h), so
// voxel_light.slang can add their light to the brick's voxels (with a march
// towards each for its shadow). What a voxel gets from lamps is kept in its
// own word beside its light word: it is what the voxel sends on (bounce,
// reflections, the far field and its distant towns glowing at night), not
// light the pixels near it take again (they are lit by the lamps
// themselves).
//
// A brick takes the lamps whose bounding sphere (a spot's round its cone)
// touches it, at most k_max_voxel_lamps, the brightest there first. Lamps
// are found through a grid of k_voxel_lamp_cell cells (a lamp too big for
// it, or a brick too big for it, looks at all).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/voxel/clipmap.h"
#include "gpu_layout.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace giant::render {

inline constexpr u32 k_max_voxel_lamps = 8;        // per brick
inline constexpr double k_voxel_lamp_cell = 32.0;  // metres

class VoxelLamps {
public:
    // This frame's lists: the jobs copied into jobs() with lamp_first /
    // lamp_count set (into entries()), their lamps' GPU records in lights()
    // (camera-relative).
    void list(std::span<const LocalLight> lights, std::span<const gpu::LightJob> jobs, const voxel::Clipmap& clipmap,
              const DVec3& camera);
    // Lit without lamps this time (their lists could not be uploaded).
    void drop();

    std::vector<gpu::LightJob>& jobs() { return m_jobs; }
    const std::vector<gpu::Light>& lights() const { return m_lights; }
    const std::vector<u32>& entries() const { return m_entries; }
    u32 lit() const { return static_cast<u32>(m_lights.size()); } // lamps reaching some brick this frame

private:
    struct Lamp {
        u32 source;   // index in the frame's lights
        DVec3 center; // world, its bounding sphere
        double radius;
        float power;  // brightest channel x intensity
        u32 gpu = ~0u; // index in m_lights this frame
        u64 stamp = 0;
    };
    std::vector<Lamp> m_lamps;
    std::vector<u32> m_big; // lamps too big for the grid
    std::unordered_map<u64, std::vector<u32>> m_grid;
    std::vector<gpu::LightJob> m_jobs;
    std::vector<gpu::Light> m_lights;
    std::vector<u32> m_entries;
    u64 m_stamp = 0;
};

} // namespace giant::render
