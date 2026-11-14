#include "skin_lod.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

float pose_reach(std::span<const float> matrices, std::span<const float> joint_spheres) {
    constexpr float k_huge = 1e30f;
    const usize joints = std::min(matrices.size() / 12, joint_spheres.size() / 4);
    float reach = 0.0f;
    for (usize j = 0; j < joints; ++j) {
        const float* s = joint_spheres.data() + j * 4;
        if (!(s[3] >= 0.0f)) continue; // moves no vertex
        const float* m = matrices.data() + j * 12;
        float moved2 = 0.0f; // |(A - I) c + t|^2
        float norm2 = 0.0f;  // |A - I|_F^2
        for (usize r = 0; r < 3; ++r) {
            float row = m[r * 4 + 3];
            for (usize c = 0; c < 3; ++c) {
                const float a = m[r * 4 + c] - (r == c ? 1.0f : 0.0f);
                row += a * s[c];
                norm2 += a * a;
            }
            moved2 += row * row;
        }
        const float moved = std::sqrt(moved2) + std::sqrt(norm2) * s[3];
        if (!(moved < k_huge)) {
            return k_huge; // not finite (or absurd)
        }
        reach = std::max(reach, moved);
    }
    // A hair over, for rounding: the cut must never be finer than it shows.
    return reach * 1.0001f + 1e-6f;
}

float morph_reach(std::span<const float> matrices, std::span<const float> joint_spheres,
                  std::span<const float> weights, std::span<const float> morph_max) {
    constexpr float k_huge = 1e30f;
    float moved = 0.0f; // sum(|weight| * morph_max)
    for (usize t = 0; t < std::min(weights.size(), morph_max.size()); ++t) {
        moved += std::fabs(weights[t]) * morph_max[t];
    }
    if (moved == 0.0f) return 0.0f;
    float scale2 = 0.0f; // the largest |A|_1 |A|_inf (over |A|_2^2)
    const usize joints = std::min(matrices.size() / 12, joint_spheres.size() / 4);
    for (usize j = 0; j < joints; ++j) {
        if (!(joint_spheres[j * 4 + 3] >= 0.0f)) continue; // moves no vertex
        const float* m = matrices.data() + j * 12;
        float rows = 0.0f, columns = 0.0f;
        for (usize i = 0; i < 3; ++i) {
            rows = std::max(rows, std::fabs(m[i * 4]) + std::fabs(m[i * 4 + 1]) + std::fabs(m[i * 4 + 2]));
            columns = std::max(columns, std::fabs(m[i]) + std::fabs(m[4 + i]) + std::fabs(m[8 + i]));
        }
        scale2 = std::max(scale2, rows * columns);
    }
    const float reach = moved * std::sqrt(scale2);
    if (!(reach < k_huge)) return k_huge;
    return reach * 1.0001f + 1e-6f; // (a hair over, for rounding)
}

std::vector<geometry::LodBounds> depth_bounds(const geometry::ClusterMesh& mesh) {
    std::vector<geometry::LodBounds> out(mesh.levels, geometry::LodBounds{{}, 0.0f, -1.0f});
    for (const geometry::ClusterGroup& g : mesh.groups) {
        if (geometry::is_root_group(g) || g.depth >= out.size()) continue;
        geometry::LodBounds& b = out[g.depth];
        const geometry::LodBounds& s = g.simplified;
        if (b.error < 0.0f) {
            b = s;
            continue;
        }
        // The smallest sphere around both.
        const Vec3 d = s.center - b.center;
        const float dist = length(d);
        if (dist + s.radius > b.radius) {
            if (dist + b.radius <= s.radius) {
                b.center = s.center;
                b.radius = s.radius;
            } else {
                const float r = (dist + b.radius + s.radius) * 0.5f;
                b.center = b.center + d * ((r - b.radius) / dist);
                b.radius = r;
            }
        }
        b.error = std::max(b.error, s.error);
    }
    for (geometry::LodBounds& b : out) {
        b.radius = b.radius * 1.0001f + 1e-6f; // a hair over, for rounding
    }
    return out;
}

u32 finest_depth(std::span<const geometry::LodBounds> depths, const geometry::LodView& view) {
    for (u32 d = 0; d < depths.size(); ++d) {
        if (depths[d].error >= 0.0f && geometry::projected_error(depths[d], view) > view.threshold_pixels) {
            return d;
        }
    }
    return depths.empty() ? 0 : static_cast<u32>(depths.size()) - 1;
}

} // namespace giant::render
