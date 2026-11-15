#include "giant/geometry/groom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace giant::geometry {

namespace {

u32 hash(u32 v) {
    v = v * 747796405u + 2891336453u;
    const u32 word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (word >> 22u) ^ word;
}

// 0..1 (below 1) from a seed and a stream.
float unit(u32 seed, u32 stream) {
    return static_cast<float>(hash(seed ^ hash(stream)) >> 8u) * (1.0f / 16777216.0f);
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Finite and no further out than a groom reaches (so no sum of squares
// over a groom's points can overflow).
bool within_reach(const Vec3& v) {
    return std::abs(v.x) <= k_max_groom_reach && std::abs(v.y) <= k_max_groom_reach &&
           std::abs(v.z) <= k_max_groom_reach; // false for NaN
}

float fraction(double v) { return static_cast<float>(v - std::floor(v)); }

// Two unit vectors across `along` (unit).
void across(const Vec3& along, Vec3& a, Vec3& b) {
    const Vec3 other = std::abs(along.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    a = normalize_or_zero(cross(other, along));
    b = cross(along, a);
}

// The guides nearest a point, found through a grid of the guides' roots.
class GuideGrid {
public:
    GuideGrid(std::span<const Vec3> roots, const Vec3& low, const Vec3& high) : roots_(roots), low_(low) {
        // Roots lie on a surface (or along a line), not through a volume:
        // a cell is about the ground one guide owns on the box's widest
        // face, or its share of the longest side when the box is a sliver.
        const Vec3 size = high - low;
        std::array<float, 3> sides{size.x, size.y, size.z};
        std::sort(sides.begin(), sides.end());
        const float guides = static_cast<float>(std::max<usize>(roots.size(), 1));
        const float owned = std::max(std::sqrt(sides[2] * sides[1] / guides), sides[2] / guides);
        cell_ = std::isfinite(owned) ? std::max(owned, 1e-4f) : 1.0f;
        for (u32 i = 0; i < roots.size(); ++i) {
            cells_[key(coord(roots[i]))].push_back(i);
        }
    }

    // The nearest guides to `point` (up to k_strand_guides), nearest first,
    // with their squared distances; the count found.
    u32 nearest(const Vec3& point, std::array<u32, k_strand_guides>& guides,
                std::array<float, k_strand_guides>& distances) const {
        u32 found = 0;
        const auto offer = [&](u32 guide) {
            const float d = length_squared(roots_[guide] - point);
            u32 at = found;
            while (at > 0 && (d < distances[at - 1] || (d == distances[at - 1] && guide < guides[at - 1]))) --at;
            if (at >= k_strand_guides) return;
            for (u32 i = std::min(found, k_strand_guides - 1); i > at; --i) {
                guides[i] = guides[i - 1];
                distances[i] = distances[i - 1];
            }
            guides[at] = guide;
            distances[at] = d;
            found = std::min(found + 1, k_strand_guides);
        };
        const u32 wanted = static_cast<u32>(std::min<usize>(roots_.size(), k_strand_guides));
        const std::array<i32, 3> centre = coord(point);
        // Shell by shell: everything within `ring` cells is seen once the
        // ring is done, so the search ends when the farthest kept guide is
        // nearer than the next shell could be.
        constexpr i32 k_rings = 4;
        for (i32 ring = 0; ring <= k_rings; ++ring) {
            for (i32 z = -ring; z <= ring; ++z) {
                for (i32 y = -ring; y <= ring; ++y) {
                    for (i32 x = -ring; x <= ring; ++x) {
                        if (std::max({std::abs(x), std::abs(y), std::abs(z)}) != ring) continue;
                        const auto it = cells_.find(key({centre[0] + x, centre[1] + y, centre[2] + z}));
                        if (it == cells_.end()) continue;
                        for (u32 guide : it->second) offer(guide);
                    }
                }
            }
            const float reach = static_cast<float>(ring) * cell_;
            if (found == wanted && distances[found - 1] <= reach * reach) return found;
        }
        // Sparse here: look at them all.
        found = 0;
        for (u32 guide = 0; guide < roots_.size(); ++guide) offer(guide);
        return found;
    }

private:
    std::array<i32, 3> coord(const Vec3& p) const {
        const auto axis = [&](float v, float from) {
            const float cell = std::floor((v - from) / cell_);
            return std::isfinite(cell) ? static_cast<i32>(std::clamp(cell, -1.0e6f, 1.0e6f)) : 0;
        };
        return {axis(p.x, low_.x), axis(p.y, low_.y), axis(p.z, low_.z)};
    }
    static u64 key(const std::array<i32, 3>& c) {
        const auto part = [](i32 v) { return static_cast<u64>(static_cast<u32>(v + (1 << 20)) & 0x1fffffu); };
        return part(c[0]) | (part(c[1]) << 21u) | (part(c[2]) << 42u);
    }

    std::span<const Vec3> roots_;
    Vec3 low_;
    float cell_ = 1.0f;
    std::unordered_map<u64, std::vector<u32>> cells_;
};

void root_bounds(std::span<const Vec3> points, Vec3& low, Vec3& high) {
    low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    high = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
    for (usize i = 0; i < points.size(); i += k_strand_points) {
        const Vec3& p = points[i];
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
}

// Picks the guides (far apart, covering the roots), moves them to the front
// keeping the others' order, has every strand follow its nearest guides,
// draws strands in to their guide by `clump`, and measures the groom.
void finish(Groom& groom, u32 guides, float clump) {
    const u32 count = groom.strand_count();
    groom.follow.assign(count, StrandFollow{});
    groom.guide_count = 0;
    groom.bounds_min = {};
    groom.bounds_max = {};
    groom.longest = 0.0f;
    if (count == 0) return;
    const u32 wanted = std::min(std::clamp(guides, 1u, k_max_guides), count);
    // Farthest-point picks among the first strands (any prefix covers the
    // scalp, and looking at them all would take far too long on a big groom).
    const u32 pool = static_cast<u32>(std::min<u64>(count, static_cast<u64>(wanted) * 16u));
    std::vector<float> nearest(pool, std::numeric_limits<float>::max());
    std::vector<u8> is_guide(count, 0);
    u32 pick = 0;
    for (u32 g = 0; g < wanted; ++g) {
        is_guide[pick] = 1;
        const Vec3 root = groom.points[static_cast<usize>(pick) * k_strand_points];
        u32 next = 0;
        float farthest = -1.0f;
        for (u32 i = 0; i < pool; ++i) {
            nearest[i] = std::min(nearest[i], length_squared(groom.points[static_cast<usize>(i) * k_strand_points] - root));
            if (is_guide[i] == 0 && nearest[i] > farthest) {
                farthest = nearest[i];
                next = i;
            }
        }
        pick = next;
    }
    // Guides first, the rest as they were.
    std::vector<u32> order;
    order.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        if (is_guide[i] != 0) order.push_back(i);
    }
    for (u32 i = 0; i < count; ++i) {
        if (is_guide[i] == 0) order.push_back(i);
    }
    Groom sorted;
    sorted.points.resize(groom.points.size());
    sorted.root_normals.resize(count);
    sorted.seeds.resize(count);
    for (u32 i = 0; i < count; ++i) {
        std::copy_n(groom.points.begin() + static_cast<std::ptrdiff_t>(order[i]) * k_strand_points, k_strand_points,
                    sorted.points.begin() + static_cast<std::ptrdiff_t>(i) * k_strand_points);
        sorted.root_normals[i] = groom.root_normals[order[i]];
        sorted.seeds[i] = groom.seeds[order[i]];
    }
    groom.points = std::move(sorted.points);
    groom.root_normals = std::move(sorted.root_normals);
    groom.seeds = std::move(sorted.seeds);
    groom.guide_count = wanted;

    std::vector<Vec3> roots(wanted);
    for (u32 g = 0; g < wanted; ++g) roots[g] = groom.points[static_cast<usize>(g) * k_strand_points];
    Vec3 low;
    Vec3 high;
    root_bounds(std::span<const Vec3>(groom.points).first(static_cast<usize>(wanted) * k_strand_points), low, high);
    const GuideGrid grid(roots, low, high);
    for (u32 s = 0; s < count; ++s) {
        StrandFollow& f = groom.follow[s];
        if (s < wanted) {
            f.guides = {s, s, s};
            continue;
        }
        std::array<float, k_strand_guides> distances{};
        const u32 found = grid.nearest(groom.points[static_cast<usize>(s) * k_strand_points], f.guides, distances);
        // Inverse-square weights; a strand rooted on a guide follows it alone.
        float sum = 0.0f;
        for (u32 k = 0; k < k_strand_guides; ++k) {
            f.weights[k] = k < found ? 1.0f / (distances[k] + 1e-10f) : 0.0f;
            if (k >= found) f.guides[k] = f.guides[0];
            sum += f.weights[k];
        }
        if (sum > 0.0f && std::isfinite(sum)) {
            for (float& w : f.weights) w /= sum;
            f.weights[0] = 1.0f - f.weights[1] - f.weights[2]; // exactly 1 in all
        } else {
            f.weights = {1.0f, 0.0f, 0.0f}; // too far from any guide to weigh them
        }
    }
    const float drawn = std::clamp(clump, 0.0f, 1.0f);
    if (drawn > 0.0f) {
        for (u32 s = wanted; s < count; ++s) {
            const usize mine = static_cast<usize>(s) * k_strand_points;
            const usize guide = static_cast<usize>(groom.follow[s].guides[0]) * k_strand_points;
            for (u32 i = 1; i < k_strand_points; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(k_strand_points - 1);
                const Vec3 apart = groom.points[mine + i] - groom.points[guide + i];
                groom.points[mine + i] = groom.points[guide + i] + apart * (1.0f - drawn * t);
            }
        }
    }
    groom.bounds_min = groom.points[0];
    groom.bounds_max = groom.points[0];
    for (const Vec3& p : groom.points) {
        groom.bounds_min = {std::min(groom.bounds_min.x, p.x), std::min(groom.bounds_min.y, p.y),
                            std::min(groom.bounds_min.z, p.z)};
        groom.bounds_max = {std::max(groom.bounds_max.x, p.x), std::max(groom.bounds_max.y, p.y),
                            std::max(groom.bounds_max.z, p.z)};
    }
    for (u32 s = 0; s < count; ++s) {
        const std::span<const Vec3> strand = groom.strand(s);
        float along = 0.0f;
        for (u32 i = 1; i < k_strand_points; ++i) along += length(strand[i] - strand[i - 1]);
        groom.longest = std::max(groom.longest, along);
    }
}

} // namespace

std::string groom_problem(const Groom& groom) {
    const usize count = groom.seeds.size();
    if (count > k_max_strands) return "a groom holds at most " + std::to_string(k_max_strands) + " strands";
    if (groom.points.size() != count * k_strand_points) {
        return "a groom has " + std::to_string(k_strand_points) + " points for every strand";
    }
    if (groom.root_normals.size() != count || groom.follow.size() != count) {
        return "a groom has a root normal and guides to follow for every strand";
    }
    if (groom.guide_count > count || groom.guide_count > k_max_guides || (count > 0 && groom.guide_count == 0)) {
        return "a groom's guides are its first strands, at least one and at most " + std::to_string(k_max_guides);
    }
    for (const Vec3& p : groom.points) {
        if (!within_reach(p)) return "a groom's points are numbers within a thousand kilometres";
    }
    for (const Vec3& n : groom.root_normals) {
        if (!finite(n) || std::abs(length(n) - 1.0f) > 1e-3f) return "a groom's root normals are unit vectors";
    }
    for (usize s = 0; s < count; ++s) {
        const StrandFollow& f = groom.follow[s];
        float sum = 0.0f;
        for (u32 k = 0; k < k_strand_guides; ++k) {
            if (f.guides[k] >= groom.guide_count) return "a strand follows guides of its groom";
            if (!(f.weights[k] >= 0.0f && f.weights[k] <= 1.0f)) return "a strand's guide weights are 0 to 1";
            sum += f.weights[k];
        }
        if (std::abs(sum - 1.0f) > 1e-3f) return "a strand's guide weights sum to 1";
        if (s < groom.guide_count && (f.guides[0] != s || f.weights[0] != 1.0f)) return "a guide follows itself";
    }
    if (!finite(groom.bounds_min) || !finite(groom.bounds_max) || !std::isfinite(groom.longest)) {
        return "a groom's bounds are finite numbers";
    }
    return {};
}

Groom generate_groom(const TriangleMesh& scalp, const GroomParams& given) {
    Groom groom;
    const auto safe = [](float v, float low, float high, float otherwise) {
        return std::isfinite(v) ? std::clamp(v, low, high) : otherwise;
    };
    const u32 strands = std::clamp(given.strands, 1u, k_max_strands);
    const float length_m = safe(given.length, 1e-3f, 10.0f, 0.25f);
    const float variation = safe(given.length_variation, 0.0f, 1.0f, 0.0f);
    const float droop = safe(given.droop, 0.0f, 1.0f, 0.0f);
    const float curl_radius = safe(given.curl_radius, 0.0f, 1.0f, 0.0f);
    const float curl_turns = safe(given.curl_turns, 0.0f, 100.0f, 0.0f);
    const float frizz = safe(given.frizz, 0.0f, 1.0f, 0.0f);

    // Triangles by area, for roots spread evenly over the surface.
    const usize triangles = scalp.indices.size() / 3;
    std::vector<double> area_to(triangles);
    double area = 0.0;
    for (usize t = 0; t < triangles; ++t) {
        const u32 a = scalp.indices[t * 3];
        const u32 b = scalp.indices[t * 3 + 1];
        const u32 c = scalp.indices[t * 3 + 2];
        if (a < scalp.vertices.size() && b < scalp.vertices.size() && c < scalp.vertices.size()) {
            const Vec3 pa = scalp.vertices[a].position;
            const Vec3 pb = scalp.vertices[b].position;
            const Vec3 pc = scalp.vertices[c].position;
            const float twice = length(cross(pb - pa, pc - pa));
            // Points out of reach (or no numbers) grow nothing.
            if (within_reach(pa) && within_reach(pb) && within_reach(pc) && std::isfinite(twice)) {
                area += static_cast<double>(twice) * 0.5;
            }
        }
        area_to[t] = area;
    }
    if (!(area > 0.0)) return groom;

    groom.points.resize(static_cast<usize>(strands) * k_strand_points);
    groom.root_normals.resize(strands);
    groom.seeds.resize(strands);
    const Vec3 down{0.0f, -1.0f, 0.0f};
    const std::array<double, 3> turned{static_cast<double>(unit(given.seed, 100)), static_cast<double>(unit(given.seed, 101)),
                                       static_cast<double>(unit(given.seed, 102))};
    for (u32 s = 0; s < strands; ++s) {
        // A low-discrepancy point (the R3 sequence): any run of strands is
        // spread evenly, which thinning by strand count leans on.
        // The seed turns the whole pattern (another seed, other roots).
        const double n = static_cast<double>(s) + 1.0;
        const double u = static_cast<double>(fraction(n * 0.8191725133961645 + turned[0])) * area;
        float bu = fraction(n * 0.6710436067037893 + turned[1]);
        float bv = fraction(n * 0.5497004779019703 + turned[2]);
        if (bu + bv > 1.0f) {
            bu = 1.0f - bu;
            bv = 1.0f - bv;
        }
        usize t = static_cast<usize>(std::upper_bound(area_to.begin(), area_to.end(), u) - area_to.begin());
        t = std::min(t, triangles - 1);
        while (t > 0 && area_to[t] == area_to[t - 1]) --t; // never a triangle without area
        const Vertex& a = scalp.vertices[scalp.indices[t * 3]];
        const Vertex& b = scalp.vertices[scalp.indices[t * 3 + 1]];
        const Vertex& c = scalp.vertices[scalp.indices[t * 3 + 2]];
        const Vec3 root = a.position + (b.position - a.position) * bu + (c.position - a.position) * bv;
        Vec3 normal = normalize_or_zero(a.normal + (b.normal - a.normal) * bu + (c.normal - a.normal) * bv);
        if (length_squared(normal) == 0.0f || !finite(normal)) {
            normal = normalize_or_zero(cross(b.position - a.position, c.position - a.position));
        }
        if (length_squared(normal) == 0.0f || !finite(normal)) normal = {0.0f, 1.0f, 0.0f};

        const u32 seed = hash(given.seed ^ hash(s + 0x9e3779b9u));
        groom.seeds[s] = seed;
        groom.root_normals[s] = normal;
        const float strand_length = length_m * (1.0f - variation * unit(seed, 0));
        const float step = strand_length / static_cast<float>(k_strand_points - 1);
        const float phase = unit(seed, 1) * 2.0f * k_pi;
        const std::array<float, 4> wander{unit(seed, 2) * 2.0f * k_pi, unit(seed, 3) * 2.0f * k_pi,
                                          1.0f + 2.0f * unit(seed, 4), 1.0f + 2.0f * unit(seed, 5)};
        Vec3* points = &groom.points[static_cast<usize>(s) * k_strand_points];
        Vec3 at = root;
        Vec3 along = normal;
        Vec3 side_a;
        Vec3 side_b;
        across(normal, side_a, side_b);
        const float lean_turn = unit(seed, 6) * 2.0f * k_pi;
        const Vec3 lean{0.05f * std::cos(lean_turn), 0.0f, 0.05f * std::sin(lean_turn)};
        points[0] = root;
        for (u32 i = 1; i < k_strand_points; ++i) {
            const float t01 = static_cast<float>(i) / static_cast<float>(k_strand_points - 1);
            // Bent a little further down at every point, as a hanging
            // strand is; one standing straight up tips over to its own side.
            const Vec3 bent = normalize_or_zero(along + (down + lean) * (droop * 0.35f));
            if (length_squared(bent) > 0.0f) along = bent;
            at = at + along * step;
            // The frame is carried along the strand, never made afresh (a
            // fresh one would jump where the strand turns past its axis).
            const Vec3 carried = normalize_or_zero(side_a - along * dot(side_a, along));
            if (length_squared(carried) > 0.0f) {
                side_a = carried;
                side_b = cross(along, side_a);
            } else {
                across(along, side_a, side_b);
            }
            // The curl and the wander grow from nothing at the root.
            const float turn = phase + 2.0f * k_pi * curl_turns * t01 * (strand_length / length_m);
            const float wound = curl_radius * std::min(1.0f, t01 * 4.0f);
            const float wa = frizz * t01 * std::sin(wander[0] + wander[2] * t01 * 2.0f * k_pi);
            const float wb = frizz * t01 * std::sin(wander[1] + wander[3] * t01 * 2.0f * k_pi);
            points[i] = at + side_a * (wound * std::cos(turn) + wa) + side_b * (wound * std::sin(turn) + wb);
        }
    }
    finish(groom, std::min(given.guides, strands), given.clump);
    return groom;
}

Groom groom_from_strands(std::span<const std::vector<Vec3>> strands, u32 guides, u32 seed) {
    Groom groom;
    std::vector<float> along;
    for (const std::vector<Vec3>& line : strands) {
        if (groom.seeds.size() >= k_max_strands) break;
        if (line.size() < 2) continue;
        along.assign(line.size(), 0.0f);
        bool fine = within_reach(line[0]);
        for (usize i = 1; i < line.size() && fine; ++i) {
            fine = within_reach(line[i]);
            along[i] = along[i - 1] + length(line[i] - line[i - 1]);
        }
        const float total = along.back();
        if (!fine || !std::isfinite(total) || !(total > 0.0f)) continue;
        usize segment = 1;
        for (u32 i = 0; i < k_strand_points; ++i) {
            const float want = total * static_cast<float>(i) / static_cast<float>(k_strand_points - 1);
            while (segment + 1 < line.size() && along[segment] < want) ++segment;
            const float span = along[segment] - along[segment - 1];
            const float t = span > 0.0f ? std::clamp((want - along[segment - 1]) / span, 0.0f, 1.0f) : 0.0f;
            groom.points.push_back(lerp(line[segment - 1], line[segment], t));
        }
        const usize first = groom.points.size() - k_strand_points;
        Vec3 normal{0.0f, 1.0f, 0.0f};
        for (u32 i = 1; i < k_strand_points; ++i) {
            const Vec3 d = normalize_or_zero(groom.points[first + i] - groom.points[first]);
            if (length_squared(d) > 0.0f) {
                normal = d;
                break;
            }
        }
        groom.root_normals.push_back(normal);
        groom.seeds.push_back(hash(seed ^ hash(static_cast<u32>(groom.seeds.size()) + 0x9e3779b9u)));
    }
    // A file's strands come in the modeller's order (often row by row):
    // shuffled, so that every prefix covers the scalp.
    const u32 count = groom.strand_count();
    for (u32 i = count; i > 1; --i) {
        const u32 j = hash(seed ^ hash(i)) % i;
        if (j == i - 1) continue;
        std::swap_ranges(groom.points.begin() + static_cast<std::ptrdiff_t>(i - 1) * k_strand_points,
                         groom.points.begin() + static_cast<std::ptrdiff_t>(i) * k_strand_points,
                         groom.points.begin() + static_cast<std::ptrdiff_t>(j) * k_strand_points);
        std::swap(groom.root_normals[i - 1], groom.root_normals[j]);
        std::swap(groom.seeds[i - 1], groom.seeds[j]);
    }
    finish(groom, guides, 0.0f);
    return groom;
}

void pose_strand(const Groom& groom, u32 strand, std::span<const Vec3> posed_guides,
                 std::span<Vec3, k_strand_points> out) {
    const std::span<const Vec3> rest = groom.strand(strand);
    const StrandFollow& f = groom.follow[strand];
    for (u32 i = 0; i < k_strand_points; ++i) {
        Vec3 moved{};
        for (u32 k = 0; k < k_strand_guides; ++k) {
            const usize at = static_cast<usize>(f.guides[k]) * k_strand_points + i;
            moved = moved + (posed_guides[at] - groom.points[at]) * f.weights[k];
        }
        out[i] = rest[i] + moved;
    }
}

} // namespace giant::geometry
