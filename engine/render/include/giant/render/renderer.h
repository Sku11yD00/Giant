// The renderer: turns plain per-frame data (a camera, lighting and a list of
// instances) into a lit image. It never sees the world or the editor, so it
// can be swapped or tested on its own (docs/architecture.md).
//
// Every mesh is cluster LOD (geometry/clusters.h). Each frame the CPU picks,
// per instance, the clusters whose detail is just under a pixel of error and
// inside the view, and each mesh is one draw over its chosen clusters
// (vertex pulling, 128 triangles per cluster), forward lit by a sun with
// GGX specular and sky/ground ambient, then tone mapped. Part 3 moves the
// choice to the GPU (culling, mesh shaders, a visibility buffer) behind the
// same interface.
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
};

struct FrameInput {
    View view;
    Lighting lighting;
    RenderSettings settings;
    std::span<const Instance> instances;
};

struct RenderStats {
    u32 instances = 0; // with at least one cluster drawn this frame
    u32 draws = 0;
    u32 clusters = 0;
    u32 triangles = 0;
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

    // The matrix that takes camera-relative positions to clip space.
    static Mat4 view_projection(const View& view, float aspect);

    // Use create(); public only for make_unique.
    Renderer(ConstructKey, rhi::Device& device);

private:
    struct Mesh {
        geometry::ClusterMesh data; // the CPU copy picks the clusters
        rhi::BufferHandle vertices; // 24-byte vertices
        rhi::BufferHandle indices;  // u32, three per triangle, per cluster
        rhi::BufferHandle clusters; // 16 bytes each: first index, triangles, level
        bool live = false;
    };
    // One chosen cluster of one instance, as the shader reads it.
    struct Entry {
        u32 instance; // into this frame's instance list
        u32 cluster;
    };
    bool init();
    void ensure_depth(u32 width, u32 height);
    void destroy(Mesh& mesh);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::TextureHandle m_depth;
    u32 m_depth_width = 0;
    u32 m_depth_height = 0;
    std::vector<Mesh> m_meshes;
    std::vector<MeshId> m_builtin; // by BuiltinMesh
    // Per-frame scratch, kept to avoid allocating every frame.
    std::vector<u32> m_order;      // instances sorted by mesh
    std::vector<u32> m_selected;   // clusters of one instance
    std::vector<u32> m_drawn;      // instances (into frame.instances) with clusters
    std::vector<Entry> m_entries;  // grouped by mesh
    RenderStats m_stats;
};

} // namespace giant::render
