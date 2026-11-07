#include "giant/geometry/gltf.h"

#include <cgltf.h>

#include <algorithm>
#include <array>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
    const std::filesystem::path file_path(std::u8string(utf8.begin(), utf8.end()));
    // A buffer URI may not reach outside the file's folder ("../"). The
    // check is on the normalised text; the file is opened as written, so
    // a symlinked folder still resolves the way the OS sees it.
    const std::filesystem::path relative =
        file_path.lexically_normal().lexically_relative(context->folder);
    if (relative.empty() || *relative.begin() == "..") {
        context->refusal = "a buffer file it refers to is outside the file's folder";
        return cgltf_result_file_not_found;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file_path, ec)) {
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

NodeTransform matrix_transform(const float* m) {
    NodeTransform t;
    std::copy_n(m, 16, t.m);
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

NodeTransform node_transform(const cgltf_node* node) {
    float m[16];
    cgltf_node_transform_world(node, m);
    return matrix_transform(m);
}

Mat4 world_matrix(const cgltf_node* node) {
    Mat4 m;
    if (node != nullptr) cgltf_node_transform_world(node, m.m);
    return m;
}

// An accessor the skin and clip readers may unpack: it has data in the
// file (so the work is bounded by the file's size), and no sparse values on
// an interleaved view (cgltf 1.15 reads those with the wrong stride).
bool readable(const cgltf_accessor* a) {
    return a != nullptr && a->buffer_view != nullptr &&
           (!a->is_sparse || a->stride == cgltf_calc_size(a->type, a->component_type));
}

// Where a primitive's vertices go: the mesh, and (skinned) their weights:
// all on one joint (`rigid`), or read from the primitive's own joints and
// weights, which index `skin_joints` (the skin's joints, as skeleton joints).
struct Target {
    TriangleMesh* mesh = nullptr;
    std::vector<SkinWeights>* weights = nullptr;
    i32 rigid = -1;
    const std::vector<u16>* skin_joints = nullptr;
};

class Importer {
public:
    explicit Importer(ImportResult& result) : m_result(result) {}

    bool node(const cgltf_node* node) {
        if (node->mesh == nullptr) {
            return true;
        }
        Target target;
        target.mesh = &m_result.mesh;
        return mesh(node, node_transform(node), target);
    }

    // Its mesh's primitives into `target`, placed by `transform`.
    bool mesh(const cgltf_node* node, const NodeTransform& transform, const Target& target) {
        m_unpacked.clear();
        for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) {
            if (!primitive(node->mesh->primitives[i], transform, target)) {
                return false;
            }
        }
        return true;
    }

    bool fail(std::string text) {
        m_result.error = std::move(text);
        return false;
    }

    void warn_once(bool& flag, const char* text) {
        if (!flag) {
            flag = true;
            m_result.warnings.emplace_back(text);
        }
    }

private:
    // A vertex's joints and weights from up to two sets: the four heaviest,
    // in 255ths summing to 255.
    bool vertex_weights(const std::array<const std::vector<Vec4>*, 2>& joints,
                        const std::array<const std::vector<Vec4>*, 2>& weights, usize v,
                        const std::vector<u16>& skin_joints, SkinWeights& out) {
        std::array<std::pair<float, u32>, 8> found{};
        usize count = 0;
        for (usize set = 0; set < 2; ++set) {
            if (joints[set] == nullptr || weights[set] == nullptr) continue;
            const Vec4 j = (*joints[set])[v];
            const Vec4 w = (*weights[set])[v];
            const float js[4] = {j.x, j.y, j.z, j.w};
            const float ws[4] = {w.x, w.y, w.z, w.w};
            for (usize k = 0; k < 4; ++k) {
                if (!std::isfinite(ws[k]) || ws[k] < 0.0f) return fail("a vertex weight is negative or not a number");
                if (ws[k] == 0.0f) continue;
                if (!(js[k] >= 0.0f) || js[k] >= static_cast<float>(skin_joints.size())) {
                    return fail("a vertex names a joint the skin does not have");
                }
                const u32 joint = skin_joints[static_cast<usize>(js[k])];
                bool merged = false;
                for (usize i = 0; i < count; ++i) {
                    if (found[i].second == joint) {
                        found[i].first += ws[k];
                        merged = true;
                    }
                }
                if (!merged) found[count++] = {ws[k], joint};
            }
        }
        out = {};
        if (count == 0) {
            // No weight at all: on the first joint it lists.
            warn_once(m_warned_unweighted, "vertices with no joint weights were bound to their first joint");
            const float j0 = joints[0] != nullptr ? (*joints[0])[v].x : 0.0f;
            const usize index = j0 >= 0.0f && j0 < static_cast<float>(skin_joints.size()) ? static_cast<usize>(j0) : 0;
            out.joints[0] = skin_joints.empty() ? 0 : skin_joints[index];
            out.weights[0] = 255;
            return true;
        }
        std::sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(count),
                  [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        if (count > 4) {
            warn_once(m_warned_weights, "vertices moved by more than four joints keep the four heaviest");
            count = 4;
        }
        float total = 0.0f;
        for (usize i = 0; i < count; ++i) total += found[i].first;
        if (!std::isfinite(total) || !(total > 0.0f)) return fail("a vertex's weights are too large to add up");
        // 255ths by largest remainder, so they sum to 255 exactly.
        std::array<float, 4> rest{};
        u32 given = 0;
        for (usize i = 0; i < count; ++i) {
            const float share = found[i].first / total * 255.0f;
            const float whole = std::floor(share);
            out.joints[i] = static_cast<u16>(found[i].second);
            out.weights[i] = static_cast<u8>(whole);
            rest[i] = share - whole;
            given += static_cast<u32>(whole);
        }
        while (given < 255) {
            usize best = 0;
            for (usize i = 1; i < count; ++i) {
                if (rest[i] > rest[best]) best = i;
            }
            ++out.weights[best];
            rest[best] = -1.0f;
            ++given;
        }
        return true;
    }

    // A vec4 accessor as floats (joints and weights), for one primitive.
    bool unpack4(const cgltf_accessor* accessor, usize count, std::vector<Vec4>& out, bool joints) {
        out.assign(count, Vec4{});
        if (accessor->type != cgltf_type_vec4 || accessor->count != count) {
            return fail("a skinned mesh's joints or weights do not match its vertices");
        }
        if (!readable(accessor)) return fail("a skinned mesh's joints or weights have no data, or are sparse and interleaved");
        if (joints && ((accessor->component_type != cgltf_component_type_r_8u &&
                        accessor->component_type != cgltf_component_type_r_16u) ||
                       accessor->normalized)) {
            return fail("a skinned mesh's joint indices are not unsigned bytes or shorts");
        }
        if (count > 0 && cgltf_accessor_unpack_floats(accessor, &out[0].x, count * 4) != count * 4) {
            return fail("could not read joints or weights");
        }
        return true;
    }

    // A vec3 accessor as floats, read once per node (primitives often share
    // one). Positions are counted against the file's vertex budget; normals
    // match them in count, so they are not counted again.
    const std::vector<Vec3>* unpack(const cgltf_accessor* accessor, bool count_vertices) {
        if (const auto it = m_unpacked.find(accessor); it != m_unpacked.end()) {
            return &it->second;
        }
        const cgltf_size count = accessor->count;
        m_vertices_read += count_vertices ? count : 0;
        if (m_vertices_read > k_max_import_vertices) {
            fail("the file has more than 48 million vertices");
            return nullptr;
        }
        std::vector<Vec3>& out = m_unpacked[accessor];
        out.resize(count);
        if (count > 0 && cgltf_accessor_unpack_floats(accessor, &out[0].x, count * 3) != count * 3) {
            fail("could not read vertex data");
            return nullptr;
        }
        return &out;
    }

    bool primitive(const cgltf_primitive& prim, const NodeTransform& transform, const Target& target) {
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
        // Accessors without a bufferView are zeros and cost nothing in the
        // file, so positions and indices must have real data: that keeps
        // the work bounded by the file's size. (This also refuses sparse
        // positions over a zero base, which no common exporter writes.)
        if (positions->buffer_view == nullptr || (prim.indices != nullptr && prim.indices->buffer_view == nullptr)) {
            return fail("a mesh has positions or indices without data");
        }
        // cgltf 1.15 reads sparse values with the base view's stride, but
        // the spec packs them tightly: refuse rather than read wrong data.
        for (const cgltf_accessor* a : std::initializer_list<const cgltf_accessor*>{positions, normals, prim.indices}) {
            if (a != nullptr && a->is_sparse &&
                a->stride != cgltf_calc_size(a->type, a->component_type)) {
                return fail("sparse accessors on interleaved buffers are not supported");
            }
        }
        const std::vector<Vec3>* p = unpack(positions, true);
        if (p == nullptr) {
            return false;
        }
        const std::vector<Vec3>* n = normals != nullptr ? unpack(normals, false) : nullptr;
        if (normals != nullptr && n == nullptr) {
            return false;
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

        TriangleMesh& mesh = *target.mesh;
        if ((mesh.indices.size() + triangles.size()) / 3 > k_max_import_triangles) {
            return fail("the mesh has more than 16 million triangles");
        }
        // Skinned: each vertex's joints and weights, read like its position.
        std::array<std::vector<Vec4>, 4> skin_data; // JOINTS_0, WEIGHTS_0, JOINTS_1, WEIGHTS_1
        std::array<const std::vector<Vec4>*, 2> joint_sets{};
        std::array<const std::vector<Vec4>*, 2> weight_sets{};
        if (target.weights != nullptr && target.rigid < 0) {
            for (cgltf_int set = 0; set < 2; ++set) {
                const cgltf_accessor* j = cgltf_find_accessor(&prim, cgltf_attribute_type_joints, set);
                const cgltf_accessor* w = cgltf_find_accessor(&prim, cgltf_attribute_type_weights, set);
                if (j == nullptr || w == nullptr) continue;
                const usize s2 = static_cast<usize>(set) * 2;
                if (!unpack4(j, vertex_count, skin_data[s2], true) || !unpack4(w, vertex_count, skin_data[s2 + 1], false)) {
                    return false;
                }
                joint_sets[static_cast<usize>(set)] = &skin_data[s2];
                weight_sets[static_cast<usize>(set)] = &skin_data[s2 + 1];
            }
            if (cgltf_find_accessor(&prim, cgltf_attribute_type_joints, 2) != nullptr) {
                warn_once(m_warned_weights, "vertices moved by more than four joints keep the four heaviest");
            }
        }
        const auto weigh = [&](u32 v) {
            if (target.weights == nullptr) return true;
            SkinWeights w;
            if (target.rigid >= 0) {
                w.joints[0] = static_cast<u16>(target.rigid);
                w.weights[0] = 255;
            } else if (!vertex_weights(joint_sets, weight_sets, v, *target.skin_joints, w)) {
                return false;
            }
            target.weights->push_back(w);
            return true;
        };
        // Only the vertices that triangles use are kept, so the mesh stays
        // bounded by its triangle count.
        constexpr u32 unused = ~u32{0};
        m_remap.assign(vertex_count, unused);
        m_points.resize(vertex_count);
        for (const u32 i : triangles) {
            if (m_remap[i] == unused) {
                m_points[i] = transform.point((*p)[i]);
                if (!finite(m_points[i])) {
                    return fail("a vertex position is not a finite number");
                }
                m_remap[i] = 0;
            }
        }
        if (n != nullptr) {
            for (const u32 i : triangles) {
                if (m_remap[i] == 0) {
                    m_remap[i] = static_cast<u32>(mesh.vertices.size()) + 1;
                    Vec3 d = transform.direction((*n)[i]);
                    if (!finite(d) || length_squared(d) == 0.0f) {
                        d = {0, 1, 0};
                    }
                    mesh.vertices.push_back({m_points[i], d});
                    if (!weigh(i)) return false;
                }
                mesh.indices.push_back(m_remap[i] - 1);
            }
        } else {
            // glTF: without normals, a mesh is flat shaded.
            for (usize t = 0; t + 2 < triangles.size(); t += 3) {
                const Vec3 a = m_points[triangles[t]], b = m_points[triangles[t + 1]],
                           c = m_points[triangles[t + 2]];
                Vec3 face = normalize_or_zero(cross(b - a, c - a));
                if (length_squared(face) == 0.0f) {
                    face = {0, 1, 0};
                }
                const u32 base = static_cast<u32>(mesh.vertices.size());
                mesh.vertices.push_back({a, face});
                mesh.vertices.push_back({b, face});
                mesh.vertices.push_back({c, face});
                for (usize k = 0; k < 3; ++k) {
                    if (!weigh(triangles[t + k])) return false;
                }
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
    bool m_warned_unweighted = false;
    bool m_warned_weights = false;
    bool m_warned_type = false;
    bool m_warned_morph = false;
    bool m_warned_positions = false;
    bool m_warned_normals = false;
    u64 m_vertices_read = 0; // over the whole file
    std::unordered_map<const cgltf_accessor*, std::vector<Vec3>> m_unpacked; // this node's
    std::vector<u32> m_remap;  // per primitive: 0 used, else output index + 1
    std::vector<Vec3> m_points; // per primitive: transformed positions
};

// --- Skins ---------------------------------------------------------------

// The skin's skeleton: its joints and any nodes between two of them, parents
// first. `joint_of` maps each such node to its joint; `skin_joints` each of
// the skin's joints (as JOINTS_0 indexes them) to its joint.
bool build_skeleton(const cgltf_skin& skin, anim::Skeleton& out,
                    std::unordered_map<const cgltf_node*, u32>& joint_of, std::vector<u16>& skin_joints,
                    std::string& error) {
    if (skin.joints_count == 0) {
        error = "the skin has no joints";
        return false;
    }
    std::unordered_map<const cgltf_node*, usize> in_skin;
    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        if (!in_skin.emplace(skin.joints[i], i).second) {
            error = "the skin lists a joint twice";
            return false;
        }
    }
    std::vector<const cgltf_node*> members(skin.joints, skin.joints + skin.joints_count);
    std::unordered_map<const cgltf_node*, bool> member;
    for (const cgltf_node* j : members) member[j] = true;
    bool root_seen = false;
    const cgltf_node* root_parent = nullptr;
    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        std::vector<const cgltf_node*> path;
        const cgltf_node* p = skin.joints[i]->parent;
        while (p != nullptr && in_skin.count(p) == 0) {
            path.push_back(p);
            p = p->parent;
            if (path.size() > 100000) {
                error = "the node hierarchy has a cycle";
                return false;
            }
        }
        if (p != nullptr) {
            for (const cgltf_node* between : path) {
                if (member.emplace(between, true).second) members.push_back(between);
            }
        } else {
            if (root_seen && skin.joints[i]->parent != root_parent) {
                error = "the skin's root joints hang under different nodes; give them one parent";
                return false;
            }
            root_seen = true;
            root_parent = skin.joints[i]->parent;
        }
    }
    if (members.size() > anim::k_max_joints) {
        error = "the skeleton has more than 1024 joints";
        return false;
    }
    // Parents first: by depth among the members, else as listed.
    const auto member_parent = [&member](const cgltf_node* n) -> const cgltf_node* {
        for (const cgltf_node* p = n->parent; p != nullptr; p = p->parent) {
            if (member.count(p) != 0) return p;
        }
        return nullptr;
    };
    std::vector<usize> depth(members.size(), 0);
    for (usize i = 0; i < members.size(); ++i) {
        for (const cgltf_node* p = member_parent(members[i]); p != nullptr; p = member_parent(p)) ++depth[i];
    }
    std::vector<usize> order(members.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&depth](usize a, usize b) { return depth[a] < depth[b]; });
    for (usize i = 0; i < order.size(); ++i) joint_of[members[order[i]]] = static_cast<u32>(i);

    std::vector<Mat4> inverse_bind(skin.joints_count);
    if (skin.inverse_bind_matrices != nullptr) {
        const cgltf_accessor* a = skin.inverse_bind_matrices;
        if (a->type != cgltf_type_mat4 || a->count != skin.joints_count || !readable(a)) {
            error = "the skin's inverse bind matrices do not match its joints";
            return false;
        }
        if (cgltf_accessor_unpack_floats(a, inverse_bind[0].m, skin.joints_count * 16) != skin.joints_count * 16) {
            error = "could not read the skin's inverse bind matrices";
            return false;
        }
    }
    const usize n = members.size();
    out.names.resize(n);
    out.parents.resize(n);
    out.rest.resize(n);
    out.inverse_bind.resize(n);
    for (usize i = 0; i < n; ++i) {
        const cgltf_node* node = members[order[i]];
        out.names[i] = node->name != nullptr && node->name[0] != '\0' ? node->name : "joint " + std::to_string(i);
        const cgltf_node* parent = member_parent(node);
        out.parents[i] = parent != nullptr ? static_cast<i32>(joint_of[parent]) : -1;
        anim::JointTransform& r = out.rest[i];
        if (node->has_matrix) {
            Mat4 local;
            std::copy_n(node->matrix, 16, local.m);
            r = anim::decompose(local);
        } else {
            if (node->has_translation) r.translation = {node->translation[0], node->translation[1], node->translation[2]};
            if (node->has_rotation) {
                r.rotation = normalize(Quat{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]});
            }
            if (node->has_scale) r.scale = {node->scale[0], node->scale[1], node->scale[2]};
        }
        const auto it = in_skin.find(node);
        // Between two joints (nothing bound to it): its own place.
        out.inverse_bind[i] = it != in_skin.end() ? inverse_bind[it->second] : anim::affine_inverse(world_matrix(node));
    }
    out.root = world_matrix(root_parent);
    skin_joints.resize(skin.joints_count);
    for (cgltf_size i = 0; i < skin.joints_count; ++i) skin_joints[i] = static_cast<u16>(joint_of[skin.joints[i]]);
    error = anim::validate(out);
    return error.empty();
}

anim::Interpolation interpolation(cgltf_interpolation_type t) {
    switch (t) {
    case cgltf_interpolation_type_step: return anim::Interpolation::step;
    case cgltf_interpolation_type_cubic_spline: return anim::Interpolation::cubic;
    default: return anim::Interpolation::linear;
    }
}

// Every animation's joint channels, as clips.
bool read_clips(const cgltf_data& data, const anim::Skeleton& skeleton,
                const std::unordered_map<const cgltf_node*, u32>& joint_of, Importer& importer,
                std::vector<anim::Clip>& clips) {
    bool warned_morph = false;
    bool warned_other = false;
    usize keys = 0;
    if (data.animations_count > anim::k_max_clips) {
        return importer.fail("the file has more than 4096 animations");
    }
    std::unordered_set<std::string> names;
    for (cgltf_size a = 0; a < data.animations_count; ++a) {
        const cgltf_animation& animation = data.animations[a];
        anim::Clip clip;
        std::string name = animation.name != nullptr && animation.name[0] != '\0' ? animation.name
                                                                                  : "clip " + std::to_string(a + 1);
        std::string unique = name;
        for (int k = 2; names.count(unique) != 0; ++k) unique = name + " (" + std::to_string(k) + ")";
        names.insert(unique);
        clip.name = unique;
        for (cgltf_size c = 0; c < animation.channels_count; ++c) {
            const cgltf_animation_channel& ch = animation.channels[c];
            if (ch.target_path == cgltf_animation_path_type_weights) {
                importer.warn_once(warned_morph, "morph target animation is not imported yet");
                continue;
            }
            const auto joint = joint_of.find(ch.target_node);
            if (ch.target_node == nullptr || joint == joint_of.end() || ch.sampler == nullptr ||
                (ch.target_path != cgltf_animation_path_type_translation &&
                 ch.target_path != cgltf_animation_path_type_rotation &&
                 ch.target_path != cgltf_animation_path_type_scale)) {
                importer.warn_once(warned_other, "animation of nodes that are not joints is skipped");
                continue;
            }
            const cgltf_animation_sampler& sampler = *ch.sampler;
            anim::Channel out;
            out.joint = joint->second;
            out.path = ch.target_path == cgltf_animation_path_type_translation ? anim::Path::translation
                       : ch.target_path == cgltf_animation_path_type_rotation  ? anim::Path::rotation
                                                                               : anim::Path::scale;
            out.interpolation = interpolation(sampler.interpolation);
            const cgltf_accessor* in = sampler.input;
            const cgltf_accessor* values = sampler.output;
            const usize per = out.interpolation == anim::Interpolation::cubic ? 3 : 1;
            const cgltf_type want = out.path == anim::Path::rotation ? cgltf_type_vec4 : cgltf_type_vec3;
            if (!readable(in) || !readable(values) || in->type != cgltf_type_scalar || values->type != want ||
                in->count > anim::k_max_keys || values->count != in->count * per) {
                return importer.fail("the clip " + clip.name + " has a channel whose values do not match its keys");
            }
            keys += in->count * per;
            if (keys > anim::k_max_keys) {
                return importer.fail("the file's animations have more than 8 million keys");
            }
            out.times.resize(in->count);
            if (in->count > 0 && cgltf_accessor_unpack_floats(in, out.times.data(), in->count) != in->count) {
                return importer.fail("could not read the clip " + clip.name);
            }
            const usize width = want == cgltf_type_vec4 ? 4 : 3;
            std::vector<float> raw(values->count * width);
            if (!raw.empty() && cgltf_accessor_unpack_floats(values, raw.data(), raw.size()) != raw.size()) {
                return importer.fail("could not read the clip " + clip.name);
            }
            out.values.resize(values->count);
            for (usize v = 0; v < values->count; ++v) {
                out.values[v] = {raw[v * width], raw[v * width + 1], raw[v * width + 2], width == 4 ? raw[v * width + 3] : 0.0f};
            }
            if (out.times.empty()) continue;
            clip.duration = std::max(clip.duration, out.times.back());
            clip.channels.push_back(std::move(out));
        }
        const std::string error = anim::validate(clip, skeleton);
        if (!error.empty()) return importer.fail(error);
        clips.push_back(std::move(clip));
    }
    return true;
}

// A skinned file: its meshes in bind space with their weights, the
// skeleton, the clips, and `mesh` as the skin has it at rest.
bool import_skinned(const cgltf_data& data, const cgltf_skin& skin, const std::vector<const cgltf_node*>& shown,
                    Importer& importer, ImportResult& result) {
    SkinnedImport& out = result.skinned.emplace();
    std::unordered_map<const cgltf_node*, u32> joint_of;
    std::vector<u16> skin_joints;
    std::string error;
    if (!build_skeleton(skin, out.skeleton, joint_of, skin_joints, error)) return importer.fail(error);
    const anim::Skeleton& skeleton = out.skeleton;
    // The rest pose's skin matrices.
    std::vector<Mat4> model(skeleton.size());
    std::vector<Mat4> rest(skeleton.size());
    anim::model_matrices(skeleton, skeleton.rest, model);
    anim::skin_matrices(skeleton, model, rest);
    std::unordered_set<const cgltf_mesh*> skinned_meshes; // each once: bind space ignores the node
    for (const cgltf_node* node : shown) {
        if (node->mesh == nullptr) continue;
        Target target;
        target.mesh = &out.mesh.mesh;
        target.weights = &out.mesh.weights;
        if (node->skin == &skin) {
            if (!skinned_meshes.insert(node->mesh).second) continue;
            // In bind space as written: a skinned mesh's node does not place it.
            target.skin_joints = &skin_joints;
            float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (!importer.mesh(node, matrix_transform(identity), target)) return false;
            continue;
        }
        // Not skinned: carried by the joint above it (else the first root),
        // where it is now.
        i32 joint = -1;
        for (const cgltf_node* p = node; p != nullptr && joint < 0; p = p->parent) {
            if (const auto it = joint_of.find(p); it != joint_of.end()) joint = static_cast<i32>(it->second);
        }
        if (joint < 0) joint = 0; // (joint 0 is a root: parents come first)
        target.rigid = joint;
        const Mat4 bind = anim::affine_inverse(rest[static_cast<usize>(joint)]) * world_matrix(node);
        if (!importer.mesh(node, matrix_transform(bind.m), target)) return false;
    }
    if (!read_clips(data, skeleton, joint_of, importer, out.clips)) return false;
    // At rest, for whatever draws it unanimated; and each joint's reach.
    const TriangleMesh& bound = out.mesh.mesh;
    result.mesh.indices = bound.indices;
    result.mesh.vertices.resize(bound.vertices.size());
    out.joint_radius.assign(skeleton.size(), 0.0f);
    std::vector<Vec3> origin(skeleton.size());
    for (usize j = 0; j < skeleton.size(); ++j) {
        const Mat4 at = anim::affine_inverse(skeleton.inverse_bind[j]);
        origin[j] = {at.at(0, 3), at.at(1, 3), at.at(2, 3)};
    }
    for (usize v = 0; v < bound.vertices.size(); ++v) {
        const SkinWeights& w = out.mesh.weights[v];
        Vertex& to = result.mesh.vertices[v];
        to.position = anim::skin_point(rest, w.joints, w.weights, bound.vertices[v].position);
        to.normal = anim::skin_direction(rest, w.joints, w.weights, bound.vertices[v].normal);
        if (length_squared(to.normal) == 0.0f) to.normal = {0, 1, 0};
        if (!finite(to.position)) return importer.fail("the skin's rest pose places a vertex at infinity");
        for (usize k = 0; k < 4; ++k) {
            if (w.weights[k] == 0) continue;
            float& r = out.joint_radius[w.joints[k]];
            r = std::max(r, length(bound.vertices[v].position - origin[w.joints[k]]));
        }
    }
    // Triangles whose joints mirror them at rest are turned back to face out.
    for (usize t = 0; t + 2 < result.mesh.indices.size(); t += 3) {
        const SkinWeights& w = out.mesh.weights[result.mesh.indices[t]];
        const Mat4& m = rest[w.joints[0]];
        const auto a = [&m](int r, int c) { return m.at(r, c); };
        const float det = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
                          a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
                          a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
        if (det < 0.0f) std::swap(result.mesh.indices[t + 1], result.mesh.indices[t + 2]);
    }
    return true;
}

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
    // The nodes shown, in order. cgltf_validate rejects cycles; the visit
    // count bounds a bad file anyway.
    std::vector<const cgltf_node*> shown;
    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();
        if (shown.size() > data->nodes_count) {
            result.error = "the node hierarchy has a cycle";
            return result;
        }
        shown.push_back(node);
        for (cgltf_size i = node->children_count; i > 0; --i) {
            stack.push_back(node->children[i - 1]);
        }
    }
    const cgltf_skin* skin = nullptr;
    for (const cgltf_node* node : shown) {
        if (node->mesh == nullptr || node->skin == nullptr) continue;
        if (skin != nullptr && node->skin != skin) {
            result.error = "the file has more than one skin; export one character per file";
            return result;
        }
        skin = node->skin;
    }
    Importer importer(result);
    if (skin != nullptr) {
        if (!import_skinned(*data, *skin, shown, importer, result)) {
            result.mesh = {};
            result.skinned.reset();
            return result;
        }
    } else {
        for (const cgltf_node* node : shown) {
            if (!importer.node(node)) {
                result.mesh = {};
                return result;
            }
        }
    }
    if (result.mesh.indices.empty()) {
        result.error = "the file has no triangles";
        result.skinned.reset();
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
    const std::u8string utf8 = full.u8string();
    return import(bytes, std::string(utf8.begin(), utf8.end()), true);
}

ImportResult import_gltf_memory(std::span<const u8> data) { return import(data, std::string(), false); }

} // namespace giant::geometry
