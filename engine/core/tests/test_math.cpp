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
