#include "giant/geometry/alembic.h"

#include "giant/geometry/gltf.h" // the curve limits shared with glTF's lines

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string_view>

// The Ogawa layout, as Alembic 1.5 and later write it (little endian):
//
//   file     "Ogawa", a byte 0xff once the writer finished, a u16 version,
//            the u64 place of the root group
//   group    a u64 child count, then a u64 per child: its place in the
//            file, the top bit set when the child is data, not a group
//            (place 0 is an empty one)
//   data     a u64 size, then its bytes
//
// and Alembic's use of it:
//
//   root     [0] and [1] versions, [2] the top object, [3] the archive's
//            words, [4] time samplings, [5] shared property words
//   object   [0] its properties (a compound), then its child objects, and
//            last a data of the children's names (ending in 32 hash bytes)
//   compound its properties, and last a data of their headers
//   property (scalar or array) per sample a data of a 16-byte key and the
//            values; an array has a data of its dimensions after each
//
// A property header is a u32 of bits (0-1 kind: 0 compound, 1 scalar, else
// array; 2-3 the width of the sizes that follow: 1, 2 or 4 bytes; 4-7 the
// value type; 8 and 9 which sample fields follow; 12-19 the values per
// element; 20-27 the index of its shared words, 0xff when its own follow),
// the sample fields, then its name's size and name.

namespace giant::geometry {

namespace {

constexpr u64 k_data_bit = u64{1} << 63;
constexpr u32 k_max_depth = 64;
constexpr u32 k_max_objects = 65536;
constexpr u32 k_max_properties = 1u << 20;
constexpr u64 k_max_children = u64{1} << 24; // child places looked at, over all objects
constexpr usize k_max_transform_ops = 255;   // (Alembic's own limit: their count is a byte)
// Curves and points looked at, kept or not (one array may be shown by many
// objects, and guides and curves of no length cost their reading too).
constexpr u64 k_max_looked_at = 8 * k_max_import_curve_points;

enum class Pod : u8 { boolean, u8, i8, u16, i16, u32, i32, u64, i64, f16, f32, f64, string, wstring };

// Row vectors, as Alembic's: a point is moved by p * M, translation in the
// last row.
struct Mat {
    std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double at(usize r, usize c) const { return m[r * 4 + c]; }
    double& at(usize r, usize c) { return m[r * 4 + c]; }
};

// A number too small to matter is none: numbers near the smallest a double
// (or a float) can hold cost a processor some hundred times a sum's time,
// which a file could ask for millions of times. With every transform value
// 0 or at least this, no product or sum of two can come near them.
double settled(double v, double least = 1e-100) { return std::fabs(v) < least ? 0.0 : v; }

Mat operator*(const Mat& a, const Mat& b) {
    Mat out;
    for (usize r = 0; r < 4; ++r) {
        for (usize c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (usize k = 0; k < 4; ++k) sum += a.at(r, k) * b.at(k, c);
            out.at(r, c) = settled(sum);
        }
    }
    return out;
}

Mat rotation(double x, double y, double z, double degrees) {
    Mat out;
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.0) || !std::isfinite(len) || !std::isfinite(degrees)) return out;
    x /= len;
    y /= len;
    z /= len;
    const double a = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    out.at(0, 0) = t * x * x + c;
    out.at(0, 1) = t * x * y + s * z;
    out.at(0, 2) = t * x * z - s * y;
    out.at(1, 0) = t * x * y - s * z;
    out.at(1, 1) = t * y * y + c;
    out.at(1, 2) = t * y * z + s * x;
    out.at(2, 0) = t * x * z + s * y;
    out.at(2, 1) = t * y * z - s * x;
    out.at(2, 2) = t * z * z + c;
    return out;
}

struct Property {
    u8 kind = 0; // 0 compound, 1 scalar, else array
    Pod pod = Pod::boolean;
    u8 extent = 0;
    std::string_view name;
    u64 place = 0; // its group, or 0 when the file gives it none
};

class Reader {
public:
    Reader(std::span<const u8> data, AlembicCurves& out) : m_data(data), m_out(out) {}

