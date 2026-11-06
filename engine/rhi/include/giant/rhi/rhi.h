// The thin graphics layer. Everything above it (render, editor) talks to the
// GPU only through these types, so a Vulkan backend can be added later.
//
// Model (N1):
// - Resources (buffers, textures, pipelines) are handles into the device.
//   Shaders reach buffers and textures bindlessly: every shader-visible
//   resource has a descriptor index (shader model 6.6 descriptor heaps), and
//   draws pass those indices in push constants. There is one global layout,
//   so pipelines need no per-shader binding setup.
// - Work is recorded into the frame's CommandList between begin_frame and
//   end_frame. Frames in flight live on the device: the CPU records frame N+1
//   while the GPU runs frame N, and destroying a resource is deferred until
//   the GPU is done with it.
// - One thread records at a time (for now). Resource states are tracked per
//   resource; barrier() moves a resource to the state the next use needs,
//   render passes move their targets themselves.
//
// See docs/architecture.md, "Graphics layer".
#pragma once

#include "giant/core/types.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace giant::rhi {

enum class Backend {
    null,  // no GPU: validates calls, for tests and headless tools
    d3d12, // Windows
};

const char* to_string(Backend backend);

// The best backend this build supports on this platform.
Backend default_backend();

struct DeviceDesc {
    Backend backend = default_backend();
    bool debug_layer = false;      // GPU validation; slower, for development
    bool software_adapter = false; // D3D12: use WARP (CPU) instead of a GPU
};

struct AdapterInfo {
    std::string name;
    bool software = false;
    u64 dedicated_video_memory = 0; // bytes
    bool mesh_shaders = false;
    bool shader_model_6_6 = false;
    // D3D12: the runtime came from the D3D12 folder next to the exe (the
    // pinned Agility SDK), not the older one built into Windows.
    bool agility_sdk_runtime = false;

    // The engine's minimum spec (DirectX 12 Ultimate class). Ray tracing is
    // deliberately not required.
    bool meets_minimum_spec() const { return mesh_shaders && shader_model_6_6; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// --- Handles -----------------------------------------------------------------

// An index into one of the device's resource pools, with a generation so a
// stale handle to a destroyed (and reused) slot is caught. Zero is "none".
template <typename Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0;

    constexpr bool valid() const { return generation != 0; }
    constexpr bool operator==(const Handle&) const = default;
};

using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using PipelineHandle = Handle<struct PipelineTag>;

// "No descriptor": what descriptor queries return for a resource without
// that kind of view. Shaders never see it for resources they are given.
constexpr u32 k_no_descriptor = 0xFFFF'FFFFu;

// Fixed samplers, by their index in the sampler heap (SamplerDescriptorHeap
// in shaders).
enum class Sampler : u32 {
    linear_clamp = 0,
    linear_wrap = 1,
    point_clamp = 2,
    anisotropic_wrap = 3, // 8x
};

// --- Resources ---------------------------------------------------------------

enum class Format {
    unknown,
    rgba8_unorm,   // gamma-space colour as stored (UI)
    rgba8_srgb,    // colour in linear space, stored sRGB-encoded
    rgba16_float,  // HDR colour
    r32_float,
    r32_uint,
    rg32_uint,     // e.g. a visibility buffer: cluster and triangle ids
    depth32_float, // reverse-Z depth: 1 is near, 0 is infinitely far
};

u32 bytes_per_pixel(Format format);
bool is_depth(Format format);

enum class Memory {
    gpu,      // fastest for the GPU; filled through uploads or by shaders
    upload,   // CPU writes (always mapped), GPU reads. The GPU may still be reading
              // it up to k_frames_in_flight frames later: for per-frame data use
              // CommandList::allocate, or keep one buffer per frame in flight.
    readback, // GPU writes (copies), CPU reads after a wait
};

// Bit flags; combine with |.
enum class BufferUsage : u32 {
    none = 0,
    shader_read = 1u << 0,  // ByteAddressBuffer in shaders
    shader_write = 1u << 1, // RWByteAddressBuffer in shaders
    index = 1u << 2,        // 32-bit index buffer
    indirect = 1u << 3,     // arguments for indirect draws and dispatches
};

enum class TextureUsage : u32 {
    none = 0,
    sampled = 1u << 0,       // Texture2D in shaders
    shader_write = 1u << 1,  // RWTexture2D in shaders
    render_target = 1u << 2, // colour attachment
    depth_target = 1u << 3,  // depth attachment
};

constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool has(BufferUsage set, BufferUsage flag) {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0;
}
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool has(TextureUsage set, TextureUsage flag) {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0;
}

struct BufferDesc {
    u64 size = 0; // bytes, a multiple of 4
    BufferUsage usage = BufferUsage::shader_read;
    Memory memory = Memory::gpu;
    const char* name = "buffer"; // shows in GPU debuggers and errors
};

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    Format format = Format::rgba8_srgb;
    TextureUsage usage = TextureUsage::sampled;
    const char* name = "texture";
};

