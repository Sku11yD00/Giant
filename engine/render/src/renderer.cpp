#include "giant/render/renderer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/geometry/mesh.h"
#include "cells.h"
#include "decals.h"
#include "exposure.h"
#include "instance_store.h"
#include "gpu_layout.h"
#include "render_shaders.h"
#include "shadow_pages.h"
#include "smaa.h"
#include "streaming.h"
#include "voxel_light.h"
#include "voxel_scene.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace giant::render {

namespace {

// Capacities of the GPU culling lists. Past them geometry is dropped (and
// RenderStats::overflow says so); streaming (N1 part 4) revisits them.
constexpr u32 k_queue_capacity = 1u << 19;    // (instance, node) items per tree level
constexpr u32 k_visible_capacity = 1u << 20;  // clusters drawn per frame
constexpr u32 k_occluded_capacity = 1u << 18; // clusters retested in the second pass
// The instances live in the InstanceStore, uploaded in chunks
// (k_max_instances: renderer.h).
constexpr u32 k_vertices_per_cluster = geometry::k_cluster_max_triangles * 3;
constexpr u32 k_max_materials = 1u << 20; // per frame (48 MiB)
constexpr u32 k_max_mesh_groups = 65535; // per dimension of a mesh dispatch
// Mesh groups in one dispatch: whole rows of 65535 under the 2^22 total.
constexpr u32 k_max_cpu_clusters = 64u * k_max_mesh_groups;

template <typename T>
std::span<const u8> bytes_of(const std::vector<T>& v) {
    return {reinterpret_cast<const u8*>(v.data()), v.size() * sizeof(T)};
}

template <typename T>
std::span<const u8> bytes_of_one(const T& v) {
    return {reinterpret_cast<const u8*>(&v), sizeof(T)};
}

void set4(float* out, const Vec3& v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

u32 groups_of(u32 items, u32 per_group) { return (items + per_group - 1) / per_group; }

gpu::Material to_gpu(const Material& m, const rhi::Device& device, std::span<const rhi::TextureHandle> textures) {
    gpu::Material out{};
    set4(out.base_roughness, m.base_color, m.roughness);
    set4(out.emissive_metallic, m.emissive, m.metallic);
    const TextureId ids[4] = {m.base_color_texture, m.normal_texture, m.roughness_metal_texture,
                              m.occlusion_texture};
    for (usize i = 0; i < 4; ++i) {
        out.textures[i] = ids[i] < textures.size() && textures[ids[i]].valid()
                              ? device.read_descriptor(textures[ids[i]])
                              : rhi::k_no_descriptor;
    }
    return out;
}

void set_bounds(float* out, const Vec3& center, float radius) { set4(out, center, radius); }

float wind_value(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 4.0f) : 0.0f; }

gpu::MaterialWind wind_of(const Material& m) { return {wind_value(m.sway), wind_value(m.flutter)}; }

// How far an instance can sway past its bounds this frame (common.slang's
// wind_reach with swayer_of): 0 for still ones and past wind_distance.
float wind_reach(const FrameInput& frame, const Instance& in, float top) {
    const float distance = frame.settings.wind_distance;
    if (!(distance > 0.0f) || !std::isfinite(distance) || !(top > 0.0f)) return 0.0f;
    const Material m = in.material < frame.materials.size() && in.material < k_max_materials
                           ? frame.materials[in.material]
                           : Material{};
    const gpu::MaterialWind w = wind_of(m);
    if (!(w.sway > 0.0f || w.flutter > 0.0f)) return 0.0f;
    const DVec3 rel = in.position - frame.view.position;
    const double away2 = rel.x * rel.x + rel.y * rel.y + rel.z * rel.z;
    if (!(away2 < static_cast<double>(distance) * static_cast<double>(distance))) return 0.0f; // still out there
    const float away = static_cast<float>(std::sqrt(away2));
    const float fade = std::clamp((distance - away) / (distance / 3.0f), 0.0f, 1.0f);
    const float scale = std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
    WindState wind = frame.wind;
    wind.velocity.y = 0.0f;
    wind.gustiness = std::isfinite(wind.gustiness) ? std::clamp(wind.gustiness, 0.0f, 1.0f) : 0.0f;
    return max_sway_offset(wind, top * scale, w.sway, w.flutter) * fade;
}

} // namespace

// This frame's uploads, shared by the passes.
// The sky's fixed textures (sky_common.slang has the same sizes).
constexpr u32 k_sky_view_width = 192;
constexpr u32 k_sky_view_height = 108;
constexpr u32 k_multi_size = 32;  // sky_common.slang's k_multi_size
constexpr u32 k_aerial_size = 32; // its k_aerial_size
constexpr u32 k_cloud_map_size = 512;
constexpr u32 k_cloud_shadow_size = 256;
constexpr double k_cloud_shadow_metres = 32000.0; // the square the cloud shadow map covers
constexpr u32 k_cover_size = 256;        // the cover map (sky_common.slang has the same)
constexpr double k_cover_metres = 256.0; // the square it covers: a metre a texel
constexpr double k_weather_wrap = 4096.0; // ground patterns repeat this far (sky_common.slang)

struct Renderer::FrameData {
    rhi::TransientMemory frame;
    rhi::TransientMemory materials; // the frame's, then the default
    u32 material_count = 0;         // the frame's
    rhi::TransientMemory decals;    // decals, tiles, entries (DecalBins)
    u32 decal_offset = 0;
    u32 tile_offset = 0;
    u32 entry_offset = 0;
    u32 tile_columns = 0; // 0: no decals this frame
    u32 tile_rows = 0;
    rhi::TransientMemory instances;
    rhi::TransientMemory meshes;
    u32 instance_count = 0;
    u32 tree_levels = 0; // deepest LOD tree of any mesh drawn
    bool occlusion = false;
    bool use_mesh_shaders = false;
    // CPU culling: the chosen (instance, cluster) pairs and their range.
    rhi::TransientMemory entries;
    rhi::TransientMemory range;
    u32 entry_count = 0;
    // The voxel world's levels (gpu::VoxelLevel each), for the voxel view
    // and the far field; null when neither is on.
    rhi::TransientMemory voxel_levels;
    // Sun shadows: the page table and levels (null when off).
    rhi::TransientMemory shadow_table;
    rhi::TransientMemory shadow_levels;
};

std::unique_ptr<Renderer> Renderer::create(rhi::Device& device, const RendererDesc& desc) {
    auto renderer = std::make_unique<Renderer>(ConstructKey{}, device, desc);
    if (!renderer->init()) {
        return nullptr;
    }
    return renderer;
}

Renderer::Renderer(ConstructKey, rhi::Device& device, const RendererDesc& desc)
    : m_device(device), m_desc(desc), m_decal_bins(std::make_unique<DecalBins>()) {}

Renderer::~Renderer() {
    // Deferred by the device until the GPU is done with them.
    for (const rhi::PipelineHandle p :
         {m_raster_mesh, m_raster_vertex, m_resolve, m_cull_reset, m_cull_prepare, m_cull_instances,
          m_cull_traverse, m_cull_retest, m_hzb_first, m_hzb_down, m_smaa_edges, m_smaa_weights,
          m_smaa_blend, m_voxel_view, m_reflect, m_resolve_direct, m_shadow_mesh, m_shadow_vertex,
          m_shadow_clear_pages, m_shadow_clear_words, m_light_clear, m_light_voxels, m_sky_view_pass,
          m_cloud_map_pass, m_cloud_shadow_pass, m_clouds_pass, m_cover_pass, m_sky_multi_pass, m_sky_aerial_pass,
          m_sky_ambient_pass, m_skin, m_histogram_clear, m_histogram, m_bloom_down, m_bloom_up, m_post}) {
        m_device.destroy(p);
    }
    for (const rhi::TextureHandle t : m_bloom_downs) {
        m_device.destroy(t);
    }
    for (const rhi::TextureHandle t : m_bloom_ups) {
        m_device.destroy(t);
    }
    m_device.destroy(m_scene);
    for (const rhi::BufferHandle b : {m_histogram_bins, m_histogram_readback[0], m_histogram_readback[1],
                                      m_histogram_readback[2]}) {
        m_device.destroy(b);
    }
    for (const rhi::TextureHandle t : {m_sky_view, m_cloud_map, m_cloud_shadow, m_clouds_a, m_clouds_b, m_cover, m_sky_multi,
                                       m_sky_aerial, m_sky_aerial_through, m_sky_ambient}) {
        m_device.destroy(t);
    }
    m_device.destroy(m_voxel_light);
    m_device.destroy(m_smaa_area);
    m_device.destroy(m_smaa_search);
    for (Mesh& mesh : m_meshes) {
        destroy(mesh);
    }
    for (const rhi::TextureHandle t : m_textures) {
        m_device.destroy(t);
    }
    m_device.destroy(m_depth);
    m_device.destroy(m_vis);
    m_device.destroy(m_hzb);
    m_device.destroy(m_color);
    m_device.destroy(m_edges);
    m_device.destroy(m_weights);
    m_device.destroy(m_radiance);
    m_device.destroy(m_gloss);
    m_device.destroy(m_fallback);
    for (const CullBuffers* c : {&m_cull, &m_shadow_cull}) {
        for (const rhi::BufferHandle b : {c->counters, c->args, c->queues[0], c->queues[1], c->visible,
                                          c->occluded_clusters, c->occluded_instances, c->readback[0],
                                          c->readback[1], c->readback[2]}) {
            m_device.destroy(b);
        }
    }
    for (const rhi::BufferHandle b : {m_shadow_atlas, m_shadow_requests, m_shadow_readback[0], m_shadow_readback[1],
                                      m_shadow_readback[2]}) {
        m_device.destroy(b);
    }
}

bool Renderer::init() {
    m_streamer = std::make_unique<GeometryStreamer>(m_device, m_desc.geometry_pool_bytes);
    if (!m_streamer->init()) {
        return false;
    }
    m_voxels = std::make_unique<VoxelScene>(m_device, m_desc.voxel_clipmap, m_desc.voxel_bricks);
    m_store = std::make_unique<InstanceStore>(m_device);
    if (!m_voxels->init()) {
        return false;
    }
    GeometryStreamer::Budget budget;
    budget.bytes_per_frame = std::max<u64>(m_desc.stream_bytes_per_frame, 1);
    m_streamer->set_budget(budget);
    const auto compute = [this](rhi::ShaderCode code, const char* name) {
        rhi::ComputePipelineDesc desc;
        desc.compute = code;
        desc.name = name;
        return m_device.create_pipeline(desc);
    };
    m_cull_reset = compute(shaders::cull_reset_cs(), "cull: reset");
    m_cull_prepare = compute(shaders::cull_prepare_cs(), "cull: prepare arguments");
    m_cull_instances = compute(shaders::cull_instances_cs(), "cull: instances");
    m_cull_traverse = compute(shaders::cull_traverse_cs(), "cull: LOD trees");
    m_cull_retest = compute(shaders::cull_retest_cs(), "cull: retest hidden clusters");
    m_hzb_first = compute(shaders::hzb_first_cs(), "HZB: from depth");
    m_hzb_down = compute(shaders::hzb_down_cs(), "HZB: down");
    m_shadow_clear_pages = compute(shaders::shadow_clear_pages_cs(), "shadows: clear pages");
    m_shadow_clear_words = compute(shaders::shadow_clear_words_cs(), "shadows: clear requests");
    m_light_clear = compute(shaders::light_clear_cs(), "GI: clear new bricks");
    m_light_voxels = compute(shaders::light_voxels_cs(), "GI: light voxels");
    m_sky_view_pass = compute(shaders::sky_view_cs(), "sky: view table");
    m_cloud_map_pass = compute(shaders::cloud_map_cs(), "sky: cloud pattern");
    m_cloud_shadow_pass = compute(shaders::cloud_shadow_cs(), "sky: cloud shadows");
    m_clouds_pass = compute(shaders::clouds_cs(), "sky: clouds");
    m_cover_pass = compute(shaders::weather_cover_cs(), "weather: cover from above");
    m_sky_multi_pass = compute(shaders::sky_multi_cs(), "sky: light scattered many times");
    m_sky_aerial_pass = compute(shaders::sky_aerial_cs(), "sky: the air over the view");
    m_sky_ambient_pass = compute(shaders::sky_ambient_cs(), "sky: its light from all round");
    m_skin = compute(shaders::skin_cs(), "skin: pose meshes");
    m_histogram_clear = compute(shaders::histogram_clear_cs(), "exposure: clear the histogram");
    m_histogram = compute(shaders::histogram_cs(), "exposure: histogram");
    m_bloom_down = compute(shaders::bloom_down_cs(), "bloom: down");
    m_bloom_up = compute(shaders::bloom_up_cs(), "bloom: up");
    m_light_plan = std::make_unique<VoxelLight>(m_voxels->capacity());
    ShadowDesc shadow_desc;
    shadow_desc.pool_pages = m_desc.shadow_pages;
    m_shadows = std::make_unique<ShadowPages>(shadow_desc);
    m_shadow_readback_windows = std::make_unique<ShadowWindows[]>(3);

    rhi::GraphicsPipelineDesc raster;
    raster.vertex = shaders::raster_vs();
    raster.pixel = shaders::raster_ps();
    raster.color_formats[0] = rhi::Format::rg32_uint;
    raster.depth_format = rhi::Format::depth32_float;
    raster.depth = rhi::DepthMode::test_write;
    raster.cull = rhi::CullMode::back;
    raster.name = "clusters to visibility (vertex)";
    m_raster_vertex = m_device.create_pipeline(raster);
    if (m_device.adapter().mesh_shaders) {
        raster.geometry = rhi::GeometryStage::mesh;
        raster.mesh = shaders::raster_ms();
        raster.name = "clusters to visibility (mesh)";
        m_raster_mesh = m_device.create_pipeline(raster);
        if (!m_raster_mesh.valid()) {
            log_warning("render", "mesh shader pipeline failed; drawing with vertex shaders");
        }
    }
    // Shadow pages: the same geometry stages, no targets (the pixel shader
    // writes the atlas), both faces (thin and open meshes cast too).
    rhi::GraphicsPipelineDesc shadow;
    shadow.vertex = shaders::raster_vs();
    shadow.pixel = shaders::raster_shadow_ps();
    shadow.color_count = 0;
    shadow.cull = rhi::CullMode::none;
    shadow.name = "clusters to shadow pages (vertex)";
    m_shadow_vertex = m_device.create_pipeline(shadow);
    if (m_raster_mesh.valid()) {
        shadow.geometry = rhi::GeometryStage::mesh;
        shadow.mesh = shaders::raster_ms();
        shadow.name = "clusters to shadow pages (mesh)";
        m_shadow_mesh = m_device.create_pipeline(shadow);
    }
    rhi::GraphicsPipelineDesc resolve;
    resolve.vertex = shaders::resolve_vs();
    resolve.pixel = shaders::resolve_ps();
    resolve.color_formats[0] = rhi::Format::rgba16_float; // radiance
    resolve.color_formats[1] = rhi::Format::rg32_uint;    // gloss
    resolve.color_formats[2] = rhi::Format::rgba16_float; // the reflected light used
    resolve.color_count = 3;
    resolve.cull = rhi::CullMode::none;
    resolve.name = "shade visibility";
    m_resolve = m_device.create_pipeline(resolve);
    resolve.pixel = shaders::resolve_direct_ps(); // without screen reflections: the radiance alone
    resolve.color_formats = {};
    resolve.color_formats[0] = rhi::Format::rgba16_float;
    resolve.color_count = 1;
    resolve.name = "shade visibility (no screen reflections)";
    m_resolve_direct = m_device.create_pipeline(resolve);
    rhi::GraphicsPipelineDesc reflect;
    reflect.vertex = shaders::reflect_vs();
    reflect.pixel = shaders::reflect_ps();
    reflect.color_formats[0] = rhi::Format::rgba16_float;
    reflect.cull = rhi::CullMode::none;
    reflect.name = "screen reflections";
    m_reflect = m_device.create_pipeline(reflect);
    rhi::GraphicsPipelineDesc post;
    post.vertex = shaders::post_vs();
    post.pixel = shaders::post_ps();
    post.color_formats[0] = rhi::Format::rgba8_srgb;
    post.cull = rhi::CullMode::none;
    post.name = "bloom, exposure and tone mapping";
    m_post = m_device.create_pipeline(post);
    rhi::GraphicsPipelineDesc smaa;
    smaa.vertex = shaders::smaa_vs();
    smaa.cull = rhi::CullMode::none;
    smaa.pixel = shaders::smaa_edges_ps();
    smaa.color_formats[0] = rhi::Format::rgba8_unorm;
    smaa.name = "SMAA: edges";
    m_smaa_edges = m_device.create_pipeline(smaa);
    smaa.pixel = shaders::smaa_weights_ps();
    smaa.name = "SMAA: blend weights";
    m_smaa_weights = m_device.create_pipeline(smaa);
    smaa.pixel = shaders::smaa_blend_ps();
    smaa.color_formats[0] = rhi::Format::rgba8_srgb;
    smaa.name = "SMAA: blend";
    m_smaa_blend = m_device.create_pipeline(smaa);
    rhi::GraphicsPipelineDesc voxels;
    voxels.vertex = shaders::voxels_vs();
    voxels.pixel = shaders::voxels_ps();
    voxels.color_formats[0] = rhi::Format::rgba8_srgb;
    voxels.cull = rhi::CullMode::none;
    voxels.name = "voxel view";
    m_voxel_view = m_device.create_pipeline(voxels);
    for (const rhi::PipelineHandle p :
         {m_cull_reset, m_cull_prepare, m_cull_instances, m_cull_traverse, m_cull_retest, m_hzb_first,
          m_hzb_down, m_raster_vertex, m_resolve, m_smaa_edges, m_smaa_weights, m_smaa_blend, m_voxel_view,
          m_reflect, m_resolve_direct, m_shadow_vertex, m_shadow_clear_pages, m_shadow_clear_words, m_light_clear,
          m_light_voxels, m_sky_view_pass, m_cloud_map_pass, m_cloud_shadow_pass, m_clouds_pass, m_cover_pass,
          m_sky_multi_pass, m_sky_aerial_pass, m_sky_ambient_pass, m_skin, m_histogram_clear, m_histogram,
          m_bloom_down, m_bloom_up, m_post}) {
        if (!p.valid()) {
            log_error("render", "could not create the renderer's pipelines");
            return false;
        }
    }
    SmaaTextures tables;
    if (!create_smaa_textures(m_device, tables)) {
        log_error("render", "could not create SMAA's lookup textures");
        return false;
    }
    m_smaa_area = tables.area;
    m_smaa_search = tables.search;
    // Auto exposure's histogram and its read-back copies (small: made once).
    {
        rhi::BufferDesc bins;
        bins.size = u64{k_exposure_bins} * 4;
        bins.usage = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
        bins.name = "exposure histogram";
        m_histogram_bins = m_device.create_buffer(bins);
        bins.usage = rhi::BufferUsage::none;
        bins.memory = rhi::Memory::readback;
        bins.name = "exposure histogram (read back)";
        for (rhi::BufferHandle& b : m_histogram_readback) {
            b = m_device.create_buffer(bins);
        }
        if (!m_histogram_bins.valid() || !m_histogram_readback[0].valid() || !m_histogram_readback[1].valid() ||
            !m_histogram_readback[2].valid()) {
            log_error("render", "could not create the exposure histogram");
            return false;
        }
    }
    // A finely cut sphere, so its LOD has room to work.
    const geometry::TriangleMesh shapes[] = {geometry::make_cube(), geometry::make_sphere(128, 64),
                                             geometry::make_plane()};
    for (const geometry::TriangleMesh& shape : shapes) {
        const MeshId id = add_mesh(geometry::build_clusters(shape));
        if (id == k_no_mesh) {
            log_error("render", "could not create the built-in meshes");
            return false;
        }
        m_builtin.push_back(id);
    }
    return true;
}

