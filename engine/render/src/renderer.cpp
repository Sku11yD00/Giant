#include "giant/render/renderer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/geometry/mesh.h"
#include "decals.h"
#include "gpu_layout.h"
#include "render_shaders.h"
#include "smaa.h"
#include "streaming.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace giant::render {

namespace {

// Capacities of the GPU culling lists. Past them geometry is dropped (and
// RenderStats::overflow says so); streaming (N1 part 4) revisits them.
constexpr u32 k_queue_capacity = 1u << 19;    // (instance, node) items per tree level
constexpr u32 k_visible_capacity = 1u << 20;  // clusters drawn per frame
constexpr u32 k_occluded_capacity = 1u << 18; // clusters retested in the second pass
// Instance culling is one dispatch (65535 groups of 64), and the instances
// are one transient upload.
constexpr u32 k_max_instances =
    std::min<u32>(65535u * 64u, rhi::k_max_transient_allocation / sizeof(gpu::Instance));
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

gpu::Instance to_gpu(const Instance& in, const DVec3& camera, u32 material_count) {
    gpu::Instance out{};
    const Vec3 t = to_camera_relative(in.position, camera);
    // Columns of R * S are the rotated axes, scaled.
    const Vec3 axes[3] = {rotate(in.rotation, {1, 0, 0}), rotate(in.rotation, {0, 1, 0}),
                          rotate(in.rotation, {0, 0, 1})};
    const float scale[3] = {in.scale.x, in.scale.y, in.scale.z};
    const float translation[3] = {t.x, t.y, t.z};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const Vec3& a = axes[col];
            const float component = row == 0 ? a.x : row == 1 ? a.y : a.z;
            out.model[row * 4 + col] = component * scale[col];
        }
        out.model[row * 4 + 3] = translation[row];
    }
    out.mesh = in.mesh;
    out.material = in.material < material_count ? in.material : material_count; // the default, last
    out.max_scale = std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
    return out;
}

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
          m_smaa_blend}) {
        m_device.destroy(p);
    }
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
    for (const rhi::BufferHandle b :
         {m_cull.counters, m_cull.args, m_cull.queues[0], m_cull.queues[1], m_cull.visible,
          m_cull.occluded_clusters, m_cull.occluded_instances, m_cull.readback[0],
          m_cull.readback[1], m_cull.readback[2]}) {
        m_device.destroy(b);
    }
}

