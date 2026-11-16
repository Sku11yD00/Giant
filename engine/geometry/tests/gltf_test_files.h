// GLB files made in memory for import tests (geometry, editor).
#pragma once

#include "giant/core/types.h"

#include <algorithm>
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
    // Morph targets: the arm (mesh "arm") gets "smile" (the middle and top
    // rows 0.1 m out along +z, the top-left's normal turned toward +x) and
    // "blink" (sparse over zeros: the bottom-left 0.05 m along +x), resting
    // at weights 0.25 and 0; the sword gets "smile" too (its tip, (1, 0, 0)
    // where it hangs, 0.5 m out along +z: its flat face turns).
    bool morphs = false;
    bool morph_names = true; // targetNames given (else none)
    bool repeated_names = false; // the arm's targets both named "smile"
    bool mirrored_uvs = false; // uvs mirrored across the middle row (its vertices split for tangents)
    // With morphs: clips "smile" (the body's weights, linear over bend's
    // keys: smile 0 to 1, blink 0 to 0.5), "wink" (the sword's one target,
    // stepping 0.2 then 0.8) and "pout" (the body's, cubic: values smile
    // 0 then 1, blink 0.1 then 0.3; out tangents at the first key 1 and 0,
    // in tangents at the second 0 and 2), after the others.
    bool morph_clip = false;
    float smile_rest = 0.25f; // the arm's "smile" rest weight
    // Hair lines: the arm's vertices again as two strips (skinned as it
    // is); on the sword a strip of its three vertices, a list without
    // indices of (0,0,0)-(0,0,1), (0,0,1)-(0,0,2) and (3,3,3)-(3,3,3), and
    // a list whose positions have no data.
    bool hair_lines = false;
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
    std::string arm_lines, sword_lines;
    if (o.hair_lines) {
        const usize lp = add(std::vector<float>{0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 2, 3, 3, 3, 3, 3, 3},
                             R"("componentType":5126,"count":6,"type":"VEC3","min":[0,0,0],"max":[3,3,3])");
        accessors.push_back(R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]})");
        const usize none = accessors.size() - 1;
        const std::string strip = R"(,{"attributes":{"POSITION":)" + n(pos) + R"(,"JOINTS_0":)" + n(jnt) + R"(,"WEIGHTS_0":)" +
                                  n(wgt) + R"(},"mode":3})";
        arm_lines = strip + strip;
        sword_lines = R"(,{"attributes":{"POSITION":)" + n(sp) + R"(},"mode":3},{"attributes":{"POSITION":)" + n(lp) +
                      R"(},"mode":1},{"attributes":{"POSITION":)" + n(none) + R"(},"mode":1})";
    }
    std::string arm_extra, arm_targets, sword_targets, arm_mesh_extra, sword_mesh_extra;
    if (o.mirrored_uvs) {
        const usize uv = add(std::vector<float>{0, 1, 1, 1, 0, 0.5f, 1, 0.5f, 0, 1, 1, 1},
                             R"("componentType":5126,"count":6,"type":"VEC2")");
        arm_extra += R"(,"TEXCOORD_0":)" + n(uv);
    }
    if (o.morphs) {
        std::vector<float> dp(18, 0.0f), dn(18, 0.0f);
        for (usize v = 2; v < 6; ++v) dp[v * 3 + 2] = 0.1f;
        dn[4 * 3] = 0.5f;
        const usize sp_ = add(dp, R"("componentType":5126,"count":6,"type":"VEC3")");
        const usize sn_ = add(dn, R"("componentType":5126,"count":6,"type":"VEC3")");
        const usize ia = bin.size();
        append(bin, std::vector<u32>{0});
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(ia) + R"(,"byteLength":4})");
        const usize index_view = views.size() - 1;
        const usize va = bin.size();
        append(bin, std::vector<float>{0.05f, 0, 0});
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(va) + R"(,"byteLength":12})");
        const usize value_view = views.size() - 1;
        accessors.push_back(R"({"componentType":5126,"count":6,"type":"VEC3","sparse":{"count":1,"indices":{"bufferView":)" +
                            n(index_view) + R"(,"componentType":5125},"values":{"bufferView":)" + n(value_view) + "}}}");
        const usize blink = accessors.size() - 1;
        const usize tip = add(std::vector<float>{0, 0, 0, 0, 0, 0.5f, 0, 0, 0}, R"("componentType":5126,"count":3,"type":"VEC3")");
        arm_targets = R"(,"targets":[{"POSITION":)" + n(sp_) + R"(,"NORMAL":)" + n(sn_) + R"(},{"POSITION":)" + n(blink) + "}]";
        sword_targets = R"(,"targets":[{"POSITION":)" + n(tip) + "}]";
        arm_mesh_extra = R"("name":"arm","weights":[)" + std::to_string(o.smile_rest) + R"(,0],)";
        if (o.morph_names) {
            arm_mesh_extra += o.repeated_names ? R"("extras":{"targetNames":["smile","smile"]},)"
                                               : R"("extras":{"targetNames":["smile","blink"]},)";
            sword_mesh_extra = R"("extras":{"targetNames":["smile"]},)";
        }
    }
    std::string more_clips;
    if (o.bogus_clip) {
        accessors.push_back(R"({"componentType":5126,"count":6148914691236517206,"type":"SCALAR"})");
        accessors.push_back(R"({"componentType":5126,"count":2,"type":"VEC3"})");
        more_clips += R"(,{"name":"bogus","samplers":[{"input":)" + n(accessors.size() - 2) + R"(,"output":)" +
                      n(accessors.size() - 1) +
                      R"(,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]})";
    }
    if (o.morphs && o.morph_clip) {
        const usize body = add(std::vector<float>{0, 0, 1, 0.5f}, R"("componentType":5126,"count":4,"type":"SCALAR")");
        const usize sword = add(std::vector<float>{0.2f, 0.8f}, R"("componentType":5126,"count":2,"type":"SCALAR")");
        more_clips += R"(,{"name":"smile","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(body) +
                      R"(}],"channels":[{"sampler":0,"target":{"node":2,"path":"weights"}}]})";
        more_clips += R"(,{"name":"wink","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(sword) +
                      R"(,"interpolation":"STEP"}],"channels":[{"sampler":0,"target":{"node":3,"path":"weights"}}]})";
        // Per key: both in tangents, both values, both out tangents.
        const usize cubic = add(std::vector<float>{9, 9, 0, 0.1f, 1, 0, 0, 2, 1, 0.3f, 9, 9},
                                R"("componentType":5126,"count":12,"type":"SCALAR")");
        more_clips += R"(,{"name":"pout","samplers":[{"input":)" + n(bt) + R"(,"output":)" + n(cubic) +
                      R"(,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":2,"path":"weights"}}]})";
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
                       R"("meshes":[{)" + arm_mesh_extra + R"("primitives":[{"attributes":{"POSITION":)" + n(pos) + R"(,"NORMAL":)" + n(nrm) +
                       R"(,"JOINTS_0":)" + n(jnt) + R"(,"WEIGHTS_0":)" + n(wgt) + extra_weights + arm_extra + R"(},"indices":)" + n(idx) +
                       arm_targets + "}" + arm_lines + R"(]},{)" + sword_mesh_extra + R"("primitives":[{"attributes":{"POSITION":)" + n(sp) +
                       R"(},"indices":)" + n(si) + sword_targets + "}" + sword_lines + "]}]," +
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

