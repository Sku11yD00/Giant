// The null backend: no GPU. Validates every call the way the D3D12 debug
// layer would (and more: stale handles, pass and pipeline mismatches), so
// tests catch misuse that a real backend would turn into a GPU crash. It
// also emulates memory: buffer contents, copies, uploads and render-pass
// clears are real, so tests can read results back. Draws and dispatches are
// counted but produce no pixels.
#include "../backends.h"
#include "../pool.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace giant::rhi {

namespace {

constexpr u32 k_max_descriptors = 1u << 16;

struct NullBuffer {
    BufferDesc desc;
    std::vector<u8> memory;
    ResourceState state = ResourceState::common;
    u32 read = k_no_descriptor;
    u32 write = k_no_descriptor;
};

struct NullTexture {
    TextureDesc desc;
    std::vector<u8> pixels;
    ResourceState state = ResourceState::common;
    u32 read = k_no_descriptor;
    u32 write = k_no_descriptor;
};

struct NullPipeline {
    bool compute = false;
    std::array<Format, k_max_color_targets> color_formats{};
    u32 color_count = 0;
    Format depth_format = Format::unknown;
};

u8 to_unorm8(float v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

float linear_to_srgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// One pixel of `format` holding `color` (or `depth`), as the GPU stores it.
std::vector<u8> encode_clear(Format format, const Color& color, float depth) {
    std::vector<u8> px(bytes_per_pixel(format));
    switch (format) {
    case Format::rgba8_unorm:
        px = {to_unorm8(color.r), to_unorm8(color.g), to_unorm8(color.b), to_unorm8(color.a)};
        break;
    case Format::rgba8_srgb:
        px = {to_unorm8(linear_to_srgb(color.r)), to_unorm8(linear_to_srgb(color.g)),
              to_unorm8(linear_to_srgb(color.b)), to_unorm8(color.a)};
        break;
    case Format::r32_float: std::memcpy(px.data(), &color.r, 4); break;
    case Format::depth32_float: std::memcpy(px.data(), &depth, 4); break;
    case Format::r32_uint: {
        const u32 v = static_cast<u32>(color.r);
        std::memcpy(px.data(), &v, 4);
        break;
    }
    case Format::rg32_uint: {
        const u32 v[2] = {static_cast<u32>(color.r), static_cast<u32>(color.g)};
        std::memcpy(px.data(), v, 8);
        break;
    }
    case Format::rgba16_float:
    case Format::unknown: break; // left zero: tests do not read these back
    }
    return px;
}

// Repeats one pixel over the whole image, doubling the copied block each
// step (a per-pixel loop is far too slow for a 4K target in debug builds).
void fill(std::vector<u8>& image, const std::vector<u8>& pixel) {
    if (pixel.empty() || image.size() < pixel.size()) {
        return;
    }
    std::memcpy(image.data(), pixel.data(), pixel.size());
    usize filled = pixel.size();
    while (filled < image.size()) {
        const usize n = std::min(filled, image.size() - filled);
        std::memcpy(image.data() + filled, image.data(), n);
        filled += n;
    }
}

class NullDevice;

class NullSwapChain final : public SwapChain {
public:
    NullSwapChain(NullDevice& device, u32 width, u32 height);
    ~NullSwapChain() override;
    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    TextureHandle current_texture() const override { return m_texture; }
    void resize(u32 width, u32 height) override;

private:
    void make_texture();
    NullDevice& m_device;
    TextureHandle m_texture;
    u32 m_width;
    u32 m_height;
};

class NullCommandList final : public CommandList {
public:
    explicit NullCommandList(NullDevice& device) : m_device(device) {}

    void barrier(BufferHandle buffer, ResourceState state) override;
    void barrier(TextureHandle texture, ResourceState state) override;
    void begin_render_pass(const RenderPassDesc& desc) override;
    void end_render_pass() override;
    void set_pipeline(PipelineHandle pipeline) override;
    void set_push_constants(const void* data, u32 bytes) override;
    void set_scissor(u32 x, u32 y, u32 width, u32 height) override;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex) override;
    void set_index_buffer(BufferHandle buffer, u64 offset) override;
    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                      i32 vertex_offset) override;
    void dispatch(u32 x, u32 y, u32 z) override;
    void copy_buffer(BufferHandle dst, u64 dst_offset, BufferHandle src, u64 src_offset,
                     u64 bytes) override;
    TransientMemory allocate(u32 bytes) override;
    void draw_ui(TextureHandle target, const UiFrame& ui) override;

