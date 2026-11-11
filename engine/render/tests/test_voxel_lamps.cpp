// Lamps in the voxel world (src/voxel_lamps.h): which lamps each voxel
// brick lit this frame takes.
#include "voxel_lamps.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

LocalLight lamp(const DVec3& p, float range, float intensity = 3.0f) {
    LocalLight l;
    l.position = p;
    l.range = range;
    l.intensity = intensity;
    return l;
}

// A job for the brick holding `p` on `level`.
gpu::LightJob job_at(const voxel::Clipmap& c, u32 level, const DVec3& p) {
    const voxel::BrickCoord b = c.brick_at(level, p);
    gpu::LightJob j{};
    j.index = 7;
    j.level = level;
    j.brick[0] = b.x;
    j.brick[1] = b.y;
    j.brick[2] = b.z;
    return j;
}

std::vector<u32> sources(const VoxelLamps& v, const gpu::LightJob& j, const std::vector<LocalLight>& lights,
                         const DVec3& camera) {
    // The lamps listed, as indices into `lights` (matched by position).
    std::vector<u32> out;
    for (u32 k = 0; k < j.lamp_count; ++k) {
        const gpu::Light& g = v.lights()[v.entries()[j.lamp_first + k]];
        for (u32 i = 0; i < lights.size(); ++i) {
            const Vec3 rel = to_camera_relative(lights[i].position, camera);
            if (std::fabs(rel.x - g.position[0]) < 1e-3f && std::fabs(rel.y - g.position[1]) < 1e-3f &&
                std::fabs(rel.z - g.position[2]) < 1e-3f) {
                out.push_back(i);
            }
        }
    }
    return out;
}

} // namespace

GIANT_TEST("voxel lamps: a brick takes the lamps reaching it, camera-relative, and not those that do not") {
    const voxel::Clipmap clip;
    const DVec3 camera{5000.0, 10.0, -3000.0};
    std::vector<LocalLight> lights{lamp({5001.0, 1.0, -3001.0}, 4.0f), lamp({5040.0, 1.0, -3001.0}, 4.0f),
                                   lamp({5001.0, 1.0, -2950.0}, 100.0f)};
    lights.push_back(lamp({5001.5, 1.0, -3001.0}, 4.0f, 0.0f)); // dark
    lights.push_back(lamp({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 4.0f));
    std::vector<gpu::LightJob> jobs{job_at(clip, 0, {5001.0, 0.5, -3001.0}), job_at(clip, 0, {5040.0, 0.5, -3001.0}),
                                    job_at(clip, 0, {5020.0, 0.5, -3020.0})};
    VoxelLamps v;
    v.list(lights, jobs, clip, camera);
    GIANT_CHECK(v.jobs().size() == 3 && v.jobs()[0].index == 7);
    std::vector<u32> a = sources(v, v.jobs()[0], lights, camera);
    std::sort(a.begin(), a.end());
    GIANT_CHECK((a == std::vector<u32>{0, 2})); // its own, and the big one 50 m off
    GIANT_CHECK((sources(v, v.jobs()[1], lights, camera) == std::vector<u32>{2, 1}) ||
                (sources(v, v.jobs()[1], lights, camera) == std::vector<u32>{1, 2}));
    GIANT_CHECK((sources(v, v.jobs()[2], lights, camera) == std::vector<u32>{2}));
    GIANT_CHECK(v.lit() == 3); // each lamp's record once
    // A lamp's record is the renderer's (camera-relative).
    const gpu::Light& g = v.lights()[v.entries()[v.jobs()[2].lamp_first]];
    GIANT_CHECK(std::fabs(g.position[0] - 1.0f) < 1e-4f && std::fabs(g.range - 100.0f) < 1e-4f);
    // Nothing in reach: no lamps; no jobs: nothing.
    const std::vector<gpu::LightJob> far{job_at(clip, 0, {9000.0, 0.5, 0.0})};
    v.list(lights, far, clip, camera);
    GIANT_CHECK(v.jobs()[0].lamp_count == 0 && v.lit() == 0);
    v.list(lights, {}, clip, camera);
    GIANT_CHECK(v.jobs().empty() && v.lit() == 0);
}

GIANT_TEST("voxel lamps: a brick takes at most eight, the brightest there first") {
    const voxel::Clipmap clip;
    std::vector<LocalLight> lights;
    for (int i = 0; i < 20; ++i) lights.push_back(lamp({0.5 + 0.05 * i, 1.0, 0.5}, 10.0f, 1.0f + float(i)));
    const std::vector<gpu::LightJob> jobs{job_at(clip, 0, {0.5, 0.5, 0.5})};
    VoxelLamps v;
    v.list(lights, jobs, clip, {});
    GIANT_CHECK(v.jobs()[0].lamp_count == k_max_voxel_lamps);
    std::vector<u32> got = sources(v, v.jobs()[0], lights, {});
    std::sort(got.begin(), got.end());
    GIANT_CHECK((got == std::vector<u32>{12, 13, 14, 15, 16, 17, 18, 19}));
    v.drop();
    GIANT_CHECK(v.jobs()[0].lamp_count == 0 && v.lit() == 0);
}

GIANT_TEST("voxel lamps: coarse bricks and lamps bigger than the grid still find each other") {
    const voxel::Clipmap clip;
    std::vector<LocalLight> lights;
    // A town of lamps 2 km out, and one floodlight reaching 900 m.
    for (int i = 0; i < 50; ++i) lights.push_back(lamp({2000.0 + 7.0 * i, 3.0, 40.0 * (i % 5)}, 8.0f));
    lights.push_back(lamp({-500.0, 20.0, 0.0}, 900.0f, 50.0f));
    const std::vector<gpu::LightJob> jobs{job_at(clip, 9, {2100.0, 0.0, 50.0}), job_at(clip, 0, {-100.0, 0.5, 0.0})};
    VoxelLamps v;
    v.list(lights, jobs, clip, {});
    // The coarse brick holds 44 of the town's lamps (those from x = 2042 m
    // on): merged by eighths of it, all their light is there.
    const gpu::LightJob& coarse = v.jobs()[0];
    GIANT_CHECK(coarse.lamp_count >= 1 && coarse.lamp_count <= k_max_voxel_lamps && v.merged() >= 1);
    float total = 0.0f;
    for (u32 k = 0; k < coarse.lamp_count; ++k) {
        const gpu::Light& g = v.lights()[v.entries()[coarse.lamp_first + k]];
        total += std::max({g.color[0], g.color[1], g.color[2]});
        GIANT_CHECK(g.position[0] > 2040.0f && g.position[0] < 2350.0f && g.range >= 8.0f);
    }
    GIANT_CHECK(std::fabs(total - 44.0f * 3.0f) < 1e-2f);
    GIANT_CHECK((sources(v, v.jobs()[1], lights, {}) == std::vector<u32>{50}));
    GIANT_CHECK(v.lit() == 1); // the floodlight's own record (the town's merged)
    // The same again (grid cells kept from before): the same lists.
    const std::vector<u32> first = v.entries();
    v.list(lights, jobs, clip, {});
    GIANT_CHECK(v.entries().size() == first.size());
}