// --- A skinned walker --------------------------------------------------------
//
// Three joints by the usual names: "Hips" 1 m up, "LeftFoot" and
// "RightFoot" under it on the ground, 0.1 m to each side (+x is its left:
// it faces +z). A triangle rides each. Clips, all moving the hips through
// model space (root motion): "idle" (1 s, standing), "walk" (1 s, 1.4 m
// along +z, a foot swinging 0.3 m each way), "run" (0.5 s, 2 m along +z:
// 4 m/s, feet 0.5 m each way) and "wave" (0.8 s, not a loop: the left
// foot lifted 0.5 m and held there).
inline std::vector<u8> skinned_walker() {
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
    const auto n = [](usize i) { return std::to_string(i); };
    // A triangle at the hips, one at each foot.
    const std::vector<float> positions{-0.1f, 1.0f,  0.0f, 0.1f,  1.0f, 0.0f, 0.0f,  1.2f, 0.0f,
                                       0.05f, 0.0f,  0.0f, 0.15f, 0.0f, 0.0f, 0.1f,  0.1f, 0.0f,
                                       -0.15f, 0.0f, 0.0f, -0.05f, 0.0f, 0.0f, -0.1f, 0.1f, 0.0f};
    std::vector<float> normals;
    for (int i = 0; i < 9; ++i) normals.insert(normals.end(), {0.0f, 0.0f, 1.0f});
    std::vector<u16> joints;
    std::vector<float> weights;
    for (u16 j = 0; j < 3; ++j) {
        for (int v = 0; v < 3; ++v) {
            joints.insert(joints.end(), {j, 0, 0, 0});
            weights.insert(weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
        }
    }
    const usize pos = add(positions, R"("componentType":5126,"count":9,"type":"VEC3","min":[-0.15,0,0],"max":[0.15,1.2,0])");
    const usize nrm = add(normals, R"("componentType":5126,"count":9,"type":"VEC3")");
    const usize jnt = add(joints, R"("componentType":5123,"count":9,"type":"VEC4")");
    const usize wgt = add(weights, R"("componentType":5126,"count":9,"type":"VEC4")");
    const usize idx = add(std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7, 8}, R"("componentType":5125,"count":9,"type":"SCALAR")");
    const usize inv = add(std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0,     -1, 0, 1,  // hips
                                             1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.1f, 0,  0, 1,  // left foot
                                             1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1f,  0,  0, 1}, // right foot
                          R"("componentType":5126,"count":3,"type":"MAT4")");
    std::string animations;
    // A clip of `seconds`: the hips to `travel` along +z, the feet swinging `reach` each way (left first forward).
    const auto go = [&](const std::string& name, float seconds, float travel, float reach) {
        const usize t2 = add(std::vector<float>{0.0f, seconds},
                             R"("componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[)" + std::to_string(seconds) + "]");
        const usize hips = add(std::vector<float>{0, 1, 0, 0, 1, travel}, R"("componentType":5126,"count":2,"type":"VEC3")");
        const usize t5 = add(std::vector<float>{0.0f, seconds * 0.25f, seconds * 0.5f, seconds * 0.75f, seconds},
                             R"("componentType":5126,"count":5,"type":"SCALAR","min":[0],"max":[)" + std::to_string(seconds) + "]");
        std::vector<float> left, right;
        for (const float z : {0.0f, reach, 0.0f, -reach, 0.0f}) {
            left.insert(left.end(), {0.1f, -1.0f, z});
            right.insert(right.end(), {-0.1f, -1.0f, -z});
        }
        const usize lf = add(left, R"("componentType":5126,"count":5,"type":"VEC3")");
        const usize rf = add(right, R"("componentType":5126,"count":5,"type":"VEC3")");
        animations += std::string(animations.empty() ? "" : ",") + R"({"name":")" + name + R"(","samplers":[{"input":)" + n(t2) +
                      R"(,"output":)" + n(hips) + R"(},{"input":)" + n(t5) + R"(,"output":)" + n(lf) + R"(},{"input":)" + n(t5) +
                      R"(,"output":)" + n(rf) + R"(}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},)" +
                      R"({"sampler":1,"target":{"node":1,"path":"translation"}},)" +
                      R"({"sampler":2,"target":{"node":2,"path":"translation"}}]})";
    };
    go("idle", 1.0f, 0.0f, 0.0f);
    go("walk", 1.0f, 1.4f, 0.3f);
    go("run", 0.5f, 2.0f, 0.5f);
    {
        const usize t = add(std::vector<float>{0.0f, 0.4f, 0.8f}, R"("componentType":5126,"count":3,"type":"SCALAR","min":[0],"max":[0.8])");
        const usize lift = add(std::vector<float>{0.1f, -1.0f, 0.0f, 0.1f, -0.5f, 0.0f, 0.1f, -0.5f, 0.0f},
                               R"("componentType":5126,"count":3,"type":"VEC3")");
        animations += R"(,{"name":"wave","samplers":[{"input":)" + n(t) + R"(,"output":)" + n(lift) +
                      R"(}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]})";
    }
    std::string json = std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,3]}],"nodes":[)") +
                       R"({"name":"Hips","translation":[0,1,0],"children":[1,2]},)" +
                       R"({"name":"LeftFoot","translation":[0.1,-1,0]},)" +
                       R"({"name":"RightFoot","translation":[-0.1,-1,0]},)" +
                       R"({"name":"body","mesh":0,"skin":0}],)" +
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":)" + n(pos) + R"(,"NORMAL":)" + n(nrm) +
                       R"(,"JOINTS_0":)" + n(jnt) + R"(,"WEIGHTS_0":)" + n(wgt) + R"(},"indices":)" + n(idx) + "}]}]," +
                       R"("skins":[{"joints":[0,1,2],"inverseBindMatrices":)" + n(inv) + "}]," +
                       R"("animations":[)" + animations + "]," +
                       R"("buffers":[{"byteLength":)" + n(bin.size()) + R"(}],"bufferViews":[)";
    for (usize i = 0; i < views.size(); ++i) json += (i ? "," : "") + views[i];
    json += R"(],"accessors":[)";
    for (usize i = 0; i < accessors.size(); ++i) json += (i ? "," : "") + accessors[i];
    json += "]}";
    return make_glb(json, bin);
}

