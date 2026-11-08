// Cloth: sheets hang, the wind blows them (harder in a storm), work stays
// bounded and the pose maps rest to where the cloth is.
#include "giant/sim/cloth.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace giant;
using namespace giant::sim;

namespace {

WindState blowing(float speed) {
    WindState w;
    w.velocity = {speed, 0.0f, 0.0f}; // along +x
    w.gustiness = 0.0f;
    return w;
}

// The sheet's points, on average (its space).
Vec3 middle(const Cloths::Sheet& s) {
    Vec3 sum{};
    for (const Vec3& p : s.points()) sum = sum + p;
    return sum * (1.0f / static_cast<float>(s.points().size()));
}

void run(Cloths& cloths, const WindState& wind, float seconds) {
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) cloths.step(1.0 / 60.0, wind, {});
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

GIANT_TEST("cloth: a sheet's grid follows its size, within the point limit, pinned as asked") {
    world::ClothSettings s;
    s.width = 1.5f;
    s.height = 3.0f;
    const Cloths::Sheet sheet(s);
    GIANT_CHECK(sheet.columns() == 11 && sheet.rows() == 21);
    GIANT_CHECK(sheet.points().size() == sheet.columns() * sheet.rows());
    GIANT_CHECK(sheet.rest()[0].x == -0.75f && sheet.rest()[0].y == 0.0f);
    GIANT_CHECK(std::fabs(sheet.rest().back().y + 3.0f) < 1e-5f);
    for (u32 c = 0; c < sheet.columns(); ++c) GIANT_CHECK(sheet.pinned(c));
    GIANT_CHECK(!sheet.pinned(sheet.columns()));
    s.width = 20.0f;
    s.height = 20.0f;
    s.pin = world::ClothPin::corners;
    const Cloths::Sheet big(s);
    GIANT_CHECK(big.points().size() == k_max_cloth_points);
    GIANT_CHECK(big.pinned(0) && big.pinned(big.columns() - 1) && !big.pinned(1));
    s = {};
    s.width = 0.0f; // clamped
    s.pin = world::ClothPin::left;
    const Cloths::Sheet flag(s);
    GIANT_CHECK(flag.columns() == 3);
    GIANT_CHECK(flag.pinned(0) && flag.pinned(flag.columns()) && !flag.pinned(1));
}

GIANT_TEST("cloth: in still air a sheet held by its corners sags and settles, then sleeps") {
    Cloths cloths;
    world::ClothSettings s;
    s.width = 2.0f;
    s.height = 1.0f;
    s.pin = world::ClothPin::corners;
    cloths.set(1, s, {});
    run(cloths, blowing(0.0f), 10.0f);
    const Cloths::Sheet& sheet = *cloths.find(1);
    // The top edge's middle hangs below its corners.
    const Vec3 top_middle = sheet.points()[sheet.columns() / 2];
    GIANT_CHECK(top_middle.y < -0.05f);
    // Nothing went sideways: still air pushes nowhere.
    for (const Vec3& p : sheet.points()) GIANT_CHECK(std::fabs(p.z) < 1e-3f);
    // The corners stay where they are pinned.
    GIANT_CHECK(sheet.points()[0] == sheet.rest()[0]);
    GIANT_CHECK(sheet.asleep());
    const u64 version = sheet.version();
    cloths.step(1.0 / 60.0, blowing(0.0f), {});
    GIANT_CHECK(sheet.version() == version);
    GIANT_CHECK(cloths.stats().asleep == 1 && cloths.stats().simulated == 0);
    // Wind wakes it.
    cloths.step(1.0 / 60.0, blowing(5.0f), {});
    GIANT_CHECK(!sheet.asleep() && sheet.version() != version);
}

GIANT_TEST("cloth: wind blows a banner downwind, and a storm blows it further than a breeze") {
    const auto lift = [](float speed) {
        Cloths cloths;
        world::ClothSettings s; // 1 x 1.5 m, its top pinned
        // Turned so the wind (+x) meets its face: its +z faces -x.
        world::Transform t;
        t.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
        cloths.set(7, s, t);
        run(cloths, blowing(speed), 6.0f);
        const Cloths::Sheet& sheet = *cloths.find(7);
        for (const Vec3& p : sheet.points()) GIANT_CHECK(finite(p));
        // Downwind is -z in its space (+x in the world); how high it is
        // lifted shows in its middle's height.
        const Vec3 m = middle(sheet);
        return Vec3{0.0f, m.y, -m.z};
    };
    const Vec3 calm = lift(0.0f);
    const Vec3 breeze = lift(2.0f);
    const Vec3 storm = lift(25.0f);
    GIANT_CHECK(std::fabs(calm.z) < 1e-3f);
    GIANT_CHECK(breeze.z > 0.05f);
    GIANT_CHECK(storm.z > breeze.z * 1.4f);
    GIANT_CHECK(storm.y > breeze.y + 0.3f); // blown up nearly level
    GIANT_CHECK(storm.z < 1.5f);            // held by its springs: no longer than it is
}

GIANT_TEST("cloth: a heavier cloth blows about less") {
    const auto lift = [](float weight) {
        Cloths cloths;
        world::ClothSettings s;
        s.weight = weight;
        world::Transform t;
        t.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
        cloths.set(1, s, t);
        run(cloths, blowing(6.0f), 5.0f);
        return -middle(*cloths.find(1)).z;
    };
    GIANT_CHECK(lift(0.1f) > lift(3.0f) * 1.2f);
}

GIANT_TEST("cloth: a flag in a storm ripples and keeps its size, its numbers sane") {
    Cloths cloths;
    world::ClothSettings s;
    s.width = 1.5f;
    s.height = 1.0f;
    s.pin = world::ClothPin::left;
    s.weight = 0.1f;
    cloths.set(1, s, {});
    WindState w = blowing(30.0f);
    float lowest_z = 0.0f, highest_z = 0.0f;
    for (int i = 0; i < 600; ++i) {
        w.flutter_clock = std::fmod(w.flutter_clock + 10.0f / 60.0f, 256.0f);
        cloths.step(1.0 / 60.0, w, {});
        if (i > 300) {
            const Cloths::Sheet& sheet = *cloths.find(1);
            const Vec3 tip = sheet.points()[sheet.columns() - 1];
            lowest_z = std::min(lowest_z, tip.z);
            highest_z = std::max(highest_z, tip.z);
        }
    }
    const Cloths::Sheet& sheet = *cloths.find(1);
    for (const Vec3& p : sheet.points()) GIANT_CHECK(finite(p));
    // The free edge streams out downwind (+x), about as far from the pole
    // (x -0.75) as the flag is long.
    const Vec3 tip = sheet.points()[sheet.columns() - 1];
    GIANT_CHECK(tip.x > 0.5f && tip.x < 0.9f);
    GIANT_CHECK(highest_z - lowest_z > 0.01f); // it ripples
    GIANT_CHECK(cloths.stats().resets == 0);
    // Bounds hold every point.
    for (const Vec3& p : sheet.points()) {
        GIANT_CHECK(length(p - sheet.bounds_center()) <= sheet.bounds_radius());
    }
}

GIANT_TEST("cloth: the pose takes each point from where it rests to where it is, turning its normal") {
    Cloths cloths;
    world::ClothSettings s;
    world::Transform t;
    t.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
    cloths.set(1, s, t);
    run(cloths, blowing(8.0f), 2.0f);
    const Cloths::Sheet& sheet = *cloths.find(1);
    std::vector<Mat4> skin;
    sheet.skin(skin);
    GIANT_CHECK(skin.size() == sheet.points().size());
    for (usize i = 0; i < skin.size(); ++i) {
        const Vec3 r = sheet.rest()[i];
        const Vec4 p = skin[i] * Vec4{r.x, r.y, r.z, 1.0f};
        GIANT_CHECK(length(Vec3{p.x, p.y, p.z} - sheet.points()[i]) < 1e-4f);
        const Vec4 n = skin[i] * Vec4{0.0f, 0.0f, 1.0f, 0.0f};
        GIANT_CHECK(std::fabs(length(Vec3{n.x, n.y, n.z}) - 1.0f) < 1e-3f);
    }
    // At rest the pose is no change at all.
    Cloths still;
    still.set(1, s, {});
    still.find(1)->skin(skin);
    for (const Mat4& m : skin) {
        for (int k = 0; k < 16; ++k) GIANT_CHECK(std::fabs(m.m[k] - Mat4{}.m[k]) < 1e-6f);
    }
}

GIANT_TEST("cloth: only the nearest sheets within reach move, within the budget") {
    ClothSettingsSim settings;
    settings.max_simulated = 3;
    Cloths cloths(settings);
    for (u32 i = 0; i < 10; ++i) {
        world::Transform t;
        t.position = {static_cast<double>(i) * 10.0, 0.0, 0.0}; // 0, 10, ... 90 m
        cloths.set(i + 1, {}, t);
    }
    cloths.step(1.0 / 60.0, blowing(5.0f), {});
    GIANT_CHECK(cloths.stats().sheets == 10);
    GIANT_CHECK(cloths.stats().simulated == 3);
    GIANT_CHECK(cloths.find(1)->version() != cloths.find(4)->version());
    const u64 far = cloths.find(9)->version();
    for (int i = 0; i < 30; ++i) cloths.step(1.0 / 60.0, blowing(5.0f), {85.0, 0.0, 0.0});
    GIANT_CHECK(cloths.find(9)->version() != far); // the camera came near
    // A long frame takes at most max_steps steps.
    cloths.step(5.0, blowing(5.0f), {});
    GIANT_CHECK(cloths.stats().steps <= settings.max_steps * settings.max_simulated);
    // Out of reach entirely: nothing moves.
    cloths.step(1.0 / 60.0, blowing(5.0f), {1e5, 0.0, 0.0});
    GIANT_CHECK(cloths.stats().simulated == 0);
    // The point budget keeps big sheets few (the nearest always moves).
    ClothSettingsSim tight;
    tight.max_points = 100;
    Cloths big(tight);
    world::ClothSettings huge;
    huge.width = huge.height = 20.0f;
    big.set(1, huge, {});
    big.set(2, huge, {});
    big.step(1.0 / 60.0, blowing(5.0f), {});
    GIANT_CHECK(big.stats().simulated == 1);
}

GIANT_TEST("cloth: a blast throws it, new settings make it again, moving it wakes it") {
    Cloths cloths;
    cloths.set(1, {}, {});
    run(cloths, blowing(0.0f), 6.0f);
    GIANT_CHECK(cloths.find(1)->asleep());
    cloths.push({0.0, -0.75, 1.0}, 2.0f, {0.0f, 0.0f, -20.0f});
    GIANT_CHECK(!cloths.find(1)->asleep());
    cloths.step(1.0 / 60.0, blowing(0.0f), {});
    GIANT_CHECK(middle(*cloths.find(1)).z < -0.01f);
    // Out of its reach: nothing.
    cloths.push({100.0, 0.0, 0.0}, 1.0f, {50.0f, 0.0f, 0.0f});
    // Bad numbers are left out.
    cloths.push({0.0, 0.0, 0.0}, std::nanf(""), {1.0f, 0.0f, 0.0f});
    world::ClothSettings s;
    s.height = 3.0f;
    cloths.set(1, s, {});
    GIANT_CHECK(cloths.find(1)->rows() == 21);
    GIANT_CHECK(cloths.find(1)->points() == cloths.find(1)->rest());
    cloths.remove(1);
    GIANT_CHECK(cloths.find(1) == nullptr && cloths.stats().sheets == 0);
}

GIANT_TEST("cloth: a storm never stretches a sheet far, however it is held") {
    // The worst stretch between neighbours on the grid, against their rest.
    const auto worst = [](const Cloths::Sheet& sheet) {
        float most = 0.0f;
        const u32 w = sheet.columns();
        for (u32 i = 0; i < sheet.points().size(); ++i) {
            for (const u32 j : {i + 1, i + w}) {
                if (j >= sheet.points().size() || (j == i + 1 && j % w == 0)) continue;
                const float rest = length(sheet.rest()[j] - sheet.rest()[i]);
                most = std::max(most, length(sheet.points()[j] - sheet.points()[i]) / rest);
            }
        }
        return most;
    };
    for (const world::ClothPin pin : {world::ClothPin::top, world::ClothPin::corners, world::ClothPin::left}) {
        for (const float speed : {15.0f, 60.0f}) {
            for (const bool face_on : {true, false}) {
                Cloths cloths;
                world::ClothSettings s;
                s.width = 4.0f;
                s.height = 6.0f;
                s.pin = pin;
                s.weight = 0.1f;
                world::Transform t;
                if (face_on) t.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
                cloths.set(1, s, t);
                WindState w = blowing(speed);
                w.gustiness = 1.0f;
                float most = 0.0f;
                for (int i = 0; i < 600; ++i) {
                    w.flutter_clock = std::fmod(w.flutter_clock + (1.0f + 0.15f * speed) / 60.0f, 256.0f);
                    w.gust_x = std::fmod(w.gust_x + speed * 0.8f / 60.0f, 64.0f);
                    cloths.step(1.0 / 60.0, w, {});
                    if (i > 300) most = std::max(most, worst(*cloths.find(1)));
                }
                std::printf("    pin %s, %.0f m/s %s: stretched at most %.2fx\n", world::to_string(pin), speed,
                            face_on ? "face-on" : "along it", most);
                GIANT_CHECK(most < 1.45f);
                GIANT_CHECK(cloths.stats().resets == 0);
            }
        }
    }
}

GIANT_TEST("cloth: a sheet made again keeps counting its versions on") {
    Cloths cloths;
    cloths.set(1, {}, {});
    cloths.step(1.0 / 60.0, blowing(5.0f), {});
    const u64 before = cloths.find(1)->version();
    world::ClothSettings s;
    s.weight = 1.0f; // the same size: the editor keeps its mesh, and must pose it again
    cloths.set(1, s, {});
    GIANT_CHECK(cloths.find(1)->version() > before);
}

GIANT_TEST("cloth: a blast throws a sheet away from where it went off, and wakes it") {
    for (const double side : {1.0, -1.0}) {
        Cloths cloths;
        cloths.set(1, {}, {});
        run(cloths, blowing(0.0f), 6.0f);
        GIANT_CHECK(cloths.find(1)->asleep());
        cloths.blast({0.0, -0.75, side}, 3.0f, 7.0f); // in front of its face, or behind: 15 m/s for 0.45 kg
        GIANT_CHECK(!cloths.find(1)->asleep());
        cloths.step(1.0 / 60.0, blowing(0.0f), {});
        GIANT_CHECK(middle(*cloths.find(1)).z * static_cast<float>(side) < -0.01f);
        cloths.blast({100.0, 0.0, 0.0}, 1.0f, 50.0f); // out of reach
        cloths.blast({0.0, 0.0, 0.0}, 1.0f, std::nanf(""));
        GIANT_CHECK(finite(middle(*cloths.find(1))));
    }
}
