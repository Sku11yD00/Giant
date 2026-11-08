#include "giant/anim/skeleton.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace giant::anim {

namespace {

bool finite(float v) { return std::isfinite(v); }
bool finite(const Vec3& v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool finite(const Vec4& v) { return finite(v.x) && finite(v.y) && finite(v.z) && finite(v.w); }
bool finite(const Quat& q) { return finite(q.x) && finite(q.y) && finite(q.z) && finite(q.w); }
bool finite(const Mat4& m) {
    return std::all_of(std::begin(m.m), std::end(m.m), [](float v) { return std::isfinite(v); });
}

float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// Along the shorter way, renormalised.
Quat nlerp(const Quat& a, Quat b, float t) {
    if (dot(a, b) < 0.0f) b = {-b.x, -b.y, -b.z, -b.w};
    return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                          a.w + (b.w - a.w) * t});
}

// At constant speed along the shorter arc (glTF's linear rotations).
Quat slerp(const Quat& a, Quat b, float t) {
    float c = dot(a, b);
    if (c < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        c = -c;
    }
    if (c > 0.9995f) return nlerp(a, b, t); // nearly the same: a lerp is as good
    const float angle = std::acos(std::min(c, 1.0f));
    const float s = std::sin(angle);
    const float wa = std::sin((1.0f - t) * angle) / s;
    const float wb = std::sin(t * angle) / s;
    return normalize(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

Vec4 lerp4(const Vec4& a, const Vec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}
Vec4 scaled(const Vec4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
Vec4 plus(const Vec4& a, const Vec4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

Quat to_quat(const Vec4& v) { return normalize(Quat{v.x, v.y, v.z, v.w}); }
Vec3 to_vec3(const Vec4& v) { return {v.x, v.y, v.z}; }

// The channel's value at `time` (clamped to its keys).
Vec4 value_at(const Channel& c, float time, bool rotation) {
    const usize keys = c.times.size();
    const bool cubic = c.interpolation == Interpolation::cubic;
    const auto value = [&](usize k) { return c.values[cubic ? k * 3 + 1 : k]; };
    if (keys == 1 || time <= c.times.front()) return value(0);
    if (time >= c.times.back()) return value(keys - 1);
    // The key at or before `time`.
    const usize k = static_cast<usize>(std::upper_bound(c.times.begin(), c.times.end(), time) - c.times.begin()) - 1;
    const float t0 = c.times[k];
    const float dt = c.times[k + 1] - t0;
    const float s = dt > 0.0f ? std::clamp((time - t0) / dt, 0.0f, 1.0f) : 0.0f;
    switch (c.interpolation) {
    case Interpolation::step: return value(k);
    case Interpolation::linear:
        if (rotation) {
            const Quat q = slerp(to_quat(value(k)), to_quat(value(k + 1)), s);
            return {q.x, q.y, q.z, q.w};
        }
        return lerp4(value(k), value(k + 1), s);
    case Interpolation::cubic: {
        // Hermite: values with the out tangent of k and the in tangent of
        // k + 1, the tangents scaled by the keys' spacing.
        const float s2 = s * s, s3 = s2 * s;
        const Vec4 v0 = c.values[k * 3 + 1];
        const Vec4 b0 = c.values[k * 3 + 2];
        const Vec4 a1 = c.values[(k + 1) * 3];
        const Vec4 v1 = c.values[(k + 1) * 3 + 1];
        Vec4 out = scaled(v0, 2.0f * s3 - 3.0f * s2 + 1.0f);
        out = plus(out, scaled(b0, dt * (s3 - 2.0f * s2 + s)));
        out = plus(out, scaled(v1, -2.0f * s3 + 3.0f * s2));
        out = plus(out, scaled(a1, dt * (s3 - s2)));
        return out;
    }
    }
    return value(k);
}

Vec3 transform_point(const Mat4& m, const Vec3& p) {
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

Vec3 transform_direction(const Mat4& m, const Vec3& d) {
    return {m.at(0, 0) * d.x + m.at(0, 1) * d.y + m.at(0, 2) * d.z,
            m.at(1, 0) * d.x + m.at(1, 1) * d.y + m.at(1, 2) * d.z,
            m.at(2, 0) * d.x + m.at(2, 1) * d.y + m.at(2, 2) * d.z};
}

// At least how far the matrix's 3x3 part can stretch a vector (its
// Frobenius norm: never below the largest singular value, shear or not).
float stretch(const Mat4& m) {
    float sum = 0.0f;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) sum += m.at(r, c) * m.at(r, c);
    }
    return std::sqrt(sum);
}

} // namespace

std::string validate(const Skeleton& s) {
    const usize n = s.parents.size();
    if (n == 0) return "the skeleton has no joints";
    if (n > k_max_joints) return "the skeleton has more than 1024 joints";
    if (s.names.size() != n || s.rest.size() != n || s.inverse_bind.size() != n) {
        return "the skeleton's joints, names, rest pose and inverse binds differ in number";
    }
    for (usize i = 0; i < n; ++i) {
        if (s.parents[i] < -1 || s.parents[i] >= static_cast<i32>(i)) return "a joint comes before its parent";
        const JointTransform& r = s.rest[i];
        if (!finite(r.translation) || !finite(r.rotation) || !finite(r.scale) || !finite(s.inverse_bind[i])) {
            return "a joint's rest pose or inverse bind is not finite";
        }
    }
    if (!finite(s.root)) return "the skeleton's root is not finite";
    return {};
}

std::string validate(const Clip& clip, const Skeleton& skeleton) {
    if (!std::isfinite(clip.duration) || clip.duration < 0.0f || clip.duration > k_max_clip_seconds) {
        return "the clip " + clip.name + " is longer than an hour (or its length is not a number)";
    }
    for (const Channel& c : clip.channels) {
        if (c.joint >= skeleton.size()) return "the clip " + clip.name + " animates a joint the skeleton lacks";
        if (c.times.empty()) return "the clip " + clip.name + " has a channel with no keys";
        const usize per = c.interpolation == Interpolation::cubic ? 3 : 1;
        if (c.values.size() != c.times.size() * per) {
            return "the clip " + clip.name + " has a channel whose values do not match its keys";
        }
        for (usize k = 0; k < c.times.size(); ++k) {
            if (!finite(c.times[k]) || c.times[k] < 0.0f || c.times[k] > k_max_clip_seconds ||
                (k > 0 && !(c.times[k] > c.times[k - 1]))) {
                return "the clip " + clip.name + " has key times that are not increasing";
            }
        }
        for (const Vec4& v : c.values) {
            if (!finite(v)) return "the clip " + clip.name + " has a value that is not a finite number";
        }
    }
    return {};
}

void sample(const Skeleton& skeleton, const Clip& clip, float time, std::span<JointTransform> local) {
    const usize n = std::min(local.size(), skeleton.rest.size());
    std::copy_n(skeleton.rest.begin(), n, local.begin());
    if (!std::isfinite(time)) time = 0.0f;
    for (const Channel& c : clip.channels) {
        if (c.joint >= n || c.times.empty()) continue;
        JointTransform& j = local[c.joint];
        switch (c.path) {
        case Path::translation: j.translation = to_vec3(value_at(c, time, false)); break;
        case Path::rotation: j.rotation = to_quat(value_at(c, time, true)); break;
        case Path::scale: j.scale = to_vec3(value_at(c, time, false)); break;
        }
    }
}

void blend(std::span<const JointTransform> a, std::span<const JointTransform> b, float t,
           std::span<JointTransform> out) {
    const usize n = std::min({a.size(), b.size(), out.size()});
    t = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
    for (usize i = 0; i < n; ++i) {
        out[i].translation = lerp(a[i].translation, b[i].translation, t);
        out[i].scale = lerp(a[i].scale, b[i].scale, t);
        out[i].rotation = nlerp(a[i].rotation, b[i].rotation, t);
    }
}

void model_matrices(const Skeleton& skeleton, std::span<const JointTransform> local, std::span<Mat4> model) {
    const usize n = std::min({skeleton.size(), local.size(), model.size()});
    for (usize i = 0; i < n; ++i) {
        const i32 p = skeleton.parents[i];
        const Mat4& parent = p < 0 ? skeleton.root : model[static_cast<usize>(p)];
        model[i] = parent * to_matrix(local[i]);
    }
}

void skin_matrices(const Skeleton& skeleton, std::span<const Mat4> model, std::span<Mat4> skin) {
    const usize n = std::min({skeleton.size(), model.size(), skin.size()});
    for (usize i = 0; i < n; ++i) skin[i] = model[i] * skeleton.inverse_bind[i];
}

Vec3 skin_point(std::span<const Mat4> skin, const std::array<u16, 4>& joints, const std::array<u8, 4>& weights,
                const Vec3& bind) {
    Vec3 out{};
    for (usize k = 0; k < 4; ++k) {
        if (weights[k] == 0 || joints[k] >= skin.size()) continue;
        out = out + transform_point(skin[joints[k]], bind) * (static_cast<float>(weights[k]) / 255.0f);
    }
    return out;
}

Vec3 skin_direction(std::span<const Mat4> skin, const std::array<u16, 4>& joints,
                    const std::array<u8, 4>& weights, const Vec3& bind) {
    Vec3 out{};
    for (usize k = 0; k < 4; ++k) {
        if (weights[k] == 0 || joints[k] >= skin.size()) continue;
        out = out + transform_direction(skin[joints[k]], bind) * (static_cast<float>(weights[k]) / 255.0f);
    }
    return normalize_or_zero(out);
}

Bounds pose_bounds(std::span<const Mat4> model, std::span<const Mat4> skin, std::span<const float> joint_radius) {
    const usize n = std::min({model.size(), skin.size(), joint_radius.size()});
    // Each joint's vertices lie within a sphere about it; a box round those,
    // then the sphere about the box's middle holding them all.
    Vec3 lo{}, hi{};
    bool any = false;
    for (usize i = 0; i < n; ++i) {
        if (!(joint_radius[i] > 0.0f)) continue;
        const Vec3 c{model[i].at(0, 3), model[i].at(1, 3), model[i].at(2, 3)};
        const float r = joint_radius[i] * stretch(skin[i]);
        const Vec3 a{c.x - r, c.y - r, c.z - r}, b{c.x + r, c.y + r, c.z + r};
        lo = any ? Vec3{std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)} : a;
        hi = any ? Vec3{std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)} : b;
        any = true;
    }
    if (!any) return {};
    Bounds out;
    out.center = (lo + hi) * 0.5f;
    for (usize i = 0; i < n; ++i) {
        if (!(joint_radius[i] > 0.0f)) continue;
        const Vec3 c{model[i].at(0, 3), model[i].at(1, 3), model[i].at(2, 3)};
        out.radius = std::max(out.radius, length(c - out.center) + joint_radius[i] * stretch(skin[i]));
    }
    return out;
}

