#include "giant/geometry/clusters.h"
#include "giant/geometry/gltf.h"
#include "giant/geometry/mesh.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::geometry;

namespace {

// Every triangle must wind counter-clockwise seen from outside, which is
// what back-face culling keeps; `outward` gives the outside direction.
template <typename Outward>
void check_mesh(const TriangleMesh& mesh, Outward outward) {
    GIANT_CHECK(!mesh.indices.empty() && mesh.indices.size() % 3 == 0);
    bool in_range = true;
    bool winding = true;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) {
            in_range = false;
            break;
        }
        const Vec3 pa = mesh.vertices[a].position;
        const Vec3 face = cross(mesh.vertices[b].position - pa, mesh.vertices[c].position - pa);
        const Vec3 centre = (pa + mesh.vertices[b].position + mesh.vertices[c].position) / 3.0f;
        if (dot(face, outward(centre)) <= 0.0f) {
            winding = false;
        }
    }
    GIANT_CHECK(in_range);
    GIANT_CHECK(winding);
    bool unit = true;
    for (const Vertex& v : mesh.vertices) {
        unit = unit && std::fabs(length(v.normal) - 1.0f) < 1e-4f;
    }
    GIANT_CHECK(unit);
}

// --- GLB writing for import tests ------------------------------------------

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

template <typename T>
void append(std::vector<u8>& bin, const std::vector<T>& values) {
    const usize at = bin.size();
    bin.resize(at + values.size() * sizeof(T));
    std::memcpy(bin.data() + at, values.data(), values.size() * sizeof(T));
}

std::vector<u8> make_glb(std::string json, std::vector<u8> bin) {
    while (json.size() % 4 != 0) json += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<u8> out;
    put_u32(out, 0x46546C67); // "glTF"
    put_u32(out, 2);
    put_u32(out, static_cast<u32>(12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size())));
    put_u32(out, static_cast<u32>(json.size()));
    put_u32(out, 0x4E4F534A); // JSON
    out.insert(out.end(), json.begin(), json.end());
    if (!bin.empty()) {
        put_u32(out, static_cast<u32>(bin.size()));
        put_u32(out, 0x004E4942); // BIN
        out.insert(out.end(), bin.begin(), bin.end());
    }
    return out;
}

// One triangle in the XY plane facing +Z (counter-clockwise from +Z), with
// optional normals and a node transform (JSON fragment, may be empty).
struct TriangleFile {
    std::vector<u8> bin;
    std::string json;
};
TriangleFile triangle_file(bool with_normals, const std::string& node_extra, const char* mode = "4",
                           std::vector<u32> indices = {0, 1, 2}) {
    TriangleFile f;
    append(f.bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    const usize normal_offset = f.bin.size();
    if (with_normals) {
        append(f.bin, std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1});
    }
    const usize index_offset = f.bin.size();
    append(f.bin, indices);
    const std::string normals =
        with_normals ? R"(,"NORMAL":1)" : "";
    const std::string normal_view =
        with_normals ? R"(,{"buffer":0,"byteOffset":)" + std::to_string(normal_offset) + R"(,"byteLength":36})" : "";
    const std::string normal_accessor =
        with_normals ? R"(,{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"})" : "";
    const int index_view = with_normals ? 2 : 1;
    f.json = std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)") +
             R"("nodes":[{"mesh":0)" + node_extra + "}]," +
             R"("meshes":[{"primitives":[{"attributes":{"POSITION":0)" + normals + R"(},"indices":)" +
             std::to_string(with_normals ? 2 : 1) + R"(,"mode":)" + mode + "}]}]," +
             R"("buffers":[{"byteLength":)" + std::to_string(f.bin.size()) + "}]," +
             R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36})" + normal_view +
             R"(,{"buffer":0,"byteOffset":)" + std::to_string(index_offset) + R"(,"byteLength":)" +
             std::to_string(indices.size() * 4) + "}]," +
             R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})" +
             normal_accessor + R"(,{"bufferView":)" + std::to_string(index_view) +
             R"(,"componentType":5125,"count":)" + std::to_string(indices.size()) + R"(,"type":"SCALAR"}]})";
    return f;
}

