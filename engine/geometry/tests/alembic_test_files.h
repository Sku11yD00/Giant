// Alembic (Ogawa) files written piece by piece for tests: see
// engine/geometry/src/alembic.cpp for the layout.
#pragma once

#include "giant/core/types.h"

#include <cstring>
#include <string>
#include <vector>

namespace giant::geometry::testing {

inline constexpr u64 k_data = u64{1} << 63;

template <typename T>
inline void put(std::vector<u8>& out, const T& value) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
inline std::vector<u8> bytes_of(const std::vector<T>& values) {
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
inline constexpr u32 k_bool = 0, k_u8 = 1, k_i8 = 2, k_i16 = 4, k_i32 = 6, k_f32 = 10, k_f64 = 11;

// A property header: kind 0 compound, 1 scalar, 2 array. `wide` writes
// sizes as 4 bytes with every optional field and words of its own.
inline std::vector<u8> header(u32 kind, u32 pod, u32 extent, const std::string& name, bool wide = false) {
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

inline u64 object(Ogawa& file, u64 properties, const std::vector<u64>& children) {
    std::vector<u64> all{properties};
    all.insert(all.end(), children.begin(), children.end());
    all.push_back(file.data(std::vector<u8>(32, 0))); // the children's names
    return file.group(all);
}

// A transform object's properties: its ops and their values.
inline u64 xform(Ogawa& file, const std::vector<u8>& ops, const std::vector<double>& values, bool wide, int visible = -1,
          bool inherits = true) {
    Compound x;
    x.add(file.sample({static_cast<u8>(inherits ? 1 : 0)}, false), header(1, k_bool, 1, ".inherits", wide));
    x.add(file.sample(ops, false), header(1, k_u8, static_cast<u32>(ops.size()), ".ops", wide));
    x.add(file.sample(bytes_of(values), false), header(1, k_f64, static_cast<u32>(values.size()), ".vals", wide));
    Compound own;
    own.add(x.write(file), header(0, 0, 0, ".xform", wide));
    own.add(file.sample({static_cast<u8>(visible)}, false), header(1, k_i8, 1, "visible", wide));
    return own.write(file);
}

struct CurveOptions {
    std::vector<u8> guide;  // groom_guide's bytes, when not empty
    u32 guide_pod = k_i8;
    bool wide = false;
    u8 basis = 1;           // 1 linear
    u32 point_pod = k_f32;
};

inline u64 curves(Ogawa& file, const std::vector<float>& points, const std::vector<i32>& counts, const CurveOptions& o = {}) {
    Compound geom;
    geom.add(file.sample(bytes_of(points), true), header(2, o.point_pod, 3, "P", o.wide));
    geom.add(file.sample(bytes_of(counts), true), header(2, k_i32, 1, "nVertices", o.wide));
    geom.add(file.sample({o.basis, 0, 0, 0}, false), header(1, k_u8, 4, "curveBasisAndType", o.wide));
    if (!o.guide.empty()) {
        Compound params;
        params.add(file.sample(o.guide, true), header(2, o.guide_pod, 1, "groom_guide", o.wide));
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
inline std::vector<u8> groom(bool wide, const CurveOptions& strands_given = {}) {
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

} // namespace giant::geometry::testing