Mat4 affine_inverse(const Mat4& m) {
    // The 3x3 part's inverse by cofactors, then the translation back.
    const auto a = [&m](int r, int c) { return m.at(r, c); };
    const float det = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
                      a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
                      a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    // Singular next to its own size (any uniform scale, however small, inverts).
    float size = 1.0f;
    for (int c = 0; c < 3; ++c) size *= length(Vec3{a(0, c), a(1, c), a(2, c)});
    if (!std::isfinite(det) || det == 0.0f || std::fabs(det) <= 1e-6f * size) return {};
    Mat4 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            // inverse(r, c) = cofactor(c, r) / det
            const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            out.at(r, c) = (a(r1, c1) * a(r2, c2) - a(r1, c2) * a(r2, c1)) / det;
        }
    }
    for (int r = 0; r < 3; ++r) {
        out.at(r, 3) = -(out.at(r, 0) * a(0, 3) + out.at(r, 1) * a(1, 3) + out.at(r, 2) * a(2, 3));
    }
    out.at(3, 0) = out.at(3, 1) = out.at(3, 2) = 0.0f;
    out.at(3, 3) = 1.0f;
    return out;
}

JointTransform decompose(const Mat4& m) {
    JointTransform t;
    t.translation = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    Vec3 col[3];
    for (int c = 0; c < 3; ++c) col[c] = {m.at(0, c), m.at(1, c), m.at(2, c)};
    t.scale = {length(col[0]), length(col[1]), length(col[2])};
    if (dot(cross(col[0], col[1]), col[2]) < 0.0f) t.scale.x = -t.scale.x; // mirrored
    const float sx = t.scale.x, sy = t.scale.y, sz = t.scale.z;
    if (sx == 0.0f || sy == 0.0f || sz == 0.0f) return t; // no rotation to find
    // The rotation matrix's columns, then the quaternion from it (Shepperd).
    const Vec3 x = col[0] * (1.0f / sx), y = col[1] * (1.0f / sy), z = col[2] * (1.0f / sz);
    const float trace = x.x + y.y + z.z;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    t.rotation = normalize(q);
    return t;
}

