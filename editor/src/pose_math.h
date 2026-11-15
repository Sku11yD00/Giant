// Small maths shared by what places things on posed characters (hit
// shapes, occluders, hair): points through joint matrices and entity
// transforms.
#pragma once

#include "giant/core/math.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <cmath>

namespace giant::editor::pose {

inline Vec3 apply(const Mat4& m, const Vec3& p) {
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

// The most a matrix stretches a length (its largest column).
inline float stretch(const Mat4& m) {
    float most = 0.0f;
    for (int c = 0; c < 3; ++c) {
        most = std::max(most, std::sqrt(m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c)));
    }
    return most;
}

// A point of an entity's model space in the world.
inline DVec3 place(const world::Transform& t, const Vec3& model) {
    const Vec3 scaled{model.x * t.scale.x, model.y * t.scale.y, model.z * t.scale.z};
    const Vec3 r = rotate(t.rotation, scaled);
    return t.position + DVec3{static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.z)};
}

} // namespace giant::editor::pose