void Renderer::destroy(Mesh& mesh) {
    for (const rhi::BufferHandle b : {mesh.clusters, mesh.groups, mesh.nodes}) {
        m_device.destroy(b);
    }
    if (mesh.skin != nullptr) {
        m_device.destroy(mesh.skin->source);
    }
    mesh = {};
}

MeshId Renderer::add_mesh(geometry::ClusterMesh data) { return add(std::move(data), nullptr, 0); }

MeshId Renderer::add_skinned_mesh(geometry::SkinnedClusterMesh skinned, u32 joints) {
    const geometry::ClusterMesh& data = skinned.mesh;
    if (data.empty() || joints == 0 || joints > k_max_skin_joints || data.levels != 1 ||
        skinned.weights.size() != data.vertices.size()) {
        log_error("render", "a skinned mesh must be one level, one weight per vertex, 1 to 1024 joints; not added");
        return k_no_mesh;
    }
    for (const geometry::ClusterGroup& g : data.groups) {
        if (!geometry::is_root_group(g)) {
            log_error("render", "a skinned mesh must be its original triangles alone; not added");
            return k_no_mesh;
        }
    }
    for (const geometry::SkinWeights& w : skinned.weights) {
        if (u32{w.weights[0]} + w.weights[1] + w.weights[2] + w.weights[3] != 255) {
            log_error("render", "a skinned mesh's vertex weights do not sum to 255; not added");
            return k_no_mesh;
        }
        for (usize k = 0; k < 4; ++k) {
            if (w.weights[k] != 0 && w.joints[k] >= joints) {
                log_error("render", "a skinned mesh's vertex is moved by a joint it does not have; not added");
                return k_no_mesh;
            }
        }
    }
    return add(std::move(skinned.mesh), &skinned.weights, joints);
}

bool Renderer::skinned(MeshId id) const {
    return id < m_meshes.size() && m_meshes[id].live && m_meshes[id].skin != nullptr;
}

bool Renderer::set_pose(MeshId id, std::span<const Mat4> skin, const geometry::Sphere& bounds) {
    if (!skinned(id)) {
        return false;
    }
    Skin& s = *m_meshes[id].skin;
    if (skin.size() != s.joints || !std::isfinite(bounds.center.x) || !std::isfinite(bounds.center.y) ||
        !std::isfinite(bounds.center.z) || !std::isfinite(bounds.radius) || !(bounds.radius >= 0.0f)) {
        return false;
    }
    for (const Mat4& m : skin) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(m.at(r, c))) {
                    return false;
                }
            }
        }
    }
    for (usize j = 0; j < skin.size(); ++j) {
        float* rows = s.matrices.data() + j * 12;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                rows[r * 4 + c] = skin[j].at(r, c);
            }
        }
    }
    s.bounds = bounds;
    ++s.version;
    return true;
}

MeshId Renderer::add(geometry::ClusterMesh data, const std::vector<geometry::SkinWeights>* weights, u32 joints) {
    if (data.empty() || m_meshes.size() >= k_no_mesh) {
        return k_no_mesh;
    }
    // Per cluster: where its geometry sits in its group's streamed data
    // (the streamer lays that out below), then its LOD data.
    std::vector<gpu::Cluster> clusters(data.clusters.size());
    for (u32 ci = 0; ci < data.clusters.size(); ++ci) {
        const geometry::Cluster& c = data.clusters[ci];
        gpu::Cluster& g = clusters[ci];
        g.group = c.group;
        g.refined = c.refined >= 0 ? static_cast<u32>(c.refined) : gpu::k_no_group;
        g.level = data.groups[c.group].depth;
        set_bounds(g.bounds, c.bounds.center, c.bounds.radius);
    }
    std::vector<gpu::Group> groups;
    groups.reserve(data.groups.size());
    for (const geometry::ClusterGroup& gr : data.groups) {
        gpu::Group g{};
        set_bounds(g.bounds, gr.simplified.center, gr.simplified.radius);
        g.error = gr.simplified.error;
        g.first_cluster = gr.first_cluster;
        g.cluster_count = gr.cluster_count;
        g.depth = gr.depth;
        groups.push_back(g);
    }
    std::vector<gpu::Node> nodes;
    nodes.reserve(data.nodes.size());
    for (const geometry::ClusterNode& n : data.nodes) {
        gpu::Node g{};
        set_bounds(g.bounds, n.bounds.center, n.bounds.radius);
        g.error = n.bounds.error;
        g.group = n.group >= 0 ? static_cast<u32>(n.group) : gpu::k_no_group;
        g.child_offset = n.child_offset;
        g.child_count = n.child_count;
        nodes.push_back(g);
    }
    // The GPU walks one tree level per dispatch: find the deepest.
    u32 tree_levels = 0;
    {
        std::vector<std::pair<u32, u32>> stack; // node, depth
        for (u32 r = 0; r < data.levels && r < data.nodes.size(); ++r) {
            stack.push_back({r, 0});
        }
        while (!stack.empty()) {
            const auto [node, depth] = stack.back();
            stack.pop_back();
            tree_levels = std::max(tree_levels, depth + 1);
            const geometry::ClusterNode& n = data.nodes[node];
            if (n.group < 0 && depth + 1 <= gpu::k_max_tree_levels) {
                for (u32 i = 0; i < n.child_count; ++i) {
                    stack.push_back({n.child_offset + i, depth + 1});
                }
            }
        }
    }
    if (tree_levels == 0 || tree_levels > gpu::k_max_tree_levels) {
        log_error("render", "a mesh's LOD tree is empty or deeper than 24 levels; mesh not added");
        return k_no_mesh;
    }

    Mesh mesh;
    const auto upload = [this](std::span<const u8> bytes, const char* name) {
        rhi::BufferDesc desc;
        desc.usage = rhi::BufferUsage::shader_read;
        desc.size = std::max<u64>(bytes.size(), 4);
        desc.name = name;
        return m_device.create_buffer(desc, bytes);
    };
    // A removed mesh's slot first, so meshes coming and going (terrain
    // tiles rebuilt) do not grow the mesh table every frame walks.
    const u32 id = m_free_meshes.empty() ? static_cast<u32>(m_meshes.size()) : m_free_meshes.back();
    if (!m_streamer->add(id, data, clusters)) {
        return k_no_mesh;
    }
    // A skinned mesh's source: its vertices at bind, cluster by cluster in
    // the order the pool has them (skin.slang poses them into it).
    std::vector<gpu::SkinVertex> source;
    if (weights != nullptr) {
        ClusterPacking packing;
        for (u32 ci = 0; ci < data.clusters.size(); ++ci) {
            packing.pack(data, ci);
            clusters[ci].skin_first = static_cast<u32>(source.size());
            for (const u32 v : packing.order) {
                const geometry::PackedVertex p = geometry::pack_vertex(data.vertices[v]);
                const geometry::SkinWeights& w = (*weights)[v];
                gpu::SkinVertex out{};
                std::copy_n(p.position, 3, out.position);
                out.normal = p.normal;
                out.tangent = p.tangent;
                for (usize k = 0; k < 4; ++k) {
                    out.joints[k] = w.weights[k] != 0 ? w.joints[k] : u16{0};
                    out.weights[k] = w.weights[k];
                }
                source.push_back(out);
            }
        }
        mesh.skin = std::make_unique<Skin>();
        mesh.skin->joints = joints;
        mesh.skin->bounds = data.bounds;
        mesh.skin->posed_bounds = data.bounds;
        // Until posed: the identity (the bind pose).
        mesh.skin->matrices.assign(usize{joints} * 12, 0.0f);
        for (u32 j = 0; j < joints; ++j) {
            float* rows = mesh.skin->matrices.data() + usize{j} * 12;
            rows[0] = rows[5] = rows[10] = 1.0f;
        }
        mesh.skin->source = upload(bytes_of(source), "skinned mesh source");
    }
    if (weights != nullptr) {
        // Its clusters' spheres are refit to every pose.
        rhi::BufferDesc desc;
        desc.usage = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
        desc.size = std::max<u64>(clusters.size() * sizeof(gpu::Cluster), 4);
        desc.name = "skinned mesh clusters";
        mesh.clusters = m_device.create_buffer(desc, bytes_of(clusters));
    } else {
        mesh.clusters = upload(bytes_of(clusters), "mesh clusters");
    }
    mesh.groups = upload(bytes_of(groups), "mesh cluster groups");
    mesh.nodes = upload(bytes_of(nodes), "mesh LOD tree");
    for (const rhi::BufferHandle b : {mesh.clusters, mesh.groups, mesh.nodes,
                                      mesh.skin != nullptr ? mesh.skin->source : mesh.nodes}) {
        if (!b.valid()) {
            destroy(mesh);
            m_streamer->remove(id);
            log_error("render", "could not upload a mesh (out of GPU memory?)");
            return k_no_mesh;
        }
    }
    mesh.tree_levels = tree_levels;
    mesh.top = 0.0f;
    for (const geometry::Vertex& v : data.vertices) {
        if (std::isfinite(v.position.y)) mesh.top = std::max(mesh.top, v.position.y);
    }
    mesh.data = std::move(data);
    mesh.live = true;
    if (mesh.skin != nullptr) {
        m_skinned.push_back(id);
    }
    if (id == m_meshes.size()) {
        m_meshes.push_back(std::move(mesh));
    } else {
        m_free_meshes.pop_back();
        m_meshes[id] = std::move(mesh);
    }
    ++m_meshes_version;
    return id;
}

void Renderer::remove_mesh(MeshId id) {
    if (id >= m_meshes.size() || std::find(m_builtin.begin(), m_builtin.end(), id) != m_builtin.end()) {
        return;
    }
    if (!m_meshes[id].live) {
        return; // removed already
    }
    if (const Skin* skin = m_meshes[id].skin.get(); skin != nullptr) {
        m_skinned.erase(std::find(m_skinned.begin(), m_skinned.end(), id));
        // Its shadow where the pose had it (shadow tracking sees only the
        // bind sphere go). With shadows off, all is redrawn when they return.
        if (skin->shadow_known && m_shadows_on) {
            m_shadows->invalidate(skin->shadow_min, skin->shadow_max);
        }
    }
    destroy(m_meshes[id]); // the slot stays, empty, until a new mesh takes it
    m_streamer->remove(id);
    m_voxels->forget_mesh(id);
    m_free_meshes.push_back(id);
    ++m_meshes_version;
}

TextureId Renderer::add_texture(const rhi::TextureDesc& desc, std::span<const u8> contents) {
    const char* problem = rhi::texture_desc_problem(desc);
    if (problem == nullptr && (desc.array || desc.layers != 1 || desc.usage != rhi::TextureUsage::sampled)) {
        problem = "a material texture is one sampled layer, not an array";
    }
    if (problem == nullptr) {
        problem = rhi::texture_contents_problem(desc, contents.size());
    }
    if (problem != nullptr) {
        log_error("render", std::string("texture refused: ") + problem);
        return k_no_texture;
    }
    rhi::TextureDesc named = desc;
    named.name = "material texture";
    const rhi::TextureHandle handle = m_device.create_texture(named, contents);
    if (!handle.valid()) {
        return k_no_texture; // the device logged why
    }
    if (!m_free_textures.empty()) {
        const TextureId id = m_free_textures.back();
        m_free_textures.pop_back();
        m_textures[id] = handle;
        return id;
    }
    m_textures.push_back(handle);
    return static_cast<TextureId>(m_textures.size() - 1);
}

void Renderer::remove_texture(TextureId id) {
    if (id >= m_textures.size() || !m_textures[id].valid()) {
        return;
    }
    m_device.destroy(m_textures[id]); // deferred by the device past frames in flight
    m_textures[id] = {};
    m_free_textures.push_back(id);
}

const rhi::TextureDesc* Renderer::texture(TextureId id) const {
    return id < m_textures.size() && m_textures[id].valid() ? m_device.describe(m_textures[id]) : nullptr;
}

bool Renderer::mesh_ready(MeshId id) const {
    return id < m_meshes.size() && m_meshes[id].live && m_streamer->ready(id);
}

const geometry::ClusterMesh* Renderer::mesh(MeshId id) const {
    return id < m_meshes.size() && m_meshes[id].live ? &m_meshes[id].data : nullptr;
}

std::vector<VoxelProbeLevel> Renderer::probe_voxels(const DVec3& position) const {
    std::vector<VoxelProbeLevel> out;
    for (u32 level = 0; level < m_voxels->levels(); ++level) {
        out.push_back({level, m_voxels->clipmap().voxel_size(level), to_string(m_voxels->probe(level, position))});
    }
    return out;
}

MeshId Renderer::builtin(BuiltinMesh mesh) const { return m_builtin[static_cast<usize>(mesh)]; }

Mat4 Renderer::view_projection(const View& view, float aspect) {
    const float fov = degrees_to_radians(std::clamp(view.vertical_fov_degrees, 1.0f, 170.0f));
    const float near_plane = std::max(view.near_plane, 1e-4f);
    return perspective_reverse_z(fov, aspect, near_plane) * view_from_orientation(view.orientation);
}

bool Renderer::ensure_targets(u32 width, u32 height) {
    if (m_depth.valid() && m_vis.valid() && m_hzb.valid() && m_color.valid() && m_edges.valid() &&
        m_weights.valid() && m_radiance.valid() && m_gloss.valid() && m_fallback.valid() && m_scene.valid() &&
        m_width == width && m_height == height) {
        return true;
    }
    m_device.destroy(m_depth);
    m_device.destroy(m_vis);
    m_device.destroy(m_hzb);
    m_device.destroy(m_color);
    m_device.destroy(m_edges);
    m_device.destroy(m_weights);
    m_device.destroy(m_radiance);
    m_device.destroy(m_gloss);
    m_device.destroy(m_fallback);
    m_device.destroy(m_scene);
    for (rhi::TextureHandle& t : m_bloom_downs) { // sized to the frame: made again below
        m_device.destroy(t);
        t = {};
    }
    for (rhi::TextureHandle& t : m_bloom_ups) {
        m_device.destroy(t);
        t = {};
    }
    m_device.destroy(m_clouds_a); // half the frame's size: made again with the sky's
    m_device.destroy(m_clouds_b);
    m_clouds_a = {};
    m_clouds_b = {};
    rhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = rhi::Format::depth32_float;
    desc.usage = rhi::TextureUsage::depth_target | rhi::TextureUsage::sampled;
    desc.name = "scene depth";
    m_depth = m_device.create_texture(desc);
    desc.format = rhi::Format::rg32_uint;
    desc.usage = rhi::TextureUsage::render_target | rhi::TextureUsage::sampled;
    desc.name = "visibility buffer";
    m_vis = m_device.create_texture(desc);
    // Anti-aliasing: the shaded image, then SMAA's two working images.
    desc.format = rhi::Format::rgba8_srgb;
    desc.name = "shaded image";
    m_color = m_device.create_texture(desc);
    desc.format = rhi::Format::rgba8_unorm;
    desc.name = "SMAA edges";
    m_edges = m_device.create_texture(desc);
    desc.name = "SMAA blend weights";
    m_weights = m_device.create_texture(desc);
    // Shading's output before the screen reflections pass finishes it.
    desc.format = rhi::Format::rgba16_float;
    desc.name = "radiance";
    m_radiance = m_device.create_texture(desc);
    desc.name = "reflected light";
    m_fallback = m_device.create_texture(desc);
    desc.name = "light with screen reflections";
    m_scene = m_device.create_texture(desc);
    desc.format = rhi::Format::rg32_uint;
    desc.name = "gloss";
    m_gloss = m_device.create_texture(desc);
    // The HZB: mip 0 the largest power of two not above the viewport.
    m_hzb_width = std::bit_floor(width);
    m_hzb_height = std::bit_floor(height);
    m_hzb_mips = static_cast<u32>(std::bit_width(std::max(m_hzb_width, m_hzb_height)));
    u64 texels = 0;
    for (u32 m = 0; m < m_hzb_mips; ++m) {
        texels += u64{std::max(m_hzb_width >> m, 1u)} * std::max(m_hzb_height >> m, 1u);
    }
    rhi::BufferDesc hzb;
    hzb.size = texels * 4;
    hzb.usage = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
    hzb.name = "depth pyramid (HZB)";
    m_hzb = m_device.create_buffer(hzb);
    m_hzb_valid = false;
    m_width = width;
    m_height = height;
    if (!m_depth.valid() || !m_vis.valid() || !m_hzb.valid() || !m_color.valid() || !m_edges.valid() ||
        !m_weights.valid() || !m_radiance.valid() || !m_gloss.valid() || !m_fallback.valid() || !m_scene.valid()) {
        // Half-made targets are not reused: the next frame tries again.
        m_width = 0;
        m_height = 0;
        log_error("render", "could not create the frame's targets (out of GPU memory?)");
        return false;
    }
    // The sky's textures with the frame's (rule 9: no allocation on the
    // first frame with weather); a failure is tried again on the next resize.
    m_sky_failed = false;
    ensure_sky_targets();
    // Bloom's levels likewise (without them, no bloom: the frame still draws).
    m_post_logged = false;
    ensure_post_targets();
    return true;
}

