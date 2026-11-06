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
// once: decals stamped on (binned into screen tiles on the CPU), then a sun
// with GGX specular and sky/ground ambient, tone mapped. Draw calls do not
// grow with instances, meshes or materials: every instance of every mesh
// goes through the same few passes.
//
// Sun shadows are virtual shadow maps (src/shadow_pages.h): a clipmap of
// pages seen from the sun, drawn from the same clusters with the same
// culling, only where shading asked for them, cached until what casts onto
// them changes, a budgeted number of pages a frame.
//
// The CPU can pick the clusters instead (RenderSettings::gpu_culling off):
// the same cut, for reference and tests, on any backend.
//
// Geometry streams: a fixed pool of GPU memory holds the cluster groups the
// view needs, loaded as culling asks for them and evicted when unused
// (src/streaming.h). Until finer detail arrives, coarser clusters stand in.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/rhi/rhi.h"
#include "giant/voxel/clipmap.h"

#include <memory>
#include <span>
#include <vector>

namespace giant::render {

namespace gpu {
struct MeshEntry; // src/gpu_layout.h
struct Material;
struct VoxelLevel;
struct ShadowLevel;
struct FrameConstants;
struct CullPush;
}
class GeometryStreamer; // src/streaming.h
class VoxelScene;       // src/voxel_scene.h
class ShadowPages;      // src/shadow_pages.h
struct DecalBins;       // src/decals.h

// A mesh loaded into the renderer: a built-in shape or one added with
// add_mesh.
using MeshId = u32;
inline constexpr MeshId k_no_mesh = ~0u;

enum class BuiltinMesh {
    cube,   // 1 m, centred
    sphere, // 1 m wide, centred
    plane,  // 1 m square at y = 0, facing +Y
};

// How a surface looks (metal/roughness PBR). Instances share them by index
// into FrameInput::materials, so a kit used a thousand times sends its few
// materials once.
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 emissive{}; // linear
};

struct Instance {
    DVec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    MeshId mesh = 0;
    // Into FrameInput::materials; out of range draws the default Material.
    u32 material = 0;
};

// The mask a decal stamps (shapes for now; image stamps arrive with textures).
enum class DecalShape : u32 {
    disc,      // round, soft-edged
    rectangle, // the whole box face
    ring,
    splat,     // an irregular blot (seeded): mud, blood, scorch
    cracks,    // cracked ground (seeded): impacts, giant footfalls
};

// A box that stamps a material onto whatever surfaces lie inside it, seen
// from its local +Y (it projects downwards, along -Y). Later decals in the
// list draw over earlier ones.
struct Decal {
    DVec3 position{};
    Quat rotation{};
    Vec3 size{1.0f, 1.0f, 1.0f}; // the box, metres: x and z the stamp, y its depth
    Vec3 base_color{0.1f, 0.1f, 0.1f};
    float opacity = 1.0f;
    float roughness = 0.8f;
    float metallic = 0.0f;
    Vec3 emissive{};
    DecalShape shape = DecalShape::disc;
    float softness = 0.1f; // edge fade, as a fraction of the stamp
    u32 seed = 0;          // varies splat and cracks
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
    voxels,     // the voxel copy of the scene (RenderSettings::voxel_world), lit simply
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
    // Smooths jagged geometry edges from the finished image (SMAA 1x): no
    // history, so the image stays as sharp moving as standing still.
    bool antialiasing = true;
    // Widens shiny highlights where the surface curves faster than a pixel
    // can show, so they do not sparkle or crawl.
    bool specular_antialiasing = true;
    // Keeps the voxel copy of the scene around the camera up to date (the
    // shared voxel world: far geometry and lighting build on it). Off, it
    // stays as it is.
    bool voxel_world = true;
    // CPU time per frame for building voxel bricks (at least one is built
    // when any wait); the rest wait for the next frame.
    float voxel_ms_per_frame = 1.0f;
    // The far field: past far_field_distance (metres) the voxel world
    // stands in for small instances (bounding radius at most
    // far_field_max_radius metres), which are not drawn as meshes at all.
    // Larger ones (terrain, mountains, big buildings) stay meshes; the
    // voxels show in front of them. Lit view only, and only with
    // voxel_world on (a frozen voxel world would show stale far geometry).
    bool voxel_far_field = true;
    float far_field_distance = 3000.0f;
    float far_field_max_radius = 50.0f;
    // Sun shadows (lit view, GPU culling only). Pages are drawn when shading
    // first asks for them and again when what casts onto them changes; at
    // most shadow_pages_per_frame a frame (at least one when any wait),
    // the rest keep their old content (or fall back to coarser pages).
    bool shadows = true;
    u32 shadow_pages_per_frame = 64;
};

