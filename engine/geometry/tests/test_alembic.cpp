#include "giant/geometry/alembic.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::geometry;

namespace {

constexpr u64 k_data = u64{1} << 63;

bool near3(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    return std::fabs(a.x - b.x) < tolerance && std::fabs(a.y - b.y) < tolerance && std::fabs(a.z - b.z) < tolerance;
}

template <typename T>
void put(std::vector<u8>& out, const T& value) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
std::vector<u8> bytes_of(const std::vector<T>& values) {
    std::vector<u8> out;
    for (const T& v : values) put(out, v);
    return out;
}

// An Ogawa file written piece by piece (see alembic.cpp for the layout).
struct Ogawa {
    std::vector<u8> bytes{'O', 'g', 'a', 'w', 'a', 0xff, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};

    u64 data(const std::vector<u8>& values) {
        if (values.empty()) return k_data;
        const u64 at = bytes.size();
        put(bytes, u64{values.size()});
        bytes.insert(bytes.end(), values.begin(), values.end());
        return at | k_data;
    }
    u64 group(const std::vector<u64>& children) {
        if (children.empty()) return 0;
        const u64 at = bytes.size();
        put(bytes, u64{children.size()});
        for (const u64 c : children) put(bytes, c);
        return at;
    }
    // A property's one sample: its key, then the values.
    u64 sample(const std::vector<u8>& values, bool array) {
        std::vector<u8> keyed(16, 0);
        keyed.insert(keyed.end(), values.begin(), values.end());
        const u64 d = data(keyed);
        return array ? group({d, data({})}) : group({d});
    }
    std::vector<u8> finish(u64 top) {
        const u64 root = group({data({0, 0, 0, 0}), data({0, 0, 0, 0}), top, data({}), data({}), data({})});
        std::memcpy(bytes.data() + 8, &root, 8);
        return bytes;
    }
};

// Value types, as Alembic numbers them.
constexpr u32 k_bool = 0, k_u8 = 1, k_i8 = 2, k_i32 = 6, k_f32 = 10, k_f64 = 11;

// A property header: kind 0 compound, 1 scalar, 2 array. `wide` writes
// sizes as 4 bytes with every optional field and words of its own.
std::vector<u8> header(u32 kind, u32 pod, u32 extent, const std::string& name, bool wide = false) {
    std::vector<u8> out;
    u32 info = kind | (pod << 4) | (extent << 12);
    if (wide) info |= (2u << 2) | (kind != 0 ? 0x300u : 0u) | (0xffu << 20);
    put(out, info);
    const auto sized = [&](u32 v) {
        if (wide) {
            put(out, v);
        } else {
            out.push_back(static_cast<u8>(v));
        }
    };
    if (kind != 0) {
        sized(1); // the next sample's index
        if (wide) {
            sized(0); // first and last changed, the time sampling
            sized(0);
            sized(0);
        }
    }
    sized(static_cast<u32>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    if (wide) {
        const std::string words = "geoScope=vtx";
        sized(static_cast<u32>(words.size()));
        out.insert(out.end(), words.begin(), words.end());
    }
    return out;
}

struct Compound {
    std::vector<u64> children;
    std::vector<u8> headers;

    void add(u64 place, const std::vector<u8>& h) {
        children.push_back(place);
        headers.insert(headers.end(), h.begin(), h.end());
    }
    u64 write(Ogawa& file) {
        std::vector<u64> all = children;
        all.push_back(file.data(headers));
        return file.group(all);
    }
};

u64 object(Ogawa& file, u64 properties, const std::vector<u64>& children) {
    std::vector<u64> all{properties};
    all.insert(all.end(), children.begin(), children.end());
    all.push_back(file.data(std::vector<u8>(32, 0))); // the children's names
    return file.group(all);
}

// A transform object's properties: its ops and their values.
u64 xform(Ogawa& file, const std::vector<u8>& ops, const std::vector<double>& values, bool wide, int visible = -1) {
    Compound x;
    x.add(file.sample({1}, false), header(1, k_bool, 1, ".inherits", wide));
    x.add(file.sample(ops, false), header(1, k_u8, static_cast<u32>(ops.size()), ".ops", wide));
    x.add(file.sample(bytes_of(values), false), header(1, k_f64, static_cast<u32>(values.size()), ".vals", wide));
    Compound own;
    own.add(x.write(file), header(0, 0, 0, ".xform", wide));
    own.add(file.sample({static_cast<u8>(visible)}, false), header(1, k_i8, 1, "visible", wide));
    return own.write(file);
}

struct CurveOptions {
    std::vector<u8> guide;  // groom_guide, when not empty
    bool wide = false;
    u8 basis = 1;           // 1 linear
    u32 point_pod = k_f32;
};

u64 curves(Ogawa& file, const std::vector<float>& points, const std::vector<i32>& counts, const CurveOptions& o = {}) {
    Compound geom;
    geom.add(file.sample(bytes_of(points), true), header(2, o.point_pod, 3, "P", o.wide));
    geom.add(file.sample(bytes_of(counts), true), header(2, k_i32, 1, "nVertices", o.wide));
    geom.add(file.sample({o.basis, 0, 0, 0}, false), header(1, k_u8, 4, "curveBasisAndType", o.wide));
    if (!o.guide.empty()) {
        Compound params;
        params.add(file.sample(o.guide, true), header(2, k_i8, 1, "groom_guide", o.wide));
        geom.add(params.write(file), header(0, 0, 0, ".arbGeomParams", o.wide));
    }
    Compound own;
    own.add(geom.write(file), header(0, 0, 0, ".geom", o.wide));
    return object(file, own.write(file), {});
}

// A groom as tools write it: a transform (10 m along z) over "guides" (two
// curves, both marked) and, under a transform that doubles, "strands"
// (three curves: one of three points up y, one of no length, one of two
// points at x 1), and a hidden transform over one more curve.
std::vector<u8> groom(bool wide, const CurveOptions& strands_given = {}) {
    Ogawa file;
    CurveOptions guides;
    guides.guide = {1, 1};
    guides.wide = wide;
    CurveOptions strands = strands_given;
    strands.wide = wide;
    const u64 g = curves(file, {0, 0, 0, 0, 1, 0, 5, 0, 0, 5, 1, 0}, {2, 2}, guides);
    const u64 s = curves(file, {0, 0, 0, 0, 1, 0, 0, 2, 0, 7, 7, 7, 7, 7, 7, 1, 0, 0, 1, 1, 0}, {3, 2, 2}, strands);
    const u64 doubled = object(file, xform(file, {0x00}, {2, 2, 2}, wide), {s});
    const u64 hidden = object(file, xform(file, {}, {}, wide, 0), {curves(file, {0, 0, 0, 0, 1, 0}, {2})});
    const u64 top_xform = object(file, xform(file, {0x10}, {0, 0, 10}, wide), {g, doubled, hidden});
    Compound none;
    return file.finish(object(file, none.write(file), {top_xform}));
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
    // A group that holds itself is stopped by the object limit.
    Ogawa loop;
    Compound none;
    const u64 properties = none.write(loop);
    const u64 names = loop.data(std::vector<u8>(32, 0));
    const u64 self = loop.bytes.size();
    loop.group({properties, self, self, names});
    const AlembicCurves r = read_alembic_curves(loop.finish(self));
    GIANT_CHECK(!r.ok());
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
