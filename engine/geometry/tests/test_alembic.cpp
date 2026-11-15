#include "giant/geometry/alembic.h"

#include "alembic_test_files.h"
#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::geometry;
using namespace giant::geometry::testing;

namespace {

bool near3(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    return std::fabs(a.x - b.x) < tolerance && std::fabs(a.y - b.y) < tolerance && std::fabs(a.z - b.z) < tolerance;
}

} // namespace

GIANT_TEST("alembic: a groom's strands are read where its transforms place them; guides and hidden curves are not") {
    for (const bool wide : {false, true}) {
        const AlembicCurves r = read_alembic_curves(groom(wide));
        GIANT_CHECK(r.ok() && r.warnings.empty());
        GIANT_CHECK(r.guides == 2 && r.curves.size() == 2);
        if (r.curves.size() != 2) continue;
        // Doubled first, then moved: (0, 2, 0) is (0, 4, 10), not (0, 4, 20).
        GIANT_CHECK(r.curves[0].size() == 3 && near3(r.curves[0][0], {0, 0, 10}) && near3(r.curves[0][1], {0, 2, 10}) &&
                    near3(r.curves[0][2], {0, 4, 10}));
        GIANT_CHECK(r.curves[1].size() == 2 && near3(r.curves[1][0], {2, 0, 10}) && near3(r.curves[1][1], {2, 2, 10}));
    }
}

GIANT_TEST("alembic: one groom_guide value stands for every curve; odd curves warn and never refuse the file") {
    // One value for all: every strand is a guide.
    CurveOptions o;
    o.guide = {1};
    AlembicCurves r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.empty() && r.guides == 5);
    // Whole numbers of four bytes (Houdini's): the second strand is a guide.
    o.guide = bytes_of(std::vector<i32>{0, 256, 0});
    o.guide_pod = k_i32;
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 3 && r.warnings.empty());
    o.guide = bytes_of(std::vector<i32>{0, 0, 1});
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 1 && r.guides == 3 && r.warnings.empty());
    // Of two bytes; and a size that is no whole count of them is ignored.
    o.guide = bytes_of(std::vector<i16>{0, 256, 0});
    o.guide_pod = k_i16;
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 3 && r.warnings.empty());
    o.guide.push_back(1);
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 2 && r.warnings.size() == 1);
    // Numbers that are not whole are ignored.
    o.guide = bytes_of(std::vector<float>{1, 1, 1});
    o.guide_pod = k_f32;
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 2 && r.warnings.size() == 1);
    o.guide_pod = k_i8;
    // A value count that fits nothing is ignored.
    o.guide = {1, 0};
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 2 && r.warnings.size() == 1);
    // Cubic curves: their control points, with a warning.
    o = {};
    o.basis = 0;
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.warnings.size() == 1);
    // Points as doubles are skipped.
    o = {};
    o.point_pod = k_f64;
    r = read_alembic_curves(groom(false, o));
    GIANT_CHECK(r.ok() && r.curves.empty() && r.guides == 2 && r.warnings.size() == 1);
    // More points claimed than the object holds: what came before is kept.
    Ogawa file;
    Compound none;
    const u64 c = curves(file, {0, 0, 0, 0, 1, 0, 0, 2, 0}, {2, 5});
    r = read_alembic_curves(file.finish(object(file, none.write(file), {c})));
    GIANT_CHECK(r.ok() && r.curves.size() == 1 && r.warnings.size() == 1);
    // A point that is no number leaves its curve out.
    Ogawa bad;
    const float nan = std::nanf("");
    const u64 n = curves(bad, {0, 0, 0, 0, nan, 0, 0, 0, 0, 0, 1, 0}, {2, 2});
    r = read_alembic_curves(bad.finish(object(bad, none.write(bad), {n})));
    GIANT_CHECK(r.ok() && r.curves.size() == 1);
}

