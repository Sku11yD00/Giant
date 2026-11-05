// The renderer: turns plain per-frame data (a camera, lighting and a list of
// instances) into a lit image. It never sees the world or the editor, so it
// can be swapped or tested on its own (docs/architecture.md).
//
// Every mesh is cluster LOD (geometry/clusters.h). Each frame the GPU picks,
// per instance, the clusters whose detail is just under a pixel of error,
// inside the view and not hidden behind what is already drawn (two-pass
// occlusion culling against a depth pyramid), and draws them with mesh
// shaders (or a vertex-shader fallback) into a visibility buffer: per pixel,
// which triangle covers it. One full-screen pass then shades every pixel
// once: a sun with GGX specular and sky/ground ambient, tone mapped.
//
// The CPU can pick the clusters instead (RenderSettings::gpu_culling off):
// the same cut, for reference and tests, on any backend.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/rhi/rhi.h"

#include <memory>
#include <span>
#include <vector>

namespace giant::render {

// A mesh loaded into the renderer: a built-in shape or one added with
// add_mesh.
using MeshId = u32;
inline constexpr MeshId k_no_mesh = ~0u;

enum class BuiltinMesh {
    cube,   // 1 m, centred
    sphere, // 1 m wide, centred
    plane,  // 1 m square at y = 0, facing +Y
};

struct Instance {
    DVec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    MeshId mesh = 0;
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 emissive{}; // linear
};

struct Lighting {
    Vec3 sun_direction{0.0f, 1.0f, 0.0f}; // unit, towards the sun
    Vec3 sun_light{3.0f, 3.0f, 3.0f};     // colour times intensity
    Vec3 sky_light{0.2f, 0.3f, 0.5f};     // ambient from above, times intensity
    Vec3 ground_light{0.1f, 0.1f, 0.1f};  // ambient from below, times intensity
    Vec3 background{0.33f, 0.52f, 0.85f}; // linear, where nothing is drawn
    float exposure = 1.0f;
};

struct View {
    DVec3 position{};
    Quat orientation{}; // looks down -Z, +Y up, before rotation
    float vertical_fov_degrees = 60.0f;
    float near_plane = 0.1f; // metres; there is no far plane (reverse-Z)
};

// What the image shows: the lit scene, or how the geometry is built.
enum class DebugView : u32 {
    lit,
    clusters,   // each cluster in its own colour
    lod_levels, // colour by level: 0 (original triangles) is red, then yellow, green, cyan...
    triangles,  // each triangle in its own colour
};

struct RenderSettings {
    // How much simplification may show: a cluster is used once its error
    // covers at most this many pixels. Lower is sharper and slower.
    float lod_error_pixels = 1.0f;
    DebugView debug_view = DebugView::lit;
    // Clusters are picked on the GPU (off: on the CPU, which also gives
    // exact, same-frame stats; the GPU's arrive a frame or two later).
    bool gpu_culling = true;
    // GPU culling skips what is hidden behind nearer geometry.
    bool occlusion_culling = true;
    // Draw with mesh shaders where the GPU has them (off: the vertex
    // shader fallback, as on GPUs without them).
    bool mesh_shaders = true;
};

struct FrameInput {
    View view;
    Lighting lighting;
    RenderSettings settings;
    std::span<const Instance> instances;
};

struct RenderStats {
    u32 instances = 0; // with at least one cluster drawn (GPU culling: passing instance culling)
    u32 draws = 0;     // draw calls this frame (raster passes and shading)
    u32 clusters = 0;
    u32 triangles = 0;
    // GPU culling: the counts above come from the GPU, from a frame or two
    // ago (they are read back once the GPU is done with that frame).
    bool from_gpu = false;
    u32 occluded_instances = 0; // hidden in the first pass (GPU, occlusion on)
    u32 occluded_clusters = 0;
    bool overflow = false; // a GPU list was full: some geometry was dropped
};

class Renderer {
    struct ConstructKey {
        explicit ConstructKey() = default;
    };

public:
    // Null (and logs why) if a pipeline cannot be created.
    static std::unique_ptr<Renderer> create(rhi::Device& device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId builtin(BuiltinMesh mesh) const;

    // Uploads a mesh; k_no_mesh if it is empty or the GPU is out of memory.
    // Ids are not reused, so a removed id stops drawing instead of drawing
    // whatever comes next. Safe mid-frame.
    MeshId add_mesh(geometry::ClusterMesh mesh);
    // Built-in meshes stay. Unknown ids are ignored.
    void remove_mesh(MeshId mesh);
    // Null for an unknown or removed id.
    const geometry::ClusterMesh* mesh(MeshId mesh) const;

    // Records the frame into `target`: an rgba8_srgb render target (the
    // swap chain's back buffer or an offscreen image). Clears it to the
    // background first. Instances with an unknown mesh are skipped.
    void render(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameInput& frame);

    const RenderStats& last_stats() const { return m_stats; }
    // GPU culling: reads the newest GPU counts now. Only after
    // Device::wait_idle (tests, screenshots), outside a frame.
    void read_gpu_stats();

    // The matrix that takes camera-relative positions to clip space.
    static Mat4 view_projection(const View& view, float aspect);

    // Use create(); public only for make_unique.
    Renderer(ConstructKey, rhi::Device& device);

private:
    struct Mesh {
        geometry::ClusterMesh data; // the CPU copy (CPU culling, stats, editor)
        rhi::BufferHandle vertices;   // 24-byte vertices
        rhi::BufferHandle vertex_ids; // u32 per cluster vertex, into vertices
        rhi::BufferHandle triangles;  // u32 per cluster triangle: 3 local indices
        rhi::BufferHandle clusters;   // gpu::Cluster
        rhi::BufferHandle groups;     // gpu::Group
        rhi::BufferHandle nodes;      // gpu::Node
        u32 tree_levels = 0;          // deepest node, plus one
        bool live = false;
    };
    // Buffers the GPU culling passes work in, made on first use.
    struct CullBuffers {
        rhi::BufferHandle counters;
        rhi::BufferHandle args;
        rhi::BufferHandle queues[2];
        rhi::BufferHandle visible;
        rhi::BufferHandle occluded_clusters;
        rhi::BufferHandle occluded_instances;
        u32 instance_capacity = 0;
        // Counters copied back each frame, tagged with the device frame.
        rhi::BufferHandle readback[3];
        u64 readback_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    };
    struct FrameData; // this frame's uploads and descriptors (renderer.cpp)

    bool init();
    bool ensure_targets(u32 width, u32 height);
    bool ensure_cull_buffers(u32 instance_count);
    void destroy(Mesh& mesh);
    void select_on_cpu(const FrameInput& frame, const Mat4& view_proj, u32 height);
    void cull_on_gpu(rhi::CommandList& cmd, const FrameData& data);
    void raster(rhi::CommandList& cmd, const FrameData& data, u32 pass, bool gpu);
    void build_hzb(rhi::CommandList& cmd);
    void read_stats_slot(u32 slot);

    rhi::Device& m_device;
    // Pipelines.
    rhi::PipelineHandle m_raster_mesh;   // invalid without mesh shaders
    rhi::PipelineHandle m_raster_vertex;
    rhi::PipelineHandle m_resolve;
    rhi::PipelineHandle m_cull_reset;
    rhi::PipelineHandle m_cull_prepare;
    rhi::PipelineHandle m_cull_instances;
    rhi::PipelineHandle m_cull_traverse;
    rhi::PipelineHandle m_cull_retest;
    rhi::PipelineHandle m_hzb_first;
    rhi::PipelineHandle m_hzb_down;
    // Targets, sized to the frame.
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_vis; // visibility buffer (rg32_uint)
    rhi::BufferHandle m_hzb;  // depth pyramid
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_hzb_width = 0;
    u32 m_hzb_height = 0;
    u32 m_hzb_mips = 0;
    // What last frame's HZB was built from, for the first occlusion pass.
    bool m_hzb_valid = false;
    DVec3 m_hzb_camera{};
    float m_hzb_view_rows[12] = {};
    float m_hzb_p00 = 0.0f;
    float m_hzb_p11 = 0.0f;
    CullBuffers m_cull;
    bool m_overflow_logged = false;

    std::vector<Mesh> m_meshes;
    std::vector<MeshId> m_builtin; // by BuiltinMesh
    // Per-frame scratch, kept to avoid allocating every frame.
    std::vector<u32> m_valid;    // instances (into frame.instances) with a live mesh
    std::vector<u32> m_selected; // CPU culling: clusters of one instance
    std::vector<u32> m_entries;  // CPU culling: (instance, cluster) pairs, flattened
    RenderStats m_stats;
    RenderStats m_gpu_stats; // newest read back
};

} // namespace giant::render