    void read() {
        if (m_data.size() >= 8 && std::memcmp(m_data.data(), "\x89HDF\r\n\x1a\n", 8) == 0) {
            fail("an Alembic file of the old HDF5 kind: export it again as Ogawa");
            return;
        }
        if (m_data.size() < 16 || std::memcmp(m_data.data(), "Ogawa", 5) != 0) {
            fail("not an Alembic (Ogawa) file");
            return;
        }
        if (m_data[5] != 0xff) {
            fail("the Alembic file was never finished by its writer");
            return;
        }
        u64 count = 0, first = 0;
        if (!group(u64_at(8), count, first)) return;
        if (count < 3) {
            fail("the Alembic file has no objects");
            return;
        }
        object(child(first, 2), Mat{}, false, 0);
    }

private:
    bool fail(const char* text) {
        if (m_out.error.empty()) m_out.error = text;
        return false;
    }
    void warn_once(bool& flag, const char* text) {
        if (!flag) m_out.warnings.emplace_back(text);
        flag = true;
    }

    // (Callers have checked the place.)
    u64 u64_at(u64 at) const {
        u64 v = 0;
        std::memcpy(&v, m_data.data() + at, 8);
        return v;
    }
    u64 child(u64 first, u64 index) const { return u64_at(first + index * 8); }

    // A group's child count and where its children's places start.
    bool group(u64 place, u64& count, u64& first) {
        count = 0;
        first = 0;
        if ((place & k_data_bit) != 0) return fail("the Alembic file is damaged (data where a group belongs)");
        if (place == 0) return true;
        const u64 size = m_data.size();
        if (place > size - 8) return fail("the Alembic file is damaged (a group lies outside it)");
        count = u64_at(place);
        if (count > (size - place - 8) / 8) return fail("the Alembic file is damaged (a group runs past its end)");
        first = place + 8;
        return true;
    }

    bool data(u64 place, std::span<const u8>& bytes) {
        bytes = {};
        if ((place & k_data_bit) == 0) return fail("the Alembic file is damaged (a group where data belongs)");
        place &= ~k_data_bit;
        if (place == 0) return true;
        const u64 size = m_data.size();
        if (place > size - 8) return fail("the Alembic file is damaged (data lies outside it)");
        const u64 length = u64_at(place);
        if (length > size - place - 8) return fail("the Alembic file is damaged (data runs past its end)");
        bytes = m_data.subspan(static_cast<usize>(place + 8), static_cast<usize>(length));
        return true;
    }

    // A compound's properties, each with its place.
    bool properties(u64 place, std::vector<Property>& out) {
        out.clear();
        u64 count = 0, first = 0;
        if (!group(place, count, first)) return false;
        if (count == 0) return true;
        const u64 last = child(first, count - 1);
        if ((last & k_data_bit) == 0) return true; // (no headers: nothing to name them by)
        std::span<const u8> headers;
        if (!data(last, headers)) return false;
        usize at = 0;
        const auto sized = [&](u32 hint, u32& value) {
            const usize width = hint == 0 ? 1 : hint == 1 ? 2 : 4;
            if (headers.size() - at < width) return false;
            value = 0;
            std::memcpy(&value, headers.data() + at, width);
            at += width;
            return true;
        };
        while (at < headers.size()) {
            if (++m_properties > k_max_properties) return fail("the Alembic file has too many properties");
            u32 info = 0;
            if (headers.size() - at < 4) return fail("the Alembic file is damaged (a property header is cut short)");
            std::memcpy(&info, headers.data() + at, 4);
            at += 4;
            const u32 hint = (info >> 2) & 3;
            Property p;
            p.kind = static_cast<u8>(info & 3);
            u32 skipped = 0, name_size = 0;
            bool whole = true;
            if (p.kind != 0) {
                const u32 pod = (info >> 4) & 15;
                p.pod = pod <= static_cast<u32>(Pod::wstring) ? static_cast<Pod>(pod) : Pod::wstring;
                p.extent = static_cast<u8>((info >> 12) & 0xff);
                whole = sized(hint, skipped); // the next sample's index
                if ((info & 0x200) != 0) whole = whole && sized(hint, skipped) && sized(hint, skipped);
                if ((info & 0x100) != 0) whole = whole && sized(hint, skipped);
            }
            whole = whole && sized(hint, name_size) && name_size <= headers.size() - at;
            if (!whole) return fail("the Alembic file is damaged (a property header is cut short)");
            p.name = std::string_view(reinterpret_cast<const char*>(headers.data() + at), name_size);
            at += name_size;
            if (((info >> 20) & 0xff) == 0xff) {
                u32 words = 0;
                if (!sized(hint, words) || words > headers.size() - at) {
                    return fail("the Alembic file is damaged (a property header is cut short)");
                }
                at += words;
            }
            if (out.size() + 1 < count) p.place = child(first, out.size());
            out.push_back(p);
        }
        return true;
    }

