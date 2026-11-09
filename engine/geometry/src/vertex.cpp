#include "giant/core/assert.h"
#include "giant/geometry/mesh.h"

#include <algorithm>
#include <cmath>

namespace giant::geometry {

namespace {

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float sign_not_zero(float f) { return f < 0.0f ? -1.0f : 1.0f; }

i32 to_snorm16(float f) {
    return static_cast<i32>(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
}
float from_snorm16(i32 q) { return std::max(static_cast<float>(q) / 32767.0f, -1.0f); }

// Octahedral coordinates of a unit vector, each in [-1, 1].
void octahedral(const Vec3& n, float& x, float& y) {
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    x = n.x / l1;
    y = n.y / l1;
    if (n.z < 0.0f) {
        const float ox = x;
        x = (1.0f - std::abs(y)) * sign_not_zero(ox);
        y = (1.0f - std::abs(ox)) * sign_not_zero(y);
    }
}

u32 pack_pair(i32 x, i32 y) {
    return (static_cast<u32>(x) & 0xFFFFu) | ((static_cast<u32>(y) & 0xFFFFu) << 16);
}
i32 low16(u32 packed) { return static_cast<i32>(static_cast<i16>(packed & 0xFFFFu)); }
i32 high16(u32 packed) { return static_cast<i32>(static_cast<i16>(packed >> 16)); }

Vec3 decode(float x, float y) {
    Vec3 n{x, y, 1.0f - std::abs(x) - std::abs(y)};
    const float t = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize_or_zero(n);
}

Vec3 unit_or(const Vec3& v, const Vec3& fallback) {
    if (!finite(v)) return fallback;
    const Vec3 n = normalize_or_zero(v);
    return length_squared(n) > 0.0f ? n : fallback;
}

// A unit vector perpendicular to unit `n`, the same for the same normal.
Vec3 any_perpendicular(const Vec3& n) {
    const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return normalize_or_zero(axis - n * dot(n, axis));
}

// `t` made perpendicular to unit `n` and unit, else any perpendicular.
Vec3 perpendicular_tangent(const Vec3& n, const Vec3& t) {
    if (finite(t)) {
        const Vec3 p = t - n * dot(n, t);
        if (length_squared(p) > 1e-12f) return normalize_or_zero(p);
    }
    return any_perpendicular(n);
}

} // namespace

u32 pack_octahedral(const Vec3& unit) {
    const Vec3 n = unit_or(unit, Vec3{0, 0, 1});
    float x = 0.0f;
    float y = 0.0f;
    octahedral(n, x, y);
    return pack_pair(to_snorm16(x), to_snorm16(y));
}

Vec3 unpack_octahedral(u32 packed) { return decode(from_snorm16(low16(packed)), from_snorm16(high16(packed))); }

PackedVertex pack_vertex(const Vertex& vertex) {
    PackedVertex out{};
    out.position[0] = vertex.position.x;
    out.position[1] = vertex.position.y;
    out.position[2] = vertex.position.z;
    const Vec3 n = unit_or(vertex.normal, Vec3{0, 1, 0});
    out.normal = pack_octahedral(n);
    const Vec3 t = perpendicular_tangent(n, vertex.tangent);
    float x = 0.0f;
    float y = 0.0f;
    octahedral(t, x, y);
    // The sign takes y's lowest bit (a 1/32767 step lost).
    const i32 qy = (to_snorm16(y) & ~1) | (vertex.bitangent_sign < 0.0f ? 1 : 0);
    out.tangent = pack_pair(to_snorm16(x), qy);
    out.uv[0] = vertex.uv.u;
    out.uv[1] = vertex.uv.v;
    return out;
}

Vertex unpack_vertex(const PackedVertex& packed) {
    Vertex v;
    v.position = {packed.position[0], packed.position[1], packed.position[2]};
    v.normal = unpack_octahedral(packed.normal);
    v.tangent = unpack_octahedral(packed.tangent);
    v.bitangent_sign = (high16(packed.tangent) & 1) != 0 ? -1.0f : 1.0f;
    v.uv = {packed.uv[0], packed.uv[1]};
    return v;
}

void generate_tangents(TriangleMesh& mesh, std::vector<SkinWeights>* weights, std::span<const u8> keep) {
    const usize count = mesh.vertices.size();
    GIANT_ASSERT(keep.empty() || keep.size() == count, "one keep flag per vertex");
    const bool split_weights = weights != nullptr && weights->size() == count;
    // Per vertex, the angle-weighted tangents of its corners whose uv
    // mapping is direct (0) or mirrored (1).
    struct Sum {
        Vec3 tangent{};
        bool used = false;
    };
    std::vector<Sum> sums(count * 2);
    std::vector<u8> mirrored(mesh.indices.size(), 0); // per corner
    const usize index_count = mesh.indices.size() - mesh.indices.size() % 3;
    for (usize i = 0; i < index_count; i += 3) {
        const u32 idx[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        GIANT_ASSERT(idx[0] < count && idx[1] < count && idx[2] < count, "index out of range");
        const Vertex& v0 = mesh.vertices[idx[0]];
        const Vertex& v1 = mesh.vertices[idx[1]];
        const Vertex& v2 = mesh.vertices[idx[2]];
        // The bitangent points up the image: toward smaller v.
        const Vec3 e1 = v1.position - v0.position;
        const Vec3 e2 = v2.position - v0.position;
        const float du1 = v1.uv.u - v0.uv.u, dv1 = v0.uv.v - v1.uv.v;
        const float du2 = v2.uv.u - v0.uv.u, dv2 = v0.uv.v - v2.uv.v;
        const float r = du1 * dv2 - du2 * dv1;
        if (!std::isfinite(r) || std::abs(r) < 1e-20f) {
            continue; // no uv direction here
        }
        const Vec3 t = (e1 * dv2 - e2 * dv1) / r;
        const Vec3 b = (e2 * du1 - e1 * du2) / r;
        if (!finite(t) || !finite(b)) continue;
        for (usize k = 0; k < 3; ++k) {
            const u32 v = idx[k];
            const Vertex& here = mesh.vertices[v];
            const Vec3 to_a = normalize_or_zero(mesh.vertices[idx[(k + 1) % 3]].position - here.position);
            const Vec3 to_b = normalize_or_zero(mesh.vertices[idx[(k + 2) % 3]].position - here.position);
            const float angle = std::acos(std::clamp(dot(to_a, to_b), -1.0f, 1.0f));
            const Vec3 n = unit_or(here.normal, Vec3{0, 1, 0});
            const Vec3 tp = normalize_or_zero(t - n * dot(n, t));
            if (length_squared(tp) == 0.0f || !(angle > 0.0f)) continue;
            const u8 m = dot(cross(n, t), b) < 0.0f ? 1 : 0;
            mirrored[i + k] = m;
            Sum& s = sums[usize{v} * 2 + m];
            s.tangent += tp * angle;
            s.used = true;
        }
    }
    std::vector<u32> copy(count, ~0u); // a split vertex's mirrored copy
    for (usize v = 0; v < count; ++v) {
        if (!keep.empty() && keep[v] != 0) continue;
        const Sum& direct = sums[v * 2];
        const Sum& flipped = sums[v * 2 + 1];
        const Vec3 n = unit_or(mesh.vertices[v].normal, Vec3{0, 1, 0});
        const auto frame = [&n](const Sum& s) { return perpendicular_tangent(n, s.tangent); };
        if (direct.used && flipped.used) {
            Vertex other = mesh.vertices[v];
            other.tangent = frame(flipped);
            other.bitangent_sign = -1.0f;
            copy[v] = static_cast<u32>(mesh.vertices.size());
            mesh.vertices.push_back(other);
            if (split_weights) weights->push_back((*weights)[v]);
        }
        Vertex& self = mesh.vertices[v];
        const bool only_flipped = flipped.used && !direct.used;
        self.tangent = frame(only_flipped ? flipped : direct);
        self.bitangent_sign = only_flipped ? -1.0f : 1.0f;
    }
    for (usize i = 0; i < index_count; ++i) {
        const u32 v = mesh.indices[i];
        if (mirrored[i] != 0 && v < count && copy[v] != ~0u) {
            mesh.indices[i] = copy[v];
        }
    }
}

} // namespace giant::geometry
