#include "giant/geometry/gltf.h"

#include <cgltf.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace giant::geometry {

namespace {

struct FileContext {
    bool allow_files = false;      // false for memory imports: no external buffers
    std::filesystem::path folder;  // external buffers must be inside it
    u64 bytes_read = 0;            // all external buffers together
    const char* refusal = nullptr; // why a buffer was refused, for the error
};

// cgltf reads external buffers through these, so paths are UTF-8 on every
// platform (cgltf's own fopen is not, on Windows).
cgltf_result read_file(const cgltf_memory_options*, const cgltf_file_options* options,
                       const char* path, cgltf_size* size, void** data) {
    auto* context = static_cast<FileContext*>(options->user_data);
    if (!context->allow_files || path == nullptr) {
        return cgltf_result_file_not_found;
    }
    const std::string_view utf8(path);
    const std::filesystem::path file_path =
        std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).lexically_normal();
    // A buffer URI may not reach outside the file's folder ("../", absolute).
    const std::filesystem::path relative = file_path.lexically_relative(context->folder);
    if (relative.empty() || *relative.begin() == "..") {
        context->refusal = "a buffer file it refers to is outside the file's folder";
        return cgltf_result_file_not_found;
    }
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return cgltf_result_file_not_found;
    }
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<u64>(length) > k_max_import_file_bytes ||
        context->bytes_read + static_cast<u64>(length) > k_max_import_file_bytes) {
        context->refusal = "its buffer files are over 2 GiB together";
        return cgltf_result_io_error;
    }
    // cgltf passes the buffer's declared byteLength and trusts the data to
    // be that long; a shorter file must be refused here.
    if (*size != 0 && static_cast<u64>(length) < *size) {
        context->refusal = "a buffer file is shorter than the glTF says";
        return cgltf_result_data_too_short;
    }
    context->bytes_read += static_cast<u64>(length);
    void* memory = std::malloc(length > 0 ? static_cast<usize>(length) : 1);
    if (memory == nullptr) {
        return cgltf_result_out_of_memory;
    }
    file.seekg(0);
    if (length > 0 && !file.read(static_cast<char*>(memory), static_cast<std::streamsize>(length))) {
        std::free(memory);
        return cgltf_result_io_error;
    }
    *size = static_cast<cgltf_size>(length);
    *data = memory;
    return cgltf_result_success;
}

void release_file(const cgltf_memory_options*, const cgltf_file_options*, void* data) {
    std::free(data);
}

const char* describe(cgltf_result result) {
    switch (result) {
    case cgltf_result_data_too_short: return "the file is cut short";
    case cgltf_result_unknown_format: return "not a glTF or GLB file";
    case cgltf_result_invalid_json: return "the glTF JSON is malformed";
    case cgltf_result_invalid_gltf: return "the file is not valid glTF 2.0";
    case cgltf_result_file_not_found: return "a buffer file it refers to was not found";
    case cgltf_result_io_error: return "a file could not be read (or is over 2 GiB)";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 files are not supported; re-export as glTF 2.0";
    default: return "the file could not be read";
    }
}

struct DataDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The node's world matrix (column-major) applied to a point, and the matrix
// for its normals (the cofactor matrix, sign-corrected so mirrored nodes
// keep outward normals). `mirrored` is true when the determinant is negative.
struct NodeTransform {
    float m[16];
    float normal[9]; // rows
    bool mirrored = false;

    Vec3 point(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    Vec3 direction(const Vec3& n) const {
        return normalize_or_zero(Vec3{normal[0] * n.x + normal[1] * n.y + normal[2] * n.z,
                                      normal[3] * n.x + normal[4] * n.y + normal[5] * n.z,
                                      normal[6] * n.x + normal[7] * n.y + normal[8] * n.z});
    }
};

NodeTransform node_transform(const cgltf_node* node) {
    NodeTransform t;
    cgltf_node_transform_world(node, t.m);
    // a(r, c) = element at row r, column c.
    const auto a = [&t](int r, int c) { return t.m[c * 4 + r]; };
    const float det = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
                      a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
                      a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    t.mirrored = det < 0.0f;
    const float sign = t.mirrored ? -1.0f : 1.0f;
    // Cofactor matrix = det * inverse-transpose: the normal matrix up to scale.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            t.normal[r * 3 + c] = sign * (a(r1, c1) * a(r2, c2) - a(r1, c2) * a(r2, c1));
        }
    }
    return t;
}