ImportResult import_triangle(bool normals, const std::string& node_extra = "", const char* mode = "4",
                             std::vector<u32> indices = {0, 1, 2}) {
    TriangleFile f = triangle_file(normals, node_extra, mode, std::move(indices));
    const std::vector<u8> glb = make_glb(f.json, f.bin);
    return import_gltf_memory(glb);
}

Vec3 face_normal(const TriangleMesh& m, usize t) {
    const Vec3 a = m.vertices[m.indices[t * 3]].position;
    const Vec3 b = m.vertices[m.indices[t * 3 + 1]].position;
    const Vec3 c = m.vertices[m.indices[t * 3 + 2]].position;
    return normalize_or_zero(cross(b - a, c - a));
}

// --- Cluster helpers -------------------------------------------------------

LodView view_from(const Vec3& camera, float threshold) {
    LodView v;
    v.position = -camera; // mesh at the origin, seen from `camera`
    v.pixels_per_unit = 0.5f / std::tan(degrees_to_radians(30.0f)) * 1080.0f;
    v.threshold_pixels = threshold;
    return v;
}

std::vector<u32> brute_force_cut(const ClusterMesh& mesh, const LodView& view) {
    std::vector<u32> out;
    for (u32 i = 0; i < mesh.clusters.size(); ++i) {
        const Cluster& c = mesh.clusters[i];
        const bool coarse_too_coarse = projected_error(mesh.groups[c.group].simplified, view) > view.threshold_pixels;
        const bool fine_enough = c.refined < 0 ||
                                 projected_error(mesh.groups[static_cast<usize>(c.refined)].simplified, view) <=
                                     view.threshold_pixels;
        if (coarse_too_coarse && fine_enough) {
            out.push_back(i);
        }
    }
    return out;
}

double cut_area(const ClusterMesh& mesh, const std::vector<u32>& cut) {
    double area = 0.0;
    for (const u32 index : cut) {
        const Cluster& c = mesh.clusters[index];
        for (u32 t = 0; t < c.triangle_count; ++t) {
            const Vec3 a = mesh.vertices[mesh.indices[c.first_index + t * 3]].position;
            const Vec3 b = mesh.vertices[mesh.indices[c.first_index + t * 3 + 1]].position;
            const Vec3 d = mesh.vertices[mesh.indices[c.first_index + t * 3 + 2]].position;
            area += 0.5 * static_cast<double>(length(cross(b - a, d - a)));
        }
    }
    return area;
}

u32 cut_triangles(const ClusterMesh& mesh, const std::vector<u32>& cut) {
    u32 n = 0;
    for (const u32 i : cut) n += mesh.clusters[i].triangle_count;
    return n;
}

} // namespace

GIANT_TEST("geometry: built-in meshes face outwards with unit normals") {
    check_mesh(make_cube(), [](const Vec3& c) { return c; });
    check_mesh(make_sphere(), [](const Vec3& c) { return c; });
    check_mesh(make_sphere(3, 2), [](const Vec3& c) { return c; });
    check_mesh(make_plane(), [](const Vec3&) { return Vec3{0, 1, 0}; });
    for (const Vertex& v : make_sphere().vertices) {
        GIANT_CHECK_NEAR(length(v.position), 0.5, 1e-5);
    }
    GIANT_CHECK(make_cube().indices.size() == 36);
}

GIANT_TEST("geometry: smooth normals are computed from the triangles") {
    TriangleMesh sphere = make_sphere(32, 16);
    for (Vertex& v : sphere.vertices) v.normal = {};
    sphere.vertices.push_back({{5, 5, 5}, {}}); // unused
    compute_normals(sphere);
    std::vector<bool> used(sphere.vertices.size());
    for (const u32 i : sphere.indices) used[i] = true;
    bool outward = true;
    for (usize i = 0; i + 1 < sphere.vertices.size(); ++i) {
        if (!used[i]) continue;
        const Vertex& v = sphere.vertices[i];
        outward = outward && dot(v.normal, normalize_or_zero(v.position)) > 0.95f; // poles see one triangle
    }
    GIANT_CHECK(outward);
    GIANT_CHECK(sphere.vertices.back().normal == (Vec3{0, 1, 0}));
}