bool Renderer::init() {
    m_streamer = std::make_unique<GeometryStreamer>(m_device, m_desc.geometry_pool_bytes);
    if (!m_streamer->init()) {
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
    rhi::GraphicsPipelineDesc resolve;
    resolve.vertex = shaders::resolve_vs();
    resolve.pixel = shaders::resolve_ps();
    resolve.color_formats[0] = rhi::Format::rgba8_srgb;
    resolve.cull = rhi::CullMode::none;
    resolve.name = "shade visibility";
    m_resolve = m_device.create_pipeline(resolve);
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
    for (const rhi::PipelineHandle p :
         {m_cull_reset, m_cull_prepare, m_cull_instances, m_cull_traverse, m_cull_retest, m_hzb_first,
          m_hzb_down, m_raster_vertex, m_resolve, m_smaa_edges, m_smaa_weights, m_smaa_blend}) {
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
    return static_cast<MeshId>(m_meshes.size() - 1);
}

void Renderer::remove_mesh(MeshId id) {
    if (id >= m_meshes.size() || std::find(m_builtin.begin(), m_builtin.end(), id) != m_builtin.end()) {
        return;
    }
    destroy(m_meshes[id]); // the slot stays, empty: ids are never reused
    m_streamer->remove(id);
}

const geometry::ClusterMesh* Renderer::mesh(MeshId id) const {
    return id < m_meshes.size() && m_meshes[id].live ? &m_meshes[id].data : nullptr;
}

MeshId Renderer::builtin(BuiltinMesh mesh) const { return m_builtin[static_cast<usize>(mesh)]; }

Mat4 Renderer::view_projection(const View& view, float aspect) {
    const float fov = degrees_to_radians(std::clamp(view.vertical_fov_degrees, 1.0f, 170.0f));
    const float near_plane = std::max(view.near_plane, 1e-4f);
    return perspective_reverse_z(fov, aspect, near_plane) * view_from_orientation(view.orientation);
}

bool Renderer::ensure_targets(u32 width, u32 height) {
    if (m_depth.valid() && m_vis.valid() && m_hzb.valid() && m_color.valid() && m_edges.valid() &&
        m_weights.valid() && m_width == width && m_height == height) {
        return true;
    }
    m_device.destroy(m_depth);
    m_device.destroy(m_vis);
    m_device.destroy(m_hzb);
    m_device.destroy(m_color);
    m_device.destroy(m_edges);
    m_device.destroy(m_weights);
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
        !m_weights.valid()) {
        // Half-made targets are not reused: the next frame tries again.
        m_width = 0;
        m_height = 0;
        log_error("render", "could not create the frame's targets (out of GPU memory?)");
        return false;
    }
    return true;
}

bool Renderer::ensure_cull_buffers(u32 instance_count) {
    CullBuffers& c = m_cull;
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
            m_entries.push_back(slot);
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
}

void Renderer::build_hzb(rhi::CommandList& cmd) {
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
}

void Renderer::cull_on_gpu(rhi::CommandList& cmd, const FrameData& data) {
    CullBuffers& c = m_cull;
    const auto rw = [this, &cmd, &c](bool with_args) {
        // Every list the culling shaders write, ready for (more) writes; a
        // repeat is a UAV barrier, so the previous dispatch's writes land.
        for (const rhi::BufferHandle b : {c.counters, c.queues[0], c.queues[1], c.visible, c.occluded_clusters,
                                          c.occluded_instances, m_streamer->requests(), m_streamer->usage()}) {
            cmd.barrier(b, rhi::ResourceState::shader_write);
        }
        if (with_args) {
            cmd.barrier(c.args, rhi::ResourceState::shader_write);
        }
    };
    gpu::CullPush base{};
    base.frame = data.frame.descriptor;
    base.frame_offset = data.frame.offset;
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
    base.request_capacity = GeometryStreamer::k_request_capacity;
    base.usage = m_device.write_descriptor(m_streamer->usage());

    const auto prepare = [&](u32 pass, u32 mode, u32 slot_in, u32 capacity, u32 arg_offset) {
        rw(true);
        gpu::CullPush p = base;
        p.pass = pass;
        p.mode = mode;
        p.slot_in = slot_in;
        p.queue_capacity = capacity;
        p.arg_offset = arg_offset;
        cmd.set_pipeline(m_cull_prepare);
        cmd.push(p);
        cmd.dispatch(1);
    };
    const auto walk_trees = [&](u32 pass) {
        for (u32 level = 0; level < data.tree_levels; ++level) {
            prepare(pass, 0, gpu::level_slot(pass, level), k_queue_capacity, gpu::k_args_traverse);
            rw(false);
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
    };

    cmd.barrier(m_hzb, rhi::ResourceState::shader_read);
    rw(true);
    cmd.set_pipeline(m_cull_reset);
    cmd.push(base);
    cmd.dispatch(1);

    // Pass 0: against last frame's HZB (when there is one).
    rw(false);
    {
        gpu::CullPush p = base;
        p.pass = 0;
        p.queue_out = m_device.write_descriptor(c.queues[0]);
        p.slot_out = gpu::level_slot(0, 0);
        cmd.set_pipeline(m_cull_instances);
        cmd.push(p);
        cmd.dispatch(groups_of(data.instance_count, 64));
    }
    walk_trees(0);
    prepare(0, 1, 0, k_visible_capacity, gpu::k_args_raster);
    raster(cmd, data, 0, true);

    if (data.occlusion) {
        // Pass 1: what pass 0 found hidden, against the HZB of what it drew.
        build_hzb(cmd);
        prepare(1, 0, gpu::k_slot_occluded_instances, c.instance_capacity, gpu::k_args_instances);
        prepare(1, 0, gpu::k_slot_occluded_clusters, k_occluded_capacity, gpu::k_args_retest);
        rw(false);
        cmd.barrier(c.args, rhi::ResourceState::indirect_argument);
        gpu::CullPush p = base;
        p.pass = 1;
        p.queue_out = m_device.write_descriptor(c.queues[0]);
        p.slot_out = gpu::level_slot(1, 0);
        cmd.set_pipeline(m_cull_instances);
        cmd.push(p);
        cmd.dispatch_indirect(c.args, gpu::k_args_instances);
        rw(false);
        cmd.set_pipeline(m_cull_retest);
        cmd.push(p);
        cmd.dispatch_indirect(c.args, gpu::k_args_retest);
        walk_trees(1);
        prepare(1, 1, 0, k_visible_capacity, gpu::k_args_raster);
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

void Renderer::render(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameInput& frame) {
    m_stats = {};
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
    m_streamer->update(cmd, frame_number);

    // Instances with a live mesh.
    m_valid.clear();
    for (u32 i = 0; i < frame.instances.size(); ++i) {
        const MeshId id = frame.instances[i].mesh;
        if (mesh(id) != nullptr && m_streamer->ready(id) && m_streamer->tables_current()) {
            m_valid.push_back(i);
        }
    }
    if (m_valid.size() > k_max_instances) {
        if (!m_instances_logged) {
            m_instances_logged = true;
            log_error("render", "more than " + std::to_string(k_max_instances) +
                                    " instances in one frame; the rest are not drawn");
        }
        m_valid.resize(k_max_instances);
    }
    if (gpu && !ensure_cull_buffers(static_cast<u32>(m_valid.size()))) {
        m_valid.clear(); // out of memory: draw nothing
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
    k.instance_count = static_cast<u32>(m_valid.size());
    k.occlusion = occlusion ? 1u : 0u;
    k.pool = m_device.read_descriptor(m_streamer->pool());
    k.frame_number = frame_number;

    FrameData data;
    data.frame = cmd.upload(bytes_of_one(k));
    data.instance_count = static_cast<u32>(m_valid.size());
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
    if (!m_valid.empty()) {
        data.instances = cmd.allocate(static_cast<u32>(m_valid.size() * sizeof(gpu::Instance)));
        if (data.instances.data == nullptr) {
            m_valid.clear(); // out of memory: the device is lost; draw nothing
            data.instance_count = 0;
        } else {
            for (usize i = 0; i < m_valid.size(); ++i) {
                const gpu::Instance g = to_gpu(frame.instances[m_valid[i]], frame.view.position, data.material_count);
                std::memcpy(data.instances.data + i * sizeof(g), &g, sizeof(g));
            }
        }
    }
    if (data.frame.data == nullptr || data.meshes.data == nullptr || data.materials.data == nullptr) {
        return; // out of memory: the device is lost
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
    const bool antialias = settings.antialiasing && settings.debug_view == DebugView::lit;
    const rhi::TextureHandle shaded = antialias ? m_color : target;
    rhi::RenderPassDesc pass;
    const Vec3& bg = light.background;
    pass.colors[0] = {shaded, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
    cmd.begin_render_pass(pass);
    cmd.set_pipeline(m_resolve);
    gpu::ResolvePush push{};
    set4(push.background, bg, 1.0f);
    push.frame = data.frame.descriptor;
    push.frame_offset = data.frame.offset;
    push.instances = data.instances.data != nullptr ? data.instances.descriptor : data.frame.descriptor;
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
    cmd.push(push);
    cmd.draw(3);
    ++m_stats.draws;
    cmd.end_render_pass();
    if (antialias) {
        smaa(cmd, target, bg);
    }
    copy_streaming_stats();
    m_stats.materials = data.material_count;
    m_stats.decals = data.tile_columns != 0 ? static_cast<u32>(m_decal_bins->decals.size()) : 0;
    m_stats.decal_overflow = m_decal_bins->overflow;
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
}

} // namespace giant::render