class Importer {
public:
    explicit Importer(ImportResult& result) : m_result(result) {}

    bool node(const cgltf_node* node) {
        if (node->mesh == nullptr) {
            return true;
        }
        if (node->skin != nullptr) {
            warn_once(m_warned_skin, "skins are not imported yet: meshes are placed in their bind pose");
        }
        const NodeTransform transform = node_transform(node);
        for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) {
            if (!primitive(node->mesh->primitives[i], transform)) {
                return false;
            }
        }
        return true;
    }

private:
    void warn_once(bool& flag, const char* text) {
        if (!flag) {
            flag = true;
            m_result.warnings.emplace_back(text);
        }
    }

    bool fail(std::string text) {
        m_result.error = std::move(text);
        return false;
    }

    bool primitive(const cgltf_primitive& prim, const NodeTransform& transform) {
        if (prim.type != cgltf_primitive_type_triangles &&
            prim.type != cgltf_primitive_type_triangle_strip &&
            prim.type != cgltf_primitive_type_triangle_fan) {
            warn_once(m_warned_type, "points and lines are skipped: only triangles are imported");
            return true;
        }
        if (prim.has_draco_mesh_compression) {
            return fail("Draco-compressed meshes are not supported; re-export without compression");
        }
        if (prim.targets_count > 0) {
            warn_once(m_warned_morph, "morph targets are not imported yet");
        }
        const cgltf_accessor* positions = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
        if (positions == nullptr || positions->type != cgltf_type_vec3) {
            warn_once(m_warned_positions, "a primitive without vec3 positions was skipped");
            return true;
        }
        const cgltf_size vertex_count = positions->count;
        const cgltf_accessor* normals = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
        if (normals != nullptr && (normals->type != cgltf_type_vec3 || normals->count != vertex_count)) {
            normals = nullptr;
            warn_once(m_warned_normals, "unusable normals were replaced by flat ones");
        }
        const cgltf_size index_count = prim.indices != nullptr ? prim.indices->count : vertex_count;
        // Bound the work before allocating: any primitive type makes at
        // most index_count triangles. Vertices are counted over the whole
        // file (a mesh shared by many nodes is read once per node), since an
        // accessor without data costs nothing in the file.
        if (index_count > k_max_import_triangles * 3) {
            return fail("the mesh has more than 16 million triangles");
        }
        m_vertices_read += vertex_count;
        if (vertex_count > k_max_import_vertices || m_vertices_read > k_max_import_vertices) {
            return fail("the file has more than 48 million vertices");
        }
        // cgltf 1.15 reads sparse values with the base view's stride, but
        // the spec packs them tightly: refuse rather than read wrong data.
        for (const cgltf_accessor* a : std::initializer_list<const cgltf_accessor*>{positions, normals, prim.indices}) {
            if (a != nullptr && a->is_sparse &&
                a->stride != cgltf_calc_size(a->type, a->component_type)) {
                return fail("sparse accessors on interleaved buffers are not supported");
            }
        }

        std::vector<Vec3> p(vertex_count);
        if (vertex_count > 0 &&
            cgltf_accessor_unpack_floats(positions, &p[0].x, vertex_count * 3) != vertex_count * 3) {
            return fail("could not read vertex positions");
        }
        std::vector<Vec3> n;
        if (normals != nullptr) {
            n.resize(vertex_count);
            if (vertex_count > 0 &&
                cgltf_accessor_unpack_floats(normals, &n[0].x, vertex_count * 3) != vertex_count * 3) {
                return fail("could not read vertex normals");
            }
        }
        std::vector<u32> indices(index_count);
        if (prim.indices != nullptr) {
            if (index_count > 0 &&
                cgltf_accessor_unpack_indices(prim.indices, indices.data(), sizeof(u32), index_count) !=
                    index_count) {
                return fail("could not read triangle indices");
            }
            for (const u32 i : indices) {
                if (i >= vertex_count) {
                    return fail("a triangle index is out of range");
                }
            }
        } else {
            for (cgltf_size i = 0; i < index_count; ++i) {
                indices[i] = static_cast<u32>(i);
            }
        }
        const std::vector<u32> triangles = to_triangle_list(prim.type, indices, transform.mirrored);

        TriangleMesh& mesh = m_result.mesh;
        if ((mesh.indices.size() + triangles.size()) / 3 > k_max_import_triangles) {
            return fail("the mesh has more than 16 million triangles");
        }
        for (Vec3& v : p) {
            v = transform.point(v);
            if (!finite(v)) {
                return fail("a vertex position is not a finite number");
            }
        }
        if (normals != nullptr) {
            // Only the vertices that triangles use, so the mesh stays
            // bounded by its triangle count.
            constexpr u32 unused = ~u32{0};
            std::vector<u32> remap(vertex_count, unused);
            for (const u32 i : triangles) {
                if (remap[i] == unused) {
                    remap[i] = static_cast<u32>(mesh.vertices.size());
                    Vec3 d = transform.direction(n[i]);
                    if (!finite(d) || length_squared(d) == 0.0f) {
                        d = {0, 1, 0};
                    }
                    mesh.vertices.push_back({p[i], d});
                }
                mesh.indices.push_back(remap[i]);
            }
        } else {
            // glTF: without normals, a mesh is flat shaded.
            for (usize t = 0; t + 2 < triangles.size(); t += 3) {
                const Vec3 a = p[triangles[t]], b = p[triangles[t + 1]], c = p[triangles[t + 2]];
                Vec3 face = normalize_or_zero(cross(b - a, c - a));
                if (length_squared(face) == 0.0f) {
                    face = {0, 1, 0};
                }
                const u32 base = static_cast<u32>(mesh.vertices.size());
                mesh.vertices.push_back({a, face});
                mesh.vertices.push_back({b, face});
                mesh.vertices.push_back({c, face});
                mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
            }
        }
        return true;
    }

    static std::vector<u32> to_triangle_list(cgltf_primitive_type type, const std::vector<u32>& in,
                                             bool mirrored) {
        std::vector<u32> out;
        const auto add = [&out, mirrored](u32 a, u32 b, u32 c) {
            if (mirrored) {
                std::swap(b, c);
            }
            out.insert(out.end(), {a, b, c});
        };
        if (type == cgltf_primitive_type_triangles) {
            out.reserve(in.size());
            for (usize i = 0; i + 2 < in.size(); i += 3) {
                add(in[i], in[i + 1], in[i + 2]);
            }
        } else if (type == cgltf_primitive_type_triangle_strip) {
            for (usize i = 0; i + 2 < in.size(); ++i) {
                // Every other triangle of a strip is wound the other way.
                if (i % 2 == 0) {
                    add(in[i], in[i + 1], in[i + 2]);
                } else {
                    add(in[i + 1], in[i], in[i + 2]);
                }
            }
        } else { // fan
            for (usize i = 1; i + 1 < in.size(); ++i) {
                add(in[0], in[i], in[i + 1]);
            }
        }
        return out;
    }

    ImportResult& m_result;
    bool m_warned_skin = false;
    bool m_warned_type = false;
    bool m_warned_morph = false;
    bool m_warned_positions = false;
    bool m_warned_normals = false;
    u64 m_vertices_read = 0;
};