GIANT_TEST("geometry: a GLB triangle imports with its node transform") {
    const ImportResult r = import_triangle(true, R"(,"translation":[10,0,0],"scale":[2,2,2])");
    GIANT_CHECK(r.ok());
    if (!r.ok()) {
        std::fprintf(stderr, "    %s\n", r.error.c_str());
        return;
    }
    GIANT_CHECK(r.mesh.indices.size() == 3 && r.mesh.vertices.size() == 3);
    GIANT_CHECK(r.mesh.vertices[1].position == (Vec3{12, 0, 0}));
    GIANT_CHECK(r.mesh.vertices[2].position == (Vec3{10, 2, 0}));
    GIANT_CHECK_NEAR(r.mesh.vertices[0].normal.z, 1.0, 1e-6);
    GIANT_CHECK_NEAR(face_normal(r.mesh, 0).z, 1.0, 1e-6);
}

GIANT_TEST("geometry: a mirrored node keeps triangles facing out") {
    // Mirroring X flips the winding; the importer rewinds so the face still
    // points along its (transformed) normal.
    const ImportResult r = import_triangle(true, R"(,"scale":[-1,1,1])");
    GIANT_CHECK(r.ok());
    if (!r.ok()) return;
    GIANT_CHECK_NEAR(face_normal(r.mesh, 0).z, 1.0, 1e-6);
    GIANT_CHECK_NEAR(r.mesh.vertices[0].normal.z, 1.0, 1e-6);
    // A rotated node turns normals with it: 90 degrees about Y takes +Z to +X.
    const ImportResult turned = import_triangle(true, R"(,"rotation":[0,0.70710678,0,0.70710678])");
    GIANT_CHECK(turned.ok());
    if (!turned.ok()) return;
    GIANT_CHECK_NEAR(turned.mesh.vertices[0].normal.x, 1.0, 1e-5);
    GIANT_CHECK_NEAR(face_normal(turned.mesh, 0).x, 1.0, 1e-5);
}

GIANT_TEST("geometry: a file without normals is flat shaded") {
    const ImportResult r = import_triangle(false);
    GIANT_CHECK(r.ok());
    if (!r.ok()) return;
    GIANT_CHECK(r.mesh.vertices.size() == 3);
    for (const Vertex& v : r.mesh.vertices) {
        GIANT_CHECK_NEAR(v.normal.z, 1.0, 1e-6);
    }
}

GIANT_TEST("geometry: strips and fans become triangle lists") {
    // Four vertices would be needed for two triangles; with three, a strip
    // and a fan each make one triangle that must face +Z.
    const ImportResult strip = import_triangle(true, "", "5");
    const ImportResult fan = import_triangle(true, "", "6");
    GIANT_CHECK(strip.ok() && fan.ok());
    if (!strip.ok() || !fan.ok()) return;
    GIANT_CHECK(strip.mesh.indices.size() == 3 && fan.mesh.indices.size() == 3);
    GIANT_CHECK_NEAR(face_normal(strip.mesh, 0).z, 1.0, 1e-6);
    GIANT_CHECK_NEAR(face_normal(fan.mesh, 0).z, 1.0, 1e-6);

    // A four-vertex strip: its second triangle is stored the other way
    // round and must still face +Z.
    std::vector<u8> bin;
    append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":5}]}],)"
        R"("buffers":[{"byteLength":48}],"bufferViews":[{"buffer":0,"byteLength":48}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";
    const ImportResult quad = import_gltf_memory(make_glb(json, bin));
    GIANT_CHECK(quad.ok() && quad.mesh.indices.size() == 6);
    if (quad.ok() && quad.mesh.indices.size() == 6) {
        GIANT_CHECK_NEAR(face_normal(quad.mesh, 0).z, 1.0, 1e-6);
        GIANT_CHECK_NEAR(face_normal(quad.mesh, 1).z, 1.0, 1e-6);
    }
    // Points are skipped with a warning, leaving nothing: an error.
    const ImportResult points = import_triangle(true, "", "0");
    GIANT_CHECK(!points.ok() && !points.warnings.empty());
}