bool Renderer::ensure_post_targets() {
    bool ok = true;
    u32 w = m_width;
    u32 h = m_height;
    for (u32 i = 0; i < k_bloom_levels; ++i) {
        w = std::max((w + 1) / 2, 1u);
        h = std::max((h + 1) / 2, 1u);
        m_bloom_width[i] = w;
        m_bloom_height[i] = h;
        const auto make = [&](rhi::TextureHandle& t, const char* name) {
            if (t.valid()) {
                return;
            }
            rhi::TextureDesc desc;
            desc.width = w;
            desc.height = h;
            desc.format = rhi::Format::rgba16_float;
            desc.usage = rhi::TextureUsage::sampled | rhi::TextureUsage::shader_write;
            desc.name = name;
            t = m_device.create_texture(desc);
            ok = ok && t.valid();
        };
        make(m_bloom_downs[i], "bloom: down");
        if (i + 1 < k_bloom_levels) {
            make(m_bloom_ups[i], "bloom: up");
        }
    }
    m_bloom_ready = ok;
    if (!ok && !m_post_logged) {
        // Tried again with the next resize.
        m_post_logged = true;
        log_error("render", "could not create bloom's textures (out of GPU memory?)");
    }
    return ok;
}

bool Renderer::ensure_cull_buffers(CullBuffers& c, u32 instance_count) {
    const auto make = [this](u64 size, rhi::BufferUsage usage, rhi::Memory memory, const char* name) {
        rhi::BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        desc.memory = memory;
        desc.name = name;
        return m_device.create_buffer(desc);
    };
    const auto rw = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
    const bool complete = c.counters.valid() && c.args.valid() && c.queues[0].valid() &&
                          c.queues[1].valid() && c.visible.valid() && c.occluded_clusters.valid() &&
                          c.readback[0].valid() && c.readback[1].valid() && c.readback[2].valid();
    if (!complete) {
        // All or nothing: a partial set from a failed attempt is made again.
        for (rhi::BufferHandle* b : {&c.counters, &c.args, &c.queues[0], &c.queues[1], &c.visible,
                                     &c.occluded_clusters, &c.readback[0], &c.readback[1],
                                     &c.readback[2]}) {
            m_device.destroy(*b);
            *b = {};
        }
        for (u64& tag : c.readback_frame) {
            tag = ~u64{0};
        }
        c.counters = make(gpu::k_counter_bytes, rw, rhi::Memory::gpu, "cull counters");
        c.args = make(gpu::k_args_bytes, rhi::BufferUsage::shader_write | rhi::BufferUsage::indirect,
                      rhi::Memory::gpu, "cull indirect arguments");
        for (rhi::BufferHandle& q : c.queues) {
            q = make(u64{k_queue_capacity} * sizeof(gpu::Entry), rw, rhi::Memory::gpu, "cull node queue");
        }
        c.visible = make(u64{k_visible_capacity} * sizeof(gpu::Entry), rw, rhi::Memory::gpu,
                         "visible clusters");
        c.occluded_clusters = make(u64{k_occluded_capacity} * sizeof(gpu::Entry), rw, rhi::Memory::gpu,
                                   "hidden clusters to retest");
        for (rhi::BufferHandle& r : c.readback) {
            r = make(gpu::k_counter_bytes, rhi::BufferUsage::none, rhi::Memory::readback,
                     "cull counters (read back)");
        }
    }
    if (instance_count > c.instance_capacity || !c.occluded_instances.valid()) {
        m_device.destroy(c.occluded_instances);
        c.instance_capacity = std::max(1024u, std::bit_ceil(instance_count));
        c.occluded_instances = make(u64{c.instance_capacity} * 4, rw, rhi::Memory::gpu,
                                    "hidden instances to retest");
    }
    for (const rhi::BufferHandle b :
         {c.counters, c.args, c.queues[0], c.queues[1], c.visible, c.occluded_clusters,
          c.occluded_instances, c.readback[0], c.readback[1], c.readback[2]}) {
        if (!b.valid()) {
            if (!m_cull_buffers_logged) {
                m_cull_buffers_logged = true;
                log_error("render", "could not create the GPU culling buffers (out of GPU memory?)");
            }
            return false;
        }
    }
    return true;
}

void Renderer::read_stats_slot(u32 slot) {
    const u8* bytes = m_device.mapped(m_cull.readback[slot]);
    if (bytes == nullptr) {
        return;
    }
    u32 counters[gpu::k_counter_slots];
    std::memcpy(counters, bytes, sizeof(counters));
    RenderStats s;
    s.from_gpu = true;
    s.clusters = std::min(counters[gpu::k_slot_visible], k_visible_capacity);
    s.triangles = counters[gpu::k_slot_triangles];
    s.instances = counters[gpu::k_slot_instances];
    s.occluded_instances = counters[gpu::k_slot_occluded_instances];
    s.occluded_clusters = counters[gpu::k_slot_occluded_clusters];
    m_request_count = counters[gpu::k_slot_requests];
    // A full retest list costs time only (the geometry is drawn at once);
    // a full queue or visible list drops geometry.
    s.overflow = (counters[gpu::k_slot_overflow] & (gpu::k_overflow_queue | gpu::k_overflow_visible)) != 0;
    if (s.overflow && !m_overflow_logged) {
        m_overflow_logged = true;
        log_warning("render", "GPU culling lists overflowed (flags " +
                                  std::to_string(counters[gpu::k_slot_overflow]) +
                                  "): some geometry was not drawn");
    }
    m_gpu_stats = s;
}

void Renderer::read_gpu_stats() {
    u32 newest = 3;
    for (u32 i = 0; i < 3; ++i) {
        if (m_cull.readback_frame[i] != ~u64{0} &&
            (newest == 3 || m_cull.readback_frame[i] > m_cull.readback_frame[newest])) {
            newest = i;
        }
    }
    if (newest < 3 && m_cull.readback[newest].valid()) {
        read_stats_slot(newest);
        // The GPU's counts replace the CPU's guesses; what the CPU knows
        // exactly (draws, materials, decals) stays.
        const RenderStats cpu = m_stats;
        m_stats = m_gpu_stats;
        m_stats.draws = cpu.draws;
        m_stats.post_draws = cpu.post_draws;
        m_stats.materials = cpu.materials;
        m_stats.decals = cpu.decals;
        m_stats.decal_overflow = cpu.decal_overflow;
        copy_streaming_stats();
    }
}

void Renderer::pose_skins(rhi::CommandList& cmd) {
    m_posed = 0;
    if (m_skinned.empty() || !m_streamer->tables_current()) {
        return; // (stale tables: posed once they are current)
    }
    bool begun = false;
    for (const MeshId id : m_skinned) {
        Mesh& mesh = m_meshes[id];
        Skin& skin = *mesh.skin;
        // A new pose, or just loaded (the pool holds the bind pose): pose it.
        if (skin.posed == skin.version || !m_streamer->ready(id)) {
            continue;
        }
        const rhi::TransientMemory matrices = cmd.upload(bytes_of(skin.matrices));
        if (matrices.data == nullptr) {
            break; // out of transient memory: next frame
        }
        if (!begun) {
            begun = true;
            cmd.begin_timing("skin");
            cmd.set_pipeline(m_skin);
            cmd.barrier(m_streamer->pool(), rhi::ResourceState::shader_write);
        }
        cmd.barrier(mesh.clusters, rhi::ResourceState::shader_write);
        gpu::SkinPush push{};
        push.source = m_device.read_descriptor(skin.source);
        push.clusters = m_device.write_descriptor(mesh.clusters);
        push.residency = m_device.read_descriptor(m_streamer->residency_table(id));
        push.pool = m_device.write_descriptor(m_streamer->pool());
        push.matrices = matrices.descriptor;
        push.matrix_offset = matrices.offset;
        push.cluster_count = static_cast<u32>(mesh.data.clusters.size());
        push.joint_count = skin.joints;
        cmd.push(push);
        const u32 n = push.cluster_count;
        cmd.dispatch(std::min(n, gpu::k_skin_dispatch_width), (n + gpu::k_skin_dispatch_width - 1) / gpu::k_skin_dispatch_width);
        cmd.barrier(mesh.clusters, rhi::ResourceState::shader_read);
        skin.posed = skin.version;
        skin.posed_bounds = skin.bounds;
        ++m_posed;
    }
    if (begun) {
        cmd.barrier(m_streamer->pool(), rhi::ResourceState::shader_read);
        cmd.end_timing();
    }
}

void Renderer::track_skin_shadows(const FrameInput& frame) {
    if (!m_shadows_on || m_skinned.empty()) {
        for (const MeshId id : m_skinned) {
            m_meshes[id].skin->shadow_known = false; // casters may change meanwhile (invalidate_all on return)
        }
        return;
    }
    // The world box around a skinned mesh's instances drawn this frame.
    // Shadow tracking sees the bind sphere; a pose moves past it, so each
    // change of pose or place dirties where its casters were and are.
    m_skin_boxes.assign(m_meshes.size(), SkinBox{});
    for (const u32 i : m_valid) {
        const Instance& in = frame.instances[i];
        const Mesh& mesh = m_meshes[in.mesh];
        if (mesh.skin == nullptr) {
            continue;
        }
        const geometry::Sphere& b = mesh.skin->posed_bounds;
        const Vec3 c = rotate(in.rotation, Vec3{b.center.x * in.scale.x, b.center.y * in.scale.y, b.center.z * in.scale.z});
        const double r = static_cast<double>(b.radius) *
                         std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
        const DVec3 centre = in.position + DVec3{c.x, c.y, c.z};
        const DVec3 lo = centre - DVec3{r, r, r}, hi = centre + DVec3{r, r, r};
        SkinBox& box = m_skin_boxes[in.mesh];
        box.min = box.any ? DVec3{std::min(box.min.x, lo.x), std::min(box.min.y, lo.y), std::min(box.min.z, lo.z)} : lo;
        box.max = box.any ? DVec3{std::max(box.max.x, hi.x), std::max(box.max.y, hi.y), std::max(box.max.z, hi.z)} : hi;
        box.any = true;
    }
    for (const MeshId id : m_skinned) {
        Skin& skin = *m_meshes[id].skin;
        const SkinBox& box = m_skin_boxes[id];
        // The pose drawn is the one in the pool.
        const bool same = skin.shadow_known == box.any &&
                          (!box.any || (skin.shadow_version == skin.posed && skin.shadow_min == box.min &&
                                        skin.shadow_max == box.max));
        if (same) {
            continue;
        }
        if (skin.shadow_known) {
            m_shadows->invalidate(skin.shadow_min, skin.shadow_max);
        }
        if (box.any) {
            m_shadows->invalidate(box.min, box.max);
        }
        skin.shadow_known = box.any;
        skin.shadow_min = box.min;
        skin.shadow_max = box.max;
        skin.shadow_version = skin.posed;
    }
}

void Renderer::select_on_cpu(const FrameInput& frame, const Mat4& view_proj, u32 height, u32 frame_number) {
    const float fov = degrees_to_radians(std::clamp(frame.view.vertical_fov_degrees, 1.0f, 170.0f));
    const geometry::Frustum frustum = geometry::frustum_from_view_projection(view_proj);
    geometry::LodView lod;
    lod.pixels_per_unit = 0.5f / std::tan(fov * 0.5f) * static_cast<float>(height);
    lod.near_plane = std::max(frame.view.near_plane, 1e-4f);
    lod.threshold_pixels = std::clamp(frame.settings.lod_error_pixels, 0.01f, 1000.0f);
    lod.frustum = &frustum;
    m_entries.clear();
    for (u32 slot = 0; slot < m_valid.size(); ++slot) {
        const Instance& in = frame.instances[m_valid[slot]];
        const Mesh& mesh = m_meshes[in.mesh];
        const geometry::ClusterMesh& data = mesh.data;
        lod.position = to_camera_relative(in.position, frame.view.position);
        lod.rotation = in.rotation;
        lod.scale = in.scale;
        m_selected.clear();
        m_wanted.clear();
        m_touched.clear();
        const geometry::LodResidency residency{&m_streamer->resident(in.mesh), &m_wanted, &m_touched};
        if (mesh.skin != nullptr) {
            // The CPU's cluster spheres are at bind: the pose's sphere
            // decides, then every cluster is drawn (one level, all terminal).
            const geometry::Sphere& b = mesh.skin->posed_bounds;
            const Vec3 c = lod.position + rotate(in.rotation, Vec3{b.center.x * in.scale.x, b.center.y * in.scale.y,
                                                                    b.center.z * in.scale.z});
            const float s = std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
            if (geometry::sphere_in_frustum(frustum, c, b.radius * s)) {
                lod.frustum = nullptr;
                geometry::select_clusters(data, lod, m_selected, &residency);
                lod.frustum = &frustum;
            }
        } else if (const float reach = wind_reach(frame, in, mesh.top); reach > 0.0f) {
            // Swaying: the CPU's cluster spheres are at rest, so the mesh's
            // sphere grown by how far it can sway decides, then every
            // cluster of the cut is drawn.
            const geometry::Sphere& b = data.bounds;
            const Vec3 c = lod.position + rotate(in.rotation, Vec3{b.center.x * in.scale.x, b.center.y * in.scale.y,
                                                                    b.center.z * in.scale.z});
            const float s = std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
            if (geometry::sphere_in_frustum(frustum, c, b.radius * s + reach)) {
                lod.frustum = nullptr;
                geometry::select_clusters(data, lod, m_selected, &residency);
                lod.frustum = &frustum;
            }
        } else {
            geometry::select_clusters(data, lod, m_selected, &residency);
        }
        m_streamer->want(in.mesh, m_wanted);
        m_streamer->touch(in.mesh, m_touched, frame_number);
        if (!m_selected.empty() && m_entries.size() / 2 < k_max_cpu_clusters) {
            ++m_stats.instances;
        }
        for (const u32 c : m_selected) {
            if (m_entries.size() / 2 >= k_max_cpu_clusters) {
                m_stats.overflow = true; // one mesh dispatch takes no more
                break;
            }
            m_entries.push_back(m_valid[slot]); // instances are addressed by their place in the frame
            m_entries.push_back(c);
            m_stats.triangles += data.clusters[c].triangle_count;
        }
    }
    m_stats.clusters = static_cast<u32>(m_entries.size() / 2);
}

void Renderer::raster(rhi::CommandList& cmd, const FrameData& data, u32 pass, bool gpu) {
    if (gpu) {
        cmd.barrier(m_cull.args, rhi::ResourceState::indirect_argument);
        cmd.barrier(m_cull.counters, rhi::ResourceState::shader_read);
        cmd.barrier(m_cull.visible, rhi::ResourceState::shader_read);
    }
    cmd.begin_timing("raster");
    rhi::RenderPassDesc desc;
    const rhi::LoadOp load = pass == 0 ? rhi::LoadOp::clear : rhi::LoadOp::load;
    desc.colors[0] = {m_vis, load, {0.0f, 0.0f, 0.0f, 0.0f}};
    desc.depth = {m_depth, load, 0.0f};
    cmd.begin_render_pass(desc);
    const bool drawing = gpu || data.entry_count > 0;
    if (drawing) {
        cmd.set_pipeline(data.use_mesh_shaders ? m_raster_mesh : m_raster_vertex);
        gpu::RasterPush push{};
        push.frame = data.frame.descriptor;
        push.frame_offset = data.frame.offset;
        push.instances = data.instances.descriptor;
        push.instance_offset = data.instances.offset;
        push.meshes = data.meshes.descriptor;
        push.mesh_offset = data.meshes.offset;
        if (gpu) {
            push.visible = m_device.read_descriptor(m_cull.visible);
            push.visible_offset = 0;
            push.range = m_device.read_descriptor(m_cull.counters);
            push.range_offset = gpu::k_slot_range * 4;
        } else {
            push.visible = data.entries.descriptor;
            push.visible_offset = data.entries.offset;
            push.range = data.range.descriptor;
            push.range_offset = data.range.offset;
        }
        cmd.push(push);
        if (gpu && data.use_mesh_shaders) {
            cmd.dispatch_mesh_indirect(m_cull.args, gpu::k_args_raster);
        } else if (gpu) {
            cmd.draw_indirect(m_cull.args, gpu::k_args_raster + 16);
        } else if (data.use_mesh_shaders) {
            const u32 n = data.entry_count;
            cmd.dispatch_mesh(std::min(n, k_max_mesh_groups), groups_of(n, k_max_mesh_groups));
        } else {
            cmd.draw(k_vertices_per_cluster, data.entry_count);
        }
        ++m_stats.draws;
    }
    cmd.end_render_pass();
    cmd.end_timing();
}