// What a resource is used for next. barrier() switches between them.
enum class ResourceState {
    common,         // just created, or untracked (copies, uploads)
    shader_read,    // sampled or read in any shader stage
    shader_write,   // UAV; barrier() to shader_write again waits for earlier writes
    render_target,
    depth_write,
    depth_read,
    index_buffer,
    indirect_argument,
    copy_source,
    copy_dest,
    present, // swap chain buffers only
};

// --- Pipelines ---------------------------------------------------------------

// Compiled bytecode for this device's backend (DXIL for D3D12), as produced
// by the build (giant_shaders in cmake/GiantShaders.cmake). The null
// backend accepts anything, including empty code.
using ShaderCode = std::span<const u8>;

enum class CullMode { none, back, front };
enum class DepthMode {
    none,       // no depth test
    test_write, // nearer (greater, reverse-Z) wins and is written
    test_only,  // tested, not written (e.g. decals, transparents)
};
enum class BlendMode {
    opaque,
    alpha, // straight alpha: src * a + dst * (1 - a)
};

constexpr u32 k_max_color_targets = 4;

// What feeds the rasterizer: a vertex shader (draw calls), or a mesh shader
// (dispatch_mesh; needs AdapterInfo::mesh_shaders).
enum class GeometryStage { vertex, mesh };

struct GraphicsPipelineDesc {
    GeometryStage geometry = GeometryStage::vertex;
    ShaderCode vertex; // for GeometryStage::vertex
    ShaderCode mesh;   // for GeometryStage::mesh
    ShaderCode pixel;
    std::array<Format, k_max_color_targets> color_formats{};
    u32 color_count = 1;
    Format depth_format = Format::unknown;
    DepthMode depth = DepthMode::none;
    CullMode cull = CullMode::back; // front faces wind counter-clockwise
    BlendMode blend = BlendMode::opaque;
    const char* name = "pipeline";
};

struct ComputePipelineDesc {
    ShaderCode compute;
    const char* name = "compute pipeline";
};

// Push constants: up to this many bytes per draw or dispatch, read in
// shaders from register b0 (see engine/rhi/shaders/giant/bindless.slang).
constexpr u32 k_max_push_constant_bytes = 128;

// --- 2D UI drawing (editor panels) ------------------------------------------
//
// Plain data, so the editor's UI library stays out of the graphics layer.
// Positions are in pixels from the top-left. UI colours and textures are
// gamma-space (as UI libraries and artists pick them) and are blended in
// gamma space, so they show exactly as chosen.

// A texture for UI drawing, as the UI library stores it; see ui_texture_id.
using TextureId = u64; // 0 = no texture