struct FrameInput {
    View view;
    Lighting lighting;
    RenderSettings settings;
    std::span<const Instance> instances;
    std::span<const Material> materials;
    std::span<const Decal> decals;
};

struct RenderStats {
    u32 instances = 0; // with at least one cluster drawn (GPU culling: passing instance culling)
    u32 draws = 0;     // draw calls this frame (raster passes and shading)
    u32 post_draws = 0; // full-screen passes after shading (anti-aliasing): fixed, not per object
    u32 clusters = 0;
    u32 triangles = 0;
    // GPU culling: the counts above come from the GPU, from a frame or two
    // ago (they are read back once the GPU is done with that frame).
    bool from_gpu = false;
    u32 occluded_instances = 0; // hidden in the first pass (GPU, occlusion on)
    u32 occluded_clusters = 0;
    bool overflow = false; // a list was full: some geometry was dropped
    u32 materials = 0;     // in the frame's material table
    u32 decals = 0;        // in view this frame (on the CPU: never late)
    bool decal_overflow = false; // too many decals on screen: some were not drawn
    // Streaming (this frame, on the CPU: never late).
    u64 pool_bytes = 0;      // GPU memory for cluster geometry
    u64 pool_used_bytes = 0;
    u32 resident_groups = 0; // cluster groups loaded, of every mesh
    u32 total_groups = 0;
    u32 streamed_in = 0;     // groups loaded this frame
    u32 evicted = 0;         // groups dropped this frame to make room
    u32 waiting = 0;         // groups asked for but not loaded yet
    // The voxel world (this frame, on the CPU).
    u32 voxel_bricks = 0;         // in the GPU pool
    u32 voxel_capacity = 0;       // the pool's size, bricks
    u32 voxel_pending = 0;        // waiting to be built
    u32 voxel_built = 0;          // built this frame
    u32 voxel_missing = 0;        // bricks that did not fit in the pool (built as room frees up)
    bool voxel_overflow = false;  // voxel_missing > 0: the pool is full
    float voxel_cpu_ms = 0.0f;    // voxel work this frame
    // The far field (this frame, on the CPU).
    bool far_field = false; // on this frame
    u32 far_culled = 0;     // instances left to the voxel world
    // Sun shadows (this frame, on the CPU; requests are a few frames old).
    bool shadows = false;     // on this frame
    u32 shadow_pages = 0;     // pages with room in the pool
    u32 shadow_capacity = 0;  // the pool, pages
    u32 shadow_drawn = 0;     // pages drawn this frame
    u32 shadow_waiting = 0;   // pages asked for or changed, not drawn yet
    u32 shadow_missing = 0;   // pages asked for that found no room
    u32 shadow_draws = 0;     // raster passes for them (one per level with pages to draw)
};

// What the voxel world holds at a point, on one clipmap level
// (Renderer::probe_voxels): "outside" its window, "building" (not built
// yet: the next coarser level stands in), "empty" or "solid".
struct VoxelProbeLevel {
    u32 level = 0;
    double voxel_size = 0.0; // metres
    const char* state = "outside";
};