void Renderer::build_hzb(rhi::CommandList& cmd) {
    cmd.begin_timing("HZB");
    cmd.barrier(m_depth, rhi::ResourceState::shader_read);
    cmd.barrier(m_hzb, rhi::ResourceState::shader_write);
    gpu::HzbPush push{};
    push.depth = m_device.read_descriptor(m_depth);
    push.hzb = m_device.write_descriptor(m_hzb);
    push.width = m_width;
    push.height = m_height;
    push.hzb_width = m_hzb_width;
    push.hzb_height = m_hzb_height;
    cmd.set_pipeline(m_hzb_first);
    cmd.push(push);
    cmd.dispatch(groups_of(m_hzb_width, 8), groups_of(m_hzb_height, 8));
    cmd.set_pipeline(m_hzb_down);
    for (u32 mip = 1; mip < m_hzb_mips; ++mip) {
        cmd.barrier(m_hzb, rhi::ResourceState::shader_write); // waits for the mip above
        push.mip = mip;
        cmd.push(push);
        cmd.dispatch(groups_of(std::max(m_hzb_width >> mip, 1u), 8),
                     groups_of(std::max(m_hzb_height >> mip, 1u), 8));
    }
    cmd.barrier(m_hzb, rhi::ResourceState::shader_read);
    cmd.end_timing();
}

void Renderer::cull_barriers(rhi::CommandList& cmd, const CullBuffers& c, bool with_args) {
    // Every list the culling shaders write, ready for (more) writes; a
    // repeat is a UAV barrier, so the previous dispatch's writes land.
    for (const rhi::BufferHandle b : {c.counters, c.queues[0], c.queues[1], c.visible, c.occluded_clusters,
                                      c.occluded_instances, m_streamer->requests(), m_streamer->usage()}) {
        cmd.barrier(b, rhi::ResourceState::shader_write);
    }
    if (with_args) {
        cmd.barrier(c.args, rhi::ResourceState::shader_write);
    }
}

gpu::CullPush Renderer::cull_push(const CullBuffers& c, const FrameData& data, const rhi::TransientMemory& frame,
                                  bool streaming_requests) {
    gpu::CullPush base{};
    base.frame = frame.descriptor;
    base.frame_offset = frame.offset;
    base.instances = data.instances.descriptor;
    base.instance_offset = data.instances.offset;
    base.meshes = data.meshes.descriptor;
    base.mesh_offset = data.meshes.offset;
    base.counters = m_device.write_descriptor(c.counters);
    base.visible = m_device.write_descriptor(c.visible);
    base.occluded_instances = m_device.write_descriptor(c.occluded_instances);
    base.occluded_clusters = m_device.write_descriptor(c.occluded_clusters);
    base.hzb = m_device.read_descriptor(m_hzb);
    base.args = m_device.write_descriptor(c.args);
    base.queue_capacity = k_queue_capacity;
    base.visible_capacity = k_visible_capacity;
    base.occluded_capacity = k_occluded_capacity;
    base.instance_capacity = c.instance_capacity;
    base.requests = m_device.write_descriptor(m_streamer->requests());
    // Without requests (shadow pages): what is loaded is drawn. The usage
    // marks still keep the groups they use loaded.
    base.request_capacity = streaming_requests ? GeometryStreamer::k_request_capacity : 0u;
    base.usage = m_device.write_descriptor(m_streamer->usage());
    return base;
}

void Renderer::cull_first_pass(rhi::CommandList& cmd, CullBuffers& c, const FrameData& data,
                               const gpu::CullPush& base) {
    cmd.barrier(m_hzb, rhi::ResourceState::shader_read);
    cull_barriers(cmd, c, true);
    cmd.set_pipeline(m_cull_reset);
    cmd.push(base);
    cmd.dispatch(1);

    // Pass 0: against last frame's HZB (when there is one, and occlusion is on).
    cull_barriers(cmd, c, false);
    {
        gpu::CullPush p = base;
        p.pass = 0;
        p.queue_out = m_device.write_descriptor(c.queues[0]);
        p.slot_out = gpu::level_slot(0, 0);
        cmd.set_pipeline(m_cull_instances);
        cmd.push(p);
        cmd.dispatch(groups_of(data.instance_count, 64));
    }
    cull_walk(cmd, c, base, data, 0);
    cull_prepare(cmd, c, base, 0, 1, 0, k_visible_capacity, gpu::k_args_raster);
}

void Renderer::cull_prepare(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base, u32 pass,
                            u32 mode, u32 slot_in, u32 capacity, u32 arg_offset) {
    cull_barriers(cmd, c, true);
    gpu::CullPush p = base;
    p.pass = pass;
    p.mode = mode;
    p.slot_in = slot_in;
    p.queue_capacity = capacity;
    p.arg_offset = arg_offset;
    cmd.set_pipeline(m_cull_prepare);
    cmd.push(p);
    cmd.dispatch(1);
}

void Renderer::cull_walk(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base,
                         const FrameData& data, u32 pass) {
    for (u32 level = 0; level < data.tree_levels; ++level) {
        cull_prepare(cmd, c, base, pass, 0, gpu::level_slot(pass, level), k_queue_capacity, gpu::k_args_traverse);
        cull_barriers(cmd, c, false);
        cmd.barrier(c.args, rhi::ResourceState::indirect_argument);
        gpu::CullPush p = base;
        p.pass = pass;
        p.queue_in = m_device.write_descriptor(c.queues[level % 2]);
        p.queue_out = m_device.write_descriptor(c.queues[(level + 1) % 2]);
        p.slot_in = gpu::level_slot(pass, level);
        p.slot_out = gpu::level_slot(pass, level + 1);
        cmd.set_pipeline(m_cull_traverse);
        cmd.push(p);
        cmd.dispatch_indirect(c.args, gpu::k_args_traverse);
    }
}

void Renderer::cull_on_gpu(rhi::CommandList& cmd, const FrameData& data) {
    CullBuffers& c = m_cull;
    // Timed apart from the raster passes and HZB builds between them.
    cmd.begin_timing("culling");
    const gpu::CullPush base = cull_push(c, data, data.frame, true);
    cull_first_pass(cmd, c, data, base);
    cmd.end_timing();
    raster(cmd, data, 0, true);

    if (data.occlusion) {
        // Pass 1: what pass 0 found hidden, against the HZB of what it drew.
        build_hzb(cmd);
        cmd.begin_timing("culling");
        cull_prepare(cmd, c, base, 1, 0, gpu::k_slot_occluded_instances, c.instance_capacity, gpu::k_args_instances);
        cull_prepare(cmd, c, base, 1, 0, gpu::k_slot_occluded_clusters, k_occluded_capacity, gpu::k_args_retest);
        cull_barriers(cmd, c, false);
        cmd.barrier(c.args, rhi::ResourceState::indirect_argument);
        gpu::CullPush p = base;
        p.pass = 1;
        p.queue_out = m_device.write_descriptor(c.queues[0]);
        p.slot_out = gpu::level_slot(1, 0);
        cmd.set_pipeline(m_cull_instances);
        cmd.push(p);
        cmd.dispatch_indirect(c.args, gpu::k_args_instances);
        cull_barriers(cmd, c, false);
        cmd.set_pipeline(m_cull_retest);
        cmd.push(p);
        cmd.dispatch_indirect(c.args, gpu::k_args_retest);
        cull_walk(cmd, c, base, data, 1);
        cull_prepare(cmd, c, base, 1, 1, 0, k_visible_capacity, gpu::k_args_raster);
        cmd.end_timing();
        raster(cmd, data, 1, true);
        // For the next frame's first pass.
        build_hzb(cmd);
    }

    // Counts for the stats, read once the GPU is done with this frame.
    const u64 frame_index = m_device.frames_completed();
    const u32 slot = static_cast<u32>(frame_index % 3);
    cmd.barrier(c.counters, rhi::ResourceState::copy_source);
    cmd.copy_buffer(c.readback[slot], 0, c.counters, 0, gpu::k_counter_bytes);
    c.readback_frame[slot] = frame_index;
    m_streamer->after_cull(cmd, slot, frame_index);
}

bool Renderer::ensure_shadow_buffers(u32 instance_count) {
    if (!ensure_cull_buffers(m_shadow_cull, instance_count)) {
        return false;
    }
    const auto make = [this](u64 size, rhi::BufferUsage usage, rhi::Memory memory, const char* name) {
        rhi::BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        desc.memory = memory;
        desc.name = name;
        return m_device.create_buffer(desc);
    };
    const auto rw = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
    const u64 request_bytes = u64{m_shadows->levels()} * (ShadowPages::k_slots / 8);
    if (!m_shadow_atlas.valid()) {
        // Zeroed: nothing there (lit) until a page is drawn.
        m_shadow_atlas = make(u64{m_shadows->desc().pool_pages} * ShadowPages::k_page_texels *
                                  ShadowPages::k_page_texels * 4,
                              rw, rhi::Memory::gpu, "shadow page atlas");
    }
    if (!m_shadow_requests.valid()) {
        m_shadow_requests = make(request_bytes, rw, rhi::Memory::gpu, "shadow page requests");
    }
    for (u32 i = 0; i < 3; ++i) {
        if (!m_shadow_readback[i].valid()) {
            m_shadow_readback[i] = make(request_bytes, rhi::BufferUsage::none, rhi::Memory::readback,
                                        "shadow page requests (read back)");
            m_shadow_readback_frame[i] = ~u64{0};
        }
    }
    const bool ok = m_shadow_atlas.valid() && m_shadow_requests.valid() && m_shadow_readback[0].valid() &&
                    m_shadow_readback[1].valid() && m_shadow_readback[2].valid();
    if (!ok && !m_shadow_buffers_logged) {
        m_shadow_buffers_logged = true;
        log_error("render", "could not create the shadow page buffers (out of GPU memory?); no sun shadows");
    }
    return ok;
}

void Renderer::plan_shadows(const FrameInput& frame, bool on) {
    m_shadows_on = on;
    if (!on) {
        m_shadows_were_on = false;
        // Requests from before are not taken when shadows come back.
        for (u64& tag : m_shadow_readback_frame) {
            tag = ~u64{0};
        }
        return;
    }
    m_shadows->begin_frame(frame.view.position, frame.lighting.sun_direction);
    if (!m_shadows_were_on) {
        // Casters may have changed while shadows were off.
        m_shadows->invalidate_all();
        m_shadows_were_on = true;
    }
    // Only what is drawn as a mesh casts (not the instances left to the
    // voxel world, nor those whose mesh is not ready).
    m_casts.assign(frame.instances.size(), 0);
    for (const u32 i : m_valid) {
        m_casts[i] = 1;
    }
    m_shadows->track(frame.instances, frame.cells, m_casts, m_meshes_version, [this](MeshId id) { return mesh(id); });
    sway_shadows(frame);
    // The newest requests the GPU is done with, taken once.
    const u64 now = m_device.frames_completed();
    u32 best = 3;
    for (u32 i = 0; i < 3; ++i) {
        const u64 tag = m_shadow_readback_frame[i];
        if (tag != ~u64{0} && tag + rhi::Device::k_frames_in_flight <= now &&
            (best == 3 || tag > m_shadow_readback_frame[best])) {
            best = i;
        }
    }
    if (best < 3 && m_shadow_readback_frame[best] != m_shadow_requests_taken) {
        m_shadow_requests_taken = m_shadow_readback_frame[best];
        const u8* bytes = m_device.mapped(m_shadow_readback[best]);
        if (bytes != nullptr) {
            m_shadow_requests_cpu.resize(size_t{m_shadows->levels()} * (ShadowPages::k_slots / 32));
            std::memcpy(m_shadow_requests_cpu.data(), bytes, m_shadow_requests_cpu.size() * 4);
            m_shadows->request(m_shadow_requests_cpu, &m_shadow_readback_windows[best]);
        }
    }
}

// What sways near the camera, gathered in 32 m squares (a few boxes for a
// forest): the shadow pages under it are drawn again in turn.
void Renderer::sway_shadows(const FrameInput& frame) {
    if (!(frame.settings.wind_distance > 0.0f) || frame.settings.wind_shadow_pages_per_frame == 0) {
        return;
    }
    const usize material_limit = std::min<usize>(frame.materials.size(), k_max_materials);
    m_material_sways.assign(material_limit + 1, 0);
    bool any = false;
    for (usize i = 0; i < material_limit; ++i) {
        const gpu::MaterialWind w = wind_of(frame.materials[i]);
        m_material_sways[i] = (w.sway > 0.0f || w.flutter > 0.0f) ? u8{1} : u8{0};
        any = any || m_material_sways[i] != 0;
    }
    if (!any) {
        return;
    }
    constexpr double k_square = 32.0;
    m_sway_boxes.clear(); // a vector: keeps its room from frame to frame
    for (const u32 i : m_valid) {
        const Instance& in = frame.instances[i];
        if (m_material_sways[std::min<usize>(in.material, material_limit)] == 0) {
            continue;
        }
        const Mesh& mesh = m_meshes[in.mesh];
        if (mesh.skin != nullptr) {
            continue;
        }
        const float reach = wind_reach(frame, in, mesh.top);
        if (!(reach > 0.0f)) {
            continue;
        }
        const geometry::Sphere& b = mesh.data.bounds;
        const Vec3 c = rotate(in.rotation, Vec3{b.center.x * in.scale.x, b.center.y * in.scale.y, b.center.z * in.scale.z});
        const double r = static_cast<double>(b.radius) *
                         std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
        const DVec3 centre = in.position + DVec3{c.x, c.y, c.z};
        const DVec3 lo = centre - DVec3{r, r, r}, hi = centre + DVec3{r, r, r};
        const i64 sx = static_cast<i64>(std::floor(centre.x / k_square));
        const i64 sz = static_cast<i64>(std::floor(centre.z / k_square));
        const u64 key = (static_cast<u64>(sx) << 32) ^ (static_cast<u64>(sz) & 0xFFFF'FFFFu);
        m_sway_boxes.push_back({key, lo, hi, reach});
    }
    // One box per square: sorted by square, then merged.
    std::sort(m_sway_boxes.begin(), m_sway_boxes.end(),
              [](const SwayBox& a, const SwayBox& b) { return a.square < b.square; });
    for (usize i = 0; i < m_sway_boxes.size();) {
        SwayBox box = m_sway_boxes[i];
        usize j = i + 1;
        for (; j < m_sway_boxes.size() && m_sway_boxes[j].square == box.square; ++j) {
            const SwayBox& b = m_sway_boxes[j];
            box.min = {std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y), std::min(box.min.z, b.min.z)};
            box.max = {std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y), std::max(box.max.z, b.max.z)};
            box.reach = std::max(box.reach, b.reach);
        }
        m_shadows->sway(box.min, box.max, box.reach);
        i = j;
    }
}