    // A scalar or array property's first sample, without its key.
    bool sample(const Property& p, std::span<const u8>& bytes) {
        bytes = {};
        u64 count = 0, first = 0;
        if (!group(p.place, count, first)) return false;
        if (count == 0) return true;
        if (!data(child(first, 0), bytes)) return false;
        bytes = bytes.size() > 16 ? bytes.subspan(16) : std::span<const u8>{};
        return true;
    }

    static const Property* find(const std::vector<Property>& all, std::string_view name) {
        for (const Property& p : all) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }

    // An object's own transform (identity when it has none) and whether it
    // stands on its parent's.
    bool transform(const Property& xform, Mat& local, bool& inherits) {
        std::vector<Property> parts;
        if (!properties(xform.place, parts)) return false;
        std::span<const u8> bytes;
        if (const Property* p = find(parts, ".inherits"); p != nullptr && p->kind != 0) {
            if (!sample(*p, bytes)) return false;
            inherits = bytes.empty() || bytes[0] != 0;
        }
        const Property* ops = find(parts, ".ops");
        const Property* vals = find(parts, ".vals");
        if (ops == nullptr || vals == nullptr || ops->kind == 0 || vals->kind == 0) return true;
        std::span<const u8> op_bytes, val_bytes;
        if (!sample(*ops, op_bytes) || !sample(*vals, val_bytes)) return false;
        if (vals->pod != Pod::f64) {
            warn_once(m_warned_transform, "an Alembic transform that is not doubles was ignored");
            return true;
        }
        if (op_bytes.size() > k_max_transform_ops) {
            warn_once(m_warned_transform, "an Alembic transform that could not be read was ignored");
            return true;
        }
        usize at = 0;
        const auto take = [&](double& v) {
            v = 0.0;
            if (val_bytes.size() - at < 8) return false;
            std::memcpy(&v, val_bytes.data() + at, 8);
            v = settled(v);
            at += 8;
            return true;
        };
        Mat all;
        for (const u8 op : op_bytes) {
            Mat m;
            std::array<double, 16> v{};
            const u32 kind = op >> 4;
            const usize wanted = kind == 0 || kind == 1 ? 3 : kind == 2 ? 4 : kind == 3 ? 16 : kind <= 6 ? 1 : 0;
            bool whole = wanted != 0;
            for (usize i = 0; i < wanted && whole; ++i) whole = take(v[i]);
            if (!whole) {
                warn_once(m_warned_transform, "an Alembic transform that could not be read was ignored");
                return true;
            }
            switch (kind) {
            case 0:
                m.at(0, 0) = v[0];
                m.at(1, 1) = v[1];
                m.at(2, 2) = v[2];
                break;
            case 1:
                m.at(3, 0) = v[0];
                m.at(3, 1) = v[1];
                m.at(3, 2) = v[2];
                break;
            case 2: m = rotation(v[0], v[1], v[2], v[3]); break;
            case 3: m.m = v; break;
            case 4: m = rotation(1, 0, 0, v[0]); break;
            case 5: m = rotation(0, 1, 0, v[0]); break;
            default: m = rotation(0, 0, 1, v[0]); break;
            }
            all = m * all;
        }
        local = all;
        return true;
    }

