#include "giant/geometry/clusters.h"
#include "giant/geometry/gltf.h"
#include "giant/geometry/mesh.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::geometry;
using namespace giant::geometry::testing;

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


bool near3(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
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

namespace {
// A triangle with uvs mapping +u to +x and up the image to +y, and with
// TANGENT `tangent` (x, y, z, w) unless it is empty.
ImportResult import_uv_triangle(const std::vector<float>& tangent, const std::string& node_extra = "") {
    std::vector<u8> bin;
    append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    append(bin, std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1});
    append(bin, std::vector<float>{0, 1, 1, 1, 0, 0});
    const usize tangent_at = bin.size();
    std::vector<float> tangents;
    for (int i = 0; i < 3 && !tangent.empty(); ++i) tangents.insert(tangents.end(), tangent.begin(), tangent.end());
    if (tangent.empty()) tangents.assign(4, 0.0f); // an unused accessor of 1
    append(bin, tangents);
    const usize index_at = bin.size();
    append(bin, std::vector<u32>{0, 1, 2});
    const bool t = !tangent.empty();
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0)") +
        node_extra + "}]," + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2)" +
        (t ? R"(,"TANGENT":3)" : "") + R"(},"indices":4}]}],)" + R"("buffers":[{"byteLength":)" +
        std::to_string(bin.size()) + "}]," + R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)" +
        R"({"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},)" +
        R"({"buffer":0,"byteOffset":)" + std::to_string(tangent_at) + R"(,"byteLength":)" +
        std::to_string(t ? 48 : 16) + "}," + R"({"buffer":0,"byteOffset":)" + std::to_string(index_at) +
        R"(,"byteLength":12}],)" +
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)" +
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},)" +
        R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},)" +
        R"({"bufferView":3,"componentType":5126,"count":)" + (t ? "3" : "1") + R"(,"type":"VEC4"},)" +
        R"({"bufferView":4,"componentType":5125,"count":3,"type":"SCALAR"}]})";
    return import_gltf_memory(make_glb(json, bin));
}
Vec3 bitangent(const Vertex& v) { return cross(v.normal, v.tangent) * v.bitangent_sign; }
} // namespace

GIANT_TEST("geometry: glTF texture coordinates and tangents are read, or tangents made from the uvs") {
    const ImportResult made = import_uv_triangle({});
    GIANT_CHECK(made.ok() && made.mesh.vertices.size() == 3);
    if (!made.ok()) return;
    GIANT_CHECK(made.has_uvs);
    GIANT_CHECK(made.mesh.vertices[0].uv == (TexCoord{0, 1}) && made.mesh.vertices[2].uv == (TexCoord{0, 0}));
    for (const Vertex& v : made.mesh.vertices) {
        GIANT_CHECK(near3(v.tangent, {1, 0, 0}) && near3(bitangent(v), {0, 1, 0})); // +u, up the image
    }
    // The file's own tangent wins, even one the uvs disagree with.
    const ImportResult given = import_uv_triangle({0, 1, 0, -1});
    GIANT_CHECK(given.ok());
    if (!given.ok()) return;
    GIANT_CHECK(near3(given.mesh.vertices[1].tangent, {0, 1, 0}) && given.mesh.vertices[1].bitangent_sign == -1.0f);
    // A mirroring node carries the tangent with the matrix and turns the
    // bitangent sign, so the bitangent is the mirrored one.
    const ImportResult mirrored = import_uv_triangle({1, 0, 0, 1}, R"(,"scale":[-1,1,1])");
    GIANT_CHECK(mirrored.ok());
    if (!mirrored.ok()) return;
    for (const Vertex& v : mirrored.mesh.vertices) {
        GIANT_CHECK(near3(v.tangent, {-1, 0, 0}) && v.bitangent_sign == -1.0f && near3(bitangent(v), {0, 1, 0}));
    }
    // Made from the uvs under the mirror, the frame matches.
    const ImportResult mirrored_made = import_uv_triangle({}, R"(,"scale":[-1,1,1])");
    GIANT_CHECK(mirrored_made.ok());
    if (!mirrored_made.ok()) return;
    for (const Vertex& v : mirrored_made.mesh.vertices) {
        GIANT_CHECK(near3(v.tangent, {-1, 0, 0}) && near3(bitangent(v), {0, 1, 0}));
    }
}