void Renderer::draw_shadows(rhi::CommandList& cmd, FrameData& data, const FrameInput& frame,
                            const gpu::FrameConstants& base) {
    const u32 budget = std::clamp<u32>(frame.settings.shadow_pages_per_frame, 1u, 4096u);
    const u32 sway_budget = std::min<u32>(frame.settings.wind_shadow_pages_per_frame, 4096u);
    const std::vector<ShadowPages::Draw> draws = m_shadows->plan(budget, sway_budget);
    // The table and levels as they are now, drawn pages marked: shading and
    // the page raster read them.
    m_shadow_table.resize(size_t{m_shadows->levels()} * ShadowPages::k_slots);
    m_shadows->write_table(m_shadow_table);
    m_shadow_levels.resize(m_shadows->levels());
    m_shadows->write_levels(m_shadow_levels.data(), frame.view.position);
    data.shadow_table = cmd.upload(bytes_of(m_shadow_table));
    data.shadow_levels = cmd.upload(bytes_of(m_shadow_levels));
    if (data.shadow_table.data == nullptr || data.shadow_levels.data == nullptr) {
        return;
    }
    cmd.begin_timing("shadows");
    // This frame's requests start empty.
    cmd.barrier(m_shadow_requests, rhi::ResourceState::shader_write);
    {
        gpu::ShadowClearPush push{};
        push.target = m_device.write_descriptor(m_shadow_requests);
        push.list = data.frame.descriptor;
        push.count = m_shadows->levels() * (ShadowPages::k_slots / 32);
        cmd.set_pipeline(m_shadow_clear_words);
        cmd.push(push);
        cmd.dispatch(groups_of(push.count, 64));
    }
    if (!draws.empty()) {
        // Pages drawn again start empty.
        std::vector<u32> pages;
        pages.reserve(draws.size());
        for (const ShadowPages::Draw& d : draws) {
            pages.push_back(d.physical);
        }
        const rhi::TransientMemory list = cmd.upload(bytes_of(pages));
        if (list.data != nullptr) {
            cmd.barrier(m_shadow_atlas, rhi::ResourceState::shader_write);
            gpu::ShadowClearPush push{};
            push.target = m_device.write_descriptor(m_shadow_atlas);
            push.list = list.descriptor;
            push.list_offset = list.offset;
            push.count = static_cast<u32>(pages.size());
            cmd.set_pipeline(m_shadow_clear_pages);
            cmd.push(push);
            constexpr u32 page_words = ShadowPages::k_page_texels * ShadowPages::k_page_texels;
            cmd.dispatch(page_words / 4 / 64, push.count);
            cmd.barrier(m_shadow_atlas, rhi::ResourceState::shader_write); // cleared before drawn
        }
    }
    // Per level with pages to draw: cull and draw what casts onto them,
    // seen from the sun over the level's window, kept to those pages.
    const ShadowPages::Basis& b = m_shadows->basis();
    const double range = m_shadows->desc().depth_range;
    const DVec3 camera_light = m_shadows->to_light(frame.view.position);
    const double depth_offset = m_shadows->depth_offset(frame.view.position);
    for (u32 level = 0; level < m_shadows->levels() && data.instance_count > 0; ++level) {
        i32 x0 = std::numeric_limits<i32>::max(), y0 = x0;
        i32 x1 = std::numeric_limits<i32>::min(), y1 = x1;
        for (const ShadowPages::Draw& d : draws) {
            if (d.level == level) {
                x0 = std::min(x0, d.page_x);
                y0 = std::min(y0, d.page_y);
                x1 = std::max(x1, d.page_x);
                y1 = std::max(y1, d.page_y);
            }
        }
        if (x0 > x1) {
            continue;
        }
        const double page = m_shadows->page_size(level);
        const double texel = m_shadows->texel_size(level);
        const double width = page * ShadowPages::k_pages_per_axis;
        const i32 wx = m_shadows->window_x(level);
        const i32 wy = m_shadows->window_y(level);
        // The window's corner, light space, relative to the camera.
        const double ox = double(wx) * page - camera_light.x;
        const double oy = double(wy) * page - camera_light.y;
        gpu::FrameConstants k = base;
        // Camera-relative world to the window's clip space: x right, y
        // down the texel rows, depth 0.5 - (z - anchor) / range (larger:
        // nearer the sun; from a fixed anchor, so cached pages stay right
        // as the camera moves).
        Mat4 m;
        const double sx = 2.0 / width;
        m.at(0, 0) = float(b.x.x * sx);
        m.at(0, 1) = float(b.x.y * sx);
        m.at(0, 2) = float(b.x.z * sx);
        m.at(0, 3) = float(-ox * sx - 1.0);
        m.at(1, 0) = float(-b.y.x * sx);
        m.at(1, 1) = float(-b.y.y * sx);
        m.at(1, 2) = float(-b.y.z * sx);
        m.at(1, 3) = float(oy * sx + 1.0);
        m.at(2, 0) = float(-b.z.x / range);
        m.at(2, 1) = float(-b.z.y / range);
        m.at(2, 2) = float(-b.z.z / range);
        m.at(2, 3) = float(0.5 + depth_offset);
        m.at(3, 0) = 0.0f;
        m.at(3, 1) = 0.0f;
        m.at(3, 2) = 0.0f;
        m.at(3, 3) = 1.0f;
        std::memcpy(k.view_proj, m.m, sizeof(k.view_proj));
        // Culling: the pages' rectangle across the light, and nothing
        // nearer the sun than half the depth range from the anchor (depth 1).
        const double lx0 = ox + double(x0 - wx) * page;
        const double lx1 = ox + double(x1 + 1 - wx) * page;
        const double ly0 = oy + double(y0 - wy) * page;
        const double ly1 = oy + double(y1 + 1 - wy) * page;
        const auto plane = [&k](u32 i, const DVec3& n, double w) {
            k.planes[i * 4 + 0] = float(n.x);
            k.planes[i * 4 + 1] = float(n.y);
            k.planes[i * 4 + 2] = float(n.z);
            k.planes[i * 4 + 3] = float(w);
        };
        plane(0, b.x, -lx0);
        plane(1, -b.x, lx1);
        plane(2, b.y, -ly0);
        plane(3, -b.y, ly1);
        plane(4, b.z, range * (0.5 - depth_offset)); // depth 1 is z_rel = range * (offset - 0.5)
        // LOD by the texel: the error over a distance that never wins.
        constexpr float k_far = 1.0e7f;
        k.near_plane = k_far;
        k.pixels_per_unit = float(double(k_far) / texel);
        k.occlusion = 0;
        k.camera_delta[3] = 0.0f;
        const rhi::TransientMemory frame_k = cmd.upload(bytes_of_one(k));
        if (frame_k.data == nullptr) {
            break;
        }
        CullBuffers& c = m_shadow_cull;
        const gpu::CullPush push = cull_push(c, data, frame_k, false);
        cull_first_pass(cmd, c, data, push);
        cmd.barrier(c.args, rhi::ResourceState::indirect_argument);
        cmd.barrier(c.counters, rhi::ResourceState::shader_read);
        cmd.barrier(c.visible, rhi::ResourceState::shader_read);
        cmd.barrier(m_shadow_atlas, rhi::ResourceState::shader_write);
        rhi::RenderPassDesc pass;
        pass.color_count = 0;
        pass.width = ShadowPages::k_pages_per_axis * ShadowPages::k_page_texels;
        pass.height = pass.width;
        cmd.begin_render_pass(pass);
        const u32 t = ShadowPages::k_page_texels;
        cmd.set_scissor(u32(x0 - wx) * t, u32(y0 - wy) * t, u32(x1 - x0 + 1) * t, u32(y1 - y0 + 1) * t);
        cmd.set_pipeline(data.use_mesh_shaders && m_shadow_mesh.valid() ? m_shadow_mesh : m_shadow_vertex);
        gpu::RasterPush r{};
        r.frame = frame_k.descriptor;
        r.frame_offset = frame_k.offset;
        r.instances = data.instances.descriptor;
        r.instance_offset = data.instances.offset;
        r.meshes = data.meshes.descriptor;
        r.mesh_offset = data.meshes.offset;
        r.visible = m_device.read_descriptor(c.visible);
        r.visible_offset = 0;
        r.range = m_device.read_descriptor(c.counters);
        r.range_offset = gpu::k_slot_range * 4;
        r.shadow_atlas = m_device.write_descriptor(m_shadow_atlas);
        r.shadow_table = data.shadow_table.descriptor;
        r.shadow_table_offset = data.shadow_table.offset + level * ShadowPages::k_slots * 4;
        r.shadow_page_x = wx;
        r.shadow_page_y = wy;
        cmd.push(r);
        if (data.use_mesh_shaders && m_shadow_mesh.valid()) {
            cmd.dispatch_mesh_indirect(c.args, gpu::k_args_raster);
        } else {
            cmd.draw_indirect(c.args, gpu::k_args_raster + 16);
        }
        cmd.end_render_pass();
        ++m_shadow_draws;
    }
    cmd.barrier(m_shadow_atlas, rhi::ResourceState::shader_read);
    cmd.end_timing();
}

