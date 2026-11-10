// GLB files made in memory for import tests (geometry, editor).
#pragma once

#include "giant/core/types.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace giant::geometry::testing {

// --- GLB writing for import tests ------------------------------------------

inline void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

template <typename T>
inline void append(std::vector<u8>& bin, const std::vector<T>& values) {
    const usize at = bin.size();
    bin.resize(at + values.size() * sizeof(T));
    std::memcpy(bin.data() + at, values.data(), values.size() * sizeof(T));
}

inline std::vector<u8> make_glb(std::string json, std::vector<u8> bin) {
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

// --- A quad with two materials ------------------------------------------------
//
// A unit quad with uvs: one triangle in material "paint" (plain, its
// metallic-roughness texture on TEXCOORD_1), two (the whole quad) in
// "rust": base colour factor (0.5, 0.25, 1), roughness 0.3, metallic 0.7,
// emissive (1, 0.5, 0) at strength 4, base colour image in the buffer
// (`png`, image/png), normal image `normal_uri` (a file beside), and
// occlusion image as a data URI ("ABC").
inline std::vector<u8> two_material_quad(const std::string& normal_uri, const std::string& png = "PNGDATA1") {
    std::vector<u8> bin;
    append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
    append(bin, std::vector<float>{0, 1, 1, 1, 1, 0, 0, 0});
    append(bin, std::vector<u32>{0, 1, 2});
    append(bin, std::vector<u32>{0, 1, 2, 0, 2, 3});
    bin.insert(bin.end(), png.begin(), png.end());
    const std::string json =
        R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_materials_emissive_strength"],)"
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0},)"
        R"({"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":3,"material":1}]}],)"
        R"("materials":[{"name":"paint","pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":0,"texCoord":1}}},)"
        R"({"name":"rust","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,1],"roughnessFactor":0.3,)"
        R"("metallicFactor":0.7,"baseColorTexture":{"index":0}},"normalTexture":{"index":1},)"
        R"("occlusionTexture":{"index":2},"emissiveFactor":[1,0.5,0],)"
        R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4}}}],)"
        R"("textures":[{"source":0},{"source":1},{"source":2}],)"
        R"("images":[{"bufferView":4,"mimeType":"image/png"},{"uri":")" + normal_uri + R"("},)"
        R"({"uri":"data:image/png;base64,QUJD"}],)"
        R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},)"
        R"({"buffer":0,"byteOffset":80,"byteLength":12},{"buffer":0,"byteOffset":92,"byteLength":24},)"
        R"({"buffer":0,"byteOffset":116,"byteLength":)" + std::to_string(png.size()) + R"(}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)"
        R"({"bufferView":2,"componentType":5125,"count":3,"type":"SCALAR"},)"
        R"({"bufferView":3,"componentType":5125,"count":6,"type":"SCALAR"}]})";
    return make_glb(json, bin);
}

// --- A skinned arm ----------------------------------------------------------
//
// Two joints (a shoulder at the origin, an elbow 1 m up), listed in the skin
// elbow first. A strip of quads 0.2 m wide from y = 0 to 2: the bottom row on
// the shoulder, the middle shared, the top on the elbow; its node is moved
// 5 m (skinned meshes ignore that). A sword triangle hangs under the elbow,
// not skinned. Clips: "bend" (the elbow a quarter turn about z over 1 s),
// "hop" (the shoulder stepping 1 m along x at 0.5 s, and the mesh node moved:
// not a joint, skipped), and a second "bend".
struct SkinOptions {
    u16 bad_joint = 0;          // a vertex's joint index, when not 0
    u32 ibm_count = 2;
    float second_key = 1.0f;    // bend's second key time
    bool second_skin = false;   // another mesh node with a skin of its own
    bool weights_1 = false;     // a second joint/weight set
    u16 weights_1_joint = 0;    // its one weight, on the bottom-left vertex
    float weights_1_value = 0.1f;
    bool joints_normalized = false;
    float weight_scale = 1.0f;
    bool two_nodes = false;     // the arm's mesh shown twice
    bool bogus_clip = false;    // a cubic clip claiming ~6e18 keys with no data
    usize clip_copies = 0;      // more clips all named copy_name
    std::string copy_name = "a";
    bool sparse_weights = false; // weights sparse over an interleaved view
};