Mat4 to_matrix(const JointTransform& t) { return mat4_from_transform(t.translation, t.rotation, t.scale); }

namespace {

// The value `share` (0 to 1) of the way through `values` (reordered).
float percentile(std::vector<float>& values, float share) {
    const usize k = std::min(values.size() - 1, static_cast<usize>(share * static_cast<float>(values.size() - 1) + 0.5f));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

} // namespace

std::vector<BoneCapsule> fit_capsules(const Skeleton& skeleton, std::span<const Vec3> bind_positions,
                                      std::span<const u16> dominant, usize min_vertices) {
    std::vector<BoneCapsule> out;
    const usize n = skeleton.size();
    if (n == 0 || skeleton.inverse_bind.size() != n || dominant.size() != bind_positions.size()) return out;
    std::vector<std::vector<Vec3>> points(n);
    for (usize v = 0; v < bind_positions.size(); ++v) {
        if (dominant[v] < n) points[dominant[v]].push_back(bind_positions[v]);
    }
    // Each joint's origin in bind (model) space.
    std::vector<Vec3> origin(n);
    for (usize j = 0; j < n; ++j) origin[j] = transform_point(affine_inverse(skeleton.inverse_bind[j]), {});
    std::vector<float> along;
    std::vector<float> across;
    for (usize j = 0; j < n; ++j) {
        if (points[j].size() < std::max<usize>(min_vertices, 2)) continue;
        const Mat4& to_joint = skeleton.inverse_bind[j];
        std::vector<Vec3>& local = points[j];
        for (Vec3& p : local) p = transform_point(to_joint, p);
        // Its way: to its children, else to its vertices.
        Vec3 toward{};
        usize children = 0;
        for (usize c = j + 1; c < n; ++c) {
            if (skeleton.parents[c] != static_cast<i32>(j)) continue;
            toward += transform_point(to_joint, origin[c]);
            ++children;
        }
        if (children > 0) toward = toward * (1.0f / static_cast<float>(children));
        if (!(length(toward) > 1e-4f)) {
            toward = {};
            for (const Vec3& p : local) toward += p;
            toward = toward * (1.0f / static_cast<float>(local.size()));
        }
        const Vec3 axis = length(toward) > 1e-4f ? normalize_or_zero(toward) : Vec3{0.0f, 1.0f, 0.0f};
        along.clear();
        across.clear();
        for (const Vec3& p : local) {
            const float t = dot(p, axis);
            along.push_back(t);
            across.push_back(length(p - axis * t));
        }
        const float lo = percentile(along, 0.02f);
        const float hi = percentile(along, 0.98f);
        const float radius = percentile(across, 0.7f);
        if (!(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(lo) || !std::isfinite(hi)) continue;
        BoneCapsule c;
        c.joint = static_cast<u32>(j);
        c.radius = radius;
        if (hi - lo > 2.0f * radius) {
            c.a = axis * (lo + radius);
            c.b = axis * (hi - radius);
        } else {
            c.a = c.b = axis * ((lo + hi) * 0.5f);
            c.radius = std::max(radius, (hi - lo) * 0.5f);
        }
        out.push_back(c);
    }
    return out;
}

} // namespace giant::anim