void Renderer::render(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameInput& frame) {
    // mark(s) gives section s the CPU time since the last mark.
    m_cpu_ms = {};
    auto since = std::chrono::steady_clock::now();
    const auto mark = [&](CpuSection section) {
        const auto now = std::chrono::steady_clock::now();
        m_cpu_ms[static_cast<usize>(section)] += std::chrono::duration<float, std::milli>(now - since).count();
        since = now;
    };
    m_stats = {};
    m_shadow_draws = 0;
    const rhi::TextureDesc* target_desc = m_device.describe(target);
    GIANT_ASSERT(target_desc != nullptr && target_desc->format == rhi::Format::rgba8_srgb,
                 "the renderer draws into a live rgba8_srgb render target");
    if (target_desc == nullptr) {
        return;
    }
    const u32 width = target_desc->width;
    const u32 height = target_desc->height;
    if (!ensure_targets(width, height)) {
        return;
    }
    const RenderSettings& settings = frame.settings;
    bool gpu = settings.gpu_culling;
    // The cells (for the stats: the voxel world and shadows keep their own).
    m_cells_count = static_cast<u32>(frame.cells.size());
    m_cells_changed = changed_cells(frame.cells, frame.instances.size(), m_seen_cells, m_changed_cells)
                          ? static_cast<u32>(m_changed_cells.size())
                          : m_cells_count;
    mark(CpuSection::cells);

    // Newest GPU counts the GPU is done with (frames in flight: two back).
    if (gpu) {
        const u64 now = m_device.frames_completed();
        u32 best = 3;
        for (u32 i = 0; i < 3; ++i) {
            const u64 tag = m_cull.readback_frame[i];
            if (tag != ~u64{0} && tag + rhi::Device::k_frames_in_flight <= now &&
                (best == 3 || tag > m_cull.readback_frame[best])) {
                best = i;
            }
        }
        if (best < 3) {
            read_stats_slot(best);
            // Each frame's streaming requests are taken once.
            if (m_cull.readback_frame[best] != m_requests_taken) {
                m_requests_taken = m_cull.readback_frame[best];
                m_streamer->take_gpu_requests(best, m_request_count);
            }
        }
    }
    // Load what culling asked for (and the coarsest levels of new meshes)
    // before anything reads the geometry.
    const u32 frame_number = static_cast<u32>(m_device.frames_completed() + 1); // 0 means never
    cmd.begin_timing("streaming");
    m_streamer->update(cmd, frame_number);
    cmd.end_timing();
    pose_skins(cmd);
    mark(CpuSection::streaming);
    if (settings.voxel_world) {
        cmd.begin_timing("voxels");
        m_voxels->set_stand_in_radius(settings.far_field_max_radius);
        // Skinned meshes are not held: their pose is not the bind the
        // voxels would be made from (they bounce no light; known gap).
        m_voxels->update(cmd, frame.view.position, frame.instances, frame.cells, frame.materials, m_meshes_version,
                         [this](MeshId id) { return skinned(id) ? nullptr : mesh(id); },
                         std::clamp(settings.voxel_ms_per_frame, 0.0f, 1000.0f));
        cmd.end_timing();
    } else {
        m_voxels->idle();
    }
    mark(CpuSection::voxels);
    // Views that light the scene: the lit view and the light alone.
    const bool lit_view = settings.debug_view == DebugView::lit || settings.debug_view == DebugView::lighting;
    // Global illumination: the voxel world lit (in the lit views and the
    // voxel view), a budgeted number of bricks a frame.
    if (!settings.gi) {
        m_voxel_light_failed = false; // try again when turned back on
    }
    m_gi_on = settings.gi && settings.voxel_world && (lit_view || settings.debug_view == DebugView::voxels) &&
              ensure_voxel_light();
    if (m_gi_on) {
        m_light_plan->plan(*m_voxels, std::clamp<u32>(settings.gi_bricks_per_frame, 1u, 4096u));
    } else {
        m_light_plan->off();
    }
    mark(CpuSection::gi_plan);

    // Instances with a live mesh, but for the small ones wholly past the
    // far field's distance: the voxel world shows those (once it holds
    // them as they are now).
    const bool far_field = settings.voxel_far_field && settings.voxel_world && lit_view;
    const double far_distance = std::max(static_cast<double>(settings.far_field_distance), 0.0);
    const double far_radius = std::max(static_cast<double>(settings.far_field_max_radius), 0.0);
    // The instance's bounding sphere is wholly past the distance, it is
    // small enough, and the voxel world shows it as it is now.
    const auto left_to_voxels = [&](u32 index, const Instance& in, const geometry::ClusterMesh* m) {
        const geometry::Sphere& bounds = m->bounds;
        const double radius = stand_in_radius_of(*m, in.scale);
        if (in.always_mesh || in.moves || !(radius <= far_radius) || skinned(in.mesh)) {
            return false;
        }
        const Vec3 c = rotate(in.rotation, Vec3{bounds.center.x * in.scale.x, bounds.center.y * in.scale.y,
                                                bounds.center.z * in.scale.z});
        const DVec3 center = in.position + DVec3{c.x, c.y, c.z};
        if (!(length(center - frame.view.position) - radius > far_distance)) {
            return false;
        }
        return m_voxels->holds(index);
    };
    m_far_field = far_field;
    m_far_culled = 0;
    m_valid.clear();
    // Instances past k_max_instances are not stored on the GPU: never drawn.
    const u32 instance_count = static_cast<u32>(std::min<usize>(frame.instances.size(), k_max_instances));
    if (instance_count < frame.instances.size() && !m_instances_logged) {
        m_instances_logged = true;
        log_error("render", "more than " + std::to_string(k_max_instances) +
                                " instances in one frame; the rest are not drawn");
    }
    for (u32 i = 0; i < instance_count; ++i) {
        const Instance& in = frame.instances[i];
        const geometry::ClusterMesh* m = mesh(in.mesh);
        if (m == nullptr || !m_streamer->ready(in.mesh) || !m_streamer->tables_current()) {
            continue;
        }
        if (far_field && left_to_voxels(i, in, m)) {
            ++m_far_culled;
            continue;
        }
        m_valid.push_back(i);
    }
    if (gpu && !ensure_cull_buffers(m_cull, static_cast<u32>(m_valid.size()))) {
        m_valid.clear(); // out of memory: draw nothing
    }
    mark(CpuSection::instances);
    // Sun shadows: the lit view, with GPU culling (pages are drawn with it).
    plan_shadows(frame, settings.shadows && gpu && lit_view &&
                            ensure_shadow_buffers(static_cast<u32>(m_valid.size())));
    track_skin_shadows(frame);
    mark(CpuSection::shadow_plan);

    // The instances, kept on the GPU by cell (before the frame constants:
    // the store may grow, which moves its not-drawn bits).
    const u32 material_count = static_cast<u32>(std::min<usize>(frame.materials.size(), k_max_materials));
    bool stored = false;
    if (!m_valid.empty()) {
        stored = m_store->update(cmd, frame.instances.first(instance_count),
                                 instance_count == frame.instances.size() ? frame.cells
                                                                          : std::span<const CellRange>{},
                                 material_count, frame.view.position, m_valid);
        if (!stored) {
            m_valid.clear(); // out of memory: draw nothing
        }
    }
    mark(CpuSection::store);

    // Frame constants.
    const Lighting& light = frame.lighting;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float fov = degrees_to_radians(std::clamp(frame.view.vertical_fov_degrees, 1.0f, 170.0f));
    const float near_plane = std::max(frame.view.near_plane, 1e-4f);
    const float focal = 1.0f / std::tan(fov * 0.5f);
    const Mat4 view_proj = view_projection(frame.view, aspect);
    const Mat4 view = view_from_orientation(frame.view.orientation);
    const geometry::Frustum frustum = geometry::frustum_from_view_projection(view_proj);
    gpu::FrameConstants k{};
    std::memcpy(k.view_proj, view_proj.m, sizeof(k.view_proj));
    float view_rows[12];
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) {
            view_rows[r * 4 + col] = view.at(r, col);
        }
        view_rows[r * 4 + 3] = 0.0f;
    }
    std::memcpy(k.view_rows, view_rows, sizeof(view_rows));
    std::memcpy(k.prev_view_rows, m_hzb_view_rows, sizeof(view_rows));
    for (u32 p = 0; p < 5; ++p) {
        const Vec4& plane = frustum.planes[p];
        k.planes[p * 4 + 0] = plane.x;
        k.planes[p * 4 + 1] = plane.y;
        k.planes[p * 4 + 2] = plane.z;
        k.planes[p * 4 + 3] = plane.w;
    }
    const bool occlusion = gpu && settings.occlusion_culling;
    // Last frame's HZB is usable if it saw this projection at this size.
    const bool hzb_usable = occlusion && m_hzb_valid && m_hzb_p00 == focal / aspect &&
                            m_hzb_p11 == focal && m_hzb_near == near_plane;
    set4(k.camera_delta, to_camera_relative(frame.view.position, m_hzb_camera), hzb_usable ? 1.0f : 0.0f);
    const float tan_y = std::tan(fov * 0.5f);
    set4(k.ray_x, rotate(frame.view.orientation, {1, 0, 0}) * (tan_y * aspect), 0.0f);
    set4(k.ray_y, rotate(frame.view.orientation, {0, 1, 0}) * tan_y, 0.0f);
    set4(k.ray_z, rotate(frame.view.orientation, {0, 0, -1}), 0.0f);
    set4(k.sun_direction, normalize_or_zero(light.sun_direction), 0.0f);
    set4(k.sun_light, light.sun_light, 0.0f);
    set4(k.sky_light, light.sky_light, 0.0f);
    set4(k.ground_light, light.ground_light, 0.0f);
    // The physical sky's ambient light: from the table a frame before
    // filled (global illumination lights voxels before this frame's sky;
    // shading, after, reads this frame's), else the flat colours.
    const bool sky_ambient = frame.sky.on && settings.physical_sky && lit_view && m_sky_ambient_made &&
                             m_sky_ambient.valid() && !m_sky_failed;
    k.sky_ambient = sky_ambient ? m_device.read_descriptor(m_sky_ambient) : 0u;
    k.sky_flags = sky_ambient ? 1u : 0u;
    k.tone_mapper = settings.tone_mapper == ToneMapper::agx ? 1u : 0u;
    k.exposure = update_exposure(frame, lit_view);
    k.debug_view = static_cast<u32>(settings.debug_view);
    k.threshold = std::clamp(settings.lod_error_pixels, 0.01f, 1000.0f);
    k.pixels_per_unit = 0.5f * focal * static_cast<float>(height);
    k.near_plane = near_plane;
    k.width = static_cast<float>(width);
    k.height = static_cast<float>(height);
    k.p00 = focal / aspect;
    k.p11 = focal;
    k.hzb_width = m_hzb_width;
    k.hzb_height = m_hzb_height;
    k.hzb_mips = m_hzb_mips;
    // Culling walks every instance, skipping those not drawn (InstanceStore).
    k.instance_count = stored ? instance_count : 0;
    k.skip_offset = m_store->skip_offset();
    k.occlusion = occlusion ? 1u : 0u;
    k.pool = m_device.read_descriptor(m_streamer->pool());
    k.frame_number = frame_number;
    // Wind: what each material does in it, and the wind itself.
    const usize material_limit = std::min<usize>(frame.materials.size(), k_max_materials);
    m_gpu_wind.clear();
    bool any_sway = false;
    for (usize i = 0; i < material_limit; ++i) {
        m_gpu_wind.push_back(wind_of(frame.materials[i]));
        any_sway = any_sway || m_gpu_wind.back().sway > 0.0f || m_gpu_wind.back().flutter > 0.0f;
    }
    m_gpu_wind.push_back(gpu::MaterialWind{});
    // Nothing that sways: the shaders skip the wind altogether.
    const rhi::TransientMemory wind_table = any_sway ? cmd.upload(bytes_of(m_gpu_wind)) : rhi::TransientMemory{};
    const WindState& wind = frame.wind;
    const bool blowing = any_sway && wind_table.data != nullptr && settings.wind_distance > 0.0f && std::isfinite(settings.wind_distance) &&
                         std::isfinite(wind.velocity.x) && std::isfinite(wind.velocity.z) &&
                         length(Vec3{wind.velocity.x, 0.0f, wind.velocity.z}) > 1e-4f;
    const float gustiness = std::isfinite(wind.gustiness) ? std::clamp(wind.gustiness, 0.0f, 1.0f) : 0.0f;
    set4(k.wind, blowing ? Vec3{wind.velocity.x, 0.0f, wind.velocity.z} : Vec3{}, gustiness);
    const auto clock = [](float v) { return std::isfinite(v) ? v : 0.0f; };
    k.wind_clock[0] = clock(wind.gust_x);
    k.wind_clock[1] = clock(wind.gust_z);
    k.wind_clock[2] = clock(wind.sway_clock);
    k.wind_clock[3] = clock(wind.flutter_clock);
    const auto modulo = [](double v) { return static_cast<float>(v - k_gust_tile * std::floor(v / k_gust_tile)); };
    k.wind_camera[0] = modulo(frame.view.position.x);
    k.wind_camera[1] = modulo(frame.view.position.y);
    k.wind_camera[2] = modulo(frame.view.position.z);
    k.wind_camera[3] = blowing ? std::min(settings.wind_distance, 100000.0f) : 0.0f;
    k.wind_table = wind_table.descriptor;
    k.wind_table_offset = wind_table.offset;
    k.wind_table_count = static_cast<u32>(material_limit);

    FrameData data;
    data.frame = cmd.upload(bytes_of_one(k));
    data.instance_count = k.instance_count;
    if (stored) {
        data.instances.buffer = m_store->buffer();
        data.instances.descriptor = m_device.read_descriptor(m_store->buffer());
        data.instances.offset = 0;
    }
    data.occlusion = occlusion;
    data.use_mesh_shaders = settings.mesh_shaders && m_raster_mesh.valid();

    // The material table, with the default appended (out-of-range indices).
    if (frame.materials.size() > material_limit && !m_materials_logged) {
        m_materials_logged = true;
        log_error("render", "more than " + std::to_string(k_max_materials) +
                                " materials in one frame; instances using the rest draw the default");
    }
    m_gpu_materials.clear();
    for (usize i = 0; i < material_limit; ++i) {
        m_gpu_materials.push_back(to_gpu(frame.materials[i], m_device, m_textures));
    }
    m_gpu_materials.push_back(to_gpu(Material{}, m_device, m_textures));
    data.material_count = static_cast<u32>(material_limit);
    data.materials = cmd.upload(bytes_of(m_gpu_materials));

    // Decals in view, binned into screen tiles (lit view only).
    if (!frame.decals.empty() && settings.debug_view == DebugView::lit) {
        bin_decals(frame.decals, frame.view.position, view_proj, frustum, width, height, *m_decal_bins);
    } else {
        m_decal_bins->decals.clear();
        m_decal_bins->overflow = false;
    }
    if (!m_decal_bins->decals.empty()) {
        const DecalBins& bins = *m_decal_bins;
        const usize decal_bytes = bins.decals.size() * sizeof(gpu::Decal);
        const usize tile_bytes = bins.tiles.size() * 4;
        const usize total = decal_bytes + tile_bytes + std::max<usize>(bins.entries.size(), 1) * 4;
        data.decals = cmd.allocate(static_cast<u32>(total));
        if (data.decals.data != nullptr) {
            std::memcpy(data.decals.data, bins.decals.data(), decal_bytes);
            std::memcpy(data.decals.data + decal_bytes, bins.tiles.data(), tile_bytes);
            std::memcpy(data.decals.data + decal_bytes + tile_bytes, bins.entries.data(), bins.entries.size() * 4);
            data.decal_offset = data.decals.offset;
            data.tile_offset = data.decals.offset + static_cast<u32>(decal_bytes);
            data.entry_offset = data.tile_offset + static_cast<u32>(tile_bytes);
            data.tile_columns = bins.columns;
            data.tile_rows = bins.rows;
        }
    }
    if (m_decal_bins->overflow && !m_decals_logged) {
        m_decals_logged = true;
        log_warning("render", "too many decals on screen; some are not drawn");
    }

    // Instances and the mesh table.
    std::vector<gpu::MeshEntry>& table = m_table;
    std::vector<bool>& used = m_used;
    table.assign(m_meshes.size(), gpu::MeshEntry{});
    used.assign(m_meshes.size(), false);
    for (const u32 i : m_valid) {
        used[frame.instances[i].mesh] = true;
    }
    for (usize id = 0; id < m_meshes.size(); ++id) {
        const Mesh& m = m_meshes[id];
        if (!m.live) {
            continue; // levels 0: nothing drawn
        }
        gpu::MeshEntry& e = table[id];
        e.residency = m_device.read_descriptor(m_streamer->residency_table(static_cast<u32>(id)));
        e.group_base = m_streamer->group_base(static_cast<u32>(id));
        e.clusters = m_device.read_descriptor(m.clusters);
        e.groups = m_device.read_descriptor(m.groups);
        e.nodes = m_device.read_descriptor(m.nodes);
        e.levels = m.data.levels;
        e.top = m.skin != nullptr ? 0.0f : m.top; // characters do not sway
        const geometry::Sphere& bounds = m.skin != nullptr ? m.skin->posed_bounds : m.data.bounds;
        set_bounds(e.bounds, bounds.center, bounds.radius);
        if (used[id]) {
            data.tree_levels = std::max(data.tree_levels, m.tree_levels);
            for (const rhi::BufferHandle b : {m.clusters, m.groups, m.nodes}) {
                cmd.barrier(b, rhi::ResourceState::shader_read);
            }
        }
    }
    data.meshes = cmd.upload(bytes_of(table));
    mark(CpuSection::tables);
    // The weather's cover map reads the voxel world too.
    const bool weather_cover = frame.sky.on && settings.weather_cover && settings.voxel_world &&
                               (frame.sky.rain > 0.0f || frame.sky.snow > 0.0f || frame.sky.wetness > 0.0f ||
                                frame.sky.snow_cover > 0.0f);
    if (far_field || m_gi_on || settings.debug_view == DebugView::voxels || weather_cover) {
        m_voxel_levels.resize(m_voxels->levels());
        m_voxels->write_levels(m_voxel_levels.data(), frame.view.position);
        data.voxel_levels = cmd.upload(bytes_of(m_voxel_levels));
    }
    if (data.frame.data == nullptr || data.meshes.data == nullptr || data.materials.data == nullptr) {
        if (m_gi_on) {
            m_light_plan->lost(); // this frame's clears never ran
        }
        return; // out of memory: the device is lost
    }
    if (!light_voxels(cmd, data)) {
        m_gi_on = false; // this frame shades without it
    }
    mark(CpuSection::light);

    if (gpu && !m_valid.empty()) {
        cull_on_gpu(cmd, data); // counts its raster passes in m_stats.draws
        const u32 draws = m_stats.draws;
        m_stats = m_gpu_stats;
        m_stats.draws = draws;
        m_stats.from_gpu = true;
    } else {
        // No GPU counts are made this frame: forget older ones, so turning
        // GPU culling back on (or filling an empty scene) never shows them.
        for (u64& tag : m_cull.readback_frame) {
            tag = ~u64{0};
        }
        m_gpu_stats = {};
        if (!gpu) {
            select_on_cpu(frame, view_proj, height, frame_number);
            // select_on_cpu stops at what one mesh dispatch takes (16 MiB of
            // entries, well inside one transient allocation).
            static_assert(u64{k_max_cpu_clusters} * 8 <= rhi::k_max_transient_allocation);
            if (m_stats.overflow && !m_cpu_clusters_logged) {
                m_cpu_clusters_logged = true;
                log_warning("render", "more than " + std::to_string(k_max_cpu_clusters) +
                                          " clusters chosen on the CPU; the rest are not drawn");
            }
            data.entry_count = static_cast<u32>(m_entries.size() / 2);
            if (data.entry_count > 0) {
                data.entries = cmd.upload(bytes_of(m_entries));
                const u32 range[2] = {0, data.entry_count};
                data.range = cmd.upload({reinterpret_cast<const u8*>(range), sizeof(range)});
                if (data.entries.data == nullptr || data.range.data == nullptr) {
                    data.entry_count = 0;
                    m_stats = {};
                }
            }
        } else {
            m_stats = {}; // GPU culling with nothing to draw
            m_stats.from_gpu = true;
        }
        raster(cmd, data, 0, false); // clears the visibility buffer, draws CPU picks
    }
    mark(CpuSection::culling);
    if (m_shadows_on) {
        draw_shadows(cmd, data, frame, k); // after the view's culling: its streaming requests go first
    }
    mark(CpuSection::shadows);
    if (data.occlusion && !m_valid.empty()) {
        m_hzb_valid = true;
        m_hzb_camera = frame.view.position;
        std::memcpy(m_hzb_view_rows, view_rows, sizeof(view_rows));
        m_hzb_p00 = k.p00;
        m_hzb_p11 = k.p11;
        m_hzb_near = near_plane;
    } else {
        m_hzb_valid = false;
    }

    // Shade each pixel once from the visibility buffer.
    cmd.barrier(m_vis, rhi::ResourceState::shader_read);
    if (gpu && m_cull.visible.valid()) {
        cmd.barrier(m_cull.visible, rhi::ResourceState::shader_read);
    }
    // With anti-aliasing the shading goes to an image of its own, which
    // SMAA then smooths into the target.
    const bool antialias = settings.antialiasing && lit_view;
    const rhi::TextureHandle shaded = antialias ? m_color : target;
    if (settings.debug_view == DebugView::voxels) {
        draw_voxels(cmd, target, data, light.background);
        finish_stats(data);
        mark(CpuSection::shading);
        return;
    }
    if (m_shadows_on) {
        cmd.barrier(m_shadow_atlas, rhi::ResourceState::shader_read);
        cmd.barrier(m_shadow_requests, rhi::ResourceState::shader_write);
    }
    const bool gi = m_gi_on && data.voxel_levels.data != nullptr;
    if (gi) {
        cmd.barrier(m_voxel_light, rhi::ResourceState::shader_write); // after lighting's writes
    }
    // Reflections (lit views): voxel cones need the lit voxel world.
    const bool reflections = settings.reflections && lit_view;
    const bool voxel_reflections = reflections && gi;
    const bool screen_reflections = reflections && settings.screen_space_reflections;
    // The sky (lit views): its view table, cloud shadows and clouds, before shading.
    rhi::TransientMemory sky_params{};
    const bool sky = frame.sky.on && lit_view && draw_sky(cmd, data, frame, sky_params);
    if (!sky) {
        m_sky_ambient_made = false; // (not kept up: the flat colours until it is filled again)
    }
    mark(CpuSection::sky);
    cmd.begin_timing("shading");
    // Shading writes the light before exposure into m_radiance (with
    // screen reflections, also what they need, and their pass adds them
    // into m_scene); post() then measures it, blooms it, exposes and tone
    // maps it into `shaded`.
    rhi::RenderPassDesc pass;
    const Vec3& bg = light.background;
    pass.colors[0] = {m_radiance, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 0.0f}};
    if (screen_reflections) {
        pass.colors[1] = {m_gloss, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 0.0f}};
        pass.colors[2] = {m_fallback, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 0.0f}};
        pass.color_count = 3;
    }
    cmd.begin_render_pass(pass);
    cmd.set_pipeline(screen_reflections ? m_resolve : m_resolve_direct);
    gpu::ResolvePush push{};
    set4(push.background, bg, 1.0f);
    push.frame = data.frame.descriptor;
    push.frame_offset = data.frame.offset;
    push.instances = data.instances.descriptor != rhi::k_no_descriptor ? data.instances.descriptor
                                                                       : data.frame.descriptor;
    push.instance_offset = data.instances.offset;
    push.meshes = data.meshes.descriptor;
    push.mesh_offset = data.meshes.offset;
    if (gpu && m_cull.visible.valid()) {
        push.visible = m_device.read_descriptor(m_cull.visible);
        push.visible_offset = 0;
    } else {
        push.visible = data.entries.data != nullptr ? data.entries.descriptor : data.frame.descriptor;
        push.visible_offset = data.entries.offset;
    }
    push.vis = m_device.read_descriptor(m_vis);
    push.materials = data.materials.descriptor;
    push.material_offset = data.materials.offset;
    push.material_count = data.material_count;
    push.decals = data.decals.data != nullptr ? data.decals.descriptor : data.frame.descriptor;
    push.decal_offset = data.decal_offset;
    push.tile_offset = data.tile_offset;
    push.entry_offset = data.entry_offset;
    push.tile_columns = data.tile_columns;
    push.tile_rows = data.tile_rows;
    push.specular_aa = settings.specular_antialiasing ? 1u : 0u;
    gpu::ResolveExtras extras{};
    // Nothing the far field could show (no level has stand-ins): no march.
    const bool far_march = far_field && data.voxel_levels.data != nullptr &&
                           std::any_of(m_voxel_levels.begin(), m_voxel_levels.end(),
                                       [](const gpu::VoxelLevel& l) { return l.brick[3] != 0; });
    // Off: valid descriptors all the same (bindless reads are checked).
    extras.voxel_pool = data.frame.descriptor;
    extras.voxel_table = data.frame.descriptor;
    extras.voxel_levels = data.frame.descriptor;
    extras.shadow_atlas = data.frame.descriptor;
    extras.shadow_table = data.frame.descriptor;
    extras.shadow_levels = data.frame.descriptor;
    extras.shadow_requests = data.frame.descriptor;
    extras.gi_light = data.frame.descriptor;
    if (far_march || gi) {
        extras.voxel_pool = m_device.read_descriptor(m_voxels->pool());
        extras.voxel_table = m_device.read_descriptor(m_voxels->table());
        extras.voxel_levels = data.voxel_levels.descriptor;
        extras.voxel_levels_offset = data.voxel_levels.offset;
        extras.voxel_level_count = m_voxels->levels();
        extras.voxel_bricks_per_axis = static_cast<u32>(m_voxels->clipmap().desc().bricks_per_axis);
        extras.far_start = static_cast<float>(far_distance);
        extras.far_radius = static_cast<float>(far_radius);
        extras.far_on = far_march ? 1u : 0u;
    }
    if (gi) {
        extras.gi_on = 1;
        extras.gi_light = m_device.write_descriptor(m_voxel_light);
    }
    extras.reflect_on = (voxel_reflections ? 1u : 0u) | (screen_reflections ? 2u : 0u);
    extras.sky_params = sky ? sky_params.descriptor : data.frame.descriptor;
    extras.sky_params_offset = sky ? sky_params.offset : 0u;
    extras.sky_on = sky ? 1u : 0u;
    const bool shadows = m_shadows_on && data.shadow_table.data != nullptr && data.shadow_levels.data != nullptr;
    if (shadows) {
        extras.shadow_atlas = m_device.read_descriptor(m_shadow_atlas);
        extras.shadow_table = data.shadow_table.descriptor;
        extras.shadow_table_offset = data.shadow_table.offset;
        extras.shadow_levels = data.shadow_levels.descriptor;
        extras.shadow_levels_offset = data.shadow_levels.offset;
        extras.shadow_level_count = m_shadows->levels();
        extras.shadow_requests = m_device.write_descriptor(m_shadow_requests);
        extras.shadow_texel = static_cast<float>(m_shadows->texel_size(0));
        const ShadowPages::Basis& b = m_shadows->basis();
        const auto axis = [](float* out, const DVec3& v, double w) {
            out[0] = static_cast<float>(v.x);
            out[1] = static_cast<float>(v.y);
            out[2] = static_cast<float>(v.z);
            out[3] = static_cast<float>(w);
        };
        axis(extras.shadow_x, b.x, 1.0 / m_shadows->desc().depth_range);
        axis(extras.shadow_y, b.y, 0.0);
        axis(extras.shadow_z, b.z, m_shadows->depth_offset(frame.view.position));
    }
    // Dust in the air (lit views): the first puffs given, camera-relative.
    extras.dust_puffs = data.frame.descriptor;
    if (lit_view && !frame.dust.empty()) {
        std::array<gpu::GpuDustPuff, k_max_dust_puffs> puffs{};
        u32 count = 0;
        // The first k_max_dust_puffs given (a bad one uses up its place).
        for (const DustPuff& d : frame.dust.first(std::min<usize>(frame.dust.size(), k_max_dust_puffs))) {
            const Vec3 c = to_camera_relative(d.center, frame.view.position);
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(d.radius) ||
                !std::isfinite(d.density) || !(d.radius > 0.0f) || !(d.density > 0.0f)) {
                continue;
            }
            gpu::GpuDustPuff& g = puffs[count++];
            g.center[0] = c.x;
            g.center[1] = c.y;
            g.center[2] = c.z;
            g.radius = std::min(d.radius, 1.0e5f);
            g.color[0] = std::isfinite(d.color.x) ? std::clamp(d.color.x, 0.0f, 1.0f) : 0.5f;
            g.color[1] = std::isfinite(d.color.y) ? std::clamp(d.color.y, 0.0f, 1.0f) : 0.5f;
            g.color[2] = std::isfinite(d.color.z) ? std::clamp(d.color.z, 0.0f, 1.0f) : 0.5f;
            g.density = std::min(d.density, 100.0f);
        }
        if (count > 0) {
            const rhi::TransientMemory dust =
                cmd.upload({reinterpret_cast<const u8*>(puffs.data()), count * sizeof(gpu::GpuDustPuff)});
            if (dust.data != nullptr) {
                extras.dust_puffs = dust.descriptor;
                extras.dust_offset = dust.offset;
                extras.dust_count = count;
            }
        }
    }
    m_stats.dust_puffs = extras.dust_count;
    const rhi::TransientMemory extras_memory = cmd.upload(bytes_of_one(extras));
    push.extras = extras_memory.data != nullptr ? extras_memory.descriptor : data.frame.descriptor;
    push.extras_offset = extras_memory.data != nullptr ? extras_memory.offset : 0u;
    if (extras_memory.data == nullptr) {
        cmd.end_render_pass();
        cmd.end_timing();
        return; // out of memory: the device is lost
    }
    cmd.push(push);
    cmd.draw(3);
    ++m_stats.draws;
    cmd.end_render_pass();
    if (screen_reflections) {
        // Screen reflections from this frame's radiance.
        cmd.barrier(m_radiance, rhi::ResourceState::shader_read);
        cmd.barrier(m_gloss, rhi::ResourceState::shader_read);
        cmd.barrier(m_fallback, rhi::ResourceState::shader_read);
        cmd.barrier(m_depth, rhi::ResourceState::shader_read);
        rhi::RenderPassDesc finish;
        // Cleared all the same: defined where no draw is recorded (the null device).
        finish.colors[0] = {m_scene, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 0.0f}};
        cmd.begin_render_pass(finish);
        cmd.set_pipeline(m_reflect);
        gpu::ReflectPush reflect{};
        reflect.frame = data.frame.descriptor;
        reflect.frame_offset = data.frame.offset;
        reflect.radiance = m_device.read_descriptor(m_radiance);
        reflect.gloss = m_device.read_descriptor(m_gloss);
        reflect.fallback = m_device.read_descriptor(m_fallback);
        reflect.depth = m_device.read_descriptor(m_depth);
        reflect.on = 1;
        cmd.push(reflect);
        cmd.draw(3);
        ++m_stats.post_draws;
        cmd.end_render_pass();
    }
    cmd.end_timing();
    post(cmd, data, frame, screen_reflections ? m_scene : m_radiance, shaded, lit_view);
    m_stats.reflections = voxel_reflections;
    m_stats.screen_reflections = screen_reflections;
    if (shadows) {
        // What shading asked for, read back once the GPU is done with it.
        const u64 frame_index = m_device.frames_completed();
        const u32 slot = static_cast<u32>(frame_index % 3);
        cmd.barrier(m_shadow_requests, rhi::ResourceState::copy_source);
        cmd.copy_buffer(m_shadow_readback[slot], 0, m_shadow_requests, 0,
                        u64{m_shadows->levels()} * (ShadowPages::k_slots / 8));
        m_shadow_readback_frame[slot] = frame_index;
        m_shadow_readback_windows[slot] = m_shadows->windows();
    }
    if (antialias) {
        cmd.begin_timing("anti-aliasing");
        smaa(cmd, target, bg);
        cmd.end_timing();
    }
    finish_stats(data);
    mark(CpuSection::shading);
}