    void begin(); // a new frame
    void end();

private:
    void check_draw();

    NullDevice& m_device;
    bool m_in_pass = false;
    std::array<Format, k_max_color_targets> m_pass_colors{};
    u32 m_pass_color_count = 0;
    Format m_pass_depth = Format::unknown;
    u32 m_pass_width = 0;
    u32 m_pass_height = 0;
    PipelineHandle m_pipeline;
    bool m_pipeline_compute = false;
    bool m_have_push = false;
    BufferHandle m_index_buffer;
    u64 m_index_offset = 0;
    BufferHandle m_transient;
    u32 m_transient_used = 0;
    std::vector<BufferHandle> m_retired; // full transient buffers of this frame
};

class NullDevice final : public Device {
public:
    NullDevice() : m_list(*this) { m_info.name = "null device"; }

    Backend backend() const override { return Backend::null; }
    const AdapterInfo& adapter() const override { return m_info; }

    std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) override {
        GIANT_ASSERT(desc.buffer_count >= 2 && desc.buffer_count <= 3, "2 or 3 back buffers");
        return std::make_unique<NullSwapChain>(*this, std::max(desc.width, 1u),
                                               std::max(desc.height, 1u));
    }

    BufferHandle create_buffer(const BufferDesc& desc, std::span<const u8> initial) override {
        GIANT_ASSERT(desc.size > 0 && desc.size % 4 == 0, "buffer size must be a positive multiple of 4");
        GIANT_ASSERT(initial.size() <= desc.size, "initial data larger than the buffer");
        GIANT_ASSERT(desc.memory == Memory::gpu || !has(desc.usage, BufferUsage::shader_write),
                     "only gpu-memory buffers can be written by shaders");
        NullBuffer buffer;
        buffer.desc = desc;
        buffer.memory.assign(desc.size, 0);
        std::copy(initial.begin(), initial.end(), buffer.memory.begin());
        if (has(desc.usage, BufferUsage::shader_read)) {
            buffer.read = allocate_descriptor();
        }
        if (has(desc.usage, BufferUsage::shader_write)) {
            buffer.write = allocate_descriptor();
        }
        return m_buffers.add(std::move(buffer));
    }

    TextureHandle create_texture(const TextureDesc& desc, std::span<const u8> initial) override {
        GIANT_ASSERT(desc.width > 0 && desc.height > 0 && desc.format != Format::unknown,
                     "texture needs a size and a format");
        GIANT_ASSERT(desc.usage != TextureUsage::none, "texture needs a usage");
        GIANT_ASSERT(is_depth(desc.format) == has(desc.usage, TextureUsage::depth_target) ||
                         (!is_depth(desc.format) && !has(desc.usage, TextureUsage::depth_target)),
                     "depth formats are for depth targets and depth targets need depth formats");
        GIANT_ASSERT(!(is_depth(desc.format) && (has(desc.usage, TextureUsage::render_target) ||
                                                 has(desc.usage, TextureUsage::shader_write))),
                     "a depth texture cannot be a colour target or shader-written");
        const usize size = static_cast<usize>(desc.width) * desc.height * bytes_per_pixel(desc.format);
        GIANT_ASSERT(initial.empty() || initial.size() == size, "initial pixels have the wrong size");
        NullTexture texture;
        texture.desc = desc;
        texture.pixels.assign(size, 0);
        std::copy(initial.begin(), initial.end(), texture.pixels.begin());
        if (has(desc.usage, TextureUsage::sampled)) {
            texture.read = allocate_descriptor();
        }
        if (has(desc.usage, TextureUsage::shader_write)) {
            texture.write = allocate_descriptor();
        }
        return m_textures.add(std::move(texture));
    }

    PipelineHandle create_pipeline(const GraphicsPipelineDesc& desc) override {
        GIANT_ASSERT(desc.color_count <= k_max_color_targets, "too many colour targets");
        GIANT_ASSERT(desc.color_count > 0 || desc.depth_format != Format::unknown,
                     "a pipeline needs a colour or depth target");
        GIANT_ASSERT(desc.depth == DepthMode::none || is_depth(desc.depth_format),
                     "depth testing needs a depth format");
        for (u32 i = 0; i < desc.color_count; ++i) {
            GIANT_ASSERT(desc.color_formats[i] != Format::unknown && !is_depth(desc.color_formats[i]),
                         "colour targets need colour formats");
        }
        NullPipeline pipeline;
        pipeline.color_formats = desc.color_formats;
        pipeline.color_count = desc.color_count;
        pipeline.depth_format = desc.depth_format;
        return m_pipelines.add(pipeline);
    }

    PipelineHandle create_pipeline(const ComputePipelineDesc&) override {
        NullPipeline pipeline;
        pipeline.compute = true;
        return m_pipelines.add(pipeline);
    }

    void destroy(BufferHandle buffer) override {
        if (!buffer.valid()) {
            return;
        }
        const NullBuffer b = m_buffers.take(buffer);
        free_descriptor(b.read);
        free_descriptor(b.write);
    }
    void destroy(TextureHandle texture) override {
        if (!texture.valid()) {
            return;
        }
        const NullTexture t = m_textures.take(texture);
        free_descriptor(t.read);
        free_descriptor(t.write);
    }
    void destroy(PipelineHandle pipeline) override {
        if (pipeline.valid()) {
            m_pipelines.take(pipeline);
        }
    }

    u32 read_descriptor(BufferHandle h) const override { return descriptor(m_buffers.get(h), true); }
    u32 write_descriptor(BufferHandle h) const override { return descriptor(m_buffers.get(h), false); }
    u32 read_descriptor(TextureHandle h) const override { return descriptor(m_textures.get(h), true); }
    u32 write_descriptor(TextureHandle h) const override {
        return descriptor(m_textures.get(h), false);
    }

    const BufferDesc* describe(BufferHandle h) const override {
        const NullBuffer* b = m_buffers.get(h);
        return b != nullptr ? &b->desc : nullptr;
    }
    const TextureDesc* describe(TextureHandle h) const override {
        const NullTexture* t = m_textures.get(h);
        return t != nullptr ? &t->desc : nullptr;
    }

    u8* mapped(BufferHandle h) override {
        NullBuffer* b = m_buffers.get(h);
        GIANT_ASSERT(b != nullptr, "mapped: dead buffer");
        GIANT_ASSERT(b->desc.memory != Memory::gpu, "only upload and readback buffers are mapped");
        return b->memory.data();
    }

    CommandList* begin_frame(SwapChain* swap_chain) override {
        GIANT_ASSERT(!m_recording, "begin_frame while another frame is recording");
        m_recording = true;
        m_swap_chain = swap_chain;
        m_stats = {};
        m_list.begin();
        if (swap_chain != nullptr) {
            NullTexture* back = m_textures.get(swap_chain->current_texture());
            GIANT_ASSERT(back != nullptr, "swap chain lost its back buffer");
            back->state = ResourceState::common; // presented buffers come back as common
        }
        return &m_list;
    }

    void end_frame() override {
        GIANT_ASSERT(m_recording, "end_frame without begin_frame");
        m_list.end();
        // As in D3D12: buffers decay to common once the frame's work is done,
        // so each frame barriers them again before use.
        m_buffers.for_each([](BufferHandle, NullBuffer& b) { b.state = ResourceState::common; });
        if (m_swap_chain != nullptr) {
            NullTexture* back = m_textures.get(m_swap_chain->current_texture());
            GIANT_ASSERT(back != nullptr, "swap chain lost its back buffer");
            if (back != nullptr && back->state != ResourceState::present) {
                ++m_stats.barriers; // D3D12 records this transition too
            }
            back->state = ResourceState::present;
        }
        m_swap_chain = nullptr;
        m_recording = false;
        m_last_stats = m_stats;
        ++m_frames;
    }

    const FrameStats& last_frame_stats() const override { return m_last_stats; }

    std::vector<u8> read_texture(TextureHandle h) override {
        GIANT_ASSERT(!m_recording, "read_texture inside a frame");
        const NullTexture* t = m_textures.get(h);
        GIANT_ASSERT(t != nullptr, "read_texture: dead texture");
        return t != nullptr ? t->pixels : std::vector<u8>{};
    }

    std::vector<u8> read_buffer(BufferHandle h) override {
        GIANT_ASSERT(!m_recording, "read_buffer inside a frame");
        const NullBuffer* b = m_buffers.get(h);
        GIANT_ASSERT(b != nullptr, "read_buffer: dead buffer");
        return b != nullptr ? b->memory : std::vector<u8>{};
    }

    void wait_idle() override { GIANT_ASSERT(!m_recording, "wait_idle inside a frame"); }
    u64 frames_completed() const override { return m_frames; }
    bool device_lost() const override { return false; }
    u32 validation_errors() const override { return 0; }
    bool validation_enabled() const override { return false; }

    // For the command list and swap chains.
    NullBuffer* buffer(BufferHandle h) { return m_buffers.get(h); }
    NullTexture* texture(TextureHandle h) { return m_textures.get(h); }
    NullPipeline* pipeline(PipelineHandle h) { return m_pipelines.get(h); }
    FrameStats& stats() { return m_stats; }
    bool recording() const { return m_recording; }

