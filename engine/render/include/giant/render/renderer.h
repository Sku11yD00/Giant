// The renderer: turns plain per-frame data (a camera, lighting and a list of
// instances) into a lit image. It never sees the world or the editor, so it
// can be swapped or tested on its own (docs/architecture.md).
//
// N1 part 1 is a forward renderer: each mesh is one instanced draw with
// vertex pulling, lit by a sun with GGX specular and sky/ground ambient,
// then tone mapped. Later parts replace the inside (clusters, GPU culling,
// a visibility buffer) behind the same interface.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/rhi/rhi.h"

#include <memory>
#include <span>
#include <vector>

namespace giant::render {

// A mesh loaded into the renderer. Built-in shapes for now; imported
// meshes arrive with N1 part 2.
using MeshId = u32;

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

struct FrameInput {
    View view;
    Lighting lighting;
    std::span<const Instance> instances;
};

struct RenderStats {
    u32 instances = 0; // drawn this frame
    u32 draws = 0;
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
        u32 first_index = 0;
        u32 index_count = 0;
        i32 base_vertex = 0;
    };
    bool init();
    void ensure_depth(u32 width, u32 height);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::BufferHandle m_vertices; // every mesh's vertices, pulled by the shader
    rhi::BufferHandle m_indices;
    rhi::TextureHandle m_depth;
    u32 m_depth_width = 0;
    u32 m_depth_height = 0;
    std::vector<Mesh> m_meshes;
    std::vector<MeshId> m_builtin; // by BuiltinMesh
    std::vector<u32> m_order;      // per-frame scratch: instances sorted by mesh
    RenderStats m_stats;
};

} // namespace giant::render