float Renderer::update_exposure(const FrameInput& frame, bool lit_view) {
    const RenderSettings& settings = frame.settings;
    const float manual = std::isfinite(frame.lighting.exposure) ? std::max(frame.lighting.exposure, 0.0f) : 1.0f;
    m_measure_exposure = settings.auto_exposure && lit_view;
    m_bloom_on = false; // until post() blooms
    m_auto_on = settings.auto_exposure;
    if (!settings.auto_exposure) {
        // Off: measurements from before are not taken when it comes back on
        // (it holds where it was until a new one comes, then eases on).
        for (u64& tag : m_histogram_frame) {
            tag = ~u64{0};
        }
        m_scene_measured = false;
        m_auto_on = false;
        m_frame_exposure = manual;
        return manual;
    }
    // The newest histogram the GPU is done with, taken once.
    const u64 now = m_device.frames_completed();
    u32 best = 3;
    for (u32 i = 0; i < 3; ++i) {
        const u64 tag = m_histogram_frame[i];
        if (tag != ~u64{0} && tag + rhi::Device::k_frames_in_flight <= now &&
            (best == 3 || tag > m_histogram_frame[best])) {
            best = i;
        }
    }
    if (best < 3 && m_histogram_frame[best] != m_histogram_taken) {
        m_histogram_taken = m_histogram_frame[best];
        const u8* bytes = m_device.mapped(m_histogram_readback[best]);
        if (bytes != nullptr) {
            std::array<u32, k_exposure_bins> bins{};
            std::memcpy(bins.data(), bytes, sizeof(bins));
            float stops = 0.0f;
            if (histogram_stops(bins, 0.1f, 0.9f, stops)) {
                m_scene_stops = stops;
                m_scene_measured = true;
            }
        }
    }
    if (m_scene_measured) {
        // From the last measurement, with this frame's settings.
        const float target = exposure_target(m_scene_stops, std::clamp(settings.exposure_compensation, -10.0f, 10.0f),
                                             std::clamp(settings.exposure_min, -20.0f, 20.0f),
                                             std::clamp(settings.exposure_max, -20.0f, 20.0f));
        if (!m_exposure_known) {
            m_exposure_known = true; // the first measurement: at once
            m_exposure_stops = target;
        }
        m_exposure_target = target;
        const float up = std::clamp(settings.exposure_speed_up, 0.0f, 100.0f);
        const float down = std::clamp(settings.exposure_speed_down, 0.0f, 100.0f);
        m_exposure_stops = ease_exposure(m_exposure_stops, m_exposure_target, up, down, frame.delta_seconds);
    }
    // Until the first measurement: Lighting::exposure alone.
    m_frame_exposure = m_exposure_known ? manual * std::exp2(m_exposure_stops) : manual;
    return m_frame_exposure;
}