private:
    template <typename R>
    static u32 descriptor(const R* resource, bool read) {
        if (resource == nullptr) {
            return k_no_descriptor;
        }
        return read ? resource->read : resource->write;
    }

    u32 allocate_descriptor() {
        if (!m_free_descriptors.empty()) {
            const u32 d = m_free_descriptors.back();
            m_free_descriptors.pop_back();
            return d;
        }
        GIANT_ASSERT(m_next_descriptor < k_max_descriptors, "out of descriptors");
        return m_next_descriptor++;
    }
    void free_descriptor(u32 d) {
        if (d != k_no_descriptor) {
            m_free_descriptors.push_back(d);
        }
    }

    AdapterInfo m_info;
    Pool<NullBuffer, BufferTag> m_buffers;
    Pool<NullTexture, TextureTag> m_textures;
    Pool<NullPipeline, PipelineTag> m_pipelines;
    std::vector<u32> m_free_descriptors;
    u32 m_next_descriptor = 0;
    NullCommandList m_list;
    SwapChain* m_swap_chain = nullptr;
    bool m_recording = false;
    FrameStats m_stats;
    FrameStats m_last_stats;
    u64 m_frames = 0;
};

// --- Swap chain --------------------------------------------------------------

NullSwapChain::NullSwapChain(NullDevice& device, u32 width, u32 height)
    : m_device(device), m_width(width), m_height(height) {
    make_texture();
}

