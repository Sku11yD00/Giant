#include "lights.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace giant::render {

namespace {

constexpr float k_pi = 3.14159265358979f;
constexpr float k_max_outer = 89.0f * k_pi / 180.0f;
constexpr float k_max_light = 1e9f; // colour x intensity, per channel

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float clamped_range(float range) { return std::isfinite(range) ? std::clamp(range, 0.01f, 1000.0f) : 0.01f; }

Vec3 spot_direction(const LocalLight& light) {
    const Vec3 d = light.direction;
    const float len = finite(d) ? length(d) : 0.0f;
    return len > 1e-6f ? d * (1.0f / len) : Vec3{0.0f, -1.0f, 0.0f};
}

float outer_angle(const LocalLight& light) {
    return std::isfinite(light.outer_angle) ? std::clamp(light.outer_angle, 0.001f, k_max_outer) : k_max_outer;
}

// The light a lamp of unit power (range, source radius) sheds on the
// ground k_ground_drop below it, summed over the ground (the shading's
// falloff, lights.slang: window^2 / (d^2 + source^2), times the cosine).
// Merged lamps are made as bright as to light the ground as much.
constexpr float k_ground_drop = 1.5f; // metres: a lamp's height over what it lights
float ground_light(float range, float source) {
    const float h = std::min(k_ground_drop, range * 0.5f);
    const float out = std::sqrt(std::max(range * range - h * h, 0.0f));
    constexpr int k_steps = 48;
    const float step = out / static_cast<float>(k_steps);
    float sum = 0.0f;
    for (int k = 0; k < k_steps; ++k) {
        const float rho = (static_cast<float>(k) + 0.5f) * step;
        const float d2 = rho * rho + h * h;
        const float q = d2 / (range * range);
        const float window = std::clamp(1.0f - q * q, 0.0f, 1.0f);
        sum += 2.0f * k_pi * rho * step * window * window / (d2 + source * source) * (h / std::sqrt(d2));
    }
    return sum;
}

// The share of a point light's light on the ground a spot sends: its cone
// (cone^2 in the shading, ramping in cos from outer to inner) over the
// sphere, all of it landing near, seen from far.
float spot_share(const LocalLight& l) {
    if (l.kind != LightKind::spot) return 1.0f;
    const float outer = outer_angle(l);
    const float inner = std::isfinite(l.inner_angle) ? std::clamp(l.inner_angle, 0.0f, outer) : 0.0f;
    return 1.0f - (2.0f * std::cos(inner) + std::cos(outer)) / 3.0f;
}

} // namespace

float light_slice_scale() {
    return static_cast<float>(k_light_slices) / std::log2(k_light_far / k_light_near);
}

u32 light_slice(float depth) {
    if (!(depth > k_light_near)) {
        return 0;
    }
    const float s = std::floor(std::log2(depth / k_light_near) * light_slice_scale());
    return s >= static_cast<float>(k_light_slices) ? k_light_slices : static_cast<u32>(s);
}

gpu::Light to_gpu(const LocalLight& light, const DVec3& camera) {
    gpu::Light out{};
    const Vec3 t = to_camera_relative(light.position, camera);
    out.position[0] = t.x;
    out.position[1] = t.y;
    out.position[2] = t.z;
    out.range = clamped_range(light.range);
    const float intensity = std::isfinite(light.intensity) ? std::max(light.intensity, 0.0f) : 0.0f;
    const Vec3 c = finite(light.color) ? light.color : Vec3{};
    // (Kept finite: an overflow would turn the pixel's light to NaN.)
    out.color[0] = std::min(std::max(c.x, 0.0f) * intensity, k_max_light);
    out.color[1] = std::min(std::max(c.y, 0.0f) * intensity, k_max_light);
    out.color[2] = std::min(std::max(c.z, 0.0f) * intensity, k_max_light);
    out.source_radius =
        std::isfinite(light.source_radius) ? std::clamp(light.source_radius, 0.0f, out.range * 0.5f) : 0.0f;
    if (light.kind == LightKind::spot) {
        const Vec3 d = spot_direction(light);
        out.direction[0] = d.x;
        out.direction[1] = d.y;
        out.direction[2] = d.z;
        const float outer = outer_angle(light);
        const float inner = std::isfinite(light.inner_angle) ? std::clamp(light.inner_angle, 0.0f, outer) : 0.0f;
        const float cos_outer = std::cos(outer);
        // (A relative floor: the narrowest cones still reach full light.)
        const float scale = 1.0f / std::max(std::cos(inner) - cos_outer, 1e-4f * (1.0f - cos_outer));
        out.spot_scale = scale;
        out.spot_offset = -cos_outer * scale;
    } else {
        out.direction[1] = -1.0f;
        out.spot_scale = 0.0f;
        out.spot_offset = 1.0f;
    }
    return out;
}