void Renderer::post(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame, rhi::TextureHandle light,
                    rhi::TextureHandle shaded, bool lit_view) {
    const RenderSettings& settings = frame.settings;
    cmd.begin_timing("post");
    cmd.barrier(light, rhi::ResourceState::shader_read);
    gpu::PostPush push{};
    push.frame = data.frame.descriptor;
    push.frame_offset = data.frame.offset;
    const u32 light_read = m_device.read_descriptor(light);
    const auto groups = [](u32 n) { return (n + 7) / 8; };
    // Auto exposure's measurement, copied out once the GPU gets there.
    if (m_measure_exposure) {
        cmd.barrier(m_histogram_bins, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_histogram_clear);
        push.target = m_device.write_descriptor(m_histogram_bins);
        cmd.push(push);
        cmd.dispatch(1, 1);
        cmd.barrier(m_histogram_bins, rhi::ResourceState::shader_write); // the clear before the counts
        cmd.set_pipeline(m_histogram);
        push.source = light_read;
        push.width = m_width;
        push.height = m_height;
        cmd.push(push);
        cmd.dispatch((m_width + 15) / 16, (m_height + 15) / 16);
        const u64 frame_index = m_device.frames_completed();
        const u32 slot = static_cast<u32>(frame_index % 3);
        cmd.barrier(m_histogram_bins, rhi::ResourceState::copy_source);
        cmd.copy_buffer(m_histogram_readback[slot], 0, m_histogram_bins, 0, u64{k_exposure_bins} * 4);
        m_histogram_frame[slot] = frame_index;
    }
    // Bloom: down six times from the light, then back up.
    const float strength = std::isfinite(settings.bloom_strength) ? std::clamp(settings.bloom_strength, 0.0f, 1.0f) : 0.0f;
    const bool bloom = settings.bloom && lit_view && strength > 0.0f && m_bloom_ready;
    if (bloom) {
        rhi::TextureHandle from = light;
        for (u32 i = 0; i < k_bloom_levels; ++i) {
            cmd.barrier(m_bloom_downs[i], rhi::ResourceState::shader_write);
            cmd.set_pipeline(m_bloom_down);
            push.source = m_device.read_descriptor(from);
            push.target = m_device.write_descriptor(m_bloom_downs[i]);
            push.width = m_bloom_width[i];
            push.height = m_bloom_height[i];
            push.flags = i == 0 ? 1u : 0u;
            cmd.push(push);
            cmd.dispatch(groups(push.width), groups(push.height));
            cmd.barrier(m_bloom_downs[i], rhi::ResourceState::shader_read);
            from = m_bloom_downs[i];
        }
        for (u32 i = k_bloom_levels - 1; i-- > 0;) {
            cmd.barrier(m_bloom_ups[i], rhi::ResourceState::shader_write);
            cmd.set_pipeline(m_bloom_up);
            push.source = m_device.read_descriptor(from);
            push.add = m_device.read_descriptor(m_bloom_downs[i]);
            push.target = m_device.write_descriptor(m_bloom_ups[i]);
            push.width = m_bloom_width[i];
            push.height = m_bloom_height[i];
            push.flags = 0;
            cmd.push(push);
            cmd.dispatch(groups(push.width), groups(push.height));
            cmd.barrier(m_bloom_ups[i], rhi::ResourceState::shader_read);
            from = m_bloom_ups[i];
        }
    }
    // Exposure and tone mapping into the image.
    const Vec3& bg = frame.lighting.background;
    rhi::RenderPassDesc pass;
    pass.colors[0] = {shaded, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
    cmd.begin_render_pass(pass);
    cmd.set_pipeline(m_post);
    push.source = light_read;
    push.add = bloom ? m_device.read_descriptor(m_bloom_ups[0]) : light_read;
    push.target = 0;
    push.width = m_width;
    push.height = m_height;
    push.flags = (settings.tone_mapper == ToneMapper::agx ? 2u : 0u) | (bloom ? 4u : 0u);
    push.strength = strength;
    push.levels = static_cast<float>(k_bloom_levels);
    cmd.push(push);
    cmd.draw(3);
    ++m_stats.post_draws;
    cmd.end_render_pass();
    cmd.end_timing();
    m_bloom_on = bloom;
}

void Renderer::finish_stats(const FrameData& data) {
    copy_streaming_stats();
    m_stats.auto_exposure = m_measure_exposure;
    m_stats.exposure = m_frame_exposure;
    m_stats.exposure_measured = m_auto_on && m_exposure_known;
    m_stats.scene_stops = m_scene_stops;
    m_stats.bloom = m_bloom_on;
    m_stats.materials = data.material_count;
    m_stats.cells = m_cells_count;
    m_stats.cells_changed = m_cells_changed;
    m_stats.decals = data.tile_columns != 0 ? static_cast<u32>(m_decal_bins->decals.size()) : 0;
    m_stats.decal_overflow = m_decal_bins->overflow;
}

void Renderer::draw_voxels(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameData& data,
                           const Vec3& background) {
    cmd.begin_timing("voxel view");
    if (m_gi_on && data.voxel_levels.data != nullptr) {
        cmd.barrier(m_voxel_light, rhi::ResourceState::shader_write); // after lighting's writes
    }
    rhi::RenderPassDesc pass;
    pass.colors[0] = {target, rhi::LoadOp::clear, {background.x, background.y, background.z, 1.0f}};
    cmd.begin_render_pass(pass);
    if (data.voxel_levels.data != nullptr) {
        cmd.set_pipeline(m_voxel_view);
        gpu::VoxelPush push{};
        push.frame = data.frame.descriptor;
        push.frame_offset = data.frame.offset;
        push.pool = m_device.read_descriptor(m_voxels->pool());
        push.table = m_device.read_descriptor(m_voxels->table());
        push.levels = data.voxel_levels.descriptor;
        push.levels_offset = data.voxel_levels.offset;
        push.level_count = m_voxels->levels();
        push.bricks_per_axis = static_cast<u32>(m_voxels->clipmap().desc().bricks_per_axis);
        set4(push.background, background, 1.0f);
        push.light = push.pool;
        if (m_gi_on) {
            push.light = m_device.write_descriptor(m_voxel_light);
            push.light_on = 1;
        }
        cmd.push(push);
        cmd.draw(3);
        ++m_stats.draws;
    }
    cmd.end_render_pass();
    cmd.end_timing();
}

void Renderer::smaa(rhi::CommandList& cmd, rhi::TextureHandle target, const Vec3& background) {
    gpu::SmaaPush push{};
    push.metrics[0] = 1.0f / static_cast<float>(m_width);
    push.metrics[1] = 1.0f / static_cast<float>(m_height);
    push.metrics[2] = static_cast<float>(m_width);
    push.metrics[3] = static_cast<float>(m_height);
    push.color = m_device.read_descriptor(m_color);
    push.edges = m_device.read_descriptor(m_edges);
    push.weights = m_device.read_descriptor(m_weights);
    push.area = m_device.read_descriptor(m_smaa_area);
    push.search = m_device.read_descriptor(m_smaa_search);
    // Each pass reads the one before. The first two clear to zero: the
    // edges pass skips pixels without an edge, and zero means "none".
    const auto pass = [&](rhi::PipelineHandle pipeline, rhi::TextureHandle out, rhi::Color clear) {
        rhi::RenderPassDesc desc;
        desc.colors[0] = {out, rhi::LoadOp::clear, clear};
        cmd.begin_render_pass(desc);
        cmd.set_pipeline(pipeline);
        cmd.push(push);
        cmd.draw(3);
        cmd.end_render_pass();
        ++m_stats.post_draws;
    };
    cmd.barrier(m_color, rhi::ResourceState::shader_read);
    cmd.barrier(m_smaa_area, rhi::ResourceState::shader_read); // no-ops once there
    cmd.barrier(m_smaa_search, rhi::ResourceState::shader_read);
    const rhi::Color zero{0.0f, 0.0f, 0.0f, 0.0f};
    pass(m_smaa_edges, m_edges, zero);
    cmd.barrier(m_edges, rhi::ResourceState::shader_read);
    pass(m_smaa_weights, m_weights, zero);
    cmd.barrier(m_weights, rhi::ResourceState::shader_read);
    // Every pixel is written; the clear (cheap) keeps the target's contents
    // defined where no draw is recorded, as on the null device.
    pass(m_smaa_blend, target, {background.x, background.y, background.z, 1.0f});
}

void Renderer::copy_streaming_stats() {
    m_stats.skinned_meshes = static_cast<u32>(m_skinned.size());
    m_stats.posed = m_posed;
    const StreamingStats& s = m_streamer->stats();
    m_stats.pool_bytes = s.pool_bytes;
    m_stats.pool_used_bytes = s.used_bytes;
    m_stats.resident_groups = s.resident_groups;
    m_stats.total_groups = s.total_groups;
    m_stats.streamed_in = s.loaded;
    m_stats.evicted = s.evicted;
    m_stats.waiting = s.waiting;
    const VoxelStats& v = m_voxels->stats();
    m_stats.voxel_bricks = v.bricks;
    m_stats.voxel_capacity = v.capacity;
    m_stats.voxel_pending = v.pending;
    m_stats.voxel_built = v.built;
    m_stats.voxel_missing = v.missing;
    m_stats.voxel_overflow = v.overflow;
    m_stats.voxel_cpu_ms = v.cpu_ms;
    m_stats.voxel_rebuilds = v.rebuilds;
    m_stats.far_field = m_far_field;
    m_stats.far_culled = m_far_culled;
    const ShadowStats& sh = m_shadows->stats();
    m_stats.shadows = m_shadows_on;
    m_stats.shadow_pages = m_shadows_on ? sh.resident : 0u;
    m_stats.shadow_capacity = sh.capacity;
    m_stats.shadow_drawn = m_shadows_on ? sh.drawn : 0u;
    m_stats.shadow_swayed = m_shadows_on ? sh.swayed : 0u;
    m_stats.shadow_waiting = m_shadows_on ? sh.waiting : 0u;
    m_stats.shadow_missing = m_shadows_on ? sh.missing : 0u;
    m_stats.shadow_draws = m_shadow_draws;
    const VoxelLightStats& gi = m_light_plan->stats();
    m_stats.gi = m_gi_on;
    m_stats.gi_lit = gi.lit;
    m_stats.gi_cleared = gi.cleared;
    m_stats.gi_waiting = gi.waiting;
}

bool Renderer::ensure_voxel_light() {
    if (!m_voxel_light.valid() && !m_voxel_light_failed) {
        rhi::BufferDesc desc;
        desc.size = u64{m_voxels->capacity()} * voxel::k_brick_voxels * 4;
        desc.usage = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
        desc.name = "voxel light";
        m_voxel_light = m_device.create_buffer(desc);
        if (!m_voxel_light.valid()) {
            // Not tried again every frame (a failing 64 MiB allocation each
            // frame would hitch); turning gi off and on tries again.
            m_voxel_light_failed = true;
            log_error("render", "could not create the voxel light pool (out of GPU memory?); no global illumination");
            return false;
        }
    }
    return m_voxel_light.valid();
}

bool Renderer::light_voxels(rhi::CommandList& cmd, const FrameData& data) {
    if (!m_gi_on) {
        return true;
    }
    cmd.begin_timing("gi");
    const std::span<const u32> clears = m_light_plan->clears();
    const std::span<const gpu::LightJob> jobs = m_light_plan->jobs();
    if (data.voxel_levels.data == nullptr) {
        m_light_plan->lost(); // out of memory: nothing lit, the clears again next time
    } else if (!clears.empty() || !jobs.empty()) {
        gpu::LightPush push{};
        push.frame = data.frame.descriptor;
        push.frame_offset = data.frame.offset;
        push.pool = m_device.read_descriptor(m_voxels->pool());
        push.table = m_device.read_descriptor(m_voxels->table());
        push.levels = data.voxel_levels.descriptor;
        push.levels_offset = data.voxel_levels.offset;
        push.level_count = m_voxels->levels();
        push.bricks_per_axis = static_cast<u32>(m_voxels->clipmap().desc().bricks_per_axis);
        push.light = m_device.write_descriptor(m_voxel_light);
        cmd.barrier(m_voxel_light, rhi::ResourceState::shader_write); // after last frame's reads
        if (!clears.empty()) {
            const rhi::TransientMemory list = cmd.upload_array(clears);
            if (list.data == nullptr) {
                m_light_plan->lost();
                cmd.end_timing();
                return false; // bricks not cleared would read another brick's light
            }
            {
                cmd.set_pipeline(m_light_clear);
                push.jobs = list.descriptor;
                for (usize first = 0; first < clears.size(); first += 65535) { // groups per dimension
                    push.jobs_offset = list.offset + static_cast<u32>(first * 4);
                    push.count = static_cast<u32>(std::min<usize>(clears.size() - first, 65535));
                    cmd.push(push);
                    cmd.dispatch(voxel::k_brick_voxels / 4 / 64, push.count);
                }
                cmd.barrier(m_voxel_light, rhi::ResourceState::shader_write); // cleared before lit
            }
        }
        if (!jobs.empty()) {
            const rhi::TransientMemory list = cmd.upload_array(jobs);
            if (list.data != nullptr) {
                push.jobs = list.descriptor;
                push.jobs_offset = list.offset;
                push.count = static_cast<u32>(jobs.size());
                cmd.set_pipeline(m_light_voxels);
                cmd.push(push);
                cmd.dispatch(push.count);
            } else {
                m_light_plan->lost(); // lit next frame instead (they read as not lit meanwhile)
            }
        }
    }
    cmd.end_timing();
    return true;
}

bool Renderer::ensure_sky_targets() {
    if (m_sky_failed) {
        return false;
    }
    const auto make = [this](rhi::TextureHandle& t, u32 w, u32 h, rhi::Format format, const char* name) {
        if (t.valid()) {
            return;
        }
        rhi::TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = format;
        desc.usage = rhi::TextureUsage::sampled | rhi::TextureUsage::shader_write;
        desc.name = name;
        t = m_device.create_texture(desc);
    };
    if (!m_cloud_map.valid()) {
        m_cloud_map_made = false;
    }
    make(m_sky_view, k_sky_view_width, k_sky_view_height, rhi::Format::rgba16_float, "sky view");
    make(m_cloud_map, k_cloud_map_size, k_cloud_map_size, rhi::Format::rgba8_unorm, "cloud pattern");
    make(m_cloud_shadow, k_cloud_shadow_size, k_cloud_shadow_size, rhi::Format::rgba16_float, "cloud shadows");
    make(m_cover, k_cover_size, k_cover_size, rhi::Format::r32_float, "weather: cover from above");
    const u32 hw = std::max((m_width + 1) / 2, 1u);
    const u32 hh = std::max((m_height + 1) / 2, 1u);
    make(m_clouds_a, hw, hh, rhi::Format::rgba16_float, "clouds: light");
    make(m_clouds_b, hw, hh, rhi::Format::rgba16_float, "clouds: distances");
    make(m_sky_multi, k_multi_size, k_multi_size, rhi::Format::rgba16_float, "sky: light scattered many times");
    make(m_sky_aerial, k_aerial_size * k_aerial_size, k_aerial_size, rhi::Format::rgba16_float, "sky: the air over the view");
    make(m_sky_aerial_through, k_aerial_size * k_aerial_size, k_aerial_size, rhi::Format::rgba16_float,
         "sky: what comes through the air over the view");
    if (!m_sky_ambient.valid()) {
        m_sky_ambient_made = false;
    }
    if (!m_sky_multi.valid()) {
        m_sky_multi_haze = -1.0f;
    }
    make(m_sky_ambient, 9, 1, rhi::Format::rgba16_float, "sky: its light from all round");
    if (!m_sky_view.valid() || !m_cloud_map.valid() || !m_cloud_shadow.valid() || !m_cover.valid() || !m_clouds_a.valid() ||
        !m_clouds_b.valid() || !m_sky_multi.valid() || !m_sky_aerial.valid() || !m_sky_aerial_through.valid() ||
        !m_sky_ambient.valid()) {
        m_sky_failed = true;
        log_error("render", "could not create the sky's textures (out of GPU memory?); the flat sky stays");
        return false;
    }
    return true;
}

bool Renderer::draw_sky(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame,
                        rhi::TransientMemory& params) {
    if (!ensure_sky_targets()) {
        return false;
    }
    const Sky& sky = frame.sky;
    const RenderSettings& settings = frame.settings;
    const Lighting& light = frame.lighting;
    const Vec3 sun = normalize_or_zero(light.sun_direction);
    const auto put3 = [](float* out, const Vec3& v) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    };
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    gpu::SkyParams p{};
    put3(p.sun_direction, sun);
    put3(p.sun_light, light.sun_light);
    put3(p.sky_light, light.sky_light);
    put3(p.ground_light, light.ground_light);
    put3(p.sun_top, sky.sun_top);
    p.cloud_cover = std::clamp(finite(sky.cloud_cover, 0.0f), 0.0f, 1.0f);
    p.cloud_density = std::clamp(finite(sky.cloud_density, 0.0f), 0.0f, 1.0f);
    p.cloud_base = finite(sky.cloud_base, 1500.0f);
    p.cloud_top = std::max(finite(sky.cloud_top, 3000.0f), p.cloud_base + 1.0f);
    // The camera's place in the clouds' pattern, within one tile (floats
    // keep it exact however far out the camera is).
    const double tile = std::max(static_cast<double>(finite(sky.cloud_tile, 32000.0f)), 1000.0);
    const DVec3 cam = frame.view.position;
    const auto wrap = [tile](double v) {
        const double m = std::fmod(v, tile);
        return std::isfinite(m) ? (m < 0.0 ? m + tile : m) : 0.0;
    };
    p.cam_x = static_cast<float>(wrap(cam.x - sky.cloud_offset.x));
    p.cam_z = static_cast<float>(wrap(cam.z - sky.cloud_offset.z));
    p.cam_alt = static_cast<float>(cam.y);
    p.cloud_tile = static_cast<float>(tile);
    p.fog_density = std::clamp(finite(sky.fog_density, 0.0f), 0.0f, 1.0f);
    p.fog_falloff = std::clamp(finite(sky.fog_falloff, 0.005f), 1e-5f, 10.0f);
    p.haze = std::clamp(finite(sky.haze, 1.0f), 0.0f, 10.0f);
    p.cloud_steps = static_cast<float>(std::clamp<u32>(settings.cloud_steps, 8u, 256u));
    const bool clouds = settings.clouds && p.cloud_cover > 0.0f && p.cloud_density > 0.0f;
    const bool shadows = clouds && settings.cloud_shadows && sun.y > 0.0f;
    // The shadow map: a square on the camera, its corner snapped to the
    // map's texels in the world (so it does not crawl as the camera moves).
    const double size = k_cloud_shadow_metres;
    const double texel = size / k_cloud_shadow_size;
    // Centred where the ground about the camera meets the sun's ray up to
    // the clouds' base (the shadows it is asked for), at most 40% of the
    // map off the camera.
    double shift_x = 0.0;
    double shift_z = 0.0;
    if (sun.y > 0.0) {
        const double rise = std::max(static_cast<double>(p.cloud_base) - cam.y, 0.0);
        const double along = rise / std::max(static_cast<double>(sun.y), 0.05);
        shift_x = std::clamp(sun.x * along, -0.4 * size, 0.4 * size);
        shift_z = std::clamp(sun.z * along, -0.4 * size, 0.4 * size);
    }
    const double min_x = std::floor((cam.x + shift_x - size * 0.5) / texel) * texel;
    const double min_z = std::floor((cam.z + shift_z - size * 0.5) / texel) * texel;
    p.shadow_origin[0] = static_cast<float>(min_x - cam.x);
    p.shadow_origin[1] = static_cast<float>(min_z - cam.z);
    p.shadow_size = static_cast<float>(size);
    p.flags = (clouds ? 1u : 0u) | (shadows ? 2u : 0u);
    const u32 hw = std::max((m_width + 1) / 2, 1u);
    const u32 hh = std::max((m_height + 1) / 2, 1u);
    p.half_size[0] = static_cast<float>(hw);
    p.half_size[1] = static_cast<float>(hh);
    p.full_size[0] = static_cast<float>(m_width);
    p.full_size[1] = static_cast<float>(m_height);
    p.sky_view = m_device.read_descriptor(m_sky_view);
    p.cloud_map = m_device.read_descriptor(m_cloud_map);
    p.cloud_shadow = m_device.read_descriptor(m_cloud_shadow);
    p.clouds_a = m_device.read_descriptor(m_clouds_a);
    p.clouds_b = m_device.read_descriptor(m_clouds_b);
    p.depth = m_device.read_descriptor(m_depth);
    // The physical sky.
    const bool physical = settings.physical_sky;
    p.multi_scatter = m_device.read_descriptor(m_sky_multi);
    p.aerial = m_device.read_descriptor(m_sky_aerial);
    p.aerial_through = m_device.read_descriptor(m_sky_aerial_through);
    p.sky_ambient = m_device.read_descriptor(m_sky_ambient);
    p.aerial_far = std::clamp(finite(settings.aerial_distance, 32000.0f), 1000.0f, 100000.0f);
    p.atmosphere = physical ? 7u : 0u; // (the ambient table is filled before the clouds and shading read it)
    // Rain and snow, and the ground under them.
    const auto unit = [&finite](float v) { return std::clamp(finite(v, 0.0f), 0.0f, 1.0f); };
    p.rain = unit(sky.rain);
    p.snow = unit(sky.snow);
    p.wetness = unit(sky.wetness);
    p.snow_cover = unit(sky.snow_cover);
    p.wind[0] = std::clamp(finite(sky.wind.x, 0.0f), -200.0f, 200.0f);
    p.wind[1] = std::clamp(finite(sky.wind.z, 0.0f), -200.0f, 200.0f);
    // Wrapped: what falls repeats every 64 s without a seam.
    // (3584 s: a whole number of apply_falling's 64 s cycles.)
    p.time = static_cast<float>(std::isfinite(sky.time) ? std::fmod(std::max(sky.time, 0.0), 3584.0) : 0.0);
    const auto wrap_place = [](double v) {
        const double m = std::fmod(v, k_weather_wrap);
        return std::isfinite(m) ? static_cast<float>(m < 0.0 ? m + k_weather_wrap : m) : 0.0f;
    };
    p.place[0] = wrap_place(cam.x);
    p.place[1] = wrap_place(cam.z);
    p.pixel_angle = 2.0f * std::tan(degrees_to_radians(std::clamp(frame.view.vertical_fov_degrees, 1.0f, 170.0f)) * 0.5f) /
                    static_cast<float>(std::max(m_height, 1u));
    const bool falling = settings.precipitation && (p.rain > 0.0f || p.snow > 0.0f);
    const bool surfaces = settings.wet_surfaces && (p.wetness > 0.0f || p.snow_cover > 0.0f);
    const bool voxels = m_voxels != nullptr && data.voxel_levels.data != nullptr && m_voxels->levels() > 0;
    const bool cover = settings.weather_cover && (falling || surfaces) && voxels;
    p.weather_flags = (falling ? 1u : 0u) | (surfaces ? 2u : 0u) | (cover ? 4u : 0u);
    // The cover map: a square on the camera snapped to its metre texels.
    const double cover_texel = k_cover_metres / k_cover_size;
    p.cover_origin[0] = static_cast<float>(std::floor((cam.x - k_cover_metres * 0.5) / cover_texel) * cover_texel - cam.x);
    p.cover_origin[1] = static_cast<float>(std::floor((cam.z - k_cover_metres * 0.5) / cover_texel) * cover_texel - cam.z);
    p.cover_size = static_cast<float>(k_cover_metres);
    p.cover = m_device.read_descriptor(m_cover);
    p.voxel_pool = data.frame.descriptor; // read, never used, without the voxel world
    p.voxel_table = data.frame.descriptor;
    p.voxel_levels = data.frame.descriptor;
    if (voxels) {
        p.voxel_pool = m_device.read_descriptor(m_voxels->pool());
        p.voxel_table = m_device.read_descriptor(m_voxels->table());
        p.voxel_levels = data.voxel_levels.descriptor;
        p.voxel_levels_offset = data.voxel_levels.offset;
        p.voxel_level_count = m_voxels->levels();
        p.voxel_bricks_per_axis = static_cast<u32>(m_voxels->clipmap().desc().bricks_per_axis);
    }
    params = cmd.upload(bytes_of_one(p));
    if (params.data == nullptr) {
        return false; // out of memory: the flat sky this frame
    }
    gpu::SkyPush push{};
    push.params = params.descriptor;
    push.params_offset = params.offset;
    push.frame = data.frame.descriptor;
    push.frame_offset = data.frame.offset;
    const auto groups = [](u32 n) { return (n + 7) / 8; };
    cmd.begin_timing("sky");
    if (!m_cloud_map_made) {
        cmd.barrier(m_cloud_map, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_cloud_map_pass);
        push.target = m_device.write_descriptor(m_cloud_map);
        cmd.push(push);
        cmd.dispatch(groups(k_cloud_map_size), groups(k_cloud_map_size));
        cmd.barrier(m_cloud_map, rhi::ResourceState::shader_read);
        m_cloud_map_made = true;
    }
    if (physical && p.haze != m_sky_multi_haze) {
        // Light scattered many times first: the sky view and the air over
        // the view read it. It depends only on the haze: made again when
        // that changes (a table, not history).
        m_sky_multi_haze = p.haze;
        cmd.barrier(m_sky_multi, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_sky_multi_pass);
        push.target = m_device.write_descriptor(m_sky_multi);
        cmd.push(push);
        cmd.dispatch(groups(k_multi_size), groups(k_multi_size));
        cmd.barrier(m_sky_multi, rhi::ResourceState::shader_read);
    }
    cmd.barrier(m_sky_view, rhi::ResourceState::shader_write);
    cmd.set_pipeline(m_sky_view_pass);
    push.target = m_device.write_descriptor(m_sky_view);
    cmd.push(push);
    cmd.dispatch(groups(k_sky_view_width), groups(k_sky_view_height));
    cmd.barrier(m_sky_view, rhi::ResourceState::shader_read);
    if (physical) {
        cmd.barrier(m_sky_aerial, rhi::ResourceState::shader_write);
        cmd.barrier(m_sky_aerial_through, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_sky_aerial_pass);
        push.target = m_device.write_descriptor(m_sky_aerial);
        push.target_b = m_device.write_descriptor(m_sky_aerial_through);
        cmd.push(push);
        cmd.dispatch(groups(k_aerial_size), groups(k_aerial_size));
        cmd.barrier(m_sky_aerial, rhi::ResourceState::shader_read);
        cmd.barrier(m_sky_aerial_through, rhi::ResourceState::shader_read);
        // The sky's light from all round, from the view table (one group).
        cmd.barrier(m_sky_ambient, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_sky_ambient_pass);
        push.target = m_device.write_descriptor(m_sky_ambient);
        cmd.push(push);
        cmd.dispatch(1);
        cmd.barrier(m_sky_ambient, rhi::ResourceState::shader_read);
        m_sky_ambient_made = true;
    } else {
        m_sky_ambient_made = false; // (on again: the flat colours until it is filled again)
    }
    if (shadows) {
        cmd.barrier(m_cloud_shadow, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_cloud_shadow_pass);
        push.target = m_device.write_descriptor(m_cloud_shadow);
        cmd.push(push);
        cmd.dispatch(groups(k_cloud_shadow_size), groups(k_cloud_shadow_size));
        cmd.barrier(m_cloud_shadow, rhi::ResourceState::shader_read);
    }
    if (cover) {
        cmd.barrier(m_cover, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_cover_pass);
        push.target = m_device.write_descriptor(m_cover);
        cmd.push(push);
        cmd.dispatch(groups(k_cover_size), groups(k_cover_size));
        cmd.barrier(m_cover, rhi::ResourceState::shader_read);
    }
    cmd.end_timing();
    if (clouds) {
        cmd.begin_timing("clouds");
        cmd.barrier(m_depth, rhi::ResourceState::shader_read);
        cmd.barrier(m_clouds_a, rhi::ResourceState::shader_write);
        cmd.barrier(m_clouds_b, rhi::ResourceState::shader_write);
        cmd.set_pipeline(m_clouds_pass);
        push.target = m_device.write_descriptor(m_clouds_a);
        push.target_b = m_device.write_descriptor(m_clouds_b);
        cmd.push(push);
        cmd.dispatch(groups(hw), groups(hh));
        cmd.barrier(m_clouds_a, rhi::ResourceState::shader_read);
        cmd.barrier(m_clouds_b, rhi::ResourceState::shader_read);
        cmd.end_timing();
    }
    m_stats.sky = true;
    m_stats.physical_sky = physical;
    m_stats.clouds = clouds;
    m_stats.cloud_shadows = shadows;
    m_stats.falling = falling;
    m_stats.wet_surfaces = surfaces;
    m_stats.weather_cover = cover;
    return true;
}

} // namespace giant::render
