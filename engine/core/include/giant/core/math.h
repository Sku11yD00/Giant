// Vectors for a world measured in tens of kilometres.
//
// World positions are DVec3 (double precision): a float only keeps about
// 7 digits, so 50 km from the origin it can no longer tell 1 cm apart and
// meshes jitter. The GPU still works in float, so positions are converted
// relative to the camera with to_camera_relative() just before rendering.
#pragma once

#include <cmath>

namespace giant {

template <typename T>
struct Vector3 {
    T x{};
    T y{};
    T z{};

    constexpr Vector3() = default;
    constexpr Vector3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}

    constexpr Vector3 operator+(const Vector3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vector3 operator-(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vector3 operator-() const { return {-x, -y, -z}; }
    constexpr Vector3 operator*(T s) const { return {x * s, y * s, z * s}; }
    constexpr Vector3 operator/(T s) const { return {x / s, y / s, z / s}; }
    constexpr Vector3& operator+=(const Vector3& o) { return *this = *this + o; }
    constexpr Vector3& operator-=(const Vector3& o) { return *this = *this - o; }
    constexpr Vector3& operator*=(T s) { return *this = *this * s; }
    constexpr bool operator==(const Vector3&) const = default;
};

using Vec3 = Vector3<float>;
using DVec3 = Vector3<double>;

template <typename T>
constexpr Vector3<T> operator*(T s, const Vector3<T>& v) {
    return v * s;
}

template <typename T>
constexpr T dot(const Vector3<T>& a, const Vector3<T>& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

template <typename T>
constexpr Vector3<T> cross(const Vector3<T>& a, const Vector3<T>& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename T>
T length(const Vector3<T>& v) {
    return std::sqrt(dot(v, v));
}

template <typename T>
constexpr T length_squared(const Vector3<T>& v) {
    return dot(v, v);
}

// Returns v scaled to length 1, or zero when v is (nearly) zero.
template <typename T>
Vector3<T> normalize_or_zero(const Vector3<T>& v) {
    const T len = length(v);
    return len > T(1e-12) ? v / len : Vector3<T>{};
}

template <typename T>
constexpr Vector3<T> lerp(const Vector3<T>& a, const Vector3<T>& b, T t) {
    return a + (b - a) * t;
}

// The only sanctioned way to turn a world position into a float for the GPU.
// Subtraction happens in double, so the result is exact to float precision
// near the camera however far the camera is from the world origin.
inline Vec3 to_camera_relative(const DVec3& world, const DVec3& camera) {
    const DVec3 d = world - camera;
    return {static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
}

inline DVec3 to_world(const Vec3& camera_relative, const DVec3& camera) {
    return camera + DVec3{camera_relative.x, camera_relative.y, camera_relative.z};
}

// --- Rotations and matrices (float: everything here is camera-relative) ----
//
// Conventions, engine-wide: metres, right-handed, +Y up, cameras look down
// -Z. Angles are radians unless a name says degrees.

constexpr float k_pi = 3.14159265358979323846f;

constexpr float degrees_to_radians(float degrees) { return degrees * (k_pi / 180.0f); }
constexpr float radians_to_degrees(float radians) { return radians * (180.0f / k_pi); }

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    constexpr bool operator==(const Vec4&) const = default;
};

// A unit quaternion: a rotation. Default is no rotation.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
    constexpr bool operator==(const Quat&) const = default;
};

// `b` first, then `a` (like matrices: (a * b) * v == a * (b * v)).
constexpr Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

constexpr Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Unit length, or no rotation when q is (nearly) zero.
inline Quat normalize(const Quat& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-12f) {
        return {};
    }
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

// `axis` need not be normalised; a zero axis gives no rotation.
inline Quat quat_from_axis_angle(const Vec3& axis, float radians) {
    const Vec3 a = normalize_or_zero(axis);
    if (a == Vec3{}) {
        return {};
    }
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}

inline Vec3 rotate(const Quat& q, const Vec3& v) {
    // v + 2w(u x v) + 2u x (u x v), with u the vector part.
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// Yaw turns around +Y (positive turns left), then pitch around the turned X
// axis (positive looks up), then roll around the view axis: the order game
// cameras and editors use. All in degrees.
struct EulerDegrees {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

inline Quat quat_from_euler(const EulerDegrees& e) {
    return quat_from_axis_angle({0, 1, 0}, degrees_to_radians(e.yaw)) *
           quat_from_axis_angle({1, 0, 0}, degrees_to_radians(e.pitch)) *
           quat_from_axis_angle({0, 0, 1}, degrees_to_radians(e.roll));
}

// The inverse of quat_from_euler (pitch in [-90, 90]).
EulerDegrees euler_from_quat(const Quat& q);

// Column-major 4x4: m[column * 4 + row], and transforms column vectors
// (M * v). Shaders get the four columns and multiply the same way.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    constexpr float& at(int row, int column) { return m[column * 4 + row]; }
    constexpr float at(int row, int column) const { return m[column * 4 + row]; }
    constexpr Vec4 column(int c) const { return {m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]}; }
};

Mat4 operator*(const Mat4& a, const Mat4& b);
Vec4 operator*(const Mat4& a, const Vec4& v);

// Scale, then rotate, then move.
Mat4 mat4_from_transform(const Vec3& translation, const Quat& rotation, const Vec3& scale);

// The view matrix for a camera at the origin (camera-relative rendering):
// only the inverse of its rotation.
Mat4 view_from_orientation(const Quat& orientation);

// Reverse-Z perspective with an infinite far plane: depth is 1 at `near`
// and falls towards 0 at infinity, which keeps float depth precise across a
// 20 km view. Clip space is D3D's (depth 0 to 1, +Y up).
Mat4 perspective_reverse_z(float vertical_fov_radians, float aspect, float near);

} // namespace giant