NullSwapChain::~NullSwapChain() { m_device.destroy(m_texture); }

void NullSwapChain::make_texture() {
    TextureDesc desc;
    desc.width = m_width;
    desc.height = m_height;
    desc.format = Format::rgba8_srgb;
    desc.usage = TextureUsage::render_target;
    desc.name = "back buffer";
    m_texture = m_device.create_texture(desc, {});
}

void NullSwapChain::resize(u32 width, u32 height) {
    GIANT_ASSERT(!m_device.recording(), "resize inside a frame");
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }
    m_device.destroy(m_texture);
    m_width = width;
    m_height = height;
    make_texture();
}

// --- Command list ------------------------------------------------------------

void NullCommandList::begin() {
    m_in_pass = false;
    m_pipeline = {};
    m_have_push = false;
    m_index_buffer = {};
    m_transient_used = 0;
}

void NullCommandList::end() {
    GIANT_ASSERT(!m_in_pass, "end_frame inside a render pass");
    // Transient memory is per frame: retire this frame's buffer.
    m_device.destroy(m_transient);
    m_transient = {};
    for (const BufferHandle old : m_retired) {
        m_device.destroy(old);
    }
    m_retired.clear();
}

void NullCommandList::barrier(BufferHandle handle, ResourceState state) {
    GIANT_ASSERT(!m_in_pass, "barriers belong outside render passes");
    NullBuffer* b = m_device.buffer(handle);
    GIANT_ASSERT(b != nullptr, "barrier on a dead buffer");
    GIANT_ASSERT(b->desc.memory == Memory::gpu, "upload and readback buffers keep their state");
    GIANT_ASSERT(state != ResourceState::shader_write || has(b->desc.usage, BufferUsage::shader_write),
                 "shader_write needs BufferUsage::shader_write");
    GIANT_ASSERT(state != ResourceState::index_buffer || has(b->desc.usage, BufferUsage::index),
                 "index_buffer needs BufferUsage::index");
    GIANT_ASSERT(state != ResourceState::indirect_argument ||
                     has(b->desc.usage, BufferUsage::indirect),
                 "indirect_argument needs BufferUsage::indirect");
    GIANT_ASSERT(state != ResourceState::render_target && state != ResourceState::depth_write &&
                     state != ResourceState::depth_read && state != ResourceState::present,
                 "not a buffer state");
    if (b->state != state || state == ResourceState::shader_write) {
        ++m_device.stats().barriers;
    }
    b->state = state;
}

