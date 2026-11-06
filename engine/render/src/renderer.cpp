#include "giant/render/renderer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/geometry/mesh.h"
#include "cells.h"
#include "decals.h"
#include "instance_store.h"
#include "gpu_layout.h"
#include "render_shaders.h"
#include "shadow_pages.h"
#include "smaa.h"
#include "streaming.h"
#include "voxel_light.h"
#include "voxel_scene.h"

#include <algorithm>
#include <bit>
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
constexpr u32 k_max_materials = 1u << 20; // per frame (32 MiB)
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

gpu::Material to_gpu(const Material& m) {
    gpu::Material out{};
    set4(out.base_roughness, m.base_color, m.roughness);
    set4(out.emissive_metallic, m.emissive, m.metallic);
    return out;
}

void set_bounds(float* out, const Vec3& center, float radius) { set4(out, center, radius); }

} // namespace

// This frame's uploads, shared by the passes.
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
          m_shadow_clear_pages, m_shadow_clear_words, m_light_clear, m_light_voxels}) {
        m_device.destroy(p);
    }
    m_device.destroy(m_voxel_light);
    m_device.destroy(m_smaa_area);
    m_device.destroy(m_smaa_search);
    for (Mesh& mesh : m_meshes) {
        destroy(mesh);
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
    resolve.pixel = shaders::resolve_direct_ps(); // without screen reflections: finished in one pass
    resolve.color_formats = {};
    resolve.color_formats[0] = rhi::Format::rgba8_srgb;
    resolve.color_count = 1;
    resolve.name = "shade visibility (no screen reflections)";
    m_resolve_direct = m_device.create_pipeline(resolve);
    rhi::GraphicsPipelineDesc reflect;
    reflect.vertex = shaders::reflect_vs();
    reflect.pixel = shaders::reflect_ps();
    reflect.color_formats[0] = rhi::Format::rgba8_srgb;
    reflect.cull = rhi::CullMode::none;
    reflect.name = "screen reflections and tone mapping";
    m_reflect = m_device.create_pipeline(reflect);
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
          m_light_voxels}) {
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
    mesh = {};
}

MeshId Renderer::add_mesh(geometry::ClusterMesh data) {
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
    const u32 id = static_cast<u32>(m_meshes.size());
    if (!m_streamer->add(id, data, clusters)) {
        return k_no_mesh;
    }
    mesh.clusters = upload(bytes_of(clusters), "mesh clusters");
    mesh.groups = upload(bytes_of(groups), "mesh cluster groups");
    mesh.nodes = upload(bytes_of(nodes), "mesh LOD tree");
    for (const rhi::BufferHandle b : {mesh.clusters, mesh.groups, mesh.nodes}) {
        if (!b.valid()) {
            destroy(mesh);
            m_streamer->remove(id);
            log_error("render", "could not upload a mesh (out of GPU memory?)");
            return k_no_mesh;
        }
    }
    mesh.tree_levels = tree_levels;
    mesh.data = std::move(data);
    mesh.live = true;
    m_meshes.push_back(std::move(mesh));
    ++m_meshes_version;
    return static_cast<MeshId>(m_meshes.size() - 1);
}

void Renderer::remove_mesh(MeshId id) {
    if (id >= m_meshes.size() || std::find(m_builtin.begin(), m_builtin.end(), id) != m_builtin.end()) {
        return;
    }
    destroy(m_meshes[id]); // the slot stays, empty: ids are never reused
    m_streamer->remove(id);
    ++m_meshes_version;
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
        m_weights.valid() && m_radiance.valid() && m_gloss.valid() && m_fallback.valid() && m_width == width &&
        m_height == height) {
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
        !m_weights.valid() || !m_radiance.valid() || !m_gloss.valid() || !m_fallback.valid()) {
        // Half-made targets are not reused: the next frame tries again.
        m_width = 0;
        m_height = 0;
        log_error("render", "could not create the frame's targets (out of GPU memory?)");
        return false;
    }
    return true;
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
        const geometry::ClusterMesh& data = m_meshes[in.mesh].data;
        lod.position = to_camera_relative(in.position, frame.view.position);
        lod.rotation = in.rotation;
        lod.scale = in.scale;
        m_selected.clear();
        m_wanted.clear();
        m_touched.clear();
        const geometry::LodResidency residency{&m_streamer->resident(in.mesh), &m_wanted, &m_touched};
        geometry::select_clusters(data, lod, m_selected, &residency);
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