struct RendererDesc {
    // GPU memory for streamed cluster geometry. Every mesh's coarsest level
    // must fit; more keeps more detail around.
    u64 geometry_pool_bytes = 256ull << 20;
    // At most this much is loaded per frame (the rest waits a frame).
    u64 stream_bytes_per_frame = 32ull << 20;
    // The voxel world: its levels, and GPU memory for its bricks (2176
    // bytes each; the default is 68 MiB).
    voxel::ClipmapDesc voxel_clipmap{};
    u32 voxel_bricks = 32768;
    // Sun shadow pages of 128 x 128 depths (64 KiB each; the default is
    // 64 MiB), made on the first frame with shadows.
    u32 shadow_pages = 1024;
};

class Renderer {
    struct ConstructKey {
        explicit ConstructKey() = default;
    };

public:
    // Null (and logs why) if a pipeline cannot be created.
    static std::unique_ptr<Renderer> create(rhi::Device& device, const RendererDesc& desc = {});
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
    // The voxel world at a world position, finest level first.
    std::vector<VoxelProbeLevel> probe_voxels(const DVec3& position) const;
    // The sun's shadow pages (src/shadow_pages.h; tests and tools).
    ShadowPages& shadow_pages() { return *m_shadows; }
    // GPU culling: reads the newest GPU counts now. Only after
    // Device::wait_idle (tests, screenshots), outside a frame.
    void read_gpu_stats();

    // The matrix that takes camera-relative positions to clip space.
    static Mat4 view_projection(const View& view, float aspect);

    // Use create(); public only for make_unique.
    Renderer(ConstructKey, rhi::Device& device, const RendererDesc& desc);

private:
    struct Mesh {
        geometry::ClusterMesh data; // the CPU copy (CPU culling, stats, editor)
        rhi::BufferHandle clusters;   // gpu::Cluster (geometry streams: GeometryStreamer)
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
    bool ensure_cull_buffers(CullBuffers& c, u32 instance_count);
    void destroy(Mesh& mesh);
    void draw_voxels(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameData& data,
                     const Vec3& background); // the voxel debug view, in place of shading
    void select_on_cpu(const FrameInput& frame, const Mat4& view_proj, u32 height, u32 frame_number);
    void cull_on_gpu(rhi::CommandList& cmd, const FrameData& data);
    // GPU culling's steps, into `c` (the view's lists, or the shadow pages').
    gpu::CullPush cull_push(const CullBuffers& c, const FrameData& data, const rhi::TransientMemory& frame,
                            bool streaming_requests);
    // Reset, the instances, the trees and the raster arguments of pass 0.
    void cull_first_pass(rhi::CommandList& cmd, CullBuffers& c, const FrameData& data, const gpu::CullPush& base);
    void cull_barriers(rhi::CommandList& cmd, const CullBuffers& c, bool with_args);
    void cull_prepare(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base, u32 pass, u32 mode,
                      u32 slot_in, u32 capacity, u32 arg_offset);
    void cull_walk(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base, const FrameData& data,
                   u32 pass);
    bool ensure_shadow_buffers(u32 instance_count);
    // Sun shadows: which pages this frame, then drawing them.
    void plan_shadows(const FrameInput& frame, bool on);
    // Uploads the page table and levels into `data` (shading reads them).
    void draw_shadows(rhi::CommandList& cmd, FrameData& data, const FrameInput& frame,
                      const gpu::FrameConstants& base);
    void raster(rhi::CommandList& cmd, const FrameData& data, u32 pass, bool gpu);
    void build_hzb(rhi::CommandList& cmd);
    void smaa(rhi::CommandList& cmd, rhi::TextureHandle target, const Vec3& background); // after shading, into target
    void read_stats_slot(u32 slot);
    void copy_streaming_stats(); // and the voxel world's
    void finish_stats(const FrameData& data);