void NullCommandList::barrier(TextureHandle handle, ResourceState state) {
    GIANT_ASSERT(!m_in_pass, "barriers belong outside render passes");
    NullTexture* t = m_device.texture(handle);
    GIANT_ASSERT(t != nullptr, "barrier on a dead texture");
    const TextureUsage usage = t->desc.usage;
    GIANT_ASSERT(state != ResourceState::shader_write || has(usage, TextureUsage::shader_write),
                 "shader_write needs TextureUsage::shader_write");
    GIANT_ASSERT(state != ResourceState::shader_read || has(usage, TextureUsage::sampled),
                 "shader_read needs TextureUsage::sampled");
    GIANT_ASSERT(state != ResourceState::render_target || has(usage, TextureUsage::render_target),
                 "render_target needs TextureUsage::render_target");
    GIANT_ASSERT((state != ResourceState::depth_write && state != ResourceState::depth_read) ||
                     has(usage, TextureUsage::depth_target),
                 "depth states need TextureUsage::depth_target");
    GIANT_ASSERT(state != ResourceState::index_buffer && state != ResourceState::indirect_argument,
                 "not a texture state");
    GIANT_ASSERT(state != ResourceState::present, "only end_frame presents");
    if (t->state != state || state == ResourceState::shader_write) {
        ++m_device.stats().barriers;
    }
    t->state = state;
}

void NullCommandList::begin_render_pass(const RenderPassDesc& desc) {
    GIANT_ASSERT(!m_in_pass, "render passes do not nest");
    GIANT_ASSERT(desc.color_count <= k_max_color_targets, "too many colour targets");
    GIANT_ASSERT(desc.color_count > 0 || desc.depth.texture.valid(), "an empty render pass");
    m_pass_width = 0;
    m_pass_height = 0;
    const auto size_check = [this](const TextureDesc& d) {
        if (m_pass_width == 0) {
            m_pass_width = d.width;
            m_pass_height = d.height;
        }
        GIANT_ASSERT(d.width == m_pass_width && d.height == m_pass_height,
                     "render pass targets differ in size");
    };
    m_pass_colors = {};
    for (u32 i = 0; i < desc.color_count; ++i) {
        const ColorTarget& target = desc.colors[i];
        NullTexture* t = m_device.texture(target.texture);
        GIANT_ASSERT(t != nullptr, "render pass: dead colour target");
        if (t == nullptr) {
            continue;
        }
        GIANT_ASSERT(has(t->desc.usage, TextureUsage::render_target), "not a render target");
        size_check(t->desc);
        barrier(target.texture, ResourceState::render_target);
        m_pass_colors[i] = t->desc.format;
        if (target.load == LoadOp::clear) {
            fill(t->pixels, encode_clear(t->desc.format, target.clear, 0.0f));
        }
    }
    m_pass_color_count = desc.color_count;
    m_pass_depth = Format::unknown;
    if (desc.depth.texture.valid()) {
        NullTexture* t = m_device.texture(desc.depth.texture);
        GIANT_ASSERT(t != nullptr, "render pass: dead depth target");
        if (t != nullptr) {
            GIANT_ASSERT(has(t->desc.usage, TextureUsage::depth_target), "not a depth target");
            size_check(t->desc);
            barrier(desc.depth.texture, ResourceState::depth_write);
            m_pass_depth = t->desc.format;
            if (desc.depth.load == LoadOp::clear) {
                fill(t->pixels, encode_clear(t->desc.format, {}, desc.depth.clear_depth));
            }
        }
    }
    m_in_pass = true;
}

void NullCommandList::end_render_pass() {
    GIANT_ASSERT(m_in_pass, "end_render_pass without begin_render_pass");
    m_in_pass = false;
}

void NullCommandList::set_pipeline(PipelineHandle handle) {
    NullPipeline* p = m_device.pipeline(handle);
    GIANT_ASSERT(p != nullptr, "set_pipeline: dead pipeline");
    m_pipeline = handle;
    m_pipeline_compute = p != nullptr && p->compute;
}

void NullCommandList::set_push_constants(const void* data, u32 bytes) {
    GIANT_ASSERT(data != nullptr || bytes == 0, "push constants without data");
    GIANT_ASSERT(bytes <= k_max_push_constant_bytes && bytes % 4 == 0,
                 "push constants: at most 128 bytes, a multiple of 4");
    m_have_push = true;
}