GIANT_TEST("geometry: bad glTF is refused with a reason") {
    GIANT_CHECK(!import_gltf_memory(std::vector<u8>{1, 2, 3}).ok());
    const std::string text = "{ not json";
    GIANT_CHECK(!import_gltf_memory({reinterpret_cast<const u8*>(text.data()), text.size()}).ok());
    // An index past the vertices.
    const ImportResult bad_index = import_triangle(true, "", "4", {0, 1, 7});
    GIANT_CHECK(!bad_index.ok());
    // A compressed file it cannot decode.
    TriangleFile f = triangle_file(true, "");
    f.json.insert(1, R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)");
    const ImportResult draco = import_gltf_memory(make_glb(f.json, f.bin));
    GIANT_CHECK(!draco.ok() && draco.error.find("compress") != std::string::npos);
    // External buffers cannot be read from memory.
    TriangleFile external = triangle_file(true, "");
    const std::string from = R"("buffers":[{"byteLength":)";
    external.json.replace(external.json.find(from), from.size(), R"("buffers":[{"uri":"tri.bin","byteLength":)");
    const ImportResult no_file = import_gltf_memory(
        {reinterpret_cast<const u8*>(external.json.data()), external.json.size()});
    GIANT_CHECK(!no_file.ok() && no_file.error.find("external") != std::string::npos);
}

GIANT_TEST("geometry: a .gltf file reads its buffer from next to it") {
    TriangleFile f = triangle_file(true, "");
    const std::string from = R"("buffers":[{"byteLength":)";
    f.json.replace(f.json.find(from), from.size(), R"("buffers":[{"uri":"tri%20data.bin","byteLength":)");
    // A non-ASCII folder: paths must be UTF-8 everywhere.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"giant_gltf_é");
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "tri.gltf", std::ios::binary) << f.json;
        std::ofstream bin(dir / "tri data.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char*>(f.bin.data()), static_cast<std::streamsize>(f.bin.size()));
    }
    const ImportResult r = import_gltf_file(dir / "tri.gltf");
    GIANT_CHECK(r.ok());
    if (!r.ok()) std::fprintf(stderr, "    %s\n", r.error.c_str());
    GIANT_CHECK(r.mesh.indices.size() == 3);
    GIANT_CHECK(!import_gltf_file(dir / "missing.gltf").ok());

    // A buffer file shorter than its byteLength must not be read past.
    {
        std::ofstream bin(dir / "tri data.bin", std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char*>(f.bin.data()), 4);
    }
    const ImportResult short_bin = import_gltf_file(dir / "tri.gltf");
    GIANT_CHECK(!short_bin.ok() && short_bin.error.find("shorter") != std::string::npos);

    // A buffer URI may not leave the file's folder.
    TriangleFile outside = triangle_file(true, "");
    outside.json.replace(outside.json.find(from), from.size(),
                         R"("buffers":[{"uri":"../giant_outside.bin","byteLength":)");
    std::ofstream(dir / "outside.gltf", std::ios::binary) << outside.json;
    {
        std::ofstream bin(dir.parent_path() / "giant_outside.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char*>(outside.bin.data()),
                  static_cast<std::streamsize>(outside.bin.size()));
    }
    const ImportResult escaped = import_gltf_file(dir / "outside.gltf");
    GIANT_CHECK(!escaped.ok() && escaped.error.find("outside") != std::string::npos);
    std::filesystem::remove(dir.parent_path() / "giant_outside.bin");

    // A URI naming the folder itself is "not found", not a size error.
    TriangleFile dot = triangle_file(true, "");
    dot.json.replace(dot.json.find(from), from.size(), R"("buffers":[{"uri":".","byteLength":)");
    std::ofstream(dir / "dot.gltf", std::ios::binary) << dot.json;
    const ImportResult folder_uri = import_gltf_file(dir / "dot.gltf");
    GIANT_CHECK(!folder_uri.ok() && folder_uri.error.find("not found") != std::string::npos);

    // "link/../tri.gltf" through a symlinked folder is the file the OS
    // opens, and its buffer is found next to it. Not on Windows, which
    // removes ".." from the text before following links.
    {
        std::ofstream bin(dir / "tri data.bin", std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char*>(f.bin.data()), static_cast<std::streamsize>(f.bin.size()));
    }
    std::filesystem::create_directories(dir / "real" / "deep");
    std::filesystem::rename(dir / "tri.gltf", dir / "real" / "tri.gltf");
    std::filesystem::rename(dir / "tri data.bin", dir / "real" / "tri data.bin");
    std::error_code ec;
