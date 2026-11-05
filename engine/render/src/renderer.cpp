#include "giant/render/renderer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/render/meshes.h"
#include "render_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace giant::render {

namespace {

// Matches FrameConstants in shaders/mesh.slang.
struct FrameConstants {
    float view_proj[16];
    float sun_direction[4];
    float sun_light[4];
    float sky_light[4];
    float ground_light[4];
    float params[4];
};
static_assert(sizeof(FrameConstants) == 144);

// Matches the 128-byte instance layout in shaders/mesh.slang.
struct GpuInstance {
    float model[12];  // rows of the 3x4 model matrix (camera-relative)
    float normal[12]; // rows of the normal matrix, w unused
    float base_roughness[4];
    float emissive_metallic[4];
};
static_assert(sizeof(GpuInstance) == 128);

struct MeshPush {
    u32 frame;
    u32 frame_offset;
    u32 instances;
    u32 instance_offset;
    u32 vertices;
    u32 pad[3];
};
static_assert(sizeof(MeshPush) == 32);

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
    m_device.destroy(m_vertices);
    m_device.destroy(m_indices);
    m_device.destroy(m_depth);
}

bool Renderer::init() {
    rhi::GraphicsPipelineDesc desc;
    desc.vertex = shaders::mesh_vs();
    desc.pixel = shaders::mesh_ps();
    desc.color_formats[0] = rhi::Format::rgba8_srgb;
    desc.depth_format = rhi::Format::depth32_float;
    desc.depth = rhi::DepthMode::test_write;
    desc.cull = rhi::CullMode::back;
    desc.name = "mesh (forward)";
    m_pipeline = m_device.create_pipeline(desc);
    if (!m_pipeline.valid()) {
        log_error("render", "could not create the mesh pipeline");
        return false;
    }

    // All built-in meshes share one vertex and one index buffer.
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
    const MeshData shapes[] = {make_cube(), make_sphere(), make_plane()};
    for (const MeshData& data : shapes) {
        Mesh mesh;
        mesh.first_index = static_cast<u32>(indices.size());
        mesh.index_count = static_cast<u32>(data.indices.size());
        mesh.base_vertex = static_cast<i32>(vertices.size());
        vertices.insert(vertices.end(), data.vertices.begin(), data.vertices.end());
        indices.insert(indices.end(), data.indices.begin(), data.indices.end());
        m_builtin.push_back(static_cast<MeshId>(m_meshes.size()));
        m_meshes.push_back(mesh);
    }
    rhi::BufferDesc vb;
    vb.size = vertices.size() * sizeof(MeshVertex);
    vb.usage = rhi::BufferUsage::shader_read;
    vb.name = "mesh vertices";
    m_vertices = m_device.create_buffer(
        vb, {reinterpret_cast<const u8*>(vertices.data()), static_cast<usize>(vb.size)});
    rhi::BufferDesc ib;
    ib.size = indices.size() * sizeof(u32);
    ib.usage = rhi::BufferUsage::index;
    ib.name = "mesh indices";
    m_indices = m_device.create_buffer(
        ib, {reinterpret_cast<const u8*>(indices.data()), static_cast<usize>(ib.size)});
    if (!m_vertices.valid() || !m_indices.valid()) {
        log_error("render", "could not create the mesh buffers");
        return false;
    }
    return true;
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
    const rhi::TransientMemory frame_memory =
        cmd.upload({reinterpret_cast<const u8*>(&constants), sizeof(constants)});

    // Instances, sorted by mesh so each mesh is one instanced draw.
    m_order.clear();
    for (u32 i = 0; i < frame.instances.size(); ++i) {
        if (frame.instances[i].mesh < m_meshes.size()) {
            m_order.push_back(i);
        }
    }
    std::stable_sort(m_order.begin(), m_order.end(), [&frame](u32 a, u32 b) {
        return frame.instances[a].mesh < frame.instances[b].mesh;
    });
    rhi::TransientMemory instance_memory;
    if (!m_order.empty()) {
        instance_memory = cmd.allocate(static_cast<u32>(m_order.size() * sizeof(GpuInstance)));
        if (instance_memory.data == nullptr) {
            m_order.clear(); // out of memory: the device is lost; draw nothing
        }
        for (usize i = 0; i < m_order.size(); ++i) {
            const GpuInstance gpu = to_gpu(frame.instances[m_order[i]], frame.view.position);
            std::memcpy(instance_memory.data + i * sizeof(GpuInstance), &gpu, sizeof(gpu));
        }
    }

    cmd.barrier(m_indices, rhi::ResourceState::index_buffer);
    cmd.barrier(m_vertices, rhi::ResourceState::shader_read);
    rhi::RenderPassDesc pass;
    const Vec3& bg = light.background;
    pass.colors[0] = {target, rhi::LoadOp::clear, {bg.x, bg.y, bg.z, 1.0f}};
    pass.depth = {m_depth, rhi::LoadOp::clear, 0.0f};
    cmd.begin_render_pass(pass);
    if (!m_order.empty()) {
        cmd.set_pipeline(m_pipeline);
        cmd.set_index_buffer(m_indices);
        usize begin = 0;
        while (begin < m_order.size()) {
            const MeshId mesh_id = frame.instances[m_order[begin]].mesh;
            usize end = begin;
            while (end < m_order.size() && frame.instances[m_order[end]].mesh == mesh_id) {
                ++end;
            }
            const Mesh& mesh = m_meshes[mesh_id];
            MeshPush push{};
            push.frame = frame_memory.descriptor;
            push.frame_offset = frame_memory.offset;
            push.instances = instance_memory.descriptor;
            push.instance_offset =
                instance_memory.offset + static_cast<u32>(begin * sizeof(GpuInstance));
            push.vertices = m_device.read_descriptor(m_vertices);
            cmd.push(push);
            cmd.draw_indexed(mesh.index_count, static_cast<u32>(end - begin), mesh.first_index,
                             mesh.base_vertex);
            ++m_stats.draws;
            begin = end;
        }
        m_stats.instances = static_cast<u32>(m_order.size());
    }
    cmd.end_render_pass();
}

} // namespace giant::render