GIANT_TEST("geometry: glTF materials are read with their images, most triangles first") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_gltf_materials";
    std::filesystem::create_directories(dir / "maps");
    const std::vector<u8> glb = two_material_quad("maps/rust%20normal.png");
    {
        std::ofstream(dir / "quad.glb", std::ios::binary)
            .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    const ImportResult r = import_gltf_file(dir / "quad.glb");
    GIANT_CHECK(r.ok() && r.materials.size() == 2);
    if (!r.ok() || r.materials.size() != 2) return;
    const ImportedMaterial& rust = r.materials[0];
    GIANT_CHECK(rust.name == "rust" && rust.triangles == 2 && r.materials[1].name == "paint" &&
                r.materials[1].triangles == 1);
    GIANT_CHECK(near3(rust.base_color, {0.5f, 0.25f, 1.0f}) && std::fabs(rust.roughness - 0.3f) < 1e-6f &&
                std::fabs(rust.metallic - 0.7f) < 1e-6f && near3(rust.emissive, {4.0f, 2.0f, 0.0f}));
    GIANT_CHECK(rust.base_color_image.embedded == 0 && rust.base_color_image.path.empty());
    // The URI decoded, relative to the file's folder (the image need not exist yet).
    GIANT_CHECK(rust.normal_image.path == "maps/rust normal.png" && rust.normal_image.embedded < 0);
    GIANT_CHECK(rust.occlusion_image.embedded == 2);
    GIANT_CHECK(rust.roughness_metal_image.empty());
    // Their bytes, read from the file again (as a worker cooking them does).
    std::vector<u8> bytes;
    std::string error;
    GIANT_CHECK(read_gltf_image(dir / "quad.glb", 0, bytes, error) && std::string(bytes.begin(), bytes.end()) == "PNGDATA1");
    GIANT_CHECK(read_gltf_image(dir / "quad.glb", 2, bytes, error) && std::string(bytes.begin(), bytes.end()) == "ABC");
    GIANT_CHECK(!read_gltf_image(dir / "quad.glb", 1, bytes, error) && error.find("beside") != std::string::npos);
    GIANT_CHECK(!read_gltf_image(dir / "quad.glb", 3, bytes, error));
    // The plain one: glTF's defaults; its map on another uv set left out, with a warning.
    const ImportedMaterial& paint = r.materials[1];
    GIANT_CHECK(near3(paint.base_color, {1, 1, 1}) && paint.roughness == 1.0f && paint.metallic == 1.0f);
    GIANT_CHECK(paint.roughness_metal_image.empty());
    usize texcoord_warnings = 0;
    for (const std::string& w : r.warnings) texcoord_warnings += w.find("TEXCOORD_0") != std::string::npos ? 1u : 0u;
    GIANT_CHECK(texcoord_warnings == 1);
    GIANT_CHECK(r.default_material_triangles == 0);
    // From memory the beside-file reference is kept (resolving it is the caller's).
    const ImportResult memory = import_gltf_memory(glb);
    GIANT_CHECK(memory.ok() && memory.materials.size() == 2 && memory.materials[0].base_color_image.embedded == 0);
    // URIs that leave the folder, name a scheme or a drive, or hide a
    // separator in a backslash are left out.
    for (const char* bad : {"../secret.png", "..%2Fsecret.png", "..%5C..%5Cetc%5Cpasswd", "/etc/passwd",
                            "file:///etc/passwd", "C:/x.png", "", "maps/..", "maps/"}) {
        const std::vector<u8> outside = two_material_quad(bad);
        const ImportResult escaped = import_gltf_memory(outside);
        GIANT_CHECK(escaped.ok() && escaped.materials.size() == 2 && escaped.materials[0].normal_image.empty());
        if (!escaped.materials.empty() && !escaped.materials[0].normal_image.empty()) {
            std::fprintf(stderr, "    accepted %s as %s\n", bad, escaped.materials[0].normal_image.path.c_str());
        }
    }
}

namespace {
// The GLB with `from` in its JSON replaced by `to` (chunks rebuilt).
std::vector<u8> replace_json(const std::vector<u8>& glb, const std::string& from, const std::string& to) {
    const u32 json_length = static_cast<u32>(glb[12] | (glb[13] << 8) | (glb[14] << 16) | (glb[15] << 24));
    std::string json(glb.begin() + 20, glb.begin() + 20 + json_length);
    const usize at = json.find(from);
    if (at != std::string::npos) json.replace(at, from.size(), to);
    const std::vector<u8> bin(glb.begin() + 20 + json_length + 8, glb.end());
    return make_glb(json, bin);
}
} // namespace

GIANT_TEST("geometry: glTF data URIs decode exactly, padded or not; bad ones are refused") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_gltf_base64";
    std::filesystem::create_directories(dir);
    const auto with_occlusion = [&](const std::string& uri) {
        const std::vector<u8> out = replace_json(two_material_quad("n.png"), "data:image/png;base64,QUJD", uri);
        std::ofstream(dir / "q.glb", std::ios::binary)
            .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    };
    std::vector<u8> bytes;
    std::string error;
    with_occlusion("data:image/png;base64,QUJDRA"); // "ABCD", unpadded
    GIANT_CHECK(read_gltf_image(dir / "q.glb", 2, bytes, error) && std::string(bytes.begin(), bytes.end()) == "ABCD");
    with_occlusion("data:image/png;base64,QUJDRA=="); // padded
    GIANT_CHECK(read_gltf_image(dir / "q.glb", 2, bytes, error) && std::string(bytes.begin(), bytes.end()) == "ABCD");
    with_occlusion("data:image/png;base64,="); // nothing
    GIANT_CHECK(read_gltf_image(dir / "q.glb", 2, bytes, error) && bytes.empty());
    with_occlusion("data:image/png;base64,QUJDR"); // one char over a group: not base64
    GIANT_CHECK(!read_gltf_image(dir / "q.glb", 2, bytes, error));
    with_occlusion("data:image/png,ABC"); // not base64
    GIANT_CHECK(!read_gltf_image(dir / "q.glb", 2, bytes, error));
}

GIANT_TEST("geometry: glTF's default material is counted; lines do not count; spec-gloss is a non-metal") {
    const std::vector<u8> glb = two_material_quad("n.png");
    // The "paint" triangle without its material: default 1, rust 2.
    const ImportResult r = import_gltf_memory(replace_json(glb, R"("indices":2,"material":0})", R"("indices":2})"));
    GIANT_CHECK(r.ok() && r.materials.size() == 1 && r.materials[0].name == "rust" && r.default_material_triangles == 1);
    // Rust drawn as lines instead: no triangles, so not listed.
    const ImportResult l =
        import_gltf_memory(replace_json(glb, R"("indices":3,"material":1})", R"("indices":3,"material":1,"mode":1})"));
    GIANT_CHECK(l.ok() && l.materials.size() == 1 && l.materials[0].name == "paint");
    // Paint as specular-glossiness: its diffuse colour, no metal, roughness from glossiness.
    const ImportResult sg = import_gltf_memory(replace_json(
        glb, R"({"name":"paint","pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":0,"texCoord":1}}})",
        R"({"name":"paint","extensions":{"KHR_materials_pbrSpecularGlossiness":{"diffuseFactor":[0.2,0.4,0.6,1],)"
        R"("glossinessFactor":0.75,"diffuseTexture":{"index":0}}}})"));
    GIANT_CHECK(sg.ok() && sg.materials.size() == 2);
    if (sg.ok() && sg.materials.size() == 2) {
        const ImportedMaterial& p = sg.materials[1];
        GIANT_CHECK(near3(p.base_color, {0.2f, 0.4f, 0.6f}) && p.metallic == 0.0f && std::fabs(p.roughness - 0.25f) < 1e-6f &&
                    p.base_color_image.embedded == 0);
    }
}

