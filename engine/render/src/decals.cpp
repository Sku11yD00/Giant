#include "decals.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

namespace {

void set4(float* out, const Vec3& v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

float positive(float v) { return std::fabs(v) < 1e-6f ? 1e-6f : std::fabs(v); }

} // namespace

gpu::Decal to_gpu(const Decal& d, const DVec3& camera) {
    gpu::Decal out{};
    const Vec3 t = to_camera_relative(d.position, camera);
    const Vec3 axes[3] = {rotate(d.rotation, {1, 0, 0}), rotate(d.rotation, {0, 1, 0}),
                          rotate(d.rotation, {0, 0, 1})};
    const float size[3] = {positive(d.size.x), positive(d.size.y), positive(d.size.z)};
    // local = S^-1 R^-1 (p - t): row i is axis i over size i.
    for (int i = 0; i < 3; ++i) {
        const Vec3 r = axes[i] * (1.0f / size[i]);
        set4(out.rows + i * 4, r, -dot(r, t));
    }
    set4(out.color_opacity, d.base_color, std::clamp(d.opacity, 0.0f, 1.0f));
    set4(out.emissive_roughness, d.emissive, std::clamp(d.roughness, 0.0f, 1.0f));
    out.metallic = std::clamp(d.metallic, 0.0f, 1.0f);
    out.softness = std::clamp(d.softness, 0.0f, 1.0f);
    out.shape = std::min(static_cast<u32>(d.shape), static_cast<u32>(DecalShape::cracks));
    out.seed = d.seed;
    return out;
}

void bin_decals(std::span<const Decal> decals, const DVec3& camera, const Mat4& view_proj,
                const geometry::Frustum& frustum, u32 width, u32 height, DecalBins& out) {
    out.decals.clear();
    out.entries.clear();
    out.overflow = false;
    out.columns = (width + k_decal_tile - 1) / k_decal_tile;
    out.rows = (height + k_decal_tile - 1) / k_decal_tile;
    out.tiles.assign(usize{out.columns} * out.rows * 2, 0);
    if (decals.empty() || out.columns == 0 || out.rows == 0) {
        return;
    }
    // Tile rectangles of the decals in view: x0, y0, x1, y1 (exclusive).
    struct Rect {
        u32 x0, y0, x1, y1;
    };
    std::vector<Rect> rects;
    u64 entries = 0;
    for (const Decal& d : decals) {
        const Vec3 t = to_camera_relative(d.position, camera);
        const Vec3 half = {std::fabs(d.size.x) * 0.5f, std::fabs(d.size.y) * 0.5f, std::fabs(d.size.z) * 0.5f};
        const float radius = length(half);
        if (!std::isfinite(radius) || !std::isfinite(t.x + t.y + t.z) ||
            !geometry::sphere_in_frustum(frustum, t, radius)) {
            continue;
        }
        const Vec3 axes[3] = {rotate(d.rotation, {half.x, 0, 0}), rotate(d.rotation, {0, half.y, 0}),
                              rotate(d.rotation, {0, 0, half.z})};
        float x0 = static_cast<float>(width), y0 = static_cast<float>(height), x1 = 0.0f, y1 = 0.0f;
        bool behind = false;
        for (u32 c = 0; c < 8 && !behind; ++c) {
            const Vec3 p = t + axes[0] * ((c & 1) ? 1.0f : -1.0f) + axes[1] * ((c & 2) ? 1.0f : -1.0f) +
                           axes[2] * ((c & 4) ? 1.0f : -1.0f);
            const Vec4 clip = view_proj * Vec4{p.x, p.y, p.z, 1.0f};
            if (!(clip.w > 1e-6f)) {
                behind = true; // reaches behind the camera: take the whole screen
                break;
            }
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width);
            const float sy = (0.5f - clip.y / clip.w * 0.5f) * static_cast<float>(height);
            x0 = std::min(x0, sx);
            y0 = std::min(y0, sy);
            x1 = std::max(x1, sx);
            y1 = std::max(y1, sy);
        }
        Rect r{0, 0, out.columns, out.rows};
        if (!behind) {
            const auto tile = [](float v, u32 limit) {
                const float clamped = std::clamp(v, 0.0f, static_cast<float>(limit));
                return std::min(static_cast<u32>(clamped), limit);
            };
            r.x0 = tile(std::floor(x0) / k_decal_tile, out.columns);
            r.y0 = tile(std::floor(y0) / k_decal_tile, out.rows);
            r.x1 = tile(std::floor(x1) / k_decal_tile + 1.0f, out.columns);
            r.y1 = tile(std::floor(y1) / k_decal_tile + 1.0f, out.rows);
            if (r.x0 >= r.x1 || r.y0 >= r.y1) {
                continue; // off screen
            }
        }
        const u64 covered = u64{r.x1 - r.x0} * (r.y1 - r.y0);
        if (out.decals.size() >= k_max_decals || entries + covered > k_max_decal_entries) {
            out.overflow = true;
            continue; // a smaller one further on may still fit
        }
        entries += covered;
        rects.push_back(r);
        out.decals.push_back(to_gpu(d, camera));
    }
    // Count per tile, then place each decal's index in its tiles, in order.
    for (const Rect& r : rects) {
        for (u32 y = r.y0; y < r.y1; ++y) {
            for (u32 x = r.x0; x < r.x1; ++x) {
                ++out.tiles[(usize{y} * out.columns + x) * 2 + 1];
            }
        }
    }
    u32 running = 0;
    for (usize i = 0; i < out.tiles.size(); i += 2) {
        out.tiles[i] = running;
        running += out.tiles[i + 1];
        out.tiles[i + 1] = 0; // refilled below
    }
    out.entries.resize(running);
    for (u32 di = 0; di < rects.size(); ++di) {
        const Rect& r = rects[di];
        for (u32 y = r.y0; y < r.y1; ++y) {
            for (u32 x = r.x0; x < r.x1; ++x) {
                const usize at = (usize{y} * out.columns + x) * 2;
                out.entries[out.tiles[at] + out.tiles[at + 1]++] = di;
            }
        }
    }
}

} // namespace giant::render
