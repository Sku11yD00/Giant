#include "giant/core/math.h"

#include "giant_test.h"

using namespace giant;

GIANT_TEST("math: vector arithmetic") {
    const DVec3 a{1.0, 2.0, 3.0};
    const DVec3 b{4.0, 5.0, 6.0};
    GIANT_CHECK((a + b) == DVec3(5.0, 7.0, 9.0));
    GIANT_CHECK((b - a) == DVec3(3.0, 3.0, 3.0));
    GIANT_CHECK((a * 2.0) == DVec3(2.0, 4.0, 6.0));
    GIANT_CHECK((2.0 * a) == DVec3(2.0, 4.0, 6.0));
    GIANT_CHECK_NEAR(dot(a, b), 32.0, 1e-12);
    GIANT_CHECK(cross(DVec3(1, 0, 0), DVec3(0, 1, 0)) == DVec3(0, 0, 1));
}

GIANT_TEST("math: normalize_or_zero handles zero vectors") {
    GIANT_CHECK(normalize_or_zero(Vec3{}) == Vec3{});
    GIANT_CHECK_NEAR(length(normalize_or_zero(Vec3{3.0f, 4.0f, 0.0f})), 1.0, 1e-6);
}

// The reason world positions are doubles: a giant's toe 1000 km from the
// origin must still sit exactly 1 mm from the camera on the GPU.
GIANT_TEST("math: camera relative keeps millimetres 1000 km from the origin") {
    const DVec3 camera{1'000'000.0, 2'500.0, -1'000'000.0};
    const DVec3 toe = camera + DVec3{0.001, 0.0, -0.001};

    const Vec3 relative = to_camera_relative(toe, camera);
    GIANT_CHECK_NEAR(relative.x, 0.001, 1e-7);
    GIANT_CHECK_NEAR(relative.z, -0.001, 1e-7);

    // Doing the same in float loses the millimetre entirely.
    const float toe_x_float = static_cast<float>(toe.x);
    const float camera_x_float = static_cast<float>(camera.x);
    GIANT_CHECK(toe_x_float - camera_x_float == 0.0f);
}

GIANT_TEST("math: to_world inverts to_camera_relative") {
    const DVec3 camera{52'345.25, 812.5, -7'000.125};
    const DVec3 point{52'350.0, 800.0, -7'010.0};
    const DVec3 back = to_world(to_camera_relative(point, camera), camera);
    GIANT_CHECK_NEAR(back.x, point.x, 1e-5);
    GIANT_CHECK_NEAR(back.y, point.y, 1e-5);
    GIANT_CHECK_NEAR(back.z, point.z, 1e-5);
}

namespace {
bool near3(const Vec3& a, const Vec3& b, float tolerance = 1e-5f) {
    return length(a - b) < tolerance;
}
} // namespace

GIANT_TEST("math: quaternion rotation follows the right-hand rule") {
    const Quat quarter_turn_y = quat_from_axis_angle({0, 1, 0}, k_pi / 2);
    // +Y up, right-handed: a quarter turn left turns -Z (forward) into -X.
    GIANT_CHECK(near3(rotate(quarter_turn_y, {0, 0, -1}), {-1, 0, 0}));
    GIANT_CHECK(near3(rotate(quarter_turn_y * conjugate(quarter_turn_y), {1, 2, 3}), {1, 2, 3}));
    // a * b applies b first.
    const Quat pitch_up = quat_from_axis_angle({1, 0, 0}, k_pi / 2);
    GIANT_CHECK(near3(rotate(quarter_turn_y * pitch_up, {0, 0, -1}), {0, 1, 0}));
    GIANT_CHECK(normalize(Quat{0, 0, 0, 0}) == Quat{});
    GIANT_CHECK(quat_from_axis_angle({0, 0, 0}, 1.0f) == Quat{});
}

GIANT_TEST("math: euler angles round-trip") {
    const EulerDegrees cases[] = {{10, 20, 30}, {-45, 170, 5}, {89, -90, 0}, {0, 0, 0}};
    for (const EulerDegrees& e : cases) {
        const EulerDegrees back = euler_from_quat(quat_from_euler(e));
        GIANT_CHECK_NEAR(back.pitch, e.pitch, 1e-2);
        GIANT_CHECK_NEAR(back.yaw, e.yaw, 1e-2);
        GIANT_CHECK_NEAR(back.roll, e.roll, 1e-2);
    }
    // Positive pitch looks up; positive yaw turns left.
    GIANT_CHECK(near3(rotate(quat_from_euler({30, 0, 0}), {0, 0, -1}),
                      {0, std::sin(degrees_to_radians(30)), -std::cos(degrees_to_radians(30))}));
    GIANT_CHECK(rotate(quat_from_euler({0, 30, 0}), {0, 0, -1}).x < 0.0f);
    // Straight down: all of the turn ends up in yaw.
    const EulerDegrees down = euler_from_quat(quat_from_euler({-90, 40, 0}));
    GIANT_CHECK_NEAR(down.pitch, -90.0, 1e-2);
    GIANT_CHECK_NEAR(down.yaw, 40.0, 0.5);
}

GIANT_TEST("math: transform matrices match rotate and scale") {
    const Quat q = quat_from_euler({15, -60, 10});
    const Mat4 m = mat4_from_transform({1, 2, 3}, q, {2, 3, 4});
    const Vec3 p{0.5f, -1.0f, 2.0f};
    const Vec4 out = m * Vec4{p.x, p.y, p.z, 1.0f};
    const Vec3 expected = rotate(q, {p.x * 2, p.y * 3, p.z * 4}) + Vec3{1, 2, 3};
    GIANT_CHECK(near3({out.x, out.y, out.z}, expected, 1e-4f));
    // The view matrix undoes the camera's rotation.
    const Mat4 view = view_from_orientation(q);
    const Vec3 forward = rotate(q, {0, 0, -1});
    const Vec4 in_view = view * Vec4{forward.x, forward.y, forward.z, 0.0f};
    GIANT_CHECK(near3({in_view.x, in_view.y, in_view.z}, {0, 0, -1}));
    GIANT_CHECK(near3({(Mat4{} * m).m[12], 0, 0}, {m.m[12], 0, 0}));
}

GIANT_TEST("math: reverse-Z depth is 1 at the near plane and falls with distance") {
    const Mat4 proj = perspective_reverse_z(degrees_to_radians(60), 16.0f / 9.0f, 0.1f);
    const auto depth = [&proj](float distance) {
        const Vec4 clip = proj * Vec4{0, 0, -distance, 1};
        return clip.z / clip.w;
    };
    GIANT_CHECK_NEAR(depth(0.1f), 1.0, 1e-6);
    GIANT_CHECK(depth(10.0f) > depth(20'000.0f));
    GIANT_CHECK(depth(20'000.0f) > 0.0f);
    // A point at the top of the view lands at clip y = w.
    const float t = std::tan(degrees_to_radians(30));
    const Vec4 top = proj * Vec4{0, t * 5.0f, -5.0f, 1};
    GIANT_CHECK_NEAR(top.y / top.w, 1.0, 1e-5);
}