void Renderer::draw_shadows(rhi::CommandList& cmd, FrameData& data, const FrameInput& frame,
                            const gpu::FrameConstants& base) {
    const u32 budget = std::clamp<u32>(frame.settings.shadow_pages_per_frame, 1u, 4096u);
    const std::vector<ShadowPages::Draw> draws = m_shadows->plan(budget);
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
    if (settings.voxel_world) {
        cmd.begin_timing("voxels");
        m_voxels->set_stand_in_radius(settings.far_field_max_radius);
        m_voxels->update(cmd, frame.view.position, frame.instances, frame.cells, frame.materials, m_meshes_version,
                         [this](MeshId id) { return mesh(id); }, std::clamp(settings.voxel_ms_per_frame, 0.0f, 1000.0f));
        cmd.end_timing();
    } else {
        m_voxels->idle();
    }
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
        if (in.always_mesh || !(radius <= far_radius)) {
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
    // Sun shadows: the lit view, with GPU culling (pages are drawn with it).
    plan_shadows(frame, settings.shadows && gpu && lit_view &&
                            ensure_shadow_buffers(static_cast<u32>(m_valid.size())));

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
    k.exposure = light.exposure;
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
    const usize material_limit = std::min<usize>(frame.materials.size(), k_max_materials);
    if (frame.materials.size() > material_limit && !m_materials_logged) {
        m_materials_logged = true;
        log_error("render", "more than " + std::to_string(k_max_materials) +
                                " materials in one frame; instances using the rest draw the default");
    }
    m_gpu_materials.clear();
    for (usize i = 0; i < material_limit; ++i) {
        m_gpu_materials.push_back(to_gpu(frame.materials[i]));
    }
    m_gpu_materials.push_back(to_gpu(Material{}));
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
        set_bounds(e.bounds, m.data.bounds.center, m.data.bounds.radius);
        if (used[id]) {
            data.tree_levels = std::max(data.tree_levels, m.tree_levels);
            for (const rhi::BufferHandle b : {m.clusters, m.groups, m.nodes}) {
                cmd.barrier(b, rhi::ResourceState::shader_read);
            }
        }
    }
    data.meshes = cmd.upload(bytes_of(table));
    if (far_field || m_gi_on || settings.debug_view == DebugView::voxels) {
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
    if (m_shadows_on) {
        draw_shadows(cmd, data, frame, k); // after the view's culling: its streaming requests go first
    }
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
    cmd.begin_timing("shading");
    // With screen reflections, shading writes radiance before exposure and
    // the screen reflections pass reflects it and tone maps it into
    // `shaded`; without, shading tone maps into `shaded` itself.
    rhi::RenderPassDesc pass;
    const Vec3& bg = light.background;
    if (screen_reflections) {
        pass.colors[0] = {m_radiance, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 0.0f}};
        pass.colors[1] = {m_gloss, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 0.0f}};
        pass.colors[2] = {m_fallback, rhi::LoadOp::clear, {0.0f, 0.0f, 0.0f, 0.0f}};
        pass.color_count = 3;
    } else {
        pass.colors[0] = {shaded, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
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
        // Screen reflections from this frame's radiance, then exposure and
        // tone mapping.
        cmd.barrier(m_radiance, rhi::ResourceState::shader_read);
        cmd.barrier(m_gloss, rhi::ResourceState::shader_read);
        cmd.barrier(m_fallback, rhi::ResourceState::shader_read);
        cmd.barrier(m_depth, rhi::ResourceState::shader_read);
        rhi::RenderPassDesc finish;
        // Cleared all the same: defined where no draw is recorded (the null device).
        finish.colors[0] = {shaded, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
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
}

void Renderer::finish_stats(const FrameData& data) {
    copy_streaming_stats();
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
    m_stats.far_field = m_far_field;
    m_stats.far_culled = m_far_culled;
    const ShadowStats& sh = m_shadows->stats();
    m_stats.shadows = m_shadows_on;
    m_stats.shadow_pages = m_shadows_on ? sh.resident : 0u;
    m_stats.shadow_capacity = sh.capacity;
    m_stats.shadow_drawn = m_shadows_on ? sh.drawn : 0u;
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

} // namespace giant::render