    // A curves object's strands, placed by `world`.
    bool curves(const Property& geom, const Mat& world) {
        std::vector<Property> parts;
        if (!properties(geom.place, parts)) return false;
        const Property* points = find(parts, "P");
        const Property* counts = find(parts, "nVertices");
        if (points == nullptr || counts == nullptr) return true; // (a mesh, or some other shape)
        if (points->kind < 2 || points->pod != Pod::f32 || points->extent != 3 || counts->kind < 2 ||
            (counts->pod != Pod::i32 && counts->pod != Pod::u32) || counts->extent != 1) {
            warn_once(m_warned_kind, "Alembic curves whose points are not floats were skipped");
            return true;
        }
        std::span<const u8> point_bytes, count_bytes, guide_bytes, bytes;
        usize guide_width = 1;
        if (!sample(*points, point_bytes) || !sample(*counts, count_bytes)) return false;
        if (const Property* kind = find(parts, "curveBasisAndType"); kind != nullptr && kind->kind != 0) {
            if (!sample(*kind, bytes)) return false;
            if (!bytes.empty() && bytes[0] != 1) {
                warn_once(m_warned_cubic, "curves that are not linear were read through their control points");
            }
        }
        // (The Unreal groom convention: a value a curve, or one for all.)
        if (const Property* more = find(parts, ".arbGeomParams"); more != nullptr && more->kind == 0) {
            std::vector<Property> params;
            if (!properties(more->place, params)) return false;
            if (const Property* guide = find(params, "groom_guide"); guide != nullptr) {
                // (A byte in Unreal's own files; Houdini's whole numbers are 4.)
                const usize width = guide->pod <= Pod::i8 ? 1 : guide->pod <= Pod::i16 ? 2 : guide->pod <= Pod::i32 ? 4 : 0;
                if (guide->kind >= 2 && guide->extent == 1 && width != 0) {
                    guide_width = width;
                    if (!sample(*guide, guide_bytes)) return false;
                } else {
                    warn_once(m_warned_guides, "a groom_guide that is not a whole number a curve was ignored");
                }
            }
        }
        const usize point_count = point_bytes.size() / 12;
        const usize curve_count = count_bytes.size() / 4;
        const usize guide_count = guide_bytes.size() / guide_width;
        if (!guide_bytes.empty() &&
            (guide_bytes.size() % guide_width != 0 || (guide_count != 1 && guide_count != curve_count))) {
            warn_once(m_warned_guides, "a groom_guide that is not a whole number a curve was ignored");
            guide_bytes = {};
        }
        const auto is_guide = [&](usize c) {
            if (guide_bytes.empty()) return false;
            const u8* value = guide_bytes.data() + (guide_count == 1 ? 0 : c) * guide_width;
            for (usize i = 0; i < guide_width; ++i) {
                if (value[i] != 0) return true;
            }
            return false;
        };
        usize at = 0;
        for (usize c = 0; c < curve_count; ++c) {
            if (++m_looked_at > k_max_looked_at) {
                warn_once(m_warned_work, "the Alembic file shows its curves too many times: the rest were skipped");
                return true;
            }
            i32 length_given = 0;
            std::memcpy(&length_given, count_bytes.data() + c * 4, 4);
            if (length_given < 0 || static_cast<usize>(length_given) > point_count - at) {
                warn_once(m_warned_counts, "Alembic curves with more points than their object holds were skipped");
                return true;
            }
            const usize n = static_cast<usize>(length_given);
            const usize from = at;
            at += n;
            if (is_guide(c)) {
                ++m_out.guides;
                continue;
            }
            if (n < 2) continue;
            if (m_out.curves.size() >= k_max_import_curves || n > k_max_import_curve_points - m_points) {
                warn_once(m_warned_full, "hair curves past the first 262144 (or 16 million points) were skipped");
                return true;
            }
            m_looked_at += n;
            // (Straight into the result: a curve may be most of the file.)
            std::vector<Vec3>& curve = m_out.curves.emplace_back();
            curve.reserve(n);
            bool sound = true;
            double run = 0.0;
            std::array<double, 3> last{};
            for (usize i = 0; i < n && sound; ++i) {
                std::array<float, 3> p{};
                std::memcpy(p.data(), point_bytes.data() + (from + i) * 12, 12);
                const double x = p[0], y = p[1], z = p[2];
                // (Nothing under 1e-25 m: a float's squares stay whole numbers.)
                const std::array<double, 3> placed{
                    settled(x * world.at(0, 0) + y * world.at(1, 0) + z * world.at(2, 0) + world.at(3, 0), 1e-25),
                    settled(x * world.at(0, 1) + y * world.at(1, 1) + z * world.at(2, 1) + world.at(3, 1), 1e-25),
                    settled(x * world.at(0, 2) + y * world.at(1, 2) + z * world.at(2, 2) + world.at(3, 2), 1e-25)};
                // (Beyond a float is no place either.)
                sound = std::fabs(placed[0]) < 1e30 && std::fabs(placed[1]) < 1e30 && std::fabs(placed[2]) < 1e30;
                if (!sound) break;
                if (i > 0) {
                    const double dx = placed[0] - last[0], dy = placed[1] - last[1], dz = placed[2] - last[2];
                    run += std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                last = placed;
                curve.push_back(Vec3{static_cast<float>(placed[0]), static_cast<float>(placed[1]),
                                     static_cast<float>(placed[2])});
            }
            if (!sound || !(run > 0.0)) {
                m_out.curves.pop_back();
                continue;
            }
            m_points += n;
        }
        return true;
    }

    // `hidden`: an object above is, and none between says it is shown.
    bool object(u64 place, const Mat& above, bool hidden, u32 depth) {
        if (depth > k_max_depth) return fail("the Alembic file's objects are nested too deep");
        if (++m_objects > k_max_objects) return fail("the Alembic file has too many objects");
        u64 count = 0, first = 0;
        if (!group(place, count, first)) return false;
        if (count == 0) return true;
        // (One object may be listed by many, and a list may be long.)
        m_children += count;
        if (m_children > k_max_children) return fail("the Alembic file has too many objects");
        Mat world = above;
        const u64 own = child(first, 0);
        if ((own & k_data_bit) == 0) {
            std::vector<Property> parts;
            if (!properties(own, parts)) return false;
            if (const Property* shown = find(parts, "visible"); shown != nullptr && shown->kind != 0) {
                std::span<const u8> bytes;
                if (!sample(*shown, bytes)) return false;
                // (0 hidden, 1 shown whatever is above, else as its parent.)
                if (!bytes.empty() && (bytes[0] == 0 || bytes[0] == 1)) hidden = bytes[0] == 0;
            }
            if (const Property* xform = find(parts, ".xform"); xform != nullptr && xform->kind == 0) {
                Mat local;
                bool inherits = true;
                if (!transform(*xform, local, inherits)) return false;
                world = inherits ? local * above : local;
            }
            if (const Property* geom = find(parts, ".geom"); !hidden && geom != nullptr && geom->kind == 0) {
                if (!curves(*geom, world)) return false;
            }
        }
        // (The last child is the data of the children's names.)
        for (u64 i = 1; i < count; ++i) {
            const u64 next = child(first, i);
            if ((next & k_data_bit) != 0) continue;
            if (!object(next, world, hidden, depth + 1)) return false;
        }
        return true;
    }

    std::span<const u8> m_data;
    AlembicCurves& m_out;
    u64 m_points = 0;
    u64 m_looked_at = 0;
    u64 m_children = 0;
    u32 m_objects = 0;
    u32 m_properties = 0;
    bool m_warned_transform = false;
    bool m_warned_kind = false;
    bool m_warned_cubic = false;
    bool m_warned_guides = false;
    bool m_warned_counts = false;
    bool m_warned_full = false;
    bool m_warned_work = false;
};

} // namespace