constexpr TextureId ui_texture_id(TextureHandle texture) {
    return (static_cast<u64>(texture.generation) << 32) | texture.index;
}
constexpr TextureHandle texture_from_ui_id(TextureId id) {
    return {static_cast<u32>(id & 0xFFFF'FFFFu), static_cast<u32>(id >> 32)};
}

struct UiVertex {
    float x, y; // pixels
    float u, v;
    u32 color; // RGBA8, red in the lowest byte
};

struct UiDrawCommand {
    u32 first_index = 0;   // into the list's indices
    u32 index_count = 0;   // a multiple of 3 (triangles)
    u32 vertex_offset = 0; // added to every index
    // Pixels outside this rectangle are not drawn.
    float clip_min_x = 0.0f, clip_min_y = 0.0f, clip_max_x = 0.0f, clip_max_y = 0.0f;
    TextureId texture = 0; // a live, sampled rgba8_unorm texture
};

struct UiDrawList {
    std::span<const UiVertex> vertices;
    std::span<const u32> indices;
    std::span<const UiDrawCommand> commands;
};

struct UiFrame {
    std::span<const UiDrawList> lists; // drawn in order
};

// --- Recording ---------------------------------------------------------------

enum class LoadOp { clear, load, discard };

struct ColorTarget {
    TextureHandle texture;
    LoadOp load = LoadOp::clear;
    Color clear{};
};

struct DepthTarget {
    TextureHandle texture; // none: no depth
    LoadOp load = LoadOp::clear;
    float clear_depth = 0.0f; // reverse-Z: 0 is the far plane
};

struct RenderPassDesc {
    std::array<ColorTarget, k_max_color_targets> colors{};
    u32 color_count = 1;
    DepthTarget depth{};
    // A pass with no targets (color_count 0, no depth) draws only for what
    // its shaders write (buffers): this is its size, pixels. Ignored when
    // there are targets (the pass takes their size).
    u32 width = 0;
    u32 height = 0;
};

// Per-frame scratch memory, written by the CPU and read by shaders during
// this frame only (instance lists, constants, UI vertices). Shaders read it
// as a ByteAddressBuffer through `descriptor`, starting at `offset`.
constexpr u32 k_transient_alignment = 256;              // every allocation starts here
constexpr u32 k_max_transient_allocation = 256u << 20; // bytes in one allocation
struct TransientMemory {
    BufferHandle buffer;
    u32 descriptor = k_no_descriptor;
    u32 offset = 0; // bytes, a multiple of k_transient_alignment
    u8* data = nullptr;
    u32 size = 0;
};

// Counts from the most recent frame (or offscreen submit); for tests, the
// editor's stats and budgets.
struct FrameStats {
    u32 draws = 0;        // draw calls (direct, indirect, mesh), including UI
    u32 dispatches = 0;
    u32 ui_draws = 0;     // UI draw commands submitted (empty clips skipped)
    u32 barriers = 0;
    u64 transient_bytes = 0;
    u64 upload_bytes = 0; // initial data of resources created inside the frame
};

// GPU time of one CommandList::begin_timing scope.
struct GpuTiming {
    std::string name;
    float ms = 0.0f;
};

// Timed scopes per frame; more are not timed (logged once).
constexpr u32 k_max_gpu_timings = 32;

class CommandList {
public:
    virtual ~CommandList() = default;

    // Moves a resource to `state` for its next use. Calling it with
    // shader_write when it already is shader_write waits for earlier writes
    // (a UAV barrier).
    virtual void barrier(BufferHandle buffer, ResourceState state) = 0;
    virtual void barrier(TextureHandle texture, ResourceState state) = 0;

    // Targets are moved to render_target / depth_write. Viewport and scissor
    // cover the first target. All targets must have the same size.
    virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
    virtual void end_render_pass() = 0;

    virtual void set_pipeline(PipelineHandle pipeline) = 0;
    // Copied now; at most k_max_push_constant_bytes, a multiple of 4.
    virtual void set_push_constants(const void* data, u32 bytes) = 0;
    template <typename T>
    void push(const T& constants) {
        static_assert(sizeof(T) % 4 == 0 && sizeof(T) <= k_max_push_constant_bytes);
        set_push_constants(&constants, sizeof(T));
    }
    // Pixels outside this rectangle are not drawn, until the next pass.
    virtual void set_scissor(u32 x, u32 y, u32 width, u32 height) = 0;

    // Vertices are pulled in the shader (SV_VertexID); there are no vertex
    // buffers. SV_InstanceID starts at 0: pass a base in push constants.
    virtual void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0) = 0;
    virtual void set_index_buffer(BufferHandle buffer, u64 offset = 0) = 0; // u32 indices
    virtual void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0,
                              i32 vertex_offset = 0) = 0;
    virtual void dispatch(u32 groups_x, u32 groups_y = 1, u32 groups_z = 1) = 0;

    // Mesh shader pipelines: groups of the mesh shader, inside a render
    // pass. At most 65535 groups per dimension (and 2^22 in all).
    virtual void dispatch_mesh(u32 groups_x, u32 groups_y = 1, u32 groups_z = 1) = 0;

    // GPU-written arguments: `args` is a gpu buffer with BufferUsage::indirect,
    // barriered to indirect_argument; `offset` is a multiple of 4.
    //   draw_indirect:          4 u32: vertex count, instance count, first vertex, first instance
    //   dispatch_indirect:      3 u32: groups x, y, z
    //   dispatch_mesh_indirect: 3 u32: groups x, y, z
    // first instance does not offset SV_InstanceID (pass a base yourself).
    virtual void draw_indirect(BufferHandle args, u64 offset = 0) = 0;
    virtual void dispatch_indirect(BufferHandle args, u64 offset = 0) = 0;
    virtual void dispatch_mesh_indirect(BufferHandle args, u64 offset = 0) = 0;

    virtual void copy_buffer(BufferHandle dst, u64 dst_offset, BufferHandle src, u64 src_offset,
                             u64 bytes) = 0;

    // Scratch memory for this frame; see TransientMemory. 1 byte to
    // k_max_transient_allocation. Fails (data null, logged) only when GPU
    // memory or descriptors run out.
    virtual TransientMemory allocate(u32 bytes) = 0;
    TransientMemory upload(std::span<const u8> bytes);
    template <typename T>
    TransientMemory upload_array(std::span<const T> items) {
        return upload({reinterpret_cast<const u8*>(items.data()), items.size_bytes()});
    }

    // Draws UI over `target` (an rgba8 render target; the swap chain's is
    // fine). Uses its own pass: call it outside begin/end_render_pass.
    virtual void draw_ui(TextureHandle target, const UiFrame& ui) = 0;

    // Times the GPU work recorded until the matching end_timing, under
    // `name` (copied). Scopes may nest; each is reported on its own, in the
    // order begun, by Device::gpu_timings a frame or two later. At most
    // k_max_gpu_timings per frame; the rest are not timed. Every scope ends
    // before end_frame.
    virtual void begin_timing(const char* name) = 0;
    virtual void end_timing() = 0;
};