#ifndef _WIN32
    std::filesystem::create_directory_symlink(dir / "real" / "deep", dir / "link", ec);
#else
    ec = std::make_error_code(std::errc::not_supported);
#endif
    if (!ec) {
        const ImportResult linked = import_gltf_file(dir / "link" / ".." / "tri.gltf");
        GIANT_CHECK(linked.ok());
        if (!linked.ok()) std::fprintf(stderr, "    %s\n", linked.error.c_str());
    }
    std::filesystem::remove_all(dir);
}

GIANT_TEST("geometry: vertex data is bounded and unused vertices dropped") {
    // One POSITION/NORMAL pair shared by many primitives (as exporters write
    // multi-material meshes) and many nodes: read once per node, and only
    // the vertices triangles use are kept.
    const auto file = [](u32 vertex_count, int primitives, int nodes, bool with_data) {
        std::vector<u8> bin;
        append(bin, std::vector<float>(usize{vertex_count} * 6, 0.5f));
        const usize index_at = bin.size();
        append(bin, std::vector<u32>{0, 1, 2});
        std::string node_list, node_defs, prims;
        for (int i = 0; i < nodes; ++i) {
            node_list += (i ? "," : "") + std::to_string(i);
            node_defs += std::string(i ? "," : "") + R"({"mesh":0,"translation":[)" + std::to_string(i) + ",0,0]}";
        }
        for (int i = 0; i < primitives; ++i) {
            prims += std::string(i ? "," : "") + R"({"attributes":{"POSITION":0,"NORMAL":1},"indices":2})";
        }
        const std::string count = std::to_string(vertex_count);
        const std::string view = with_data ? R"("bufferView":0,)" : "";
        const std::string json =
            R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[)" + node_list + "]}]," +
            R"("nodes":[)" + node_defs + "]," + R"("meshes":[{"primitives":[)" + prims + "]}]," +
            R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]," +
            R"("bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(index_at) +
            R"(,"byteStride":24},{"buffer":0,"byteOffset":)" + std::to_string(index_at) +
            R"(,"byteLength":12}],)" +
            R"("accessors":[{)" + view + R"("componentType":5126,"count":)" + count + R"(,"type":"VEC3"},)" +
            R"({"bufferView":0,"byteOffset":12,"componentType":5126,"count":)" + count + R"(,"type":"VEC3"},)" +
            R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]})";
        return make_glb(json, bin);
    };
    const ImportResult shared = import_gltf_memory(file(100'000, 20, 8, true));
    GIANT_CHECK(shared.ok());
    if (!shared.ok()) std::fprintf(stderr, "    %s\n", shared.error.c_str());
    GIANT_CHECK(shared.mesh.indices.size() == 3 * 20 * 8);
    GIANT_CHECK(shared.mesh.vertices.size() <= 3 * 20 * 8); // not 16 million
    // Positions without data would let a tiny file claim millions.
    const ImportResult empty = import_gltf_memory(file(3, 1, 1, false));
    GIANT_CHECK(!empty.ok() && empty.error.find("without data") != std::string::npos);
}

GIANT_TEST("geometry: sparse data on an interleaved buffer is refused") {
    // cgltf would read the sparse values with the base view's stride.
    std::vector<u8> bin;
    append(bin, std::vector<float>{0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1});
    const usize sparse_at = bin.size();
    append(bin, std::vector<u32>{1});
    append(bin, std::vector<float>{2, 0, 0});
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)") +
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}],)" +
        R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]," +
        R"("bufferViews":[{"buffer":0,"byteLength":72,"byteStride":24},)" +
        R"({"buffer":0,"byteOffset":)" + std::to_string(sparse_at) + R"(,"byteLength":4},)" +
        R"({"buffer":0,"byteOffset":)" + std::to_string(sparse_at + 4) + R"(,"byteLength":12}],)" +
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)" +
        R"("sparse":{"count":1,"indices":{"bufferView":1,"componentType":5125},"values":{"bufferView":2}}},)" +
        R"({"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"}]})";
    const ImportResult r = import_gltf_memory(make_glb(json, bin));
    GIANT_CHECK(!r.ok() && r.error.find("sparse") != std::string::npos);
}