GIANT_TEST("geometry: vertices pack to 28 bytes and back within a hair") {
    // Directions over the sphere, both hemispheres and the axes.
    std::vector<Vec3> dirs = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int i = 0; i < 200; ++i) {
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 200.0f;
        const float a = static_cast<float>(i) * 2.39996f;
        const float r = std::sqrt(1.0f - z * z);
        dirs.push_back({r * std::cos(a), r * std::sin(a), z});
    }
    float worst = 1.0f;
    for (const Vec3& d : dirs) {
        worst = std::min(worst, dot(unpack_octahedral(pack_octahedral(d)), d));
    }
    GIANT_CHECK(worst > 0.99999f); // under about 0.26 degrees... in practice ~0.005
    for (usize i = 0; i + 1 < dirs.size(); ++i) {
        Vertex v;
        v.position = {1.5f, -2.25f, 1e5f};
        v.normal = dirs[i];
        v.tangent = dirs[i + 1]; // not perpendicular: packing makes it so
        v.bitangent_sign = i % 2 == 0 ? 1.0f : -1.0f;
        v.uv = {0.125f * static_cast<float>(i), -3.5f};
        const Vertex back = unpack_vertex(pack_vertex(v));
        GIANT_CHECK(back.position == v.position && back.uv == v.uv && back.bitangent_sign == v.bitangent_sign);
        GIANT_CHECK(dot(back.normal, v.normal) > 0.99999f);
        GIANT_CHECK(std::fabs(dot(back.normal, back.tangent)) < 1e-3f && std::fabs(length(back.tangent) - 1.0f) < 1e-4f);
        const Vec3 expected = normalize_or_zero(v.tangent - v.normal * dot(v.normal, v.tangent));
        if (length_squared(expected) > 0.0f) GIANT_CHECK(dot(back.tangent, expected) > 0.9999f);
    }
    // Broken input still packs to a sound frame.
    Vertex bad;
    bad.normal = {};
    bad.tangent = {0, 1, 0};
    const Vertex fixed = unpack_vertex(pack_vertex(bad));
    GIANT_CHECK(near3(fixed.normal, {0, 1, 0}, 1e-4f) && std::fabs(dot(fixed.normal, fixed.tangent)) < 1e-3f);
    bad.normal = {std::nanf(""), 0, 0};
    const Vertex nan_fixed = unpack_vertex(pack_vertex(bad));
    GIANT_CHECK(near3(nan_fixed.normal, {0, 1, 0}, 1e-4f));
}

GIANT_TEST("geometry: tangents follow the uvs; mirrored uv seams split, kept tangents stay") {
    // Built-in shapes: the cube's faces run +u along each face's u axis.
    const TriangleMesh plane = make_plane();
    for (const Vertex& v : plane.vertices) {
        GIANT_CHECK(near3(v.tangent, {1, 0, 0}) && near3(bitangent(v), {0, 0, -1})); // the image's top toward -Z
    }
    for (const Vertex& v : make_cube().vertices) {
        GIANT_CHECK(std::fabs(dot(v.tangent, v.normal)) < 1e-5f && v.bitangent_sign == 1.0f);
    }
    for (const Vertex& v : make_sphere().vertices) {
        GIANT_CHECK(std::fabs(dot(v.tangent, v.normal)) < 1e-4f && std::fabs(length(v.tangent) - 1.0f) < 1e-4f);
    }
    // Two triangles sharing an edge, the second with its uvs mirrored in u
    // (a symmetric character's seam): the shared vertices split.
    TriangleMesh m;
    const Vec3 up{0, 0, 1};
    m.vertices = {{{0, 0, 0}, up, {0.5f, 1}}, {{0, 1, 0}, up, {0.5f, 0}}, {{1, 0, 0}, up, {1, 1}},
                  {{-1, 0, 0}, up, {1, 1}}};
    m.indices = {0, 2, 1, 0, 1, 3};
    std::vector<SkinWeights> weights(4);
    for (usize i = 0; i < 4; ++i) weights[i].joints[0] = static_cast<u16>(i);
    generate_tangents(m, &weights);
    GIANT_CHECK(m.vertices.size() == 6 && weights.size() == 6);
    GIANT_CHECK(weights[4].joints[0] == 0 && weights[5].joints[0] == 1); // the copies keep their weights
    for (usize t = 0; t < 2; ++t) {
        const Vertex& a = m.vertices[m.indices[t * 3]];
        for (usize k = 0; k < 3; ++k) {
            const Vertex& v = m.vertices[m.indices[t * 3 + k]];
            GIANT_CHECK(v.bitangent_sign == (t == 0 ? 1.0f : -1.0f) && near3(bitangent(v), {0, 1, 0}));
            GIANT_CHECK(near3(v.tangent, a.tangent));
        }
    }
    GIANT_CHECK(near3(m.vertices[m.indices[0]].tangent, {1, 0, 0}));
    GIANT_CHECK(near3(m.vertices[m.indices[3]].tangent, {-1, 0, 0}));
    // Kept vertices keep theirs; no uvs gives a perpendicular.
    TriangleMesh flat = make_plane();
    for (Vertex& v : flat.vertices) v.uv = {};
    flat.vertices[0].tangent = {0, 0, 1};
    const std::vector<u8> keep{1, 0, 0, 0};
    generate_tangents(flat, nullptr, keep);
    GIANT_CHECK(flat.vertices.size() == 4 && flat.vertices[0].tangent == (Vec3{0, 0, 1}));
    for (usize i = 1; i < 4; ++i) {
        GIANT_CHECK(std::fabs(dot(flat.vertices[i].tangent, flat.vertices[i].normal)) < 1e-5f &&
                    std::fabs(length(flat.vertices[i].tangent) - 1.0f) < 1e-5f);
    }
}

