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
// touches it, at most k_max_voxel_lamps, the brightest there first; a
// coarse brick (voxels k_voxel_lamp_merge or more across) with more takes
// the small lamps inside each eighth of it merged into one (summed, at
// their centre, reaching as far as they all do; a spot as the point light
// sending as much light), so a town far off lights it whole. Lamps
// are found through grids of cells k_voxel_lamp_cell across and doubling
// to k_voxel_lamp_grids sizes: a brick looks in the finest whose cells are
// at least its size (a few cells), a lamp too big for a grid's cells is
// looked at by every brick using that grid.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/voxel/clipmap.h"
#include "gpu_layout.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace giant::render {

inline constexpr u32 k_max_voxel_lamps = 8;        // per brick
inline constexpr double k_voxel_lamp_cell = 32.0;  // metres: the finest grid's cells
inline constexpr u32 k_voxel_lamp_grids = 7;       // 32 m to 2048 m cells
inline constexpr double k_voxel_lamp_merge = 2.0;  // metres: voxels this big merge lamps

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
    u32 lit() const { return m_listed; }     // lamps lighting some brick this frame (listed or merged)
    u32 merged() const { return m_merged; }  // merged lamps made this frame

private:
    struct Lamp {
        u32 source;   // index in the frame's lights
        DVec3 center; // world, its bounding sphere
        double radius;
        float power;  // brightest channel x intensity
        // For merging (worked out once a frame): the light it sends (a
        // spot's as the point light sending as much), where, how far.
        Vec3 sent;
        DVec3 position;
        float range;
        u32 gpu = ~0u; // index in m_lights this frame
        u64 stamp = 0;
        bool counted = false; // in lit() this frame (listed or merged)
    };
    struct Pick {
        u32 lamp;
        double score;
    };
    struct Item {
        u32 at;     // a lamp, or an eighth of the brick (part)
        double score;
        bool part;
    };
    u32 record(u32 lamp, std::span<const LocalLight> lights, const DVec3& camera); // its index in m_lights
    void count(Lamp& lamp);
    void merge(gpu::LightJob& j, std::span<const Pick> picks, std::span<const LocalLight> lights, const DVec3& lo,
               double size, const DVec3& camera);
    std::vector<Item> m_items;
    std::vector<Lamp> m_lamps;
    std::vector<Pick> m_picks;
    std::array<std::vector<u32>, 8> m_parts; // a coarse brick's lamps by eighth
    u32 m_listed = 0;
    u32 m_merged = 0;
    u32 m_filled = 0; // grids holding lamps (since the last list)
    std::array<std::vector<u32>, k_voxel_lamp_grids> m_big; // per grid: lamps too big for its cells
    std::array<std::unordered_map<u64, std::vector<u32>>, k_voxel_lamp_grids> m_grid;
    std::vector<gpu::LightJob> m_jobs;
    std::vector<gpu::Light> m_lights;
    std::vector<u32> m_entries;
    u64 m_stamp = 0;
};

} // namespace giant::render