GIANT_TEST("geometry: clusters cover the mesh and stay within limits") {
    const TriangleMesh sphere = make_sphere(192, 96);
    const ClusterMesh m = build_clusters(sphere);
    GIANT_CHECK(!m.empty());
    GIANT_CHECK(m.source_triangles == sphere.indices.size() / 3);
    GIANT_CHECK(m.levels >= 4);
    GIANT_CHECK(m.nodes.size() >= m.levels);
    u32 original = 0;
    bool limits = true;
    bool contiguous = true;
    for (u32 g = 0; g < m.groups.size(); ++g) {
        const ClusterGroup& group = m.groups[g];
        for (u32 i = 0; i < group.cluster_count; ++i) {
            contiguous = contiguous && m.clusters[group.first_cluster + i].group == g;
        }
    }
    for (const Cluster& c : m.clusters) {
        if (c.refined < 0) original += c.triangle_count;
        limits = limits && c.triangle_count >= 1 && c.triangle_count <= k_cluster_max_triangles &&
                 c.vertex_count <= k_cluster_max_vertices &&
                 c.first_index + c.triangle_count * 3 <= m.indices.size();
        std::set<u32> distinct;
        for (u32 k = 0; k < c.triangle_count * 3; ++k) {
            const u32 v = m.indices[c.first_index + k];
            limits = limits && v < m.vertices.size();
            distinct.insert(v);
        }
        limits = limits && distinct.size() == c.vertex_count;
        // The culling sphere holds the cluster.
        for (const u32 v : distinct) {
            limits = limits && length(m.vertices[v].position - c.bounds.center) <= c.bounds.radius * 1.001f + 1e-5f;
        }
    }
    GIANT_CHECK(contiguous);
    GIANT_CHECK(limits);
    GIANT_CHECK(original == m.source_triangles); // level 0 is the original mesh
    GIANT_CHECK_NEAR(m.bounds.radius, 0.5, 0.01);

    // Coarser always means a larger error (the cut depends on it), and every
    // terminal group is marked.
    bool monotonic = true;
    for (const Cluster& c : m.clusters) {
        if (c.refined >= 0) {
            const LodBounds& fine = m.groups[static_cast<usize>(c.refined)].simplified;
            const LodBounds& coarse = m.groups[c.group].simplified;
            monotonic = monotonic && fine.error <= coarse.error;
        }
    }
    GIANT_CHECK(monotonic);
}