void NullCommandList::set_scissor(u32 x, u32 y, u32 width, u32 height) {
    GIANT_ASSERT(m_in_pass, "set_scissor outside a render pass");
    GIANT_ASSERT(static_cast<u64>(x) + width <= m_pass_width &&
                     static_cast<u64>(y) + height <= m_pass_height,
                 "scissor outside the render targets");
}

void NullCommandList::check_draw() {
    GIANT_ASSERT(m_in_pass, "draw outside a render pass");
    NullPipeline* p = m_device.pipeline(m_pipeline);
    GIANT_ASSERT(p != nullptr && !p->compute, "draw needs a live graphics pipeline");
    if (p == nullptr) {
        return;
    }
    GIANT_ASSERT(p->color_count == m_pass_color_count && p->depth_format == m_pass_depth,
                 "pipeline targets do not match the render pass");
    for (u32 i = 0; i < p->color_count; ++i) {
        GIANT_ASSERT(p->color_formats[i] == m_pass_colors[i],
                     "pipeline colour format does not match the render pass");
    }
    ++m_device.stats().draws;
}

void NullCommandList::draw(u32, u32, u32) { // zero counts are a no-op, as in D3D12
    check_draw();
}

void NullCommandList::set_index_buffer(BufferHandle handle, u64 offset) {
    NullBuffer* b = m_device.buffer(handle);
    GIANT_ASSERT(b != nullptr && has(b->desc.usage, BufferUsage::index),
                 "set_index_buffer needs a live buffer with BufferUsage::index");
    GIANT_ASSERT(offset % 4 == 0, "index buffer offset must be a multiple of 4");
    if (b != nullptr && b->desc.memory == Memory::gpu) {
        GIANT_ASSERT(b->state == ResourceState::index_buffer,
                     "barrier the index buffer to index_buffer first");
    }
    m_index_buffer = handle;
    m_index_offset = offset;
}

void NullCommandList::draw_indexed(u32 index_count, u32, u32 first_index, i32) {
    check_draw();
    NullBuffer* b = m_device.buffer(m_index_buffer);
    GIANT_ASSERT(b != nullptr, "draw_indexed needs an index buffer");
    if (b != nullptr) {
        GIANT_ASSERT(m_index_offset + (static_cast<u64>(first_index) + index_count) * 4 <= b->desc.size,
                     "draw_indexed reads past the index buffer");
    }
}

void NullCommandList::dispatch(u32 x, u32 y, u32 z) {
    GIANT_ASSERT(!m_in_pass, "dispatch inside a render pass");
    NullPipeline* p = m_device.pipeline(m_pipeline);
    GIANT_ASSERT(p != nullptr && p->compute, "dispatch needs a live compute pipeline");
    // Zero groups is a legal no-op (an empty culling pass), as in D3D12.
    GIANT_ASSERT(x <= 65535 && y <= 65535 && z <= 65535, "dispatch group counts are at most 65535");
    ++m_device.stats().dispatches;
}

void NullCommandList::copy_buffer(BufferHandle dst, u64 dst_offset, BufferHandle src,
                                  u64 src_offset, u64 bytes) {
    GIANT_ASSERT(!m_in_pass, "copy inside a render pass");
    NullBuffer* d = m_device.buffer(dst);
    NullBuffer* s = m_device.buffer(src);
    GIANT_ASSERT(d != nullptr && s != nullptr, "copy_buffer: dead buffer");
    if (d == nullptr || s == nullptr) {
        return;
    }
    GIANT_ASSERT(dst_offset + bytes <= d->desc.size && src_offset + bytes <= s->desc.size,
                 "copy_buffer out of bounds");
    GIANT_ASSERT(d->desc.memory != Memory::upload, "cannot copy into an upload buffer");
    GIANT_ASSERT(dst != src, "copy_buffer within one buffer");
    if (d->desc.memory == Memory::gpu) {
        GIANT_ASSERT(d->state == ResourceState::copy_dest, "barrier the destination to copy_dest");
    }
    if (s->desc.memory == Memory::gpu) {
        GIANT_ASSERT(s->state == ResourceState::copy_source,
                     "barrier the source to copy_source");
    }
    std::memmove(d->memory.data() + dst_offset, s->memory.data() + src_offset, bytes);
}

