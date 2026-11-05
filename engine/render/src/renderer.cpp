#include "giant/render/renderer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/geometry/mesh.h"
#include "render_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace giant::render {

namespace {

// Matches FrameConstants in shaders/clusters.slang.
struct FrameConstants {
    float view_proj[16];
    float sun_direction[4];
    float sun_light[4];
    float sky_light[4];
    float ground_light[4];
    float params[4];
};
static_assert(sizeof(FrameConstants) == 144);

// Matches the 128-byte instance layout in shaders/clusters.slang.
struct GpuInstance {
    float model[12];  // rows of the 3x4 model matrix (camera-relative)
    float normal[12]; // rows of the normal matrix, w unused
    float base_roughness[4];
    float emissive_metallic[4];
};
static_assert(sizeof(GpuInstance) == 128);

struct ClusterPush {
    u32 frame;
    u32 frame_offset;
    u32 instances;
    u32 instance_offset;
    u32 entries;
    u32 entry_offset;
    u32 vertices;
    u32 indices;
    u32 clusters;
    u32 pad[3];
};
static_assert(sizeof(ClusterPush) == 48);

// Matches k_cluster_bytes in shaders/clusters.slang.
struct GpuCluster {
    u32 first_index;
    u32 triangle_count;
    u32 level;
    u32 pad;
};
static_assert(sizeof(GpuCluster) == 16);

constexpr u32 k_vertices_per_cluster = geometry::k_cluster_max_triangles * 3;

template <typename T>
std::span<const u8> bytes_of(const std::vector<T>& v) {
    return {reinterpret_cast<const u8*>(v.data()), v.size() * sizeof(T)};
}

void set4(float* out, const Vec3& v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

GpuInstance to_gpu(const Instance& in, const DVec3& camera) {
    GpuInstance out{};
    const Vec3 t = to_camera_relative(in.position, camera);
    // Columns of R * S are the rotated axes scaled; the normal matrix is the
    // inverse transpose, R * S^-1, so the same axes divided by the scale.
    const Vec3 axes[3] = {rotate(in.rotation, {1, 0, 0}), rotate(in.rotation, {0, 1, 0}),
                          rotate(in.rotation, {0, 0, 1})};
    const float scale[3] = {in.scale.x, in.scale.y, in.scale.z};
    const float translation[3] = {t.x, t.y, t.z};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const Vec3& a = axes[col];
            const float component = row == 0 ? a.x : row == 1 ? a.y : a.z;
            out.model[row * 4 + col] = component * scale[col];
            // A zero scale flattens the mesh; keep its normals finite.
            const float s = std::fabs(scale[col]) < 1e-12f ? 1e-12f : scale[col];
            out.normal[row * 4 + col] = component / s;
        }
        out.model[row * 4 + 3] = translation[row];
        out.normal[row * 4 + 3] = 0.0f;
    }
    set4(out.base_roughness, in.base_color, in.roughness);
    set4(out.emissive_metallic, in.emissive, in.metallic);
    return out;
}

} // namespace

std::unique_ptr<Renderer> Renderer::create(rhi::Device& device) {
    auto renderer = std::make_unique<Renderer>(ConstructKey{}, device);
    if (!renderer->init()) {
        return nullptr;
    }
    return renderer;
}

Renderer::Renderer(ConstructKey, rhi::Device& device) : m_device(device) {}

Renderer::~Renderer() {
    // Deferred by the device until the GPU is done with them.
    m_device.destroy(m_pipeline);
    for (Mesh& mesh : m_meshes) {
        destroy(mesh);
    }
    m_device.destroy(m_depth);
}