// --- A skinned person --------------------------------------------------------
//
// A figure 1.7 m tall standing in a T, facing +z (+x its left), a box of
// eight vertices round each bone, by the usual names: "Hips" (1 m up),
// "Spine", "Head", "LeftArm" and "LeftForeArm" out along +x (0.3 m each),
// "RightArm" and "RightForeArm" along -x, "LeftUpLeg" and "LeftLeg" down
// (0.45 m each), "RightUpLeg" and "RightLeg", and a finger 3 cm long,
// "LeftHandIndex1", at the left forearm's end. Clips: "idle" (1 s,
// standing as at rest) and "raise" (1 s: the left arm held 60 degrees up).
struct PersonJoint {
    const char* name;
    int parent;
    float at[3];   // in model space
    float to[3];   // its box runs from `at` to here
    float thick;   // half its box's width across
};
inline constexpr PersonJoint k_person[] = {
    {"Hips", -1, {0.0f, 1.0f, 0.0f}, {0.0f, 1.15f, 0.0f}, 0.12f},
    {"Spine", 0, {0.0f, 1.2f, 0.0f}, {0.0f, 1.45f, 0.0f}, 0.11f},
    {"Head", 1, {0.0f, 1.5f, 0.0f}, {0.0f, 1.7f, 0.0f}, 0.09f},
    {"LeftArm", 1, {0.2f, 1.45f, 0.0f}, {0.5f, 1.45f, 0.0f}, 0.04f},
    {"LeftForeArm", 3, {0.5f, 1.45f, 0.0f}, {0.8f, 1.45f, 0.0f}, 0.035f},
    {"RightArm", 1, {-0.2f, 1.45f, 0.0f}, {-0.5f, 1.45f, 0.0f}, 0.04f},
    {"RightForeArm", 5, {-0.5f, 1.45f, 0.0f}, {-0.8f, 1.45f, 0.0f}, 0.035f},
    {"LeftUpLeg", 0, {0.1f, 0.95f, 0.0f}, {0.1f, 0.5f, 0.0f}, 0.07f},
    {"LeftLeg", 7, {0.1f, 0.5f, 0.0f}, {0.1f, 0.05f, 0.0f}, 0.055f},
    {"RightUpLeg", 0, {-0.1f, 0.95f, 0.0f}, {-0.1f, 0.5f, 0.0f}, 0.07f},
    {"RightLeg", 9, {-0.1f, 0.5f, 0.0f}, {-0.1f, 0.05f, 0.0f}, 0.055f},
    {"LeftHandIndex1", 4, {0.8f, 1.45f, 0.0f}, {0.83f, 1.45f, 0.0f}, 0.008f},
};
inline constexpr usize k_person_joints = sizeof(k_person) / sizeof(k_person[0]);