GIANT_TEST("geometry: duplicated vertices share one tangent frame; flat triangles add nothing") {
    // The sphere unwelded, three vertices a triangle (as unindexed files
    // are): alike vertices get one frame, so clustering merges them again.
    const TriangleMesh welded = make_sphere(32, 16);
    TriangleMesh loose;
    for (const u32 i : welded.indices) {
        Vertex v = welded.vertices[i];
        v.tangent = {1, 0, 0};
        v.bitangent_sign = 1.0f;
        loose.indices.push_back(static_cast<u32>(loose.vertices.size()));
        loose.vertices.push_back(v);
    }
    generate_tangents(loose);
    bool alike = true;
    for (usize i = 0; i < loose.vertices.size(); ++i) {
        const Vertex& v = loose.vertices[i];
        const Vertex& w = welded.vertices[welded.indices[i]];
        alike = alike && near3(v.tangent, w.tangent, 1e-5f) && v.bitangent_sign == w.bitangent_sign;
    }
    GIANT_CHECK(alike);
    GIANT_CHECK(build_clusters(loose).vertices.size() == build_clusters(welded).vertices.size());
    // A collinear triangle (zero area, uvs anything) on a quad's corner
    // leaves that corner's tangent alone.
    TriangleMesh quad = make_plane();
    const Vec3 up{0, 1, 0};
    quad.vertices.push_back({{0.5f, 0, -1.5f}, up, {1.3f, 1}});
    quad.vertices.push_back({{0.5f, 0, 2.0f}, up, {0.5f, 1.2f}});
    quad.indices.insert(quad.indices.end(), {4, 1, 5}); // vertex 1 sits on the line between
    generate_tangents(quad);
    GIANT_CHECK(near3(quad.vertices[1].tangent, {1, 0, 0}) && quad.vertices[1].bitangent_sign == 1.0f);
    GIANT_CHECK(quad.vertices.size() == 6); // and no split from it
}