bool Renderer::init() {
    rhi::GraphicsPipelineDesc desc;
    desc.vertex = shaders::clusters_vs();
    desc.pixel = shaders::clusters_ps();
    desc.color_formats[0] = rhi::Format::rgba8_srgb;
    desc.depth_format = rhi::Format::depth32_float;
    desc.depth = rhi::DepthMode::test_write;
    desc.cull = rhi::CullMode::back;
    desc.name = "clusters (forward)";
    m_pipeline = m_device.create_pipeline(desc);
    if (!m_pipeline.valid()) {
        log_error("render", "could not create the cluster pipeline");
        return false;
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
    m_device.destroy(mesh.vertices);
    m_device.destroy(mesh.indices);
    m_device.destroy(mesh.clusters);
    mesh = {};
}

MeshId Renderer::add_mesh(geometry::ClusterMesh data) {
    if (data.empty() || m_meshes.size() >= k_no_mesh) {
        return k_no_mesh;
    }
    std::vector<GpuCluster> table;
    table.reserve(data.clusters.size());
    for (const geometry::Cluster& c : data.clusters) {
        table.push_back({c.first_index, c.triangle_count, data.groups[c.group].depth, 0});
    }
    Mesh mesh;
    rhi::BufferDesc desc;
    desc.usage = rhi::BufferUsage::shader_read;
    desc.size = data.vertices.size() * sizeof(geometry::Vertex);
    desc.name = "mesh vertices";
    mesh.vertices = m_device.create_buffer(desc, bytes_of(data.vertices));
    desc.size = data.indices.size() * sizeof(u32);
    desc.name = "mesh cluster triangles";
    mesh.indices = m_device.create_buffer(desc, bytes_of(data.indices));
    desc.size = table.size() * sizeof(GpuCluster);
    desc.name = "mesh clusters";
    mesh.clusters = m_device.create_buffer(desc, bytes_of(table));
    if (!mesh.vertices.valid() || !mesh.indices.valid() || !mesh.clusters.valid()) {
        destroy(mesh);
        log_error("render", "could not upload a mesh (out of GPU memory?)");
        return k_no_mesh;
    }
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

void Renderer::ensure_depth(u32 width, u32 height) {
    if (m_depth.valid() && m_depth_width == width && m_depth_height == height) {
        return;
    }
    m_device.destroy(m_depth);
    rhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = rhi::Format::depth32_float;
    desc.usage = rhi::TextureUsage::depth_target;
    desc.name = "scene depth";
    m_depth = m_device.create_texture(desc);
    m_depth_width = width;
    m_depth_height = height;
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
    ensure_depth(width, height);

    // Frame constants.
    const Lighting& light = frame.lighting;
    FrameConstants constants{};
    const Mat4 view_proj =
        view_projection(frame.view, static_cast<float>(width) / static_cast<float>(height));
    std::memcpy(constants.view_proj, view_proj.m, sizeof(constants.view_proj));
    set4(constants.sun_direction, normalize_or_zero(light.sun_direction), 0.0f);
    set4(constants.sun_light, light.sun_light, 0.0f);
    set4(constants.sky_light, light.sky_light, 0.0f);
    set4(constants.ground_light, light.ground_light, 0.0f);
    constants.params[0] = light.exposure;
    constants.params[1] = static_cast<float>(static_cast<u32>(frame.settings.debug_view));
    const rhi::TransientMemory frame_memory =
        cmd.upload({reinterpret_cast<const u8*>(&constants), sizeof(constants)});

    // Instances by mesh, so each mesh is one draw over all its chosen clusters.
    m_order.clear();
    for (u32 i = 0; i < frame.instances.size(); ++i) {
        if (mesh(frame.instances[i].mesh) != nullptr) {
            m_order.push_back(i);
        }
    }
    std::stable_sort(m_order.begin(), m_order.end(), [&frame](u32 a, u32 b) {
        return frame.instances[a].mesh < frame.instances[b].mesh;
    });

    // Choose each instance's clusters: about a pixel of error, in view.
    const float fov = degrees_to_radians(std::clamp(frame.view.vertical_fov_degrees, 1.0f, 170.0f));
    const geometry::Frustum frustum = geometry::frustum_from_view_projection(view_proj);
    geometry::LodView lod;
    lod.pixels_per_unit = 0.5f / std::tan(fov * 0.5f) * static_cast<float>(height);
    lod.near_plane = std::max(frame.view.near_plane, 1e-4f);
    lod.threshold_pixels = std::clamp(frame.settings.lod_error_pixels, 0.01f, 1000.0f);
    lod.frustum = &frustum;
    m_drawn.clear();
    m_entries.clear();
    for (const u32 i : m_order) {
        const Instance& in = frame.instances[i];
        const geometry::ClusterMesh& data = m_meshes[in.mesh].data;
        lod.position = to_camera_relative(in.position, frame.view.position);
        lod.rotation = in.rotation;
        lod.scale = in.scale;
        m_selected.clear();
        geometry::select_clusters(data, lod, m_selected);
        if (m_selected.empty()) {
            continue;
        }
        const u32 slot = static_cast<u32>(m_drawn.size());
        m_drawn.push_back(i);
        for (const u32 c : m_selected) {
            m_entries.push_back({slot, c});
            m_stats.triangles += data.clusters[c].triangle_count;
        }
    }

    rhi::TransientMemory instance_memory;
    rhi::TransientMemory entry_memory;
    if (!m_entries.empty()) {
        instance_memory = cmd.allocate(static_cast<u32>(m_drawn.size() * sizeof(GpuInstance)));
        entry_memory = cmd.allocate(static_cast<u32>(m_entries.size() * sizeof(Entry)));
        if (instance_memory.data == nullptr || entry_memory.data == nullptr) {
            m_entries.clear(); // out of memory: the device is lost; draw nothing
        } else {
            for (usize i = 0; i < m_drawn.size(); ++i) {
                const GpuInstance gpu = to_gpu(frame.instances[m_drawn[i]], frame.view.position);
                std::memcpy(instance_memory.data + i * sizeof(GpuInstance), &gpu, sizeof(gpu));
            }
            std::memcpy(entry_memory.data, m_entries.data(), m_entries.size() * sizeof(Entry));
        }
    }

    // Draws: runs of entries whose instances share a mesh (entries follow
    // m_drawn, which follows mesh order).
    struct Draw {
        MeshId mesh;
        u32 first_entry;
        u32 entry_count;
    };
    std::vector<Draw> draws;
    for (u32 e = 0; e < m_entries.size(); ++e) {
        const MeshId id = frame.instances[m_drawn[m_entries[e].instance]].mesh;
        if (draws.empty() || draws.back().mesh != id) {
            draws.push_back({id, e, 0});
        }
        ++draws.back().entry_count;
    }
    for (const Draw& d : draws) {
        const Mesh& mesh = m_meshes[d.mesh];
        cmd.barrier(mesh.vertices, rhi::ResourceState::shader_read);
        cmd.barrier(mesh.indices, rhi::ResourceState::shader_read);
        cmd.barrier(mesh.clusters, rhi::ResourceState::shader_read);
    }

    rhi::RenderPassDesc pass;
    const Vec3& bg = light.background;
    pass.colors[0] = {target, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
    pass.depth = {m_depth, rhi::LoadOp::clear, 0.0f};
    cmd.begin_render_pass(pass);
    if (!draws.empty()) {
        cmd.set_pipeline(m_pipeline);
        for (const Draw& d : draws) {
            const Mesh& mesh = m_meshes[d.mesh];
            ClusterPush push{};
            push.frame = frame_memory.descriptor;
            push.frame_offset = frame_memory.offset;
            push.instances = instance_memory.descriptor;
            push.instance_offset = instance_memory.offset;
            push.entries = entry_memory.descriptor;
            push.entry_offset = entry_memory.offset + d.first_entry * static_cast<u32>(sizeof(Entry));
            push.vertices = m_device.read_descriptor(mesh.vertices);
            push.indices = m_device.read_descriptor(mesh.indices);
            push.clusters = m_device.read_descriptor(mesh.clusters);
            cmd.push(push);
            cmd.draw(k_vertices_per_cluster, d.entry_count);
            ++m_stats.draws;
        }
        m_stats.instances = static_cast<u32>(m_drawn.size());
        m_stats.clusters = static_cast<u32>(m_entries.size());
    } else {
        m_stats.triangles = 0;
    }
    cmd.end_render_pass();
}

} // namespace giant::render
