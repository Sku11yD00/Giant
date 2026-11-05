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

} // namespace giant