GIANT_TEST("geometry: clusters keep each vertex's uv and tangent through simplification") {
    const TriangleMesh sphere = make_sphere(96, 48);
    const ClusterMesh clusters = build_clusters(sphere);
    GIANT_CHECK(clusters.levels > 1);
    // Every vertex is one of the source's (simplification only drops some).
    usize matched = 0;
    for (const Vertex& v : clusters.vertices) {
        for (const Vertex& s : sphere.vertices) {
            if (s.position == v.position && s.uv == v.uv && s.tangent == v.tangent) {
                ++matched;
                break;
            }
        }
    }
    GIANT_CHECK(matched == clusters.vertices.size());
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
    // Its buffer files, decoded (to tell when images inside them change).
    const std::vector<std::filesystem::path> buffers = gltf_buffer_files(dir / "tri.gltf");
    GIANT_CHECK(buffers.size() == 1 && buffers[0].filename() == "tri data.bin" &&
                std::filesystem::exists(buffers[0]));
    GIANT_CHECK(gltf_buffer_files(dir / "missing.gltf").empty());

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

GIANT_TEST("geometry: a skinned glTF imports its skeleton, weights, rigid parts and clips") {
    const ImportResult r = import_gltf_memory(skinned_arm());
    GIANT_CHECK(r.ok());
    GIANT_CHECK(r.skinned.has_value());
    if (!r.ok() || !r.skinned) return;
    const SkinnedImport& s = *r.skinned;
    // Parents first, whatever the skin's order.
    GIANT_CHECK(s.skeleton.size() == 2 && s.skeleton.names[0] == "shoulder" && s.skeleton.names[1] == "elbow");
    GIANT_CHECK(s.skeleton.parents == (std::vector<i32>{-1, 0}));
    GIANT_CHECK(near3(s.skeleton.rest[1].translation, {0.0f, 1.0f, 0.0f}));
    GIANT_CHECK(std::fabs(s.skeleton.inverse_bind[1].at(1, 3) + 1.0f) < 1e-6f);
    // 6 arm vertices + 3 sword ones; the arm in bind space (its node's 5 m ignored).
    GIANT_CHECK(s.mesh.mesh.vertices.size() == 9 && s.mesh.weights.size() == 9 && s.mesh.mesh.indices.size() == 15);
    const auto find = [&s](const Vec3& p) {
        for (usize v = 0; v < s.mesh.mesh.vertices.size(); ++v) {
            if (near3(s.mesh.mesh.vertices[v].position, p)) return v;
        }
        return usize{99};
    };
    const usize bottom = find({-0.1f, 0.0f, 0.0f}), middle = find({-0.1f, 1.0f, 0.0f}), top = find({0.1f, 2.0f, 0.0f});
    GIANT_CHECK(bottom < 9 && middle < 9 && top < 9);
    if (bottom >= 9 || middle >= 9 || top >= 9) return;
    // Weights: the bottom row on the shoulder, the middle split, the top on the elbow.
    GIANT_CHECK(s.mesh.weights[bottom] == (SkinWeights{{0, 0, 0, 0}, {255, 0, 0, 0}}));
    GIANT_CHECK(s.mesh.weights[middle].weights[0] + s.mesh.weights[middle].weights[1] == 255);
    GIANT_CHECK(s.mesh.weights[middle].joints[0] != s.mesh.weights[middle].joints[1]);
    GIANT_CHECK(s.mesh.weights[top] == (SkinWeights{{1, 0, 0, 0}, {255, 0, 0, 0}}));
    // The sword: all on the elbow, bound where it hangs (1.5 m up).
    const usize hilt = find({0.0f, 1.5f, 0.0f}), tip = find({1.0f, 1.5f, 0.0f});
    GIANT_CHECK(hilt < 9 && tip < 9);
    if (hilt >= 9 || tip >= 9) return;
    GIANT_CHECK(s.mesh.weights[hilt] == (SkinWeights{{1, 0, 0, 0}, {255, 0, 0, 0}}));
    // At rest (bound as it stands): the same places.
    GIANT_CHECK(r.mesh.vertices.size() == 9);
    for (usize v = 0; v < 9; ++v) GIANT_CHECK(near3(r.mesh.vertices[v].position, s.mesh.mesh.vertices[v].position));
    // Clips: names made unique; the mesh node's channel skipped with a warning.
    GIANT_CHECK(s.clips.size() == 3 && s.clips[0].name == "bend" && s.clips[1].name == "hop" &&
                s.clips[2].name == "bend (2)");
    GIANT_CHECK(s.clips[1].channels.size() == 1 && s.clips[1].channels[0].interpolation == anim::Interpolation::step);
    GIANT_CHECK(std::fabs(s.clips[0].duration - 1.0f) < 1e-6f && std::fabs(s.clips[1].duration - 0.5f) < 1e-6f);
    GIANT_CHECK(std::any_of(r.warnings.begin(), r.warnings.end(),
                            [](const std::string& w) { return w.find("not joints") != std::string::npos; }));
    // Joint reach: the shoulder's farthest vertex is the middle row's, the elbow's the sword's tip.
    GIANT_CHECK(std::fabs(s.joint_radius[0] - std::sqrt(1.01f)) < 1e-4f);
    GIANT_CHECK(std::fabs(s.joint_radius[1] - 1.5f) < 1e-4f);
    // Bent a quarter at the elbow: the arm's top right corner and the sword's tip swing round it.
    std::vector<anim::JointTransform> local(2);
    std::vector<Mat4> model(2), skin(2);
    anim::sample(s.skeleton, s.clips[0], 1.0f, local);
    anim::model_matrices(s.skeleton, local, model);
    anim::skin_matrices(s.skeleton, model, skin);
    const auto at = [&](usize v) {
        return anim::skin_point(skin, s.mesh.weights[v].joints, s.mesh.weights[v].weights, s.mesh.mesh.vertices[v].position);
    };
    GIANT_CHECK(near3(at(top), {-1.0f, 1.1f, 0.0f}));
    GIANT_CHECK(near3(at(bottom), {-0.1f, 0.0f, 0.0f})); // the shoulder's rows stay
    GIANT_CHECK(near3(at(tip), {-0.5f, 2.0f, 0.0f}));    // the sword's (1, 1.5) corner
}

GIANT_TEST("geometry: a skinned mesh shown by two nodes is read once") {
    SkinOptions o;
    o.two_nodes = true;
    const ImportResult r = import_gltf_memory(skinned_arm(o));
    // Bind space ignores the node, so a second copy would lie on the first.
    GIANT_CHECK(r.ok() && r.skinned && r.skinned->mesh.mesh.vertices.size() == 9 && r.mesh.indices.size() == 15);
}

GIANT_TEST("geometry: clip names are cut to 128 bytes on a character's start, repeats still unique") {
    SkinOptions o;
    o.clip_copies = 3;
    for (int i = 0; i < 100; ++i) o.copy_name += "\xC3\xA9"; // "a" then 100 é (2 bytes each)
    const ImportResult r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(r.ok() && r.skinned && r.skinned->clips.size() == 6);
    if (!r.ok() || !r.skinned) return;
    std::set<std::string> names;
    for (usize i = 3; i < 6; ++i) {
        const std::string& n = r.skinned->clips[i].name;
        names.insert(n);
        GIANT_CHECK(n.size() <= anim::k_max_clip_name_bytes && n.size() >= anim::k_max_clip_name_bytes - 5);
    }
    const std::string& first = r.skinned->clips[3].name;
    GIANT_CHECK(first.size() == 127 && first.back() == '\xA9'); // "a" + 63 é: the 64th would split
    GIANT_CHECK(names.size() == 3 && r.skinned->clips[4].name.ends_with(" (2)") && r.skinned->clips[5].name.ends_with(" (3)"));
}

GIANT_TEST("geometry: bad skins are refused; extra weights are cut to four") {
    SkinOptions o;
    o.bad_joint = 7;
    ImportResult r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("joint") != std::string::npos && !r.skinned && r.mesh.vertices.empty());
    o = {};
    o.ibm_count = 1;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("inverse bind") != std::string::npos);
    o = {};
    o.second_key = 0.0f; // keys not increasing
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("increasing") != std::string::npos);
    o = {};
    o.second_skin = true;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("more than one skin") != std::string::npos);
    o = {};
    o.weights_1 = true;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(r.ok() && r.skinned.has_value());
    if (r.skinned) {
        // The bottom-left vertex: 1 on the shoulder and 0.1 on the elbow, in 255ths.
        for (usize v = 0; v < r.skinned->mesh.mesh.vertices.size(); ++v) {
            if (near3(r.skinned->mesh.mesh.vertices[v].position, {-0.1f, 0.0f, 0.0f})) {
                GIANT_CHECK(r.skinned->mesh.weights[v] == (SkinWeights{{0, 1, 0, 0}, {232, 23, 0, 0}}));
            }
        }
    }
    // Unskinned files are as before.
    GIANT_CHECK(!import_triangle(true).skinned.has_value());
    // Files that would read past their data, claim huge counts, or misuse types.
    o = {};
    o.sparse_weights = true;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("sparse") != std::string::npos);
    o = {};
    o.bogus_clip = true;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("bogus") != std::string::npos);
    o = {};
    o.joints_normalized = true;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("joint indices") != std::string::npos);
    o = {};
    o.weight_scale = 3e38f; // the bottom-left vertex's shoulder weight, twice over: too large to add
    o.weights_1 = true;
    o.weights_1_joint = 1;
    o.weights_1_value = 3e38f;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("too large") != std::string::npos);
    // Many clips of one name: unique names, quickly; too many: refused.
    o = {};
    o.clip_copies = 1500;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(r.ok() && r.skinned && r.skinned->clips.size() == 1503 && r.skinned->clips.back().name == "a (1500)");
    o.clip_copies = 4100;
    r = import_gltf_memory(skinned_arm(o));
    GIANT_CHECK(!r.ok() && r.error.find("4096") != std::string::npos);
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