inline std::vector<u8> skinned_person() {
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
    const auto n = [](usize i) { return std::to_string(i); };
    std::vector<float> positions, normals, weights, inverse;
    std::vector<u16> joints;
    std::vector<u32> indices;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (usize j = 0; j < k_person_joints; ++j) {
        const PersonJoint& p = k_person[j];
        // The box: `thick` across the two axes it does not run along.
        int along = 0;
        for (int a = 1; a < 3; ++a) {
            if (std::fabs(p.to[a] - p.at[a]) > std::fabs(p.to[along] - p.at[along])) along = a;
        }
        const u32 first = static_cast<u32>(positions.size() / 3);
        for (int corner = 0; corner < 8; ++corner) {
            float v[3];
            for (int a = 0; a < 3; ++a) {
                const bool high = (corner >> a & 1) != 0;
                v[a] = a == along ? (high ? p.to[a] : p.at[a]) : p.at[a] + (high ? p.thick : -p.thick);
                lo[a] = std::min(lo[a], v[a]);
                hi[a] = std::max(hi[a], v[a]);
            }
            positions.insert(positions.end(), {v[0], v[1], v[2]});
            float out[3], len = 0.0f;
            for (int a = 0; a < 3; ++a) {
                out[a] = v[a] - 0.5f * (p.at[a] + p.to[a]);
                len += out[a] * out[a];
            }
            len = std::sqrt(len);
            normals.insert(normals.end(), {out[0] / len, out[1] / len, out[2] / len});
            joints.insert(joints.end(), {static_cast<u16>(j), 0, 0, 0});
            weights.insert(weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
        }
        // (Corner bits: x 1, y 2, z 4.)
        for (const u32 i : {0u, 2u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 5u, 7u, 6u, 0u, 1u, 4u, 1u, 5u, 4u,
                            2u, 6u, 3u, 3u, 6u, 7u, 0u, 4u, 2u, 2u, 4u, 6u, 1u, 3u, 5u, 3u, 7u, 5u}) {
            indices.push_back(first + i);
        }
        inverse.insert(inverse.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p.at[0], -p.at[1], -p.at[2], 1});
    }
    const usize count = positions.size() / 3;
    const auto list = [](const float v[3]) {
        return "[" + std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) + "]";
    };
    const usize pos = add(positions, R"("componentType":5126,"count":)" + n(count) + R"(,"type":"VEC3","min":)" + list(lo) +
                                         R"(,"max":)" + list(hi));
    const usize nrm = add(normals, R"("componentType":5126,"count":)" + n(count) + R"(,"type":"VEC3")");
    const usize jnt = add(joints, R"("componentType":5123,"count":)" + n(count) + R"(,"type":"VEC4")");
    const usize wgt = add(weights, R"("componentType":5126,"count":)" + n(count) + R"(,"type":"VEC4")");
    const usize idx = add(indices, R"("componentType":5125,"count":)" + n(indices.size()) + R"(,"type":"SCALAR")");
    const usize inv = add(inverse, R"("componentType":5126,"count":)" + n(k_person_joints) + R"(,"type":"MAT4")");
    const usize times = add(std::vector<float>{0.0f, 1.0f}, R"("componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1])");
    const usize stand = add(std::vector<float>{0, 1, 0, 0, 1, 0}, R"("componentType":5126,"count":2,"type":"VEC3")");
    // (60 degrees about +z: the left arm, out along +x, goes up.)
    const usize up = add(std::vector<float>{0, 0, 0.5f, 0.8660254f, 0, 0, 0.5f, 0.8660254f},
                         R"("componentType":5126,"count":2,"type":"VEC4")");
    std::string nodes;
    std::string skin_joints;
    for (usize j = 0; j < k_person_joints; ++j) {
        const PersonJoint& p = k_person[j];
        float local[3];
        for (int a = 0; a < 3; ++a) local[a] = p.at[a] - (p.parent >= 0 ? k_person[p.parent].at[a] : 0.0f);
        std::string children;
        for (usize c = 0; c < k_person_joints; ++c) {
            if (k_person[c].parent == static_cast<int>(j)) children += (children.empty() ? "" : ",") + n(c);
        }
        nodes += std::string(R"({"name":")") + p.name + R"(","translation":)" + list(local) +
                 (children.empty() ? "" : R"(,"children":[)" + children + "]") + "},";
        skin_joints += (j ? "," : "") + n(j);
    }
    std::string json = std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,)") + n(k_person_joints) +
                       R"(]}],"nodes":[)" + nodes + R"({"name":"body","mesh":0,"skin":0}],)" +
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":)" + n(pos) + R"(,"NORMAL":)" + n(nrm) +
                       R"(,"JOINTS_0":)" + n(jnt) + R"(,"WEIGHTS_0":)" + n(wgt) + R"(},"indices":)" + n(idx) + "}]}]," +
                       R"("skins":[{"joints":[)" + skin_joints + R"(],"inverseBindMatrices":)" + n(inv) + "}]," +
                       R"("animations":[{"name":"idle","samplers":[{"input":)" + n(times) + R"(,"output":)" + n(stand) +
                       R"(}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]},)" +
                       R"({"name":"raise","samplers":[{"input":)" + n(times) + R"(,"output":)" + n(up) +
                       R"(}],"channels":[{"sampler":0,"target":{"node":3,"path":"rotation"}}]}],)" +
                       R"("buffers":[{"byteLength":)" + n(bin.size()) + R"(}],"bufferViews":[)";
    for (usize i = 0; i < views.size(); ++i) json += (i ? "," : "") + views[i];
    json += R"(],"accessors":[)";
    for (usize i = 0; i < accessors.size(); ++i) json += (i ? "," : "") + accessors[i];
    json += "]}";
    return make_glb(json, bin);
}

} // namespace giant::geometry::testing