GIANT_TEST("geometry: the LOD cut follows distance and matches a brute-force cut") {
    const ClusterMesh m = build_clusters(make_sphere(192, 96));
    // Up close every original triangle is needed; far away only a few.
    const std::vector<u32> all = [&] {
        std::vector<u32> out;
        select_clusters(m, view_from({0, 0, 0.6f}, 0.0f), out);
        return out;
    }();
    GIANT_CHECK(cut_triangles(m, all) == m.source_triangles);
    std::vector<u32> far_away;
    select_clusters(m, view_from({0, 0, 5000.0f}, 1.0f), far_away);
    GIANT_CHECK(!far_away.empty() && cut_triangles(m, far_away) < m.source_triangles / 20);

    // The tree only prunes: it picks exactly what testing every cluster picks.
    const double sphere_area = cut_area(m, all);
    u32 previous = ~0u;
    bool same = true;
    bool shrinking = true;
    bool covered = true;
    for (const float distance : {0.8f, 2.0f, 5.0f, 20.0f, 80.0f, 400.0f}) {
        for (const float threshold : {0.5f, 1.0f, 4.0f}) {
            const LodView view = view_from({distance * 0.6f, distance * 0.48f, distance * 0.64f}, threshold);
            std::vector<u32> fast;
            select_clusters(m, view, fast);
            std::vector<u32> slow = brute_force_cut(m, view);
            std::sort(fast.begin(), fast.end());
            same = same && fast == slow;
            // Each cut covers the sphere once: no holes or doubles. (Up
            // close the error is tiny, so the area barely changes.)
            if (distance <= 2.0f && threshold <= 1.0f) {
                const double area = cut_area(m, fast);
                covered = covered && std::fabs(area - sphere_area) < sphere_area * 0.02;
            }
            if (threshold == 1.0f) {
                const u32 tris = cut_triangles(m, fast);
                shrinking = shrinking && tris <= previous;
                previous = tris;
            }
        }
    }
    GIANT_CHECK(same);
    GIANT_CHECK(covered);
    GIANT_CHECK(shrinking);

    // A bigger instance needs more detail at the same distance.
    LodView small = view_from({0, 0, 50.0f}, 1.0f);
    LodView big = small;
    big.scale = {4, 4, 4};
    big.position = {0, 0, -50.0f};
    std::vector<u32> a, b;
    select_clusters(m, small, a);
    select_clusters(m, big, b);
    GIANT_CHECK(cut_triangles(m, b) > cut_triangles(m, a));
}

GIANT_TEST("geometry: clusters outside the view are culled") {
    const ClusterMesh m = build_clusters(make_sphere(64, 32));
    // Camera at +5 on Z looking down -Z (towards the mesh), 60 degree fov.
    const Mat4 proj = perspective_reverse_z(degrees_to_radians(60.0f), 1.0f, 0.1f);
    const Frustum frustum = frustum_from_view_projection(proj);
    GIANT_CHECK(sphere_in_frustum(frustum, {0, 0, -5}, 0.5f));
    GIANT_CHECK(!sphere_in_frustum(frustum, {0, 0, 5}, 0.5f));   // behind
    GIANT_CHECK(!sphere_in_frustum(frustum, {20, 0, -5}, 0.5f)); // far right
    GIANT_CHECK(sphere_in_frustum(frustum, {3.2f, 0, -5}, 0.5f)); // touching the edge
    LodView in_front = view_from({0, 0, 5}, 1.0f);
    in_front.frustum = &frustum;
    std::vector<u32> seen;
    select_clusters(m, in_front, seen);
    GIANT_CHECK(!seen.empty());
    LodView behind = view_from({0, 0, -5}, 1.0f); // the mesh is behind the camera
    behind.frustum = &frustum;
    std::vector<u32> none;
    select_clusters(m, behind, none);
    GIANT_CHECK(none.empty());
}

GIANT_TEST("geometry: tiny and empty meshes") {
    GIANT_CHECK(build_clusters(TriangleMesh{}).empty());
    const ClusterMesh plane = build_clusters(make_plane());
    GIANT_CHECK(plane.clusters.size() == 1 && plane.levels == 1);
    std::vector<u32> cut;
    select_clusters(plane, view_from({0, 3, 0}, 1.0f), cut);
    GIANT_CHECK(cut.size() == 1);
    // Degenerate triangles and duplicate vertices are fine.
    TriangleMesh messy = make_cube();
    const usize n = messy.vertices.size();
    messy.vertices.insert(messy.vertices.end(), messy.vertices.begin(), messy.vertices.end());
    for (usize i = 0; i < 6; ++i) messy.indices.push_back(static_cast<u32>(n + i % 2));
    const ClusterMesh cube = build_clusters(messy);
    GIANT_CHECK(!cube.empty());
}