namespace {

// What the cut over partly loaded groups must be, cluster by cluster: in a
// loaded group whose replacement is too coarse, and either original, or its
// finer version not loaded, or that version precise enough.
std::vector<u32> brute_force_cut(const ClusterMesh& mesh, const LodView& view, const std::vector<u8>& resident) {
    std::vector<u32> out;
    for (u32 i = 0; i < mesh.clusters.size(); ++i) {
        const Cluster& c = mesh.clusters[i];
        if (resident[c.group] == 0 ||
            projected_error(mesh.groups[c.group].simplified, view) <= view.threshold_pixels) {
            continue;
        }
        if (c.refined < 0 || resident[static_cast<usize>(c.refined)] == 0 ||
            projected_error(mesh.groups[static_cast<usize>(c.refined)].simplified, view) <= view.threshold_pixels) {
            out.push_back(i);
        }
    }
    return out;
}

} // namespace

GIANT_TEST("geometry: group links run from the original triangles to the roots") {
    const ClusterMesh m = build_clusters(make_sphere(96, 48));
    const GroupLinks links = link_groups(m);
    GIANT_CHECK(links.parent_offsets.size() == m.groups.size() + 1);
    bool ok = true;
    for (u32 g = 0; g < m.groups.size(); ++g) {
        const bool root = is_root_group(m.groups[g]);
        ok = ok && root == links.parents_of(g).empty();
        for (const u32 p : links.parents_of(g)) {
            ok = ok && m.groups[p].depth > m.groups[g].depth;
            const auto back = links.children_of(p);
            ok = ok && std::find(back.begin(), back.end(), g) != back.end();
        }
    }
    GIANT_CHECK(ok);
}

GIANT_TEST("geometry: a cut over partly loaded groups stays whole") {
    const ClusterMesh m = build_clusters(make_sphere(96, 48));
    const GroupLinks links = link_groups(m);
    const LodView view = view_from({0, 0, 0.9f}, 0.5f);
    std::vector<u32> full;
    select_clusters(m, view, full);
    // Load the roots, then random groups whose parents are all loaded.
    std::vector<u8> resident(m.groups.size(), 0);
    for (u32 g = 0; g < m.groups.size(); ++g) resident[g] = is_root_group(m.groups[g]) ? 1 : 0;
    u32 seed = 12345;
    bool whole = true;
    bool wanted_ok = true;
    for (int round = 0; round < 12; ++round) {
        std::vector<u32> cut, wanted, used;
        const LodResidency residency{&resident, &wanted, &used};
        select_clusters(m, view, cut, &residency);
        std::sort(cut.begin(), cut.end());
        whole = whole && cut == brute_force_cut(m, view, resident);
        for (const u32 g : wanted) wanted_ok = wanted_ok && resident[g] == 0;
        for (const u32 g : used) wanted_ok = wanted_ok && resident[g] != 0;
        // Load half of what was wanted, when its parents are in.
        for (const u32 g : wanted) {
            seed = seed * 1664525u + 1013904223u;
            bool parents_in = true;
            for (const u32 p : links.parents_of(g)) parents_in = parents_in && resident[p] != 0;
            if (parents_in && (seed >> 16) % 2 == 0) resident[g] = 1;
        }
    }
    GIANT_CHECK(whole);
    GIANT_CHECK(wanted_ok);
    // Loading everything wanted converges on the full cut.
    for (int round = 0; round < 64; ++round) {
        std::vector<u32> cut, wanted;
        const LodResidency residency{&resident, &wanted, nullptr};
        select_clusters(m, view, cut, &residency);
        if (wanted.empty()) break;
        for (const u32 g : wanted) {
            bool parents_in = true;
            for (const u32 p : links.parents_of(g)) parents_in = parents_in && resident[p] != 0;
            if (parents_in) resident[g] = 1;
        }
    }
    std::vector<u32> converged;
    const LodResidency final_residency{&resident, nullptr, nullptr};
    select_clusters(m, view, converged, &final_residency);
    std::sort(converged.begin(), converged.end());
    std::sort(full.begin(), full.end());
    GIANT_CHECK(converged == full);
}

GIANT_TEST("geometry: locked vertices keep their place at every level") {
    // A bumpy 64 x 64 grid: plenty to simplify, with its border locked.
    const u32 n = 64;
    TriangleMesh mesh;
    std::vector<u8> locked;
    for (u32 j = 0; j <= n; ++j) {
        for (u32 i = 0; i <= n; ++i) {
            const float x = static_cast<float>(i);
            const float z = static_cast<float>(j);
            mesh.vertices.push_back({{x, 2.0f * std::sin(x * 0.3f) * std::cos(z * 0.2f), z}, {0.0f, 1.0f, 0.0f}});
            locked.push_back(i == 0 || j == 0 || i == n || j == n ? 1 : 0);
        }
    }
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const u32 a = j * (n + 1) + i;
            mesh.indices.insert(mesh.indices.end(), {a, a + n + 1, a + n + 2, a, a + n + 2, a + 1});
        }
    }
    compute_normals(mesh);
    const ClusterMesh m = build_clusters(mesh, locked);
    GIANT_CHECK(m.levels >= 3);
    std::set<std::pair<float, float>> border;
    for (usize v = 0; v < mesh.vertices.size(); ++v) {
        if (locked[v] != 0) border.insert({mesh.vertices[v].position.x, mesh.vertices[v].position.z});
    }
    for (u32 depth = 0; depth < m.levels; ++depth) {
        std::set<std::pair<float, float>> seen;
        for (const Cluster& c : m.clusters) {
            if (m.groups[c.group].depth != depth) continue;
            for (u32 k = 0; k < c.triangle_count * 3; ++k) {
                const Vec3 p = m.vertices[m.indices[c.first_index + k]].position;
                if (border.contains({p.x, p.z})) seen.insert({p.x, p.z});
            }
        }
        GIANT_CHECK(seen.size() == border.size());
    }
    // No locks: the same mesh as before the parameter existed.
    const ClusterMesh plain = build_clusters(mesh);
    const ClusterMesh empty_locks = build_clusters(mesh, {});
    GIANT_CHECK(plain.clusters.size() == empty_locks.clusters.size() && plain.indices == empty_locks.indices);
}