    rhi::Device& m_device;
    RendererDesc m_desc;
    std::unique_ptr<GeometryStreamer> m_streamer;
    std::unique_ptr<DecalBins> m_decal_bins; // this frame's, reused
    std::unique_ptr<VoxelScene> m_voxels;
    u64 m_meshes_version = 0; // changes on every add_mesh and remove_mesh
    u32 m_request_count = 0; // GPU streaming requests in the slot read last
    u64 m_requests_taken = ~u64{0}; // the frame whose requests were taken last
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
    rhi::PipelineHandle m_smaa_edges;
    rhi::PipelineHandle m_smaa_weights;
    rhi::PipelineHandle m_smaa_blend;
    rhi::PipelineHandle m_voxel_view;
    rhi::TextureHandle m_smaa_area;   // SMAA's precomputed tables
    rhi::TextureHandle m_smaa_search;
    // Targets, sized to the frame.
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_vis; // visibility buffer (rg32_uint)
    rhi::BufferHandle m_hzb;  // depth pyramid
    rhi::TextureHandle m_color;   // shaded image before anti-aliasing (rgba8_srgb)
    rhi::TextureHandle m_edges;   // SMAA edges (rgba8_unorm)
    rhi::TextureHandle m_weights; // SMAA blend weights (rgba8_unorm)
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
    float m_hzb_near = 0.0f;
    CullBuffers m_cull;
    // Sun shadows.
    std::unique_ptr<ShadowPages> m_shadows;
    CullBuffers m_shadow_cull; // its own lists: pages are drawn after the view's raster
    rhi::BufferHandle m_shadow_atlas;    // u32 depths, page after page
    rhi::BufferHandle m_shadow_requests; // a bit per page slot, written by shading
    rhi::BufferHandle m_shadow_readback[3];
    u64 m_shadow_readback_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    u64 m_shadow_requests_taken = ~u64{0};
    bool m_shadows_on = false; // this frame
    bool m_shadows_were_on = false;
    bool m_shadow_buffers_logged = false;
    u32 m_shadow_draws = 0; // this frame
    rhi::PipelineHandle m_shadow_mesh; // invalid without mesh shaders
    rhi::PipelineHandle m_shadow_vertex;
    rhi::PipelineHandle m_shadow_clear_pages;
    rhi::PipelineHandle m_shadow_clear_words;
    std::vector<u8> m_casts; // per frame instance: drawn as a mesh, so it casts
    std::vector<u32> m_shadow_requests_cpu;
    std::vector<u32> m_shadow_table;
    std::vector<gpu::ShadowLevel> m_shadow_levels;
    bool m_overflow_logged = false;
    bool m_cull_buffers_logged = false;
    bool m_instances_logged = false;
    bool m_cpu_clusters_logged = false;
    bool m_materials_logged = false;
    bool m_decals_logged = false;

    std::vector<Mesh> m_meshes;
    std::vector<MeshId> m_builtin; // by BuiltinMesh
    // Per-frame scratch, kept to avoid allocating every frame.
    std::vector<u32> m_valid;    // instances (into frame.instances) with a live mesh, drawn as meshes
    bool m_far_field = false;    // this frame
    std::vector<gpu::VoxelLevel> m_voxel_levels; // this frame's, reused
    u32 m_far_culled = 0;        // this frame: instances left to the voxel world
    std::vector<u32> m_selected; // CPU culling: clusters of one instance
    std::vector<u32> m_entries;  // CPU culling: (instance, cluster) pairs, flattened
    std::vector<u32> m_wanted;   // CPU culling: groups of one instance not loaded
    std::vector<u32> m_touched;  // CPU culling: loaded groups it used
    std::vector<gpu::MeshEntry> m_table; // the mesh table
    std::vector<gpu::Material> m_gpu_materials;
    std::vector<bool> m_used;            // meshes drawn this frame
    RenderStats m_stats;
    RenderStats m_gpu_stats; // newest read back
};

} // namespace giant::render