struct SwapChainDesc {
    void* native_window = nullptr; // HWND on Windows; ignored by the null backend
    u32 width = 0;
    u32 height = 0;
    u32 buffer_count = 3; // 2 or 3
    bool vsync = true;
};

// Destroy every swap chain before the device that created it.
class SwapChain {
public:
    virtual ~SwapChain() = default;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    // The back buffer to draw into this frame (rgba8_srgb render target);
    // valid between begin_frame and end_frame.
    virtual TextureHandle current_texture() const = 0;
    // Waits for the GPU and resizes the back buffers. A zero size (minimised
    // window) is ignored. Not inside a frame.
    virtual void resize(u32 width, u32 height) = 0;
};

class Device {
public:
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual const AdapterInfo& adapter() const = 0;

    // Frames in flight: how many frames the CPU may record ahead of the GPU.
    static constexpr u32 k_frames_in_flight = 2;

    // --- Resources. Creation logs why it failed and returns an invalid
    // handle. Destroying is deferred until the GPU no longer uses the
    // resource, so it is safe at any time, even mid-frame.
    virtual std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) = 0;
    // `initial` (optional) fills the start of the buffer. Outside a frame
    // (load time) the copy is waited for. Inside a frame (streaming) nothing
    // waits: the data is staged in per-frame memory and copied on the GPU
    // before the frame's work, so the buffer is ready for anything the frame
    // records. Uploads inside a frame are counted in FrameStats::upload_bytes;
    // the caller keeps them within its per-frame budget.
    virtual BufferHandle create_buffer(const BufferDesc& desc, std::span<const u8> initial = {}) = 0;
    // `initial` (optional): width * height * bytes_per_pixel, rows top to
    // bottom. Copied like create_buffer's (waited for only outside a frame).
    virtual TextureHandle create_texture(const TextureDesc& desc,
                                         std::span<const u8> initial = {}) = 0;
    virtual PipelineHandle create_pipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle create_pipeline(const ComputePipelineDesc& desc) = 0;
    virtual void destroy(BufferHandle buffer) = 0;
    virtual void destroy(TextureHandle texture) = 0;
    virtual void destroy(PipelineHandle pipeline) = 0;

    // Descriptor indices for shaders (k_no_descriptor if the resource has no
    // such view: e.g. read for a buffer without shader_read usage).
    virtual u32 read_descriptor(BufferHandle buffer) const = 0;
    virtual u32 write_descriptor(BufferHandle buffer) const = 0;
    virtual u32 read_descriptor(TextureHandle texture) const = 0;
    virtual u32 write_descriptor(TextureHandle texture) const = 0;

    virtual const BufferDesc* describe(BufferHandle buffer) const = 0;   // null if dead
    virtual const TextureDesc* describe(TextureHandle texture) const = 0; // null if dead
    // CPU pointer of an upload or readback buffer (always mapped).
    virtual u8* mapped(BufferHandle buffer) = 0;

    // --- Frames. begin_frame returns null when the frame must be skipped
    // (device lost, or the swap chain could not be resized); do not call
    // end_frame then. `swap_chain` may be null for offscreen work (tests,
    // screenshots); end_frame then submits without presenting.
    virtual CommandList* begin_frame(SwapChain* swap_chain) = 0;
    virtual void end_frame() = 0;
    virtual const FrameStats& last_frame_stats() const = 0;

    // GPU times of the newest frame the GPU has finished (usually two
    // frames back; never waited for): each begin_timing scope in the order
    // begun, and the whole frame's GPU time (its uploads included). Empty and
    // 0 until a timed frame finished. The null backend reports the scopes of
    // the last frame ended, at 0 ms.
    virtual std::span<const GpuTiming> gpu_timings() const = 0;
    virtual float gpu_frame_ms() const = 0;

    // Waits for the GPU, then returns the texture's pixels (width * height *
    // bytes_per_pixel, rows top to bottom), or empty on failure. For tests
    // and screenshots; not inside a frame.
    virtual std::vector<u8> read_texture(TextureHandle texture) = 0;
    virtual std::vector<u8> read_buffer(BufferHandle buffer) = 0;

    // Blocks until the GPU has finished all submitted work, and frees
    // destroyed resources.
    virtual void wait_idle() = 0;

    // Frames ended since the device was created (presented or not).
    virtual u64 frames_completed() const = 0;

    // True once the GPU was removed or reset (driver crash, update, TDR).
    // The device cannot recover; the caller must recreate it.
    virtual bool device_lost() const = 0;

    // Errors reported by the GPU validation layer since the device was
    // created. New ones are logged on each call. Always 0 when the debug
    // layer is off or unavailable.
    virtual u32 validation_errors() const = 0;
    // Whether the validation layer is really running.
    virtual bool validation_enabled() const = 0;
};

// Returns null (and logs why) if the backend is unavailable or fails.
std::unique_ptr<Device> create_device(const DeviceDesc& desc);

// Clears a small texture on the GPU and reads it back: proves the device
// really renders. Used by tests and the editor's startup check. Not inside
// a frame. Returns the colour read, or a negative colour on failure.
Color clear_and_read_back(Device& device, const Color& color);

// Draws `ui` over an offscreen rgba8_unorm image cleared to `color` and
// returns its pixels (width * height * 4), or empty on failure. `color` is
// stored as given (gamma space, like the UI). Not inside a frame.
std::vector<u8> render_ui_offscreen(Device& device, const UiFrame& ui, u32 width, u32 height,
                                    const Color& color);

} // namespace giant::rhi