TransientMemory NullCommandList::allocate(u32 bytes) {
    GIANT_ASSERT(bytes > 0 && bytes <= k_max_transient_allocation,
                 "transient allocations are 1 byte to 256 MiB");
    if (bytes == 0 || bytes > k_max_transient_allocation) {
        return {};
    }
    const u32 aligned = (bytes + (k_transient_alignment - 1)) & ~(k_transient_alignment - 1);
    NullBuffer* current = m_device.buffer(m_transient);
    if (current == nullptr || u64{m_transient_used} + aligned > current->desc.size) {
        // A new, larger buffer; the old one lives until the frame ends in a
        // real backend, which the null one models by never moving memory.
        const u64 size = std::max<u64>(u64{1} << 20, u64{aligned} * 2);
        BufferDesc desc;
        desc.size = size;
        desc.memory = Memory::upload;
        desc.usage = BufferUsage::shader_read;
        desc.name = "transient";
        const BufferHandle old = m_transient;
        m_transient = m_device.create_buffer(desc, {});
        m_transient_used = 0;
        // Keep the old allocation's pointer valid until the frame ends.
        if (old.valid()) {
            m_retired.push_back(old);
        }
        current = m_device.buffer(m_transient);
    }
    TransientMemory memory;
    memory.buffer = m_transient;
    memory.descriptor = current->read;
    memory.offset = m_transient_used;
    memory.data = current->memory.data() + m_transient_used;
    memory.size = bytes;
    m_transient_used += aligned;
    m_device.stats().transient_bytes += aligned;
    return memory;
}

void NullCommandList::draw_ui(TextureHandle target, const UiFrame& ui) {
    GIANT_ASSERT(!m_in_pass, "draw_ui opens its own pass");
    NullTexture* t = m_device.texture(target);
    GIANT_ASSERT(t != nullptr && has(t->desc.usage, TextureUsage::render_target) &&
                     (t->desc.format == Format::rgba8_unorm || t->desc.format == Format::rgba8_srgb),
                 "draw_ui needs a live rgba8 render target");
    if (t == nullptr) {
        return;
    }
    // As in D3D12: sampled textures become readable before the pass.
    for (const UiDrawList& list : ui.lists) {
        for (const UiDrawCommand& cmd : list.commands) {
            const TextureHandle texture = texture_from_ui_id(cmd.texture);
            NullTexture* tex = m_device.texture(texture);
            if (tex != nullptr && has(tex->desc.usage, TextureUsage::sampled) && !(texture == target)) {
                barrier(texture, ResourceState::shader_read);
            }
        }
    }
    barrier(target, ResourceState::render_target);
    const float w = static_cast<float>(t->desc.width);
    const float h = static_cast<float>(t->desc.height);
    u32 draws = 0;
    for (const UiDrawList& list : ui.lists) {
        for (const UiDrawCommand& cmd : list.commands) {
            GIANT_ASSERT(cmd.index_count % 3 == 0, "UI commands draw whole triangles");
            GIANT_ASSERT(static_cast<u64>(cmd.first_index) + cmd.index_count <= list.indices.size(),
                         "UI command reads past its indices");
            NullTexture* tex = m_device.texture(texture_from_ui_id(cmd.texture));
            GIANT_ASSERT(tex != nullptr && has(tex->desc.usage, TextureUsage::sampled) &&
                             tex->desc.format == Format::rgba8_unorm &&
                             !(texture_from_ui_id(cmd.texture) == target),
                         "UI command needs a live, sampled rgba8_unorm texture (not the target)");
            for (u32 i = 0; i < cmd.index_count && cmd.first_index + i < list.indices.size(); ++i) {
                GIANT_ASSERT(static_cast<u64>(list.indices[cmd.first_index + i]) + cmd.vertex_offset <
                                 list.vertices.size(),
                             "UI index points past the vertices");
            }
            // Counted like D3D12: clip rectangles clamped to the target.
            const float x0 = std::max(cmd.clip_min_x, 0.0f);
            const float y0 = std::max(cmd.clip_min_y, 0.0f);
            const float x1 = std::min(cmd.clip_max_x, w);
            const float y1 = std::min(cmd.clip_max_y, h);
            if (cmd.index_count > 0 && x1 > x0 && y1 > y0) {
                ++draws;
            }
        }
    }
    m_device.stats().ui_draws += draws;
    m_device.stats().draws += draws;
}

} // namespace

std::unique_ptr<Device> create_null_device() { return std::make_unique<NullDevice>(); }

} // namespace giant::rhi