GIANT_TEST("geometry: skinned clusters are the original triangles alone, weights kept per vertex") {
    using namespace giant::geometry;
    // A finely cut sphere (many clusters); its top half moved by joint 1,
    // the rest by joint 0, and one vertex's twin at the same place with
    // other weights (it must not merge).
    SkinnedTriangleMesh skinned;
    skinned.mesh = make_sphere(64, 32);
    for (const Vertex& v : skinned.mesh.vertices) {
        SkinWeights w;
        w.joints = {v.position.y > 0.0f ? u16{1} : u16{0}, 0, 0, 0};
        w.weights = {255, 0, 0, 0};
        skinned.weights.push_back(w);
    }
    const SkinnedClusterMesh out = build_skinned_clusters(skinned);
    GIANT_CHECK(!out.mesh.empty());
    GIANT_CHECK(out.mesh.levels == 1);
    GIANT_CHECK(out.weights.size() == out.mesh.vertices.size());
    usize triangles = 0;
    for (const ClusterGroup& g : out.mesh.groups) {
        GIANT_CHECK(g.depth == 0 && is_root_group(g));
    }
    for (const Cluster& c : out.mesh.clusters) {
        GIANT_CHECK(c.refined == -1);
        triangles += c.triangle_count;
    }
    GIANT_CHECK(out.mesh.clusters.size() > 8);
    GIANT_CHECK(triangles == skinned.mesh.indices.size() / 3); // nothing simplified, nothing lost
    // Each vertex keeps its own weights.
    for (usize i = 0; i < out.mesh.vertices.size(); ++i) {
        const u16 want = out.mesh.vertices[i].position.y > 0.0f ? 1 : 0;
        GIANT_CHECK(out.weights[i].joints[0] == want && out.weights[i].weights[0] == 255);
    }
    // Every cut selects every cluster: one level, all terminal.
    LodView view;
    view.position = {0.0f, 0.0f, -1000.0f}; // far: a plain mesh would be one cluster
    std::vector<u32> picked;
    select_clusters(out.mesh, view, picked);
    GIANT_CHECK(picked.size() == out.mesh.clusters.size());

    // Twins: the same corner with other weights stays two vertices.
    SkinnedTriangleMesh twins;
    twins.mesh.vertices = {{{0, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}},
                           {{0, 0, 0}, {0, 0, 1}}, {{0, -1, 0}, {0, 0, 1}}};
    twins.mesh.indices = {0, 1, 2, 3, 4, 1};
    SkinWeights a, b;
    a.weights = {255, 0, 0, 0};
    b.joints = {2, 0, 0, 0};
    b.weights = {255, 0, 0, 0};
    twins.weights = {a, a, a, b, b};
    const SkinnedClusterMesh t = build_skinned_clusters(twins);
    GIANT_CHECK(t.mesh.vertices.size() == 5);
    // The same twins with equal weights merge.
    twins.weights = {a, a, a, a, b};
    GIANT_CHECK(build_skinned_clusters(twins).mesh.vertices.size() == 4);
    // Not one weight per vertex: nothing.
    twins.weights.pop_back();
    GIANT_CHECK(build_skinned_clusters(twins).mesh.empty());
    GIANT_CHECK(build_skinned_clusters({}).mesh.empty());
}

