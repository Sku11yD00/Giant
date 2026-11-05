#include "giant/core/math.h"

#include <algorithm>

namespace giant {

namespace {

// The rotation part as a row-major 3x3 (r[row][column]).
struct Rotation3 {
    float r[3][3];
};

Rotation3 rotation_matrix(const Quat& q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {{{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
             {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
             {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}}};
}

} // namespace

EulerDegrees euler_from_quat(const Quat& q) {
    // R = Ry(yaw) * Rx(pitch) * Rz(roll) gives r[1][2] = -sin(pitch),
    // r[1][0] / r[1][1] = tan(roll) and r[0][2] / r[2][2] = tan(yaw).
    const Rotation3 m = rotation_matrix(normalize(q));
    const float sin_pitch = std::clamp(-m.r[1][2], -1.0f, 1.0f);
    EulerDegrees e;
    e.pitch = radians_to_degrees(std::asin(sin_pitch));
    if (std::abs(sin_pitch) < 0.9999f) {
        e.yaw = radians_to_degrees(std::atan2(m.r[0][2], m.r[2][2]));
        e.roll = radians_to_degrees(std::atan2(m.r[1][0], m.r[1][1]));
    } else {
        // Looking straight up or down: yaw and roll turn the same axis, so
        // put it all in yaw.
        e.yaw = radians_to_degrees(std::atan2(-m.r[2][0], m.r[0][0]));
        e.roll = 0.0f;
    }
    return e;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(r, k) * b.at(k, c);
            }
            out.at(r, c) = sum;
        }
    }
    return out;
}

Vec4 operator*(const Mat4& a, const Vec4& v) {
    const float in[4] = {v.x, v.y, v.z, v.w};
    float out[4] = {};
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) {
            out[r] += a.at(r, k) * in[k];
        }
    }
    return {out[0], out[1], out[2], out[3]};
}

Mat4 mat4_from_transform(const Vec3& translation, const Quat& rotation, const Vec3& scale) {
    const Rotation3 rot = rotation_matrix(normalize(rotation));
    const float s[3] = {scale.x, scale.y, scale.z};
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            m.at(r, c) = rot.r[r][c] * s[c];
        }
    }
    m.at(0, 3) = translation.x;
    m.at(1, 3) = translation.y;
    m.at(2, 3) = translation.z;
    return m;
}

Mat4 view_from_orientation(const Quat& orientation) {
    // The inverse of a rotation is its transpose.
    const Rotation3 rot = rotation_matrix(normalize(orientation));
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            m.at(r, c) = rot.r[c][r];
        }
    }
    return m;
}

Mat4 perspective_reverse_z(float vertical_fov_radians, float aspect, float near) {
    const float f = 1.0f / std::tan(vertical_fov_radians * 0.5f);
    Mat4 m;
    m.m[0] = f / aspect; // x
    m.m[5] = f;          // y
    m.m[10] = 0.0f;      // z: clip z = near, clip w = -view z, so depth = near / distance
    m.m[11] = -1.0f;
    m.m[14] = near;
    m.m[15] = 0.0f;
    return m;
}

} // namespace giant