AlembicCurves read_alembic_curves(std::span<const u8> data) {
    AlembicCurves out;
    if (data.size() > k_max_alembic_bytes) {
        out.error = "the Alembic file is larger than 2 GB";
        return out;
    }
    Reader(data, out).read();
    if (!out.ok()) {
        out.curves.clear();
        out.guides = 0;
    }
    return out;
}

AlembicCurves read_alembic_curves_file(const std::filesystem::path& path) {
    AlembicCurves out;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    std::ifstream file(path, std::ios::binary);
    if (ec || !file) {
        out.error = "could not open the Alembic file";
        return out;
    }
    if (size > k_max_alembic_bytes) {
        out.error = "the Alembic file is larger than 2 GB";
        return out;
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<usize>(file.gcount()) != bytes.size()) {
        out.error = "could not read the Alembic file";
        return out;
    }
    return read_alembic_curves(bytes);
}

void to_engine_space(std::vector<std::vector<Vec3>>& curves, float scale, FileAxes axes) {
    for (std::vector<Vec3>& curve : curves) {
        for (Vec3& p : curve) {
            switch (axes) {
            case FileAxes::y_up: p = p * scale; break;
            case FileAxes::z_up: p = Vec3{p.x * scale, p.z * scale, -p.y * scale}; break;
            case FileAxes::z_up_left: p = Vec3{p.x * scale, p.z * scale, p.y * scale}; break;
            }
        }
    }
}

} // namespace giant::geometry