inline std::vector<u8> skinned_arm(const SkinOptions& o = {}) {
    std::vector<u8> bin;
    std::vector<std::string> views;
    std::vector<std::string> accessors;
    const auto add = [&](const auto& values, const std::string& accessor) {
        const usize at = bin.size();
        append(bin, values);
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(at) + R"(,"byteLength":)" +
                        std::to_string(bin.size() - at) + "}");
        accessors.push_back(R"({"bufferView":)" + std::to_string(views.size() - 1) + "," + accessor + "}");
        return accessors.size() - 1;
    };
    std::vector<float> positions;
    for (int r = 0; r < 3; ++r) positions.insert(positions.end(), {-0.1f, float(r), 0.0f, 0.1f, float(r), 0.0f});
    std::vector<float> normals;
    for (int i = 0; i < 6; ++i) normals.insert(normals.end(), {0.0f, 0.0f, 1.0f});
    // Skin indices: 0 the elbow, 1 the shoulder.
    std::vector<u16> joints{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (o.bad_joint != 0) joints[0] = o.bad_joint;
    std::vector<float> weights{1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (float& w : weights) w *= o.weight_scale;
    const usize pos = add(positions, R"("componentType":5126,"count":6,"type":"VEC3","min":[-0.1,0,0],"max":[0.1,2,0])");
    const usize nrm = add(normals, R"("componentType":5126,"count":6,"type":"VEC3")");
    const usize jnt = add(joints, std::string(R"("componentType":5123,"count":6,"type":"VEC4")") +
                                      (o.joints_normalized ? R"(,"normalized":true)" : ""));
    usize wgt = add(weights, R"("componentType":5126,"count":6,"type":"VEC4")");
    if (o.sparse_weights) {
        // A base view of 6 weights with a 32-byte stride (twice what it
        // holds) and one sparse value on top: cgltf would read the sparse
        // value with the stride, past its view.
        std::vector<float> base(6 * 8, 0.0f);
        const usize at = bin.size();
        append(bin, base);
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(at) + R"(,"byteLength":)" +
                        std::to_string(bin.size() - at) + R"(,"byteStride":32})");
        const usize base_view = views.size() - 1;
        const usize ia = bin.size();
        append(bin, std::vector<u32>{0});
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(ia) + R"(,"byteLength":4})");
        const usize index_view = views.size() - 1;
        const usize va = bin.size();
        append(bin, std::vector<float>{1, 0, 0, 0});
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(va) + R"(,"byteLength":16})");
        const usize value_view = views.size() - 1;
        accessors.push_back(R"({"bufferView":)" + std::to_string(base_view) +
                            R"(,"componentType":5126,"count":6,"type":"VEC4","sparse":{"count":1,"indices":{"bufferView":)" +
                            std::to_string(index_view) + R"(,"componentType":5125},"values":{"bufferView":)" +
                            std::to_string(value_view) + "}}}");
        wgt = accessors.size() - 1;
    }
    const usize idx = add(std::vector<u32>{0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}, R"("componentType":5125,"count":12,"type":"SCALAR")");
    std::vector<float> ibm{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1, // elbow: 1 m down
                           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ibm.resize(16 * o.ibm_count);
    const usize inv = add(ibm, R"("componentType":5126,"count":)" + std::to_string(o.ibm_count) + R"(,"type":"MAT4")");
    const float s = std::sqrt(0.5f);
    const usize bt = add(std::vector<float>{0.0f, o.second_key}, R"("componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1])");
    const usize br = add(std::vector<float>{0, 0, 0, 1, 0, 0, s, s}, R"("componentType":5126,"count":2,"type":"VEC4")");
    const usize ht = add(std::vector<float>{0.0f, 0.5f}, R"("componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[0.5])");
    const usize hv = add(std::vector<float>{0, 0, 0, 1, 0, 0}, R"("componentType":5126,"count":2,"type":"VEC3")");
    const usize sp = add(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}, R"("componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0])");
    const usize si = add(std::vector<u32>{0, 1, 2}, R"("componentType":5125,"count":3,"type":"SCALAR")");
    std::string extra_weights;
    if (o.weights_1) {
        // A second set: a little of the elbow (skin index 0) on the bottom-left vertex.
        std::vector<u16> j1(24, 0);
        std::vector<float> w1(24, 0.0f);
        j1[0] = o.weights_1_joint;
        w1[0] = o.weights_1_value;
        const usize j1a = add(j1, R"("componentType":5123,"count":6,"type":"VEC4")");
        const usize w1a = add(w1, R"("componentType":5126,"count":6,"type":"VEC4")");
        extra_weights = R"(,"JOINTS_1":)" + std::to_string(j1a) + R"(,"WEIGHTS_1":)" + std::to_string(w1a);
    }
    const auto n = [](usize i) { return std::to_string(i); };
    std::string more_clips;
    if (o.bogus_clip) {
        accessors.push_back(R"({"componentType":5126,"count":6148914691236517206,"type":"SCALAR"})");
        accessors.push_back(R"({"componentType":5126,"count":2,"type":"VEC3"})");
        more_clips += R"(,{"name":"bogus","samplers":[{"input":)" + n(accessors.size() - 2) + R"(,"output":)" +
                      n(accessors.size() - 1) +
                      R"(,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]})";
    }
    for (usize i = 0; i < o.clip_copies; ++i) {
        more_clips += R"(,{"name":")" + o.copy_name + R"(","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(br) +
                      R"(}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]})";
    }
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2)" +
                       std::string(o.second_skin || o.two_nodes ? ",4" : "") + R"(]}],"nodes":[)" +
                       R"({"name":"shoulder","children":[1]},)" +
                       R"({"name":"elbow","translation":[0,1,0],"children":[3]},)" +
                       R"({"name":"body","mesh":0,"skin":0,"translation":[5,0,0]},)" +
                       R"({"name":"sword","mesh":1,"translation":[0,0.5,0]})" +
                       std::string(o.second_skin ? R"(,{"mesh":0,"skin":1})" : "") +
                       std::string(o.two_nodes ? R"(,{"mesh":0,"skin":0,"translation":[9,0,0]})" : "") + "]," +
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":)" + n(pos) + R"(,"NORMAL":)" + n(nrm) +
                       R"(,"JOINTS_0":)" + n(jnt) + R"(,"WEIGHTS_0":)" + n(wgt) + extra_weights + R"(},"indices":)" + n(idx) +
                       R"(}]},{"primitives":[{"attributes":{"POSITION":)" + n(sp) + R"(},"indices":)" + n(si) + "}]}]," +
                       R"("skins":[{"joints":[1,0],"inverseBindMatrices":)" + n(inv) + "}" +
                       std::string(o.second_skin ? R"(,{"joints":[0]})" : "") + "]," +
                       R"("animations":[)" +
                       R"({"name":"bend","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(br) +
                       R"(}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]},)" +
                       R"({"name":"hop","samplers":[{"input":)" + n(ht) + R"(,"output":)" + n(hv) +
                       R"(,"interpolation":"STEP"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},)" +
                       R"({"sampler":0,"target":{"node":2,"path":"translation"}}]},)" +
                       R"({"name":"bend","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(br) +
                       R"(}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]})" + more_clips + "]," +
                       R"("buffers":[{"byteLength":)" + n(bin.size()) + R"(}],"bufferViews":[)";
    for (usize i = 0; i < views.size(); ++i) json += (i ? "," : "") + views[i];
    json += R"(],"accessors":[)";
    for (usize i = 0; i < accessors.size(); ++i) json += (i ? "," : "") + accessors[i];
    json += "]}";
    return make_glb(json, bin);
}

} // namespace giant::geometry::testing