GIANT_TEST("alembic: rotations turn as Alembic's do") {
    // A quarter turn about z takes +x to +y; about an axis given as (0, 0, 2) the same.
    for (const bool about_axis : {false, true}) {
        Ogawa file;
        const u64 c = curves(file, {0, 0, 0, 1, 0, 0}, {2});
        const u64 turned = about_axis ? object(file, xform(file, {0x20}, {0, 0, 2, 90}, false), {c})
                                      : object(file, xform(file, {0x60}, {90}, false), {c});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {turned})));
        GIANT_CHECK(r.ok() && r.curves.size() == 1);
        if (r.curves.size() == 1) GIANT_CHECK(near3(r.curves[0][1], {0, 1, 0}));
    }
    // About x: +y to +z. About y: +z to +x.
    for (const bool about_x : {false, true}) {
        Ogawa file;
        const u64 c = about_x ? curves(file, {0, 0, 0, 0, 1, 0}, {2}) : curves(file, {0, 0, 0, 0, 0, 1}, {2});
        const u64 turned = object(file, xform(file, {static_cast<u8>(about_x ? 0x40 : 0x50)}, {90}, false), {c});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {turned})));
        GIANT_CHECK(r.ok() && r.curves.size() == 1);
        if (r.curves.size() == 1) GIANT_CHECK(near3(r.curves[0][1], about_x ? Vec3{0, 0, 1} : Vec3{1, 0, 0}));
    }
    // Steps of one transform: the last listed moves a point first (doubled,
    // then 1 m along x: 3, not 4). A transform that does not inherit
    // forgets those above it.
    for (const bool inherits : {true, false}) {
        Ogawa file;
        const u64 c = curves(file, {0, 0, 0, 1, 0, 0}, {2});
        const u64 inner = object(file, xform(file, {0x10, 0x00}, {1, 0, 0, 2, 2, 2}, false, -1, inherits), {c});
        const u64 outer = object(file, xform(file, {0x10}, {0, 5, 0}, false), {inner});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {outer})));
        GIANT_CHECK(r.ok() && r.curves.size() == 1);
        if (r.curves.size() == 1) GIANT_CHECK(near3(r.curves[0][1], {3, inherits ? 5.0f : 0.0f, 0}));
    }
    // A matrix: its last row moves.
    Ogawa file;
    const u64 c = curves(file, {0, 0, 0, 1, 0, 0}, {2});
    const u64 moved = object(file, xform(file, {0x30}, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1}, false), {c});
    Compound none;
    const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {moved})));
    GIANT_CHECK(r.ok() && r.curves.size() == 1);
    if (r.curves.size() == 1) GIANT_CHECK(near3(r.curves[0][0], {3, 4, 5}) && near3(r.curves[0][1], {4, 4, 5}));
}

GIANT_TEST("alembic: files that are not Alembic, unfinished, cut short or damaged are refused, never trusted") {
    const std::vector<u8> whole = groom(false);
    GIANT_CHECK(!read_alembic_curves({}).ok());
    const std::string words = "glTF is not Alembic at all";
    GIANT_CHECK(!read_alembic_curves(std::span<const u8>(reinterpret_cast<const u8*>(words.data()), words.size())).ok());
    std::vector<u8> unfinished = whole;
    unfinished[5] = 0;
    GIANT_CHECK(!read_alembic_curves(unfinished).ok());
    // Cut short anywhere: the root group is written last, so every cut loses it.
    for (usize size = 0; size < whole.size(); ++size) {
        const AlembicCurves r = read_alembic_curves(std::span<const u8>(whole.data(), size));
        GIANT_CHECK(!r.ok() && r.curves.empty());
    }
    // Any byte changed: read or refused, and never more than the file holds.
    u32 state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (u32 i = 0; i < 20000; ++i) {
        std::vector<u8> damaged = whole;
        const u32 changes = 1 + next() % 3;
        for (u32 c = 0; c < changes; ++c) damaged[16 + next() % (damaged.size() - 16)] = static_cast<u8>(next());
        const AlembicCurves r = read_alembic_curves(damaged);
        GIANT_CHECK(r.ok() || r.curves.empty());
        GIANT_CHECK(r.curves.size() <= 8);
    }
    // A group that holds itself is stopped by the depth limit.
    Ogawa loop;
    Compound none;
    const u64 properties = none.write(loop);
    const u64 names = loop.data(std::vector<u8>(32, 0));
    const u64 self = loop.bytes.size();
    loop.group({properties, self, self, names});
    const AlembicCurves r = read_alembic_curves(loop.finish(self));
    GIANT_CHECK(!r.ok());
}

GIANT_TEST("alembic: an object said to be shown is, under a hidden one") {
    for (const int visible : {1, -1}) {
        Ogawa file;
        const u64 c = curves(file, {0, 0, 0, 1, 0, 0}, {2});
        const u64 inner = object(file, xform(file, {}, {}, false, visible), {c});
        const u64 outer = object(file, xform(file, {}, {}, false, 0), {inner});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {outer})));
        GIANT_CHECK(r.ok() && r.curves.size() == (visible == 1 ? 1u : 0u));
    }
    // Guides under a hidden object are not counted.
    Ogawa file;
    CurveOptions marked;
    marked.guide = {1};
    const u64 c = curves(file, {0, 0, 0, 1, 0, 0}, {2}, marked);
    const u64 outer = object(file, xform(file, {}, {}, false, 0), {c});
    Compound none;
    const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {outer, c})));
    GIANT_CHECK(r.ok() && r.curves.empty() && r.guides == 1);
}