GIANT_TEST("geometry: skinned clusters with levels: the whole hierarchy, each vertex's own weights, depths left unposed") {
    using namespace giant::geometry;
    // The finely cut sphere, its top half on joint 1.
    SkinnedTriangleMesh skinned;
    skinned.mesh = make_sphere(96, 48);
    for (const Vertex& v : skinned.mesh.vertices) {
        SkinWeights w;
        w.joints = {v.position.y > 0.0f ? u16{1} : u16{0}, 0, 0, 0};
        w.weights = {255, 0, 0, 0};
        skinned.weights.push_back(w);
    }
    const SkinnedClusterMesh out = build_skinned_clusters(skinned, true);
    GIANT_CHECK(out.mesh.levels > 2 && out.weights.size() == out.mesh.vertices.size());
    // The same hierarchy as the plain mesh's (one weight a side changes no connectivity there).
    const ClusterMesh plain = build_clusters(skinned.mesh);
    GIANT_CHECK(plain.levels == out.mesh.levels && plain.groups.size() == out.mesh.groups.size());
    std::vector<usize> per_depth(out.mesh.levels, 0);
    for (const Cluster& c : out.mesh.clusters) per_depth[out.mesh.groups[c.group].depth] += c.triangle_count;
    GIANT_CHECK(per_depth[0] == skinned.mesh.indices.size() / 3 && per_depth.back() < per_depth[0] / 4);

    // A depth whose tree root projects under the threshold has nothing drawn,
    // near or far, with every group loaded or some coarse ones not.
    // (Unloaded: the depths up to 2 whole, so every loaded group's parents are loaded.)
    std::vector<u8> resident(out.mesh.groups.size(), 1);
    for (usize g = 0; g < out.mesh.groups.size(); ++g) {
        const ClusterGroup& group = out.mesh.groups[g];
        if (group.depth <= 2 && group.simplified.error < k_terminal_error) resident[g] = 0;
    }
    std::vector<u32> wanted;
    const LodResidency some{&resident, &wanted, nullptr};
    usize skipped = 0;
    for (const float distance : {0.8f, 2.0f, 6.0f, 20.0f, 80.0f, 400.0f}) {
        for (const LodResidency* residency : {static_cast<const LodResidency*>(nullptr), &some}) {
            LodView view;
            view.position = {0.0f, 0.0f, -distance};
            std::vector<u32> picked;
            select_clusters(out.mesh, view, picked, residency);
            GIANT_CHECK(!picked.empty());
            // Each depth's root bounds every group of it, as projected.
            for (const ClusterGroup& g : out.mesh.groups) {
                GIANT_CHECK(projected_error(g.simplified, view) <= projected_error(out.mesh.nodes[g.depth].bounds, view));
            }
            for (u32 d = 0; d < out.mesh.levels; ++d) {
                if (projected_error(out.mesh.nodes[d].bounds, view) > view.threshold_pixels) continue;
                ++skipped;
                for (const u32 c : picked) GIANT_CHECK(out.mesh.groups[out.mesh.clusters[c].group].depth != d);
            }
            if (distance < 1.0f && residency == nullptr) {
                for (const u32 c : picked) GIANT_CHECK(out.mesh.groups[out.mesh.clusters[c].group].depth == 0);
            }
            if (distance > 300.0f) GIANT_CHECK(picked.size() < out.mesh.clusters.size() / 8);
        }
    }
    GIANT_CHECK(skipped > 4); // (some depths do go unposed)

    // A weight seam round the middle: the top half's triangles on joint 1,
    // the rest on joint 0, the corners along the seam one each side. No
    // triangle at any level takes a corner from the other side.
    SkinnedTriangleMesh halves;
    const TriangleMesh sphere = make_sphere(96, 48);
    std::map<std::pair<u32, u16>, u32> made;
    for (usize t = 0; t + 2 < sphere.indices.size(); t += 3) {
        float y = 0.0f;
        for (usize k = 0; k < 3; ++k) y += sphere.vertices[sphere.indices[t + k]].position.y;
        const u16 joint = y > 0.0f ? 1 : 0;
        for (usize k = 0; k < 3; ++k) {
            const u32 v = sphere.indices[t + k];
            const auto [at, fresh] = made.try_emplace({v, joint}, static_cast<u32>(halves.mesh.vertices.size()));
            if (fresh) {
                halves.mesh.vertices.push_back(sphere.vertices[v]);
                SkinWeights w;
                w.joints = {joint, 0, 0, 0};
                w.weights = {255, 0, 0, 0};
                halves.weights.push_back(w);
            }
            halves.mesh.indices.push_back(at->second);
        }
    }
    const SkinnedClusterMesh split = build_skinned_clusters(halves, true);
    GIANT_CHECK(split.mesh.levels > 2 && split.weights.size() == split.mesh.vertices.size());
    usize mixed = 0, coarse = 0;
    for (const Cluster& c : split.mesh.clusters) {
        if (split.mesh.groups[c.group].depth > 0) coarse += c.triangle_count;
        for (u32 t = 0; t < c.triangle_count; ++t) {
            const u32* tri = &split.mesh.indices[c.first_index + t * 3];
            const u16 j0 = split.weights[tri[0]].joints[0];
            if (split.weights[tri[1]].joints[0] != j0 || split.weights[tri[2]].joints[0] != j0) ++mixed;
        }
    }
    GIANT_CHECK(mixed == 0 && coarse > 0);
    // Without levels: one level, terminal.
    const SkinnedClusterMesh flat = build_skinned_clusters(skinned);
    GIANT_CHECK(flat.mesh.levels == 1 && flat.mesh.groups[0].simplified.error == k_terminal_error);
}

GIANT_TEST("geometry: the sphere stays closed at every level of detail") {
    // Its uv seam and poles are duplicated vertices at bit-identical
    // positions, so simplification keeps them together: every edge of any
    // cut, loaded wholly or in part, has its twin running the other way (no
    // cracks for pixels to fall through).
    const ClusterMesh mesh = build_clusters(make_sphere());
    const GroupLinks links = link_groups(mesh);
    const auto open_edges = [&mesh](const std::vector<u32>& cut) {
        std::map<std::array<float, 6>, int> open;
        for (const u32 ci : cut) {
            const Cluster& c = mesh.clusters[ci];
            for (u32 t = 0; t < c.triangle_count; ++t) {
                Vec3 p[3];
                for (u32 k = 0; k < 3; ++k) p[k] = mesh.vertices[mesh.indices[c.first_index + t * 3 + k]].position;
                for (u32 k = 0; k < 3; ++k) {
                    const Vec3 a = p[k], b = p[(k + 1) % 3];
                    if (a == b) continue;
                    const std::array<float, 6> reverse{b.x, b.y, b.z, a.x, a.y, a.z};
                    if (const auto it = open.find(reverse); it != open.end()) {
                        if (--it->second == 0) open.erase(it);
                    } else {
                        ++open[{a.x, a.y, a.z, b.x, b.y, b.z}];
                    }
                }
            }
        }
        return open.size();
    };
    usize worst = 0;
    u32 seed = 777;
    for (int i = 0; i < 400; ++i) {
        const float d = std::pow(1.03f, static_cast<float>(i % 200)) * 0.8f;
        const float turn = static_cast<float>(i) * 0.7f;
        const LodView view =
            view_from({d * std::sin(turn), d * std::cos(turn * 0.3f) * 0.5f, d * std::cos(turn)}, i < 200 ? 1.0f : 0.37f);
        std::vector<u32> cut;
        select_clusters(mesh, view, cut);
        worst = std::max(worst, open_edges(cut));
        // Partly loaded: the roots, then some of what is wanted.
        std::vector<u8> resident(mesh.groups.size(), 0);
        for (u32 g = 0; g < mesh.groups.size(); ++g) resident[g] = is_root_group(mesh.groups[g]) ? 1 : 0;
        for (int round = 0; round < 4; ++round) {
            std::vector<u32> part, wanted;
            const LodResidency residency{&resident, &wanted, nullptr};
            select_clusters(mesh, view, part, &residency);
            worst = std::max(worst, open_edges(part));
            for (const u32 g : wanted) {
                seed = seed * 1664525u + 1013904223u;
                bool parents_in = true;
                for (const u32 parent : links.parents_of(g)) parents_in = parents_in && resident[parent] != 0;
                if (parents_in && (seed >> 16) % 2 == 0) resident[g] = 1;
            }
        }
    }
    GIANT_CHECK(worst == 0);
}