ImportResult import(std::span<const u8> bytes, const std::string& path_utf8, bool allow_files) {
    ImportResult result;
    FileContext context;
    context.allow_files = allow_files;
    if (allow_files) {
        const std::u8string u8(path_utf8.begin(), path_utf8.end());
        context.folder = std::filesystem::path(u8).parent_path().lexically_normal();
    }
    cgltf_options options{};
    options.file.read = &read_file;
    options.file.release = &release_file;
    options.file.user_data = &context;

    cgltf_data* raw = nullptr;
    cgltf_result r = cgltf_parse(&options, bytes.data(), bytes.size(), &raw);
    std::unique_ptr<cgltf_data, DataDeleter> data(raw);
    if (r != cgltf_result_success) {
        result.error = describe(r);
        return result;
    }
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const std::string_view name = data->extensions_required[i];
        if (name.find("compression") != std::string_view::npos) {
            result.error = "compressed glTF (" + std::string(name) +
                           ") is not supported yet; re-export without compression";
            return result;
        }
        if (name != "KHR_mesh_quantization") {
            result.warnings.push_back("the required extension " + std::string(name) +
                                      " is ignored");
        }
    }
    r = cgltf_load_buffers(&options, data.get(), path_utf8.c_str());
    if (r != cgltf_result_success) {
        result.error = context.refusal != nullptr ? context.refusal
                       : allow_files || r != cgltf_result_file_not_found
                           ? describe(r)
                           : "the file refers to external buffers; import it from a file instead";
        return result;
    }
    if (cgltf_validate(data.get()) != cgltf_result_success) {
        result.error = describe(cgltf_result_invalid_gltf);
        return result;
    }

    // The default scene, else the first, else every root node.
    std::vector<const cgltf_node*> stack;
    const cgltf_scene* scene = data->scene != nullptr        ? data->scene
                               : data->scenes_count > 0 ? &data->scenes[0]
                                                        : nullptr;
    if (scene != nullptr) {
        for (cgltf_size i = scene->nodes_count; i > 0; --i) {
            stack.push_back(scene->nodes[i - 1]);
        }
    } else {
        for (cgltf_size i = data->nodes_count; i > 0; --i) {
            if (data->nodes[i - 1].parent == nullptr) {
                stack.push_back(&data->nodes[i - 1]);
            }
        }
    }
    if (data->materials_count > 0 || data->textures_count > 0) {
        result.warnings.emplace_back(
            "materials and textures are not imported yet; set the entity's material instead");
    }
    Importer importer(result);
    // cgltf_validate rejects cycles; the visit count bounds a bad file anyway.
    usize visits = 0;
    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();
        if (++visits > data->nodes_count + 1) {
            result.error = "the node hierarchy has a cycle";
            return result;
        }
        if (!importer.node(node)) {
            result.mesh = {};
            return result;
        }
        for (cgltf_size i = node->children_count; i > 0; --i) {
            stack.push_back(node->children[i - 1]);
        }
    }
    if (result.mesh.indices.empty()) {
        result.error = "the file has no triangles";
    }
    return result;
}

} // namespace

ImportResult import_gltf_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        ImportResult result;
        result.error = "could not open the file";
        return result;
    }
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<u64>(length) > k_max_import_file_bytes) {
        ImportResult result;
        result.error = describe(cgltf_result_io_error);
        return result;
    }
    std::vector<u8> bytes(static_cast<usize>(length));
    file.seekg(0);
    if (length > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        ImportResult result;
        result.error = describe(cgltf_result_io_error);
        return result;
    }
    // Absolute, so buffer URIs can be checked against the file's folder.
    std::error_code ec;
    std::filesystem::path full = std::filesystem::absolute(path, ec);
    if (ec) {
        full = path;
    }
    const std::u8string utf8 = full.lexically_normal().u8string();
    return import(bytes, std::string(utf8.begin(), utf8.end()), true);
}

ImportResult import_gltf_memory(std::span<const u8> data) { return import(data, std::string(), false); }

} // namespace giant::geometry