void light_bounds(const LocalLight& light, const DVec3& camera, Vec3& center, float& radius) {
    const Vec3 t = to_camera_relative(light.position, camera);
    const float range = clamped_range(light.range);
    center = t;
    radius = range;
    if (light.kind != LightKind::spot) {
        return;
    }
    // The sphere around the cone out to `range` (and its round cap).
    const Vec3 d = spot_direction(light);
    const float angle = outer_angle(light);
    if (angle <= k_pi * 0.25f) {
        radius = range / (2.0f * std::cos(angle));
        center = t + d * radius;
    } else {
        radius = range * std::sin(angle);
        center = t + d * (range * std::cos(angle));
    }
}

void bin_lights(std::span<const LocalLight> lights, const View& view, float aspect,
                const geometry::Frustum& frustum, LightBins& out) {
    out.lights.clear();
    out.sources.clear();
    out.entries.clear();
    out.overflow = false;
    out.lamps = 0;
    out.merged = 0;
    out.clusters.assign(usize{k_light_clusters} * 2, 0);
    if (lights.empty() || !(aspect > 0.0f)) {
        return;
    }
    const Quat q = normalize(view.orientation); // (as view_from_orientation)
    const Vec3 right = rotate(q, {1.0f, 0.0f, 0.0f});
    const Vec3 up = rotate(q, {0.0f, 1.0f, 0.0f});
    const Vec3 forward = rotate(q, {0.0f, 0.0f, -1.0f});
    const float fov = degrees_to_radians(std::clamp(view.vertical_fov_degrees, 1.0f, 170.0f));
    const float p11 = 1.0f / std::tan(fov * 0.5f);
    const float p00 = p11 / aspect;
    const float near_plane = std::max(view.near_plane, 1e-4f);

    struct Candidate {
        u32 index; // in the frame's lights, or (merged) in `merged`
        float key; // distance to the light's sphere (0 inside)
        Vec3 center;
        float radius;
        bool merged = false;
        u32 lamps = 1; // the lamps it stands for
    };
    std::vector<Candidate> candidates;
    // Far lamps by merge cell: (level, cell) -> their group.
    struct Group {
        std::vector<u32> lamps; // indices in the frame's lights
    };
    std::vector<Group> groups;
    std::unordered_map<u64, u32> group_of;
    // In the frustum and not wholly past the far distance.
    const auto seen = [&](const Vec3& c, float r) {
        return geometry::sphere_in_frustum(frustum, c, r) && dot(c, forward) - r <= k_light_far;
    };
    for (u32 i = 0; i < lights.size(); ++i) {
        const LocalLight& l = lights[i];
        if (!std::isfinite(l.position.x + l.position.y + l.position.z) || !(l.intensity > 0.0f) || !std::isfinite(l.intensity) ||
            !finite(l.color) || !(std::max({l.color.x, l.color.y, l.color.z}) > 0.0f)) {
            continue;
        }
        Vec3 c;
        float r;
        light_bounds(l, view.position, c, r);
        r += r * 1e-3f + 1e-3f; // (the light itself can sit on the sphere: GPU rounding must not lose it)
        if (!finite(c)) {
            continue;
        }
        const float key = std::max(length(c) - r, 0.0f);
        if (key >= k_light_merge_distance) {
            // A far lamp small against its merge cell joins the cell's
            // group, in view or not (by distance alone: turning the view
            // must not change a group; the group is tested for view after).
            const float cell = std::exp2(std::floor(std::log2(length(c) * k_light_merge_angle)));
            if (r <= cell * 0.5f) {
                const auto at = [&](double v) {
                    return static_cast<u64>(static_cast<i64>(std::floor(v / double(cell)))) & 0x7ffffull;
                };
                const u64 level = static_cast<u64>(std::log2(cell)) & 0x3full;
                const u64 k = level | (at(l.position.x) << 6) | (at(l.position.y) << 25) | (at(l.position.z) << 44);
                const auto [it, added] = group_of.try_emplace(k, static_cast<u32>(groups.size()));
                if (added) groups.emplace_back();
                groups[it->second].lamps.push_back(i);
                continue;
            }
        }
        if (seen(c, r)) {
            candidates.push_back({i, key, c, r});
        }
    }
    // Each group: one lamp alone stays itself; more are summed into one.
    std::vector<LocalLight> merged;
    std::vector<double> weights; // a group's lamps' powers (brightest channel)
    for (const Group& g : groups) {
        if (g.lamps.size() == 1) {
            const LocalLight& l = lights[g.lamps[0]];
            Vec3 c;
            float r;
            light_bounds(l, view.position, c, r);
            r += r * 1e-3f + 1e-3f;
            if (seen(c, r)) candidates.push_back({g.lamps[0], std::max(length(c) - r, 0.0f), c, r});
            continue;
        }
        Vec3 total{};
        DVec3 centre{};
        double weight = 0.0;
        double ground = 0.0; // what they all light of the ground (power x ground_light)
        weights.clear();
        for (const u32 i : g.lamps) {
            const LocalLight& l = lights[i];
            const float power = std::max(l.intensity, 0.0f) * spot_share(l);
            const Vec3 c{std::min(std::max(l.color.x, 0.0f) * power, k_max_light),
                         std::min(std::max(l.color.y, 0.0f) * power, k_max_light),
                         std::min(std::max(l.color.z, 0.0f) * power, k_max_light)};
            total = total + c;
            const double w = std::max({c.x, c.y, c.z});
            weights.push_back(w);
            centre = centre + l.position * w;
            weight += w;
            const float range = clamped_range(l.range);
            const float source = std::isfinite(l.source_radius) ? std::clamp(l.source_radius, 0.0f, range * 0.5f) : 0.0f;
            ground += w * double(ground_light(range, source));
        }
        const float peak = std::max({total.x, total.y, total.z});
        if (!(weight > 0.0) || !(peak > 0.0f) || !std::isfinite(peak)) continue;
        centre = centre * (1.0 / weight);
        double reach = 0.0;
        double spread = 0.0; // power-weighted mean squared distance from the centre
        for (usize k = 0; k < g.lamps.size(); ++k) {
            const LocalLight& l = lights[g.lamps[k]];
            const double d = length(l.position - centre);
            reach = std::max(reach, d + double(clamped_range(l.range)));
            spread += d * d * weights[k];
        }
        LocalLight m;
        m.position = centre;
        m.kind = LightKind::point;
        m.range = clamped_range(static_cast<float>(reach));
        m.source_radius = std::min(static_cast<float>(std::sqrt(std::max(spread / weight, 0.0))), m.range * 0.5f);
        // As bright as lights the ground as much as they all did: spread
        // wider and softer, the same light in all.
        const double own = double(ground_light(m.range, m.source_radius));
        const double scale = own > 0.0 ? ground / (weight * own) : 0.0;
        m.intensity = static_cast<float>(std::min(double(peak) * scale, double(k_max_light)));
        m.color = total * (1.0f / peak);
        if (!(m.intensity > 0.0f) || !std::isfinite(m.intensity)) continue;
        Vec3 c;
        float r;
        light_bounds(m, view.position, c, r);
        r += r * 1e-3f + 1e-3f;
        if (!finite(c) || !seen(c, r)) continue;
        candidates.push_back({static_cast<u32>(merged.size()), std::max(length(c) - r, 0.0f), c, r, true,
                              static_cast<u32>(g.lamps.size())});
        merged.push_back(m);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    if (candidates.size() > k_max_lights) {
        candidates.resize(k_max_lights);
        out.overflow = true;
    }

    // The clusters each light may touch: tiles x0..x1, y0..y1, slices z0..z1 (inclusive).
    struct Box {
        u32 x0, x1, y0, y1, z0, z1;
    };
    std::vector<Box> boxes;
    boxes.reserve(candidates.size());
    const auto tile = [](float ndc01, u32 count) {
        const float v = std::floor(ndc01 * static_cast<float>(count));
        return static_cast<u32>(std::clamp(v, 0.0f, static_cast<float>(count - 1)));
    };
    for (const Candidate& c : candidates) {
        const float depth = dot(c.center, forward);
        const float d0 = depth - c.radius;
        const float d1 = depth + c.radius;
        Box b{0, k_light_columns - 1, 0, k_light_rows - 1, 0, 0};
        if (d0 > near_plane) {
            // The corners of the sphere's view-space box, projected.
            const float cx = dot(c.center, right);
            const float cy = dot(c.center, up);
            const float x0 = p00 * std::min((cx - c.radius) / d0, (cx - c.radius) / d1);
            const float x1 = p00 * std::max((cx + c.radius) / d0, (cx + c.radius) / d1);
            const float y0 = p11 * std::min((cy - c.radius) / d0, (cy - c.radius) / d1);
            const float y1 = p11 * std::max((cy + c.radius) / d0, (cy + c.radius) / d1);
            if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) {
                continue; // off screen
            }
            b.x0 = tile(x0 * 0.5f + 0.5f, k_light_columns);
            b.x1 = tile(x1 * 0.5f + 0.5f, k_light_columns);
            b.y0 = tile(0.5f - y1 * 0.5f, k_light_rows); // screen rows run down
            b.y1 = tile(0.5f - y0 * 0.5f, k_light_rows);
        }
        b.z0 = std::min(light_slice(d0), k_light_slices - 1);
        b.z1 = std::min(light_slice(d1), k_light_slices - 1);
        boxes.push_back(b);
        out.lights.push_back(to_gpu(c.merged ? merged[c.index] : lights[c.index], view.position));
        out.sources.push_back(c.merged ? k_merged_light : c.index);
        out.merged += c.merged ? 1u : 0u;
        out.lamps += c.lamps;
    }

    // Count per cluster (the first k_max_lights_per_cluster in order),
    // then place each light's index in its clusters in the same order.
    const auto each_cluster = [](const Box& b, auto&& fn) {
        for (u32 z = b.z0; z <= b.z1; ++z) {
            for (u32 y = b.y0; y <= b.y1; ++y) {
                for (u32 x = b.x0; x <= b.x1; ++x) {
                    fn(light_cluster(x, y, z));
                }
            }
        }
    };
    for (const Box& b : boxes) {
        each_cluster(b, [&](u32 cluster) {
            u32& count = out.clusters[usize{cluster} * 2 + 1];
            if (count < k_max_lights_per_cluster) {
                ++count;
            } else {
                out.overflow = true;
            }
        });
    }
    u32 running = 0;
    for (usize i = 0; i < out.clusters.size(); i += 2) {
        out.clusters[i] = running;
        running += out.clusters[i + 1];
    }
    out.entries.resize(running);
    std::vector<u32> filled(k_light_clusters, 0);
    for (u32 li = 0; li < boxes.size(); ++li) {
        each_cluster(boxes[li], [&](u32 cluster) {
            u32& n = filled[cluster];
            if (n < out.clusters[usize{cluster} * 2 + 1]) {
                out.entries[out.clusters[usize{cluster} * 2] + n++] = li;
            }
        });
    }
}

} // namespace giant::render