GIANT_TEST("alembic: the work a file can ask for is bounded") {
    // A transform of more steps than Alembic can write (255) is ignored,
    // not worked through: 255 turns of a degree about x are taken.
    for (const usize steps : {usize{255}, usize{256}}) {
        Ogawa file;
        const u64 c = curves(file, {0, 0, 0, 0, 1, 0}, {2});
        const u64 turned =
            object(file, xform(file, std::vector<u8>(steps, 0x40), std::vector<double>(steps, 1.0), false), {c});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {turned})));
        GIANT_CHECK(r.ok() && r.curves.size() == 1 && r.warnings.size() == (steps == 256 ? 1u : 0u));
        if (r.curves.size() == 1) {
            GIANT_CHECK(near3(r.curves[0][1], steps == 256 ? Vec3{0, 1, 0} : Vec3{0, -0.258819f, -0.965926f}, 1e-3f));
        }
    }
    // Numbers too small to matter are none (they are slow to work with): a
    // scale of 1e-120 and one of 1e120 after it do not give 1 back.
    {
        Ogawa file;
        const u64 c = curves(file, {1, 0, 0, 1, 1, 0}, {2});
        const u64 scaled = object(file, xform(file, {0x00, 0x00}, {1e120, 1, 1, 1e-120, 1, 1}, false), {c});
        Compound none;
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, none.write(file), {scaled})));
        GIANT_CHECK(r.ok() && r.curves.size() == 1);
        if (r.curves.size() == 1) GIANT_CHECK(near3(r.curves[0][0], {0, 0, 0}) && near3(r.curves[0][1], {0, 1, 0}));
    }
    // One empty object listed 65536 times is as many as are read; once more is refused.
    for (const usize listings : {usize{65535}, usize{65536}}) {
        Ogawa file;
        Compound none;
        const u64 properties = none.write(file);
        const u64 leaf = object(file, properties, {});
        const AlembicCurves r = read_alembic_curves(file.finish(object(file, properties, std::vector<u64>(listings, leaf))));
        GIANT_CHECK(r.ok() == (listings == 65535));
    }
    // An object of many children listed many times over: refused.
    {
        Ogawa file;
        Compound none;
        const u64 properties = none.write(file);
        std::vector<u64> many(4096, k_data);
        many[0] = properties;
        const u64 wide = file.group(many);
        std::vector<u64> listed(4098, wide);
        listed[0] = properties;
        listed.back() = file.data(std::vector<u8>(32, 0));
        const AlembicCurves r = read_alembic_curves(file.finish(file.group(listed)));
        GIANT_CHECK(!r.ok());
    }
    // Objects nested 64 deep are read; deeper than any real file's are refused.
    for (const int depth : {64, 65}) {
        Ogawa file;
        Compound none;
        const u64 properties = none.write(file);
        u64 inner = curves(file, {0, 0, 0, 1, 0, 0}, {2});
        for (int i = 0; i < depth; ++i) inner = object(file, properties, {inner});
        const AlembicCurves r = read_alembic_curves(file.finish(inner));
        GIANT_CHECK(r.ok() == (depth == 64) && r.curves.size() == (depth == 64 ? 1u : 0u));
    }
}

GIANT_TEST("alembic: a file is read from disk; a missing one and the old HDF5 kind say so") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("giant_alembic_tests_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    const std::vector<u8> whole = groom(false);
    {
        std::ofstream out(dir / "groom.abc", std::ios::binary);
        out.write(reinterpret_cast<const char*>(whole.data()), static_cast<std::streamsize>(whole.size()));
    }
    const AlembicCurves r = read_alembic_curves_file(dir / "groom.abc");
    GIANT_CHECK(r.ok() && r.curves.size() == 2 && r.guides == 2);
    GIANT_CHECK(!read_alembic_curves_file(dir / "none.abc").ok());
    GIANT_CHECK(!read_alembic_curves_file(dir).ok());
    const std::vector<u8> old{0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0, 0, 0, 0, 0};
    GIANT_CHECK(read_alembic_curves(old).error.find("HDF5") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

GIANT_TEST("alembic: curves turn to the engine's space") {
    std::vector<std::vector<Vec3>> curves{{{100, 200, 300}, {0, 0, 150}}};
    to_engine_space(curves, 0.01f, FileAxes::z_up);
    // Centimetres with z up: z becomes height, -y forward.
    GIANT_CHECK(near3(curves[0][0], {1, 3, -2}) && near3(curves[0][1], {0, 1.5f, 0}));
    to_engine_space(curves, 2.0f, FileAxes::y_up);
    GIANT_CHECK(near3(curves[0][0], {2, 6, -4}));
    // Unreal's left-handed z up: +y forward.
    curves = {{{100, 200, 300}}};
    to_engine_space(curves, 0.01f, FileAxes::z_up_left);
    GIANT_CHECK(near3(curves[0][0], {1, 3, 2}));
}
